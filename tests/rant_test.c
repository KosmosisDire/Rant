/* The test and benchmark CLI: selftest, sweep, serve, node and the benches. Built against
 * dist/rant.h with RANT_IMPLEMENTATION so the sendto and recvfrom wrappers can intercept. */
#if !defined(_WIN32)
  #ifndef _POSIX_C_SOURCE
  #define _POSIX_C_SOURCE 200809L
  #endif
  #ifndef _DEFAULT_SOURCE
  #define _DEFAULT_SOURCE 1
  #endif
  #ifndef _DARWIN_C_SOURCE
  #define _DARWIN_C_SOURCE 1   /* Darwin hides its own extensions under _POSIX_C_SOURCE without this */
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

/* Syscall instrumentation on Windows: wraps the transport's sendto and recvfrom. Two QPC
 * reads per call are cheap enough to keep on. POSIX has no wrappers, the counters stay 0. */
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
/* selftest fault injection: force transport datagrams (submessage type byte 1 to 4) to
 * would block, so the threaded phases prove eviction is surfaced. Discovery passes. */
static volatile int g_tx_block_data;
/* swallow uDTL DETAIL_RESP datagrams to this port, 0 = off: that node never verifies its
 * candidates, which is how the match wait phases hold one side of the exchange open. */
static volatile unsigned g_tx_block_detail_resp_port;
static unsigned long long g_tx_type[5], g_rx_type[5];   /* by submessage type, 0 = other */
static unsigned long long g_tx_data_ch[4], g_rx_data_ch[4];
/* the under one MTU invariant: the largest datagram any layer handed to sendto since the
 * last reset, plus the uDTL paging counters. The interest phase asserts on them. */
static volatile int g_tx_max_len;
static volatile unsigned long long g_tx_interest_req;
static int g_trace = 0, g_trace_left = 24;

#ifdef _WIN32
static LARGE_INTEGER g_qpf;

static void diag_classify(const char *b, int len, unsigned long long *types,
                          unsigned long long *data_ch){
    /* datagrams may carry several concatenated submessages, count each */
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
    } else {
        diag_classify(buf, len, g_tx_type, g_tx_data_ch);
        if (len > g_tx_max_len) g_tx_max_len = len;
        if (len >= 5 && buf[0]=='u' && buf[1]=='D' && buf[2]=='T' && buf[3]=='L'
            && (unsigned char)buf[4]==3) g_tx_interest_req++;
    }
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

#define RANT_IMPLEMENTATION
#include "rant.h"   /* discovery + transport + node runtime */

#undef setsockopt
#ifdef _WIN32
#undef sendto
#undef recvfrom
#endif

/* Test shims keeping the index based call sites concise: rant_node_topic maps a creation
 * index back to its handle, so a (node, index) call form maps onto the handle calls. */
#define rant_node_send(n, idx, d, l)           rant_topic_send(rant_node_topic((n),(idx)), rant_bytes((d),(l)), NULL)
#define rant_node_set_role(n, idx, r)          rant_topic_set_role(rant_node_topic((n),(idx)), (r))
#define rant_node_drain(n, idx, ms)            rant_topic_drain(rant_node_topic((n),(idx)), (ms))
#define rant_node_publisher_match_count(n, idx) rant_topic_match_count(rant_node_topic((n),(idx)))
#define rant_node_repair_stats(n, idx, o)      rant_topic_repair_stats(rant_node_topic((n),(idx)), (o))
#define rant_node_subscriber_progress(n, idx, p, b, h, t) \
        rant_topic_subscriber_progress(rant_node_topic((n),(idx)), (p),(b),(h),(t))

/* Open a node and create its topics from a RantTopicDef array in index order, so the array
 * index is the handle index the shims above resolve. */
static RantNode *test_node_open(uint8_t *mem, size_t cap, const char *name, RantMsgFn on_msg,
                               RantEventFn on_event, RantNodeOpts opts, const RantTopicDef *chans, uint16_t nch){
    RantNode *node; uint16_t i; RantAllocator alloc = rant_allocator_heap(0);
    (void)mem; (void)cap;             /* heap-backed: the node self-sizes (was a static arena) */
    if (!opts.max_topics) opts.max_topics = nch ? nch : 1;
    node = rant_node_open(&alloc, name, on_msg, on_event, &opts);
    if (!node) return NULL;
    for (i=0;i<nch;i++){
        RantTopicOpts co; memset(&co, 0, sizeof co);
        co.qos = chans[i].qos;
        if (!rant_node_create_topic(node, chans[i].name, (RantRole)chans[i].role, NULL, &co)){
            rant_node_close(node, 0); return NULL;
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
static int         g_spread = 0;      /* 1 = load round robins across them. 2 = the same, but extras
                                         are PUB_ONLY on every node, so those writes have no readers */

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

/* deferred pong queue (no rant_* calls from on_message) */
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

static void lat_on_event(const RantEvent *ev){
    if (ev->kind == RANT_MSG_LOST){ g_gap_evt++; g_gap_tus += ev->lost_count; }
}

static void lat_on_message(const RantMsg *msg){
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

    static RantTopicDef ch[2+XCH_MAX];
    static char xnames[XCH_MAX][8];   /* "x0".."x511": extra topics' topic names */
    memset(ch, 0, sizeof ch);
    ch[0].name = "probe";
    /* depth must cover the pongs one tick can stage (one per peer) or the ring
       evicts them before the flush and RTT samples are lost */
    ch[0].qos.reliability = RANT_BEST_EFFORT; ch[0].qos.keep_last = 16; ch[0].qos.max_message_bytes = 64;
    ch[1].name = "load";
    ch[1].qos.reliability = reliable ? RANT_RELIABLE : RANT_BEST_EFFORT;
    ch[1].qos.keep_last = LOAD_DEPTH; ch[1].qos.max_message_bytes = 64;
    ch[1].qos.backpressure_wait_us = (uint32_t)(block_ms > 0 ? block_ms : 0) * 1000u;
    { int i;     /* extra topics: idle (depth 1) or load-bearing when spread */
      for (i=0;i<g_xch;i++){
          ch[2+i] = ch[1];
          sprintf(xnames[i], "x%d", i); ch[2+i].name = xnames[i];
          ch[2+i].qos.keep_last = g_spread ? 128 : 1;
          if (g_spread==2) ch[2+i].role = RANT_PUB_ONLY;     /* nobody subscribes */
      } }

    /* disable_shm: the sweep measures the UDP path, so same host peers must not switch
       to shared memory */
    RantNodeOpts opts = {
        .domain      = domain,
        .disable_shm = 1,
        .discovery   = { .max_peers = MAX_PEERS },
    };
    if (if_ip)
        opts.net.multicast_interface = if_ip;       /* pin discovery to this one interface */
    else if (if_mode==1)
        opts.net.multicast_interface = "127.0.0.1";   /* loopback only, off the LAN */
    RantDiscoveryAddr seed;
    if (peer_ip){                     /* bootstrap without multicast */
        uint32_t a4 = inet_addr(peer_ip);
        if (a4 != INADDR_NONE){
            memset(&seed, 0, sizeof seed);
            memcpy(seed.ip, &a4, 4); seed.ip_len = 4;   /* port 0 = disc port */
            opts.net.seed_peers = &seed; opts.net.n_seed_peers = 1;
        }
    }
    { const char *rb = getenv("RANT_DIAG_RCVBUF"), *sb = getenv("RANT_DIAG_SNDBUF");
      if (rb) opts.net.recv_buffer_bytes = (uint32_t)atoi(rb);
      if (sb) opts.net.send_buffer_bytes = (uint32_t)atoi(sb);
      if (rb || sb) printf("[%s] buffer override rcvbuf=%u sndbuf=%u\n",
                           g_name, opts.net.recv_buffer_bytes, opts.net.send_buffer_bytes); }
    g_trace = (getenv("RANT_DIAG_TRACE") != NULL);

    static uint8_t mem[48<<20];  /* deep load ring + extra topics x 64 peers */
    RantNode *n = test_node_open(mem, sizeof mem, g_name, lat_on_message, lat_on_event, opts, ch, (uint16_t)(2+g_xch));
    if (!n){ fprintf(stderr, "[%s] rant_node_open failed\n", g_name); return 1; }

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
    /* stall detection: a loop gap far beyond wait_ms means we lost the CPU. Voluntary
       backpressure waits are subtracted and reported separately as blk_wait_ms. */
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
          rant_node_backpressure_stats(n, &wait_us, NULL);
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
            rant_node_send(n, CH_PROBE, o, PROBE_LEN);
        }
        g_pong_n = 0;

        if (now - last_ping >= PING_INTERVAL_NS){
            last_ping = now;
            build_probe(o, PROBE_PING, g_tag, ping_seq++, now_ns(), g_name);
            rant_node_send(n, CH_PROBE, o, PROBE_LEN);
        }

        if (load_hz > 0){
            double elapsed = (now - start)/1e9;
            unsigned long want = (unsigned long)(elapsed * (double)load_hz);
            unsigned long done = load_sent + load_forgiven;
            int burst = 0;
            if (want > done && (unsigned long long)(want - done) > max_deficit)
                max_deficit = want - done;
            /* forgive backlog older than about 250 ms: repaying a stall as one burst from every
               node overflows peers' receive buffers and reads as drops (spec/testing.md) */
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
                rant_node_send(n, chid, o, LOAD_LEN);
                load_sent++; burst++;
            }
            t_stage += now_ns() - ph;
            if (burst >= LOAD_BURST_MAX) burst_capped++;
        }

        ph = now_ns(); rant_node_poll(n, 0);         t_poll0 += now_ns() - ph;  /* flush */
        ph = now_ns(); rant_node_poll(n, wait_ms); t_poll1 += now_ns() - ph;    /* wait  */

        if (g_trace && !traced_peers && now_ns() - start > 3000000000ull){
            traced_peers = 1;
            printf("TRACE node fd=%u domain=%u\n", (unsigned)n->fd, n->domain);
            for (i = 0; i < (int)i_rant_node_core_max_peers(n->core); i++){
                uint32_t pid; uint8_t pip[16]; uint16_t pport;
                if (i_rant_node_core_peer_at(n->core, (uint16_t)i, &pid, pip, NULL, &pport))
                    printf("TRACE peer id=%u addr=%u.%u.%u.%u:%u\n", pid,
                           pip[0], pip[1], pip[2], pip[3], pport);
            }
        }

        now = now_ns();
        /* periodic reports only in run forever mode: stdout is unbuffered, so a blocked write
           on an unpumped pipe freezes the node for seconds. Timed runs need only the SUMMARY. */
        if (duration_s == 0 && now - last_report >= REPORT_INTERVAL_NS){
            last_report = now;
            print_report();
            g_report_ns += now_ns() - now;
        }
        if (duration_s > 0 && (now - start) >= (uint64_t)duration_s*1000000000ull){
            rant_node_backpressure_stats(n, &g_wait_us, &g_wait_n);
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
    rant_node_close(n, 1);     /* BYE: peers drop us now instead of after timeout */
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

    /* multicast send cost against the number of local subscriber sockets. Windows
       charges the sender per local delivery, so this scales with the joiner count. */
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

/* The selftest: two nodes in one process, a pub only writer and a sub only reader, so the
 * test controls exactly when each side runs. spec/testing.md catalogues the phases. */

/* Selftest domains are per process: every phase offsets from this base, picked from the
 * clock at entry, so concurrent runs and stray nodes land on disjoint domains. */
static uint16_t st_domain_base = 33;
#define ST_DOMAIN   st_domain_base
/* topic handles are array indices (declaration order in ch[]) */
#define ST_CH_GAP   0   /* reliable, depth 4, no backpressure  */
#define ST_CH_BLOCK 1   /* reliable, depth 4, slow_reader_wait 100 ms */
#define ST_CH_DYN   2   /* reliable, depth 4, reader starts RANT_INACTIVE */
#define ST_CH_BLOCK2 3  /* like BLOCK but repair_delay>0: the reader's ack is
                           timer-armed, so it relies on the periodic sweep */
#define ST_DEPTH    4
#define ST_BLOCK_US 100000u
#define ST_NACK_US  5000u
#define ST_NCH      40

static int st_fail = 0;
/* the discovery peer table behind a node, for raw announce checks. The public walk is
   rant_node_peers_next */
static const RantDiscoveryPeer *st_peers(RantNode *n, uint16_t *count){
    return rant_discovery_peers(n->discovery, count);
}
/* cond is evaluated exactly once: a condition with side effects must not run twice, or
   the second evaluation fails silently after the first printed ok */
#define ST_CHECK(cond, ...) do { int st_ok_ = !!(cond); \
    printf(st_ok_ ? "  ok   " : "  FAIL "); printf(__VA_ARGS__); printf("\n"); \
    if (!st_ok_) st_fail = 1; } while (0)

static unsigned long st_samples[8], st_gap_calls[8], st_gap_tus[8], st_any;
static char st_last_sender[64];   /* publisher_name of the most recently delivered message */

static void st_on_message(const RantMsg *msg){
    if (msg->topic_index < 8) st_samples[msg->topic_index]++;
    st_last_sender[0] = '\0';
    if (msg->publisher_name.data){
        size_t n = msg->publisher_name.len < sizeof st_last_sender - 1 ? msg->publisher_name.len : sizeof st_last_sender - 1;
        memcpy(st_last_sender, msg->publisher_name.data, n); st_last_sender[n] = '\0';
    }
    st_any++;
}
static unsigned long st_collisions;
static void st_on_event(const RantEvent *ev){
    if (ev->kind == RANT_MSG_LOST){
        if (ev->topic < 8){ st_gap_calls[ev->topic]++; st_gap_tus[ev->topic] += (unsigned long)ev->lost_count; }
    } else if (ev->kind == RANT_ERROR && ev->error == RANT_E_NAME_COLLISION){
        st_collisions++;
    }
}

static void st_pump(RantNode *a, RantNode *b, int ms){         /* run both nodes */
    uint64_t end = i_rant_plat_now_us() + (uint64_t)ms*1000u;
    while (i_rant_plat_now_us() < end){ rant_node_poll(a, 1); if (b) rant_node_poll(b, 0); }
}

/* A one way link between raw transports: hand src's interest to dst and run the pairwise
   detail exchange by hand, as the runtimes would. The announce only nominates. */
static void st_apply_verified(RantTransportState *dst, uint32_t src_id, RantTransportState *src){
    uint8_t ib[512], rq[512], rp[2048]; size_t il, rl, pl; uint16_t nw;
    RantDetailWant wl[16];
    il = rant_transport_build_interest(src, ib, sizeof ib);
    rant_transport_apply_peer_interest(dst, src_id, rant_bytes(ib, il));
    nw = rant_transport_detail_wants(dst, NULL, src_id, rant_bytes(ib, il), wl, 16);
    if (!nw) return;                       /* nothing shared (or already verified) */
    rl = rant_detail_req_build(0, 0, wl, nw, rq, sizeof rq);
    pl = rant_transport_detail_respond(src, NULL, 0, rant_bytes(rq, rl), rp, sizeof rp);
    rant_transport_apply_peer_details(dst, src_id, rant_bytes(rp, pl));
    rant_transport_apply_peer_interest(dst, src_id, rant_bytes(ib, il));
}

/* Best effort rate throttle on the transport core with a controlled clock. The writer
   paces its lane to the subscriber's max_rate_hz (spec/testing.md). The pump can drop. */
static int rate_recv, rate_lost, rate_drop;
static uint64_t rate_clk;
static RantTransportState *rate_W, *rate_R;
static int rate_on_msg(void *u, uint16_t ch, uint32_t from, RantBytes d){
    (void)u;(void)ch;(void)from;(void)d; rate_recv++; return 0;
}
static void rate_on_event(const RantTransportEvent *ev){
    if (ev->kind==RANT_TRANSPORT_MSG_LOST) rate_lost += (int)ev->lost_count;
}
static void rate_send(void){
    static unsigned char p[16];
    rant_transport_send(rate_W, 0, rant_bytes(p, sizeof p), rate_clk);
}
static void rate_pump(uint64_t dt){   /* flush W to R, then advance the clock */
    uint8_t buf[RANT_DGRAM_MAX]; uint32_t to; size_t ol;
    while (rant_transport_poll_send(rate_W,&to,buf,sizeof buf,&ol,rate_clk)){
        if (rate_drop>0 && (buf[0]&0x07u)==1u){ rate_drop--; continue; }   /* drop this DATA */
        rant_transport_on_datagram(rate_R, 1u, rant_bytes(buf, ol), rate_clk);
    }
    rate_clk += dt;
}
static void rate_checks(void){
    RantTopicDef cw, cr; RantConfig wc, rc; void *mw, *mr; size_t nw, nr; int i;
    RantAllocator wa = rant_allocator_heap(0);
    RantAllocator ra = rant_allocator_heap(0);
    RantQos q; memset(&q,0,sizeof q); q.reliability=RANT_BEST_EFFORT; q.keep_last=4;
    memset(&cw,0,sizeof cw); cw.name="ratech"; cw.qos=q; cw.role=RANT_PUB_ONLY;
    memset(&cr,0,sizeof cr); cr.name="ratech"; cr.qos=q; cr.qos.max_rate_hz=100; cr.role=RANT_SUB_ONLY;
    memset(&wc,0,sizeof wc); wc.topics=&cw; wc.n_topics=1; wc.max_peers=2;
    wc.allocator=rant_allocator_alloc; wc.user=&wa;
    memset(&rc,0,sizeof rc); rc.topics=&cr; rc.n_topics=1; rc.max_peers=2;
    rc.allocator=rant_allocator_alloc; rc.user=&ra;
    rc.on_message=rate_on_msg; rc.on_event=rate_on_event;
    nw=rant_transport_required_memory(&wc); mw=malloc(nw); rate_W=rant_transport_init(mw,nw,&wc);
    nr=rant_transport_required_memory(&rc); mr=malloc(nr); rate_R=rant_transport_init(mr,nr,&rc);
    rate_clk=1000000;
    rant_transport_peer_add(rate_W,2u,RANT_FRAG_SIZE); rant_transport_peer_add(rate_R,1u,RANT_FRAG_SIZE);
    st_apply_verified(rate_R, 1u, rate_W);   /* reader learns the writer */
    st_apply_verified(rate_W, 2u, rate_R);   /* writer learns the reader + its advertised rate */
    ST_CHECK(rant_transport_publisher_match_count(rate_W,0)>0, "rate: writer matched the throttled reader");

    /* decimation: publish 200 samples across ~100 ms of virtual time. At 100 Hz the reader
       gets ~10-12, not 200, and NO false loss (the paced skips are absorbed by wire_skip). */
    rate_recv=0; rate_lost=0; rate_drop=0;
    for (i=0;i<200;i++){ rate_send(); rate_pump(500u); }
    for (i=0;i<3;i++) rate_pump(20000u);        /* drain the final held tick via the sweep */
    ST_CHECK(rate_recv>=5 && rate_recv<=20, "rate: decimated to ~rate*time (%d of 200 delivered)", rate_recv);
    ST_CHECK(rate_lost==0, "rate: paced skips are not loss (lost=%d)", rate_lost);

    /* a dropped sent sample must still be reported: send and tick, drop that DATA, then
       let later ticks deliver, so the reader's per peer seqno gap surfaces as one MSG_LOST */
    rate_recv=0; rate_lost=0;
    rate_send(); rate_clk += 20000u; rate_drop=1;   /* the next tick's DATA is dropped */
    rate_pump(20000u);
    ST_CHECK(rate_recv==0, "rate: the tick's only sample was dropped on the wire (recv=%d)", rate_recv);
    for (i=0;i<5;i++){ rate_send(); rate_pump(20000u); }
    ST_CHECK(rate_recv>=1 && rate_lost>=1,
             "rate: a dropped SENT sample IS reported as loss (recv=%d lost=%d)", rate_recv, rate_lost);

    rant_transport_destroy(rate_W); rant_transport_destroy(rate_R); free(mw); free(mr);
    rant_allocator_reset(&wa); rant_allocator_reset(&ra);
}


/* Lapped reader and NACK merge on the transport core with a controlled clock: a six
   fragment reliable stream through a pump that can drop DATA (spec/testing.md). */
static int lap_recv, lap_lost, lap_drop_all; static unsigned lap_drop_mask;
static uint64_t lap_clk;
static RantTransportState *lap_W, *lap_R;
static uint8_t lap_held[8][RANT_DGRAM_MAX]; static size_t lap_hl[8]; static int lap_nh;
static int lap_on_msg(void *u, uint16_t ch, uint32_t from, RantBytes d){
    (void)u;(void)ch;(void)from;(void)d; lap_recv++; return 0;
}
static void lap_on_event(const RantTransportEvent *ev){
    if (ev->kind==RANT_TRANSPORT_MSG_LOST) lap_lost += (int)ev->lost_count;
}
static void lap_send(void){                       /* one 6-fragment message */
    static unsigned char p[6*RANT_FRAG_SIZE - 100];
    rant_transport_send(lap_W, 0, rant_bytes(p, sizeof p), lap_clk);
}
/* R to W: collect now, feed later, so ACKNACKs armed at different points land in one pass */
static void lap_collect_r(void){
    uint8_t buf[RANT_DGRAM_MAX]; uint32_t to; size_t ol;
    while (lap_nh<8 && rant_transport_poll_send(lap_R,&to,buf,sizeof buf,&ol,lap_clk)){
        memcpy(lap_held[lap_nh], buf, ol); lap_hl[lap_nh++] = ol; }
}
static void lap_feed_w(void){
    int i;
    for (i=0;i<lap_nh;i++) rant_transport_on_datagram(lap_W, 2u, rant_bytes(lap_held[i], lap_hl[i]), lap_clk);
    lap_nh = 0;
}
/* W to R, dropping DATA per the lap_drop switches, one message. After split_after DATA
   fragments R's pending ACKNACK is collected mid stream. A batched HB still passes. */
static void lap_flush_w(int split_after){
    uint8_t buf[RANT_DGRAM_MAX]; uint32_t to; size_t ol; int fed=0;
    while (rant_transport_poll_send(lap_W,&to,buf,sizeof buf,&ol,lap_clk)){
        size_t off=0;
        if ((buf[0]&0x07u)==1u && !(buf[0]&RANT_F_SINGLE)){
            unsigned frag = (unsigned)buf[RANT_OFFSET_FRAG] | ((unsigned)buf[RANT_OFFSET_FRAG+1]<<8);
            size_t sub = RANT_HEADER_DATA_MULTI + ((size_t)buf[RANT_OFFSET_PAYLOAD_LEN] | ((size_t)buf[RANT_OFFSET_PAYLOAD_LEN+1]<<8));
            int drop = lap_drop_all || (lap_drop_mask & (1u<<frag));
            if (frag==5) lap_drop_mask = 0;
            if (drop) off = sub; else fed++;
        }
        if (off < ol) rant_transport_on_datagram(lap_R, 1u, rant_bytes(buf+off, ol-off), lap_clk);
        if (split_after && fed==split_after){ lap_collect_r(); split_after=0; }
    }
}
static void lap_pump(uint64_t dt){ lap_flush_w(0); lap_collect_r(); lap_feed_w(); lap_clk += dt; }
static void lap_run(int ms){ while (ms-- > 0) lap_pump(1000); }   /* 1 ms steps */
static void lapped_checks(void){
    RantTopicDef cw, cr; RantConfig wc, rc; void *mw, *mr; size_t nw, nr; int i;
    RantAllocator wa = rant_allocator_heap(0);
    RantAllocator ra = rant_allocator_heap(0);
    RantRepairStats rs;
    RantQos q; memset(&q,0,sizeof q); q.reliability=RANT_RELIABLE; q.keep_last=4;
    q.heartbeat_us=30000; q.repair_delay_us=50000;
    memset(&cw,0,sizeof cw); cw.name="lapped"; cw.qos=q; cw.role=RANT_PUB_ONLY;
    memset(&cr,0,sizeof cr); cr.name="lapped"; cr.qos=q; cr.role=RANT_SUB_ONLY;
    memset(&wc,0,sizeof wc); wc.topics=&cw; wc.n_topics=1; wc.max_peers=2;
    wc.allocator=rant_allocator_alloc; wc.user=&wa;
    memset(&rc,0,sizeof rc); rc.topics=&cr; rc.n_topics=1; rc.max_peers=2;
    rc.allocator=rant_allocator_alloc; rc.user=&ra;
    rc.on_message=lap_on_msg; rc.on_event=lap_on_event;
    nw=rant_transport_required_memory(&wc); mw=malloc(nw); lap_W=rant_transport_init(mw,nw,&wc);
    nr=rant_transport_required_memory(&rc); mr=malloc(nr); lap_R=rant_transport_init(mr,nr,&rc);
    lap_clk=1000000; lap_nh=0; lap_drop_all=0; lap_drop_mask=0;
    rant_transport_peer_add(lap_W,2u,RANT_FRAG_SIZE); rant_transport_peer_add(lap_R,1u,RANT_FRAG_SIZE);
    st_apply_verified(lap_R, 1u, lap_W);
    st_apply_verified(lap_W, 2u, lap_R);
    ST_CHECK(rant_transport_publisher_match_count(lap_W,0)>0, "lapped: writer matched reader");

    /* [a] baseline */
    lap_recv=0; lap_lost=0; lap_send(); lap_pump(1000);
    ST_CHECK(lap_recv==1 && lap_lost==0, "lapped: [a] a 6-fragment message delivers (recv=%d)", lap_recv);

    /* [b] NACK merge: frags 1 and 4 of one message are lost, so two requests reach the
       writer in one pass. Both holes must be resent at once, an overwrite would resend only 4. */
    lap_recv=0; lap_lost=0; lap_drop_mask=(1u<<1)|(1u<<4);
    lap_send(); lap_flush_w(2); lap_collect_r();
    ST_CHECK(lap_nh==2, "lapped: [b] two ACKNACKs collected in one pass (%d)", lap_nh);
    lap_feed_w(); lap_pump(1000);
    rant_transport_repair_stats(lap_W, 0, &rs);
    ST_CHECK(lap_recv==1 && rs.frags_resent==2,
             "lapped: [b] merged request: both holes resent at once, message complete (recv=%d resent=%llu)",
             lap_recv, (unsigned long long)rs.frags_resent);

    /* [c] the writer's floor passes a reader that heard nothing: it restarts at the oldest
       cached sample and repairs the cached window (6 sent, keep_last 4: 2 lost, 4 recovered). */
    lap_recv=0; lap_lost=0; lap_drop_all=1;
    for (i=0;i<6;i++){ lap_send(); lap_pump(1000); }
    lap_drop_all=0;
    lap_run(120);     /* the tail HB skips, the backstop NACK resends */
    ST_CHECK(lap_recv==4 && lap_lost==12,
             "lapped: [c] one floor skip restarts at the oldest cached sample (recv=%d lost=%d)", lap_recv, lap_lost);

    /* [d] lapped: a second floor skip with nothing delivered in between. The reader rejoins
       at the writer's head, the cached window is reported lost, the next message is in order. */
    lap_recv=0; lap_lost=0; lap_drop_all=1;
    for (i=0;i<6;i++){ lap_send(); lap_pump(1000); }
    lap_run(30);                                       /* skip #1 (its resends stay dropped) */
    ST_CHECK(lap_recv==0 && lap_lost==12, "lapped: [d] first skip (recv=%d lost=%d)", lap_recv, lap_lost);
    lap_send(); lap_pump(1000); lap_send(); lap_pump(1000);
    lap_run(30);                                       /* skip #2 while still fetching: lapped */
    ST_CHECK(lap_recv==0 && lap_lost==48, "lapped: [d] second skip gives up the cached window (lost=%d)", lap_lost);
    lap_drop_all=0;
    rant_transport_repair_stats(lap_W, 0, &rs);
    { unsigned long long resent0 = rs.frags_resent;
      lap_send(); lap_run(5);
      rant_transport_repair_stats(lap_W, 0, &rs);
      ST_CHECK(lap_recv==1 && lap_lost==48 && rs.frags_resent==resent0,
               "lapped: [d] rejoined at the head: next message in order, no repair (recv=%d lost=%d resent=%llu)",
               lap_recv, lap_lost, (unsigned long long)(rs.frags_resent-resent0)); }
    /* [e] a delivery resets the streak: the stream runs on, and one later skip restarts at
       the floor again rather than the head */
    for (i=0;i<3;i++){ lap_send(); lap_pump(1000); }
    ST_CHECK(lap_recv==4, "lapped: [e] stream continues after the rejoin (recv=%d)", lap_recv);
    lap_recv=0; lap_lost=0; lap_drop_all=1;
    for (i=0;i<6;i++){ lap_send(); lap_pump(1000); }
    lap_drop_all=0;
    lap_run(120);
    ST_CHECK(lap_recv==4 && lap_lost==12, "lapped: [e] streak reset: a later skip repairs the window again (recv=%d lost=%d)", lap_recv, lap_lost);

    rant_transport_destroy(lap_W); rant_transport_destroy(lap_R); free(mw); free(mr);
    rant_allocator_reset(&wa); rant_allocator_reset(&ra);
}


/* Per peer RTT estimation on the transport core: W to R through a pump with a symmetric
   one way delay L, so every round trip is exactly 2L (spec/testing.md). */
static int rt_recv, rt_drop_all, rt_drop_ack; static unsigned rt_drop_mask, rt_drop_resend_of;
static uint64_t rt_clk, rt_lat;
static RantTransportState *rt_W, *rt_R;
static int rt_hb_seen;
static int rt_on_msg(void *u, uint16_t ch, uint32_t from, RantBytes d){
    (void)u;(void)ch;(void)from;(void)d; rt_recv++; return 0;
}
static void rt_send(void){                       /* one 6-fragment message */
    static unsigned char p[6*RANT_FRAG_SIZE - 100];
    rant_transport_send(rt_W, 0, rant_bytes(p, sizeof p), rt_clk);
}
/* one tick: W builds at clk, R hears it at clk plus L and answers, W hears that at clk
   plus 2L. DATA fragments drop per the rt_drop switches, rt_drop_ack drops R's ACKNACKs. */
static void rt_pump(void){
    uint8_t buf[RANT_DGRAM_MAX]; uint32_t to; size_t ol;
    while (rant_transport_poll_send(rt_W,&to,buf,sizeof buf,&ol,rt_clk)){
        size_t off=0;
        if ((buf[0]&0x07u)==1u && !(buf[0]&RANT_F_SINGLE)){
            unsigned frag = (unsigned)buf[RANT_OFFSET_FRAG] | ((unsigned)buf[RANT_OFFSET_FRAG+1]<<8);
            size_t sub = RANT_HEADER_DATA_MULTI + ((size_t)buf[RANT_OFFSET_PAYLOAD_LEN] | ((size_t)buf[RANT_OFFSET_PAYLOAD_LEN+1]<<8));
            int drop = rt_drop_all || (rt_drop_mask & (1u<<frag));
            if (!drop && rt_drop_resend_of == frag + 1u){ drop = 1; rt_drop_resend_of = 0; }
            if (frag==5) rt_drop_mask = 0;
            if (drop) off = sub;
        } else if ((buf[0]&0x07u)==2u) rt_hb_seen++;
        if (off < ol) rant_transport_on_datagram(rt_R, 1u, rant_bytes(buf+off, ol-off), rt_clk + rt_lat);
    }
    while (rant_transport_poll_send(rt_R,&to,buf,sizeof buf,&ol,rt_clk + rt_lat)){
        if (rt_drop_ack && (buf[0]&0x07u)==3u) continue;
        rant_transport_on_datagram(rt_W, 2u, rant_bytes(buf, ol), rt_clk + 2u*rt_lat);
    }
    rt_clk += 2u*rt_lat;
}
static void rtt_checks(void){
    RantTopicDef cw, cr; RantConfig wc, rc; void *mw, *mr; size_t nw, nr; int i, n;
    RantAllocator wa = rant_allocator_heap(0);
    RantAllocator ra = rant_allocator_heap(0);
    RantPeerRtt e;
    RantQos q; memset(&q,0,sizeof q); q.reliability=RANT_RELIABLE; q.keep_last=8;
    q.heartbeat_us=200000;   /* idle HB far off: only the tail HB matters here */
    memset(&cw,0,sizeof cw); cw.name="rtt"; cw.qos=q; cw.role=RANT_PUB_ONLY;
    memset(&cr,0,sizeof cr); cr.name="rtt"; cr.qos=q; cr.role=RANT_SUB_ONLY;
    memset(&wc,0,sizeof wc); wc.topics=&cw; wc.n_topics=1; wc.max_peers=2;
    wc.allocator=rant_allocator_alloc; wc.user=&wa;
    memset(&rc,0,sizeof rc); rc.topics=&cr; rc.n_topics=1; rc.max_peers=2;
    rc.allocator=rant_allocator_alloc; rc.user=&ra;
    rc.on_message=rt_on_msg;
    nw=rant_transport_required_memory(&wc); mw=malloc(nw); rt_W=rant_transport_init(mw,nw,&wc);
    nr=rant_transport_required_memory(&rc); mr=malloc(nr); rt_R=rant_transport_init(mr,nr,&rc);
    rt_clk=1000000; rt_lat=2500; rt_drop_all=0; rt_drop_mask=0; rt_drop_resend_of=0; rt_drop_ack=0;
    rant_transport_peer_add(rt_W,2u,RANT_FRAG_SIZE); rant_transport_peer_add(rt_R,1u,RANT_FRAG_SIZE);
    st_apply_verified(rt_R, 1u, rt_W);
    st_apply_verified(rt_W, 2u, rt_R);
    ST_CHECK(rant_transport_publisher_match_count(rt_W,0)>0, "rtt: writer matched reader");
    ST_CHECK(rant_transport_peer_rtt(rt_W, 2u, &e) && e.samples==0, "rtt: a fresh peer has no estimate");
    ST_CHECK(!rant_transport_peer_rtt(rt_W, 9u, &e), "rtt: an unknown peer answers 0");

    /* [a] writer side: every clean sample is push-to-ack = one round trip = 2L */
    rt_recv=0;
    for (i=0;i<10;i++){ rt_send(); rt_pump(); rt_pump(); }
    rant_transport_peer_rtt(rt_W, 2u, &e);
    ST_CHECK(rt_recv==10 && e.samples==10 && e.rtt_us==5000 && e.rtt_min_us==5000 && e.rtt_last_us==5000,
             "rtt: [a] writer samples push-to-ack (samples=%u rtt=%u min=%u last=%u)",
             e.samples, e.rtt_us, e.rtt_min_us, e.rtt_last_us);
    ST_CHECK(e.rtt_jitter_us < 500, "rtt: [a] jitter decays on a steady path (%u)", e.rtt_jitter_us);
    rant_transport_peer_rtt(rt_R, 1u, &e);
    ST_CHECK(e.samples==0, "rtt: [a] a reader that never repaired has no estimate (%u)", e.samples);

    /* [b] reader side, first loss: frag 2 and its resend are lost. No estimate yet, so the
       re ask waits the 50 ms default. The re asked seqno is ambiguous, so it is no sample. */
    rt_recv=0; rt_drop_mask=(1u<<2); rt_drop_resend_of=2u+1u;
    rt_send();
    for (n=0; n<40 && rt_recv==0; n++) rt_pump();
    ST_CHECK(rt_recv==1 && n>=10 && n<=13, "rtt: [b] unmeasured reader re-asks at the 50 ms default (%d ticks)", n);
    rant_transport_peer_rtt(rt_R, 1u, &e);
    ST_CHECK(e.samples==0, "rtt: [b] a seqno asked twice yields no sample, Karn (samples=%u)", e.samples);

    /* [c] a few clean repairs (one lost frag each) build the reader's estimate ... */
    for (i=0;i<6;i++){ rt_recv=0; rt_drop_mask=(1u<<3); rt_send(); for (n=0;n<8 && rt_recv==0;n++) rt_pump(); }
    rant_transport_peer_rtt(rt_R, 1u, &e);
    ST_CHECK(e.samples==6 && e.rtt_us==5000 && e.rtt_jitter_us < 1000,
             "rtt: [c] reader samples request-to-resend (samples=%u rtt=%u jitter=%u)", e.samples, e.rtt_us, e.rtt_jitter_us);
    /* ... and the same double loss now re-asks at the RTT bound (5 ms + max(1 ms, 4 x jitter),
       about 7 ms): delivered in 3 to 5 ticks of 5 ms instead of 12 */
    rt_recv=0; rt_drop_mask=(1u<<2); rt_drop_resend_of=2u+1u;
    rt_send();
    for (n=0; n<40 && rt_recv==0; n++) rt_pump();
    ST_CHECK(rt_recv==1 && n>=3 && n<=5, "rtt: [c] measured reader re-asks at the RTT bound (%d ticks)", n);

    /* [d] the writer's tail heartbeat follows the RTT bound: drop the final ack, the HB that
       chases it comes at ~6 ms (2 ticks), not the 20 ms RANT_HB_TAIL_US (4 ticks) */
    rt_recv=0; rt_drop_ack=1; rt_hb_seen=0;
    rt_send(); rt_pump();
    ST_CHECK(rt_recv==1 && rt_hb_seen==0, "rtt: [d] delivered, ack dropped, no HB yet (recv=%d hb=%d)", rt_recv, rt_hb_seen);
    for (n=0; n<10 && rt_hb_seen==0; n++) rt_pump();
    ST_CHECK(rt_hb_seen>=1 && n<=2, "rtt: [d] tail HB at the RTT bound, not the 20 ms default (%d ticks)", n);
    rt_drop_ack=0; rt_pump(); rt_pump();

    rant_transport_destroy(rt_W); rant_transport_destroy(rt_R); free(mw); free(mr);
    rant_allocator_reset(&wa); rant_allocator_reset(&ra);
}


/* One sample held ahead of the head on the transport core with a controlled clock
   (spec/testing.md). Messages are 6 fragments, tagged with their number in byte 0. */
static int ah_recv, ah_lost, ah_refuse, ah_order_bad; static unsigned ah_drop_mask, ah_tag, ah_last;
static uint64_t ah_drop_lo, ah_drop_hi;   /* drop DATA seqnos in [lo, hi), a message plus resends */
static uint64_t ah_clk, ah_first_base; static int ah_have_first;
static RantTransportState *ah_W, *ah_R;
static uint8_t ah_held[8][RANT_DGRAM_MAX]; static size_t ah_hl[8]; static int ah_nh;
static int ah_on_msg(void *u, uint16_t ch, uint32_t from, RantBytes d){
    (void)u;(void)ch;(void)from;
    if (ah_refuse) return 1;
    {   unsigned tag = ((const unsigned char*)d.data)[RANT_TIMESTAMP_BYTES];
        if (tag <= ah_last) ah_order_bad++;   /* strictly increasing, no dup or step back */
        ah_last = tag; }
    ah_recv++; return 0;
}
static void ah_on_event(const RantTransportEvent *ev){
    if (ev->kind==RANT_TRANSPORT_MSG_LOST) ah_lost += (int)ev->lost_count;
}
static void ah_send(void){                       /* one 6-fragment message, tagged */
    static unsigned char p[6*RANT_FRAG_SIZE - 100];
    p[0] = (unsigned char)++ah_tag;
    rant_transport_send(ah_W, 0, rant_bytes(p, sizeof p), ah_clk);
}
static uint64_t ah_base_of(unsigned tag){ return ah_first_base + 6u * (uint64_t)(tag - 1u); }
static void ah_collect_r(void){
    uint8_t buf[RANT_DGRAM_MAX]; uint32_t to; size_t ol;
    while (ah_nh<8 && rant_transport_poll_send(ah_R,&to,buf,sizeof buf,&ol,ah_clk)){
        memcpy(ah_held[ah_nh], buf, ol); ah_hl[ah_nh++] = ol; }
}
static void ah_feed_w(void){
    int i;
    for (i=0;i<ah_nh;i++) rant_transport_on_datagram(ah_W, 2u, rant_bytes(ah_held[i], ah_hl[i]), ah_clk);
    ah_nh = 0;
}
static void ah_flush_w(void){
    uint8_t buf[RANT_DGRAM_MAX]; uint32_t to; size_t ol;
    while (rant_transport_poll_send(ah_W,&to,buf,sizeof buf,&ol,ah_clk)){
        size_t off=0;
        if ((buf[0]&0x07u)==1u && !(buf[0]&RANT_F_SINGLE)){
            uint64_t seqno = i_rant_le_r64(buf+RANT_OFFSET_SEQNO);
            unsigned frag = (unsigned)buf[RANT_OFFSET_FRAG] | ((unsigned)buf[RANT_OFFSET_FRAG+1]<<8);
            size_t sub = RANT_HEADER_DATA_MULTI + ((size_t)buf[RANT_OFFSET_PAYLOAD_LEN] | ((size_t)buf[RANT_OFFSET_PAYLOAD_LEN+1]<<8));
            int drop;
            if (!ah_have_first){ ah_first_base = seqno - frag; ah_have_first = 1; }
            drop = (ah_drop_mask & (1u<<frag)) || (seqno >= ah_drop_lo && seqno < ah_drop_hi);
            if (frag==5) ah_drop_mask = 0;
            if (drop) off = sub;
        }
        if (off < ol) rant_transport_on_datagram(ah_R, 1u, rant_bytes(buf+off, ol-off), ah_clk);
    }
}
static void ah_pump(uint64_t dt){ ah_flush_w(); ah_collect_r(); ah_feed_w(); ah_clk += dt; }
static void ah_run(int ms){ while (ms-- > 0) ah_pump(1000); }
static void ahead_checks(void){
    RantTopicDef cw, cr; RantConfig wc, rc; void *mw, *mr; size_t nw, nr;
    RantAllocator wa = rant_allocator_heap(0);
    RantAllocator ra = rant_allocator_heap(0);
    RantRepairStats ws, rs; uint64_t resent0, ahead0;
    RantQos q; memset(&q,0,sizeof q); q.reliability=RANT_RELIABLE; q.keep_last=4;
    q.heartbeat_us=30000; q.repair_delay_us=50000;
    memset(&cw,0,sizeof cw); cw.name="ahead"; cw.qos=q; cw.role=RANT_PUB_ONLY;
    memset(&cr,0,sizeof cr); cr.name="ahead"; cr.qos=q; cr.role=RANT_SUB_ONLY;
    memset(&wc,0,sizeof wc); wc.topics=&cw; wc.n_topics=1; wc.max_peers=2;
    wc.allocator=rant_allocator_alloc; wc.user=&wa;
    memset(&rc,0,sizeof rc); rc.topics=&cr; rc.n_topics=1; rc.max_peers=2;
    rc.allocator=rant_allocator_alloc; rc.user=&ra;
    rc.on_message=ah_on_msg; rc.on_event=ah_on_event;
    nw=rant_transport_required_memory(&wc); mw=malloc(nw); ah_W=rant_transport_init(mw,nw,&wc);
    nr=rant_transport_required_memory(&rc); mr=malloc(nr); ah_R=rant_transport_init(mr,nr,&rc);
    ah_clk=1000000; ah_nh=0; ah_drop_mask=0; ah_drop_lo=ah_drop_hi=0; ah_refuse=0;
    ah_tag=0; ah_last=0; ah_order_bad=0; ah_have_first=0;
    rant_transport_peer_add(ah_W,2u,RANT_FRAG_SIZE); rant_transport_peer_add(ah_R,1u,RANT_FRAG_SIZE);
    st_apply_verified(ah_R, 1u, ah_W);
    st_apply_verified(ah_W, 2u, ah_R);
    ST_CHECK(rant_transport_publisher_match_count(ah_W,0)>0, "ahead: writer matched reader");

    /* [a] baseline */
    ah_recv=0; ah_lost=0; ah_send(); ah_pump(1000);
    ST_CHECK(ah_recv==1 && ah_lost==0, "ahead: [a] a 6-fragment message delivers (recv=%d)", ah_recv);

    /* [b] frag 2 of message 2 is lost and message 3 arrives whole before the resend: it is
       HELD, not dropped, and delivers right behind the repaired head. One resend, in order. */
    rant_transport_repair_stats(ah_W,0,&ws); resent0=ws.frags_resent;
    rant_transport_repair_stats(ah_R,0,&rs); ahead0=rs.frags_ahead;
    ah_recv=0; ah_drop_mask=(1u<<2);
    ah_send(); ah_send(); ah_pump(1000);
    rant_transport_repair_stats(ah_R,0,&rs);
    ST_CHECK(ah_recv==0 && rs.frags_ahead==ahead0, "ahead: [b] the next message is held while the head repairs (recv=%d ahead=%llu)",
             ah_recv, (unsigned long long)(rs.frags_ahead-ahead0));
    ah_pump(1000);
    rant_transport_repair_stats(ah_W,0,&ws);
    ST_CHECK(ah_recv==2 && ah_order_bad==0 && ws.frags_resent-resent0==1,
             "ahead: [b] one resend completes the head, the held message follows at once (recv=%d resent=%llu)",
             ah_recv, (unsigned long long)(ws.frags_resent-resent0));

    /* [c] two messages ahead: the first is held, the second is still dropped and comes back
       in order through repair (6 resends), everything in order */
    rant_transport_repair_stats(ah_W,0,&ws); resent0=ws.frags_resent;
    rant_transport_repair_stats(ah_R,0,&rs); ahead0=rs.frags_ahead;
    ah_recv=0; ah_drop_mask=(1u<<2);
    ah_send(); ah_send(); ah_send(); ah_pump(1000);
    rant_transport_repair_stats(ah_R,0,&rs);
    ST_CHECK(ah_recv==0 && rs.frags_ahead-ahead0==6, "ahead: [c] the second future message is dropped (ahead=%llu)",
             (unsigned long long)(rs.frags_ahead-ahead0));
    ah_run(5);
    rant_transport_repair_stats(ah_W,0,&ws);
    ST_CHECK(ah_recv==3 && ah_order_bad==0 && ws.frags_resent-resent0==7,
             "ahead: [c] head repaired, held one delivered, dropped one refetched (recv=%d resent=%llu)",
             ah_recv, (unsigned long long)(ws.frags_resent-resent0));

    /* [d] the consumer refuses the head (parked): the next message still fills the hold, and
       an accepted retry delivers both with no resend */
    rant_transport_repair_stats(ah_W,0,&ws); resent0=ws.frags_resent;
    rant_transport_repair_stats(ah_R,0,&rs); ahead0=rs.frags_ahead;
    ah_recv=0; ah_refuse=1;
    ah_send(); ah_pump(1000);
    ah_send(); ah_pump(1000);
    rant_transport_repair_stats(ah_R,0,&rs);
    ST_CHECK(ah_recv==0 && rs.frags_ahead==ahead0 && rant_transport_deliver_parked(ah_R,0,ah_clk)==1,
             "ahead: [d] parked head, the next message held meanwhile (ahead=%llu)", (unsigned long long)(rs.frags_ahead-ahead0));
    ah_refuse=0;
    ST_CHECK(rant_transport_deliver_parked(ah_R,0,ah_clk)==0 && ah_recv==2 && ah_order_bad==0,
             "ahead: [d] unpark delivers the head and the held message (recv=%d)", ah_recv);
    ah_pump(1000);
    rant_transport_repair_stats(ah_W,0,&ws);
    ST_CHECK(ws.frags_resent==resent0, "ahead: [d] no resend was needed (resent=%llu)", (unsigned long long)(ws.frags_resent-resent0));

    /* [e] a message is lost whole and the next one is held. The writer evicts the lost one
       and its floor lands on the held message: delivered from the hold, one MSG_LOST */
    ah_recv=0; ah_lost=0;
    ah_drop_lo=ah_base_of(ah_tag+1u); ah_drop_hi=ah_drop_lo+6u;
    ah_send(); ah_send(); ah_pump(1000);          /* 7 lost whole, 8 held */
    ah_send(); ah_pump(1000); ah_send(); ah_pump(1000); ah_send(); ah_pump(1000);
    ah_run(120);
    ST_CHECK(ah_recv==4 && ah_lost==6 && ah_order_bad==0,
             "ahead: [e] the floor landed on the held message: delivered, one lost, rest refetched in order (recv=%d lost=%d)",
             ah_recv, ah_lost);
    ah_drop_lo=ah_drop_hi=0;

    /* [f] a hold across a wholly lost message: 12 has a hole, 13 is lost whole once, 14 is
       held. 12 repairs, 13 is fetched in order, 14 delivers from the hold */
    rant_transport_repair_stats(ah_W,0,&ws); resent0=ws.frags_resent;
    rant_transport_repair_stats(ah_R,0,&rs); ahead0=rs.frags_ahead;
    ah_recv=0; ah_lost=0; ah_drop_mask=(1u<<2);
    ah_drop_lo=ah_base_of(ah_tag+2u); ah_drop_hi=ah_drop_lo+6u;
    ah_send(); ah_send(); ah_send(); ah_pump(1000);
    ah_drop_lo=ah_drop_hi=0;                     /* 13's resends get through */
    ah_run(5);
    rant_transport_repair_stats(ah_W,0,&ws); rant_transport_repair_stats(ah_R,0,&rs);
    ST_CHECK(ah_recv==3 && ah_lost==0 && ah_order_bad==0 && ws.frags_resent-resent0==7 && rs.frags_ahead==ahead0,
             "ahead: [f] hold across a lost message: repaired, fetched, delivered in order (recv=%d resent=%llu ahead=%llu)",
             ah_recv, (unsigned long long)(ws.frags_resent-resent0), (unsigned long long)(rs.frags_ahead-ahead0));

    rant_transport_destroy(ah_W); rant_transport_destroy(ah_R); free(mw); free(mr);
    rant_allocator_reset(&wa); rant_allocator_reset(&ra);
}

/* ---- discovery-core (sans-IO) checks: peer lifecycle without sockets ---- */
static uint32_t dc_up_id, dc_up_n, dc_down_id, dc_down_n, dc_refused_n;
static int      dc_down_reason;
static void dc_event(const RantDiscoveryEvent *ev){
    if      (ev->kind==RANT_DISCOVERY_PEER_UP)       { dc_up_id=ev->peer; dc_up_n++; }
    else if (ev->kind==RANT_DISCOVERY_PEER_DOWN)     { dc_down_id=ev->peer; dc_down_reason=(int)ev->reason; dc_down_n++; }
    else if (ev->kind==RANT_DISCOVERY_PEER_REFUSED){ dc_refused_n++; }
}

/* craft a v3 announce for sender `uid` (uuid = all uid bytes), meta_len 0 */
static size_t dc_mk(uint8_t *p, uint8_t uid, uint8_t flags, uint16_t dom, uint16_t port, uint32_t mver){
    size_t off = RANT_DISCOVERY_META_OFF;
    uint8_t *b = p + off;
    memset(p, 0, off);
    p[0]='u';p[1]='D';p[2]='S';p[3]='C';
    p[4]=(uint8_t)RANT_DISCOVERY_PROTO_VERSION;
    p[5]=flags;                                   /* 0x01 = BYE (internal flag) */
    p[6]=(uint8_t)dom; p[7]=(uint8_t)(dom>>8);
    memset(p+8, uid, 16);                         /* a distinct uuid per uid */
    p[off-6]=(uint8_t)mver; p[off-5]=(uint8_t)(mver>>8);
    p[off-4]=(uint8_t)(mver>>16); p[off-3]=(uint8_t)(mver>>24);
    /* blob = the discovery section [u16 data_port][u8 self_ip_len 0][u8 name_len 0], no
       overlay. self_ip_len 0 means the receiver uses the datagram source ip. */
    b[0]=(uint8_t)port; b[1]=(uint8_t)(port>>8); b[2]=0; b[3]=0;
    p[off-2]=4; p[off-1]=0;                        /* meta_len = 4 (the discovery section) */
    return off + 4;
}

/* craft an announce that states its locator: [u16 port][u8 ip_len 4][ip][u8 name_len 0].
   flags 0x04 = RELAY_ME, 0x08 = PROXIED (a relay speaking for the origin). */
static size_t dc_mk_ip(uint8_t *p, uint8_t uid, uint8_t flags, uint16_t dom, uint16_t port,
                       uint32_t mver, const uint8_t ip[4]){
    size_t off = RANT_DISCOVERY_META_OFF;
    uint8_t *b = p + off;
    memset(p, 0, off);
    p[0]='u';p[1]='D';p[2]='S';p[3]='C';
    p[4]=(uint8_t)RANT_DISCOVERY_PROTO_VERSION;
    p[5]=flags;
    p[6]=(uint8_t)dom; p[7]=(uint8_t)(dom>>8);
    memset(p+8, uid, 16);
    p[off-6]=(uint8_t)mver; p[off-5]=(uint8_t)(mver>>8);
    p[off-4]=(uint8_t)(mver>>16); p[off-3]=(uint8_t)(mver>>24);
    b[0]=(uint8_t)port; b[1]=(uint8_t)(port>>8); b[2]=4;
    memcpy(b+3, ip, 4); b[7]=0;                    /* name_len 0, no overlay */
    p[off-2]=8; p[off-1]=0;                        /* meta_len = 8 */
    return off + 8;
}

/* feed one crafted datagram to a core as if received from (ip, sport). via 0 = the
   discovery socket, 1 = the data port socket */
static void dc_feed(RantDiscoveryState *st, const uint8_t ip[4], uint16_t sport, int via,
                    const uint8_t *buf, size_t n, uint64_t now){
    RantDiscoveryAddr src;
    memset(&src, 0, sizeof src);
    memcpy(src.ip, ip, 4); src.ip_len = 4; src.port = sport;
    rant_discovery_on_datagram(st, &src,
                               via ? RANT_DISCOVERY_VIA_DATA : RANT_DISCOVERY_VIA_DISCOVERY,
                               rant_bytes(buf, n), now);
}

static void disc_core_checks(void){
    static uint8_t mem[8192];
    uint8_t buf[RANT_DISCOVERY_WIRE_MAX], out[RANT_DISCOVERY_WIRE_MAX];
    uint8_t sa[4]={10,0,0,1}, sb[4]={10,0,0,2}, sc[4]={10,0,0,3};
    RantDiscoveryCoreConfig c; RantDiscoveryState *st;
    uint32_t idA, idB; size_t n;
    memset(&c,0,sizeof c);
    memset(c.uuid,0xEE,16);                        /* receiver uuid, distinct from senders */
    c.domain_id=99; c.announce_interval_us=1000000; c.peer_timeout_us=1000000; c.max_peers=2;
    c.on_event=dc_event;
    st = rant_discovery_init(mem,sizeof mem,&c);
    ST_CHECK(st!=NULL, "disc-core: init");
    if (!st) return;
    rant_discovery_update(st, 1000, out, sizeof out);     /* start */

    /* 1. two peers announce: two ups, both ACTIVE */
    dc_up_n=dc_down_n=dc_refused_n=0;
    n=dc_mk(buf,1,0,99,5001,1); dc_feed(st,sa,7400,0,buf,n,2000); idA=dc_up_id;
    n=dc_mk(buf,2,0,99,5002,1); dc_feed(st,sb,7400,0,buf,n,2000); idB=dc_up_id;
    ST_CHECK(dc_up_n==2 && rant_discovery_peer_count(st)==2,
             "disc-core: two peers up (ups=%u count=%u)", dc_up_n, rant_discovery_peer_count(st));

    /* 2. a new peer is REFUSED when the table is full of ACTIVE peers */
    dc_up_n=dc_refused_n=0;
    n=dc_mk(buf,3,0,99,5003,1); dc_feed(st,sc,7400,0,buf,n,2000);
    ST_CHECK(dc_refused_n==1 && dc_up_n==0 && rant_discovery_peer_count(st)==2,
             "disc-core: refuse new peer when full of active (refused=%u up=%u count=%u)",
             dc_refused_n, dc_up_n, rant_discovery_peer_count(st));

    /* 3. silence past the timeout drops both, kept but excluded from the count */
    dc_down_n=0;
    rant_discovery_update(st, 2002000, out, sizeof out);
    ST_CHECK(dc_down_n==2 && dc_down_reason==(int)RANT_DISCOVERY_DROP && rant_discovery_peer_count(st)==0,
             "disc-core: timeout drops both (downs=%u reason=%d count=%u)",
             dc_down_n, dc_down_reason, rant_discovery_peer_count(st));

    /* 4. the same uuid returns: RESUME under the same local_id */
    dc_up_n=0;
    n=dc_mk(buf,1,0,99,5001,1); dc_feed(st,sa,7400,0,buf,n,2100000);
    ST_CHECK(dc_up_n==1 && dc_up_id==idA && rant_discovery_peer_count(st)==1,
             "disc-core: same uuid resumes same id (up=%u sameid=%d count=%u)",
             dc_up_n, dc_up_id==idA, rant_discovery_peer_count(st));

    /* 5. a new peer now evicts the oldest DROPPED peer (B) as GONE */
    dc_down_n=dc_up_n=0;
    n=dc_mk(buf,3,0,99,5003,1); dc_feed(st,sc,7400,0,buf,n,2100000);
    ST_CHECK(dc_down_n==1 && dc_down_id==idB && dc_down_reason==(int)RANT_DISCOVERY_GONE && dc_up_n==1,
             "disc-core: new peer evicts oldest dropped as GONE (downs=%u sameid=%d reason=%d up=%u)",
             dc_down_n, dc_down_id==idB, dc_down_reason, dc_up_n);

    /* 6. BYE is GONE */
    dc_down_n=0;
    n=dc_mk(buf,1,0x01,99,5001,1); dc_feed(st,sa,7400,0,buf,n,2100000);
    ST_CHECK(dc_down_n==1 && dc_down_id==idA && dc_down_reason==(int)RANT_DISCOVERY_GONE
             && rant_discovery_peer_count(st)==1,
             "disc-core: BYE is GONE (downs=%u sameid=%d reason=%d count=%u)",
             dc_down_n, dc_down_id==idA, dc_down_reason, rant_discovery_peer_count(st));

    /* 7. a new uuid announcing from an (ip, port) we already hold: that process restarted,
       so the predecessor is evicted as GONE rather than shadowing the newcomer's data */
    { uint32_t idP;
      st = rant_discovery_init(mem,sizeof mem,&c);              /* fresh receiver */
      rant_discovery_update(st, 1000, out, sizeof out);
      dc_up_n=dc_down_n=0;
      n=dc_mk(buf,7,0,99,6001,1); dc_feed(st,sa,7400,0,buf,n,3000); idP=dc_up_id;
      dc_up_n=dc_down_n=0;
      n=dc_mk(buf,8,0,99,6001,1); dc_feed(st,sa,7400,0,buf,n,3100);  /* new uuid, same ip:port */
      ST_CHECK(dc_down_n==1 && dc_down_id==idP && dc_down_reason==(int)RANT_DISCOVERY_GONE
               && dc_up_n==1 && rant_discovery_peer_count(st)==1,
               "disc-core: new uuid at a held ip:port evicts the predecessor (downs=%u sameid=%d reason=%d up=%u count=%u)",
               dc_down_n, dc_down_id==idP, dc_down_reason, dc_up_n, rant_discovery_peer_count(st));
    }

    /* 8. RELAY, the rules the loopback phase cannot see: who enlists us, one hop only, and
       a proxied locator losing to a direct path we still hear */
    { uint32_t id9; RantDiscoveryAddr a; size_t pn; uint16_t pport;
      st = rant_discovery_init(mem,sizeof mem,&c);              /* fresh receiver */
      rant_discovery_update(st, 1000, out, sizeof out);
      /* (a) a DIRECT relay-me announce enlists us: we owe a proxied announce for it */
      n=dc_mk(buf,9,0x04,99,7001,1); dc_feed(st,sa,7400,0,buf,n,3000);
      id9=dc_up_id;
      pn = rant_discovery_poll_relay(st, out, sizeof out);
      /* PROXIED is the loop stop, 8b proves it defeats enlistment even with RELAY_ME set.
         RELAY_ME rides along as origin info, so a stated locator is no endpoint identity */
      ST_CHECK(pn > 0 && (out[5] & 0x08) && (out[5] & 0x04),
               "disc-core: relay-me peer is proxied PROXIED, origin's RELAY_ME carried (n=%u flags=0x%02X)",
               (unsigned)pn, (unsigned)(pn ? out[5] : 0));
      pport = pn ? (uint16_t)(out[RANT_DISCOVERY_META_OFF] |
                             ((uint16_t)out[RANT_DISCOVERY_META_OFF+1] << 8)) : 0;
      ST_CHECK(pn > 0 && out[8]==9 && out[RANT_DISCOVERY_META_OFF+2]==4 &&
               memcmp(out+RANT_DISCOVERY_META_OFF+3, sa, 4)==0 && pport==7001,
               "disc-core: the proxy carries the ORIGIN's uuid + the locator we hold (uuid=%u port=%u)",
               (unsigned)(pn ? out[8] : 0), (unsigned)pport);
      ST_CHECK(rant_discovery_poll_relay(st, out, sizeof out)==0,
               "disc-core: one proxied announce per peer per interval");
      /* (b) a PROXIED announce never enlists a SECOND hop, even carrying RELAY_ME: a
             peer we know only second-hand is not ours to introduce (the loop stop) */
      n=dc_mk_ip(buf,11,(uint8_t)(0x08|0x04),99,7011,1,sc);
      dc_feed(st,sb,7400,0,buf,n,3100);
      ST_CHECK(rant_discovery_peer_count(st)==2 && rant_discovery_poll_relay(st,out,sizeof out)==0,
               "disc-core: a proxied announce never enlists a second relay hop");
      /* (c) a proxied locator is a CANDIDATE: the direct path we are still hearing wins,
             so a relay's view cannot flap the address a peer's data is unicast to */
      n=dc_mk_ip(buf,9,0x08,99,7001,2,sb);        /* the relay says peer 9 is at 10.0.0.2 */
      dc_feed(st,sc,7400,0,buf,n,3200);
      memset(&a,0,sizeof a); rant_discovery_addr_of_id(st, id9, &a);
      ST_CHECK(memcmp(a.ip, sa, 4)==0,
               "disc-core: a proxied locator never stomps a live direct one (%u.%u.%u.%u)",
               a.ip[0], a.ip[1], a.ip[2], a.ip[3]);
      /* (d) contrast: the same stated address WITHOUT the proxied flag is the peer
             speaking for itself, which stays authoritative and does move it */
      n=dc_mk_ip(buf,9,0,99,7001,3,sb);
      dc_feed(st,sc,7400,0,buf,n,3300);
      memset(&a,0,sizeof a); rant_discovery_addr_of_id(st, id9, &a);
      ST_CHECK(memcmp(a.ip, sb, 4)==0,
               "disc-core: ...but a peer's OWN stated locator still is (%u.%u.%u.%u)",
               a.ip[0], a.ip[1], a.ip[2], a.ip[3]);
    }

    /* 9. SELF-IP: a node states its locator outright. It must ride our own announce, and a
       relay must propagate the stated address, not the source it saw. */
    { RantDiscoveryCoreConfig sc2; RantDiscoveryState *s2; RantDiscoveryAddr a;
      uint8_t pub_ip[4]={203,0,113,7}; uint32_t id12; size_t pn;
      sc2 = c;                                     /* same domain/timing, our own uuid */
      memcpy(sc2.self_ip, pub_ip, 4); sc2.self_ip_len = 4; sc2.data_port = 7400;
      s2 = rant_discovery_init(mem,sizeof mem,&sc2);
      ST_CHECK(s2!=NULL, "disc-core: self-ip core init");
      if (s2){
          n = rant_discovery_update(s2, 1000, out, sizeof out);          /* start: solicit + blob */
          ST_CHECK(n > (size_t)RANT_DISCOVERY_META_OFF + 3 &&
                   out[RANT_DISCOVERY_META_OFF+2]==4 &&
                   memcmp(out+RANT_DISCOVERY_META_OFF+3, pub_ip, 4)==0,
                   "disc-core: our announce states the configured self ip");
      }
      /* the relay side: peer 12 announces RELAY_ME FROM sa while STATING pub_ip. The relay
         must hold (and hand on) the stated address, so the override survives the hop. */
      st = rant_discovery_init(mem+4096,sizeof mem-4096,&c);
      rant_discovery_update(st, 1000, out, sizeof out);
      n=dc_mk_ip(buf,12,0x04,99,7400,1,pub_ip);
      dc_feed(st,sa,7400,0,buf,n,4000);      /* source sa, states pub_ip */
      id12=dc_up_id;
      memset(&a,0,sizeof a); rant_discovery_addr_of_id(st, id12, &a);
      ST_CHECK(memcmp(a.ip, pub_ip, 4)==0,
               "disc-core: a stated locator beats the source it arrived from (%u.%u.%u.%u)",
               a.ip[0], a.ip[1], a.ip[2], a.ip[3]);
      pn = rant_discovery_poll_relay(st, out, sizeof out);
      ST_CHECK(pn > 0 && memcmp(out+RANT_DISCOVERY_META_OFF+3, pub_ip, 4)==0,
               "disc-core: the relay proxies the STATED locator, not the source it saw");
    }

    /* 10. OBSERVED SOURCES: a unicast only peer behind a NAT advertises a fiction, so the
       receiver binds one observed source per local channel and routes everything there. */
    { RantDiscoveryCoreConfig c4; RantDiscoveryAddr a; uint32_t id20, id21, id22, idX, idU;
      uint8_t na[4]={192,168,1,50}, sx[4]={192,168,1,200}, su[4]={192,168,1,51};
      size_t pn; int ex;
      c4 = c; c4.max_peers = 4;
      st = rant_discovery_init(mem,sizeof mem,&c4);
      rant_discovery_update(st, 1000, out, sizeof out);
      /* (a) bind and route: a RELAY_ME announce arrives on our data channel from (na, 33333).
         Data goes to the observed source, attribution refuses the phantom locator. */
      dc_up_n=0;
      n=dc_mk(buf,20,0x04,99,6000,1); dc_feed(st,na,33333,1,buf,n,5000); id20=dc_up_id;
      memset(&a,0,sizeof a); rant_discovery_addr_of_id(st, id20, &a);
      ST_CHECK(dc_up_n==1 && a.port==33333 && memcmp(a.ip,na,4)==0,
               "disc-core: data routes to the observed source, not the locator (port=%u)", a.port);
      ST_CHECK(rant_discovery_id_for_addr(st,na,4,33333,NULL) &&
               !rant_discovery_id_for_addr(st,na,4,6000,NULL),
               "disc-core: attribution accepts the observed source, refuses the phantom locator");
      /* (b) the discovery channel binds independently: the same announce heard on our
         discovery socket from another flow, so discovery TX hits exactly that endpoint */
      n=dc_mk(buf,20,0x04,99,6000,1); dc_feed(st,na,44444,0,buf,n,5001);
      { int k = rant_discovery_peer_addr(st, 0, &a);
        ST_CHECK(k==2 && a.port==44444,
                 "disc-core: discovery TX targets the discovery-channel source exactly (k=%d port=%u)",
                 k, a.port); }
      /* (c) quiet rebind: the NAT expired and the next announce arrives from a new flow.
         Sends follow at once and no peer_up refires. */
      dc_up_n=0;
      n=dc_mk(buf,20,0x04,99,6000,1); dc_feed(st,na,55555,1,buf,n,5002);
      memset(&a,0,sizeof a); rant_discovery_addr_of_id(st, id20, &a);
      ST_CHECK(a.port==55555 && dc_up_n==0,
               "disc-core: a rebound mapping redirects sends quietly (port=%u ups=%u)", a.port, dc_up_n);
      /* (d) two NAT'd containers on ONE device advertise the SAME phantom locator: they
         must coexist (neither is that endpoint), distinguished by their observed sources */
      dc_down_n=0; dc_up_n=0;
      n=dc_mk(buf,21,0x04,99,6000,1); dc_feed(st,na,55666,1,buf,n,5003); id21=dc_up_id;
      ST_CHECK(dc_up_n==1 && dc_down_n==0 && rant_discovery_peer_count(st)==2,
               "disc-core: same phantom locator never evicts a NAT'd sibling (downs=%u count=%u)",
               dc_down_n, rant_discovery_peer_count(st));
      { uint32_t got=0;
        ST_CHECK(rant_discovery_id_for_addr(st,na,4,55666,&got) && got==id21 &&
                 rant_discovery_id_for_addr(st,na,4,55555,&got) && got==id20,
                 "disc-core: observed sources tell the siblings apart"); }
      /* (e) ...but a NEW uuid arriving from an endpoint held as some peer's OBSERVED
         source is that socket reused by a new process: the old occupant is evicted */
      dc_down_n=0;
      n=dc_mk(buf,22,0x04,99,6000,1); dc_feed(st,na,55666,1,buf,n,5004); id22=dc_up_id;
      ST_CHECK(dc_down_n==1 && dc_down_id==id21 && dc_down_reason==(int)RANT_DISCOVERY_GONE,
               "disc-core: a reused observed endpoint evicts its old occupant (downs=%u sameid=%d)",
               dc_down_n, dc_down_id==id21);
      (void)id22;
      /* (f) a stated self_ip is authoritative: observed never binds over the operator's
         asserted locator, even for a RELAY_ME sender */
      { uint8_t pub2[4]={203,0,113,9};
        n=dc_mk_ip(buf,23,0x04,99,6001,1,pub2); dc_feed(st,su,60123,1,buf,n,5005);
        memset(&a,0,sizeof a); rant_discovery_addr_of_id(st, dc_up_id, &a);
        ST_CHECK(memcmp(a.ip,pub2,4)==0 && a.port==6001,
                 "disc-core: a stated self_ip beats the observed source (%u.%u.%u.%u:%u)",
                 a.ip[0],a.ip[1],a.ip[2],a.ip[3],a.port); }
      /* (g) INTRODUCTIONS: the inbound half of relaying. A relay-me peer is told who we
         hear directly, at its observed source, and re-told when someone new appears. */
      c4.max_peers = 6;
      st = rant_discovery_init(mem,sizeof mem,&c4);          /* fresh relayer */
      rant_discovery_update(st, 1000, out, sizeof out);
      n=dc_mk(buf,30,0,99,7000,1); dc_feed(st,sx,7400,0,buf,n,6000); idX=dc_up_id;   /* peer X */
      n=dc_mk(buf,31,0x04,99,6000,1); dc_feed(st,su,50001,1,buf,n,6001); idU=dc_up_id; /* relay U */
      pn = rant_discovery_poll_introduce(st, out, sizeof out, &a, &ex);
      ST_CHECK(pn > 0 && out[8]==30 && ex==1 && a.port==50001 && memcmp(a.ip,su,4)==0,
               "disc-core: a new relay-me peer is introduced to everyone we hear (uuid=%u to=%u)",
               (unsigned)(pn?out[8]:0), a.port);
      ST_CHECK(rant_discovery_poll_introduce(st, out, sizeof out, &a, &ex)==0,
               "disc-core: ...exactly once (change-triggered, not periodic)");
      /* a newcomer re-triggers a walk to every relay-me peer */
      n=dc_mk(buf,32,0,99,7002,1); dc_feed(st,sx,7401,0,buf,n,6002);
      { int saw32=0, k2;
        for (k2=0;k2<8;k2++){
            pn = rant_discovery_poll_introduce(st, out, sizeof out, &a, &ex);
            if (!pn) break;
            if (out[8]==32) saw32=1;
        }
        ST_CHECK(saw32, "disc-core: a newcomer is introduced to existing relay-me peers"); }
      /* a peer known only from a PROXY is not ours to introduce (the one-hop rule) */
      n=dc_mk_ip(buf,33,0x08,99,7003,1,sx); dc_feed(st,sx,7400,0,buf,n,6003);
      { int saw33=0, k2;
        n=dc_mk(buf,34,0,99,7004,1); dc_feed(st,sx,7402,0,buf,n,6004);   /* re-trigger a walk */
        for (k2=0;k2<8;k2++){
            pn = rant_discovery_poll_introduce(st, out, sizeof out, &a, &ex);
            if (!pn) break;
            if (out[8]==33) saw33=1;
        }
        ST_CHECK(!saw33, "disc-core: a proxy-heard peer is never introduced onward"); }
      (void)idX; (void)idU;
    }

    /* 11. GHOST GATE: relaying is sustained by direct liveness only. Proxies still refresh
       plain liveness, so the entry outlives the origin by one timeout, then dies. */
    { RantDiscoveryCoreConfig c5; uint64_t t; size_t pn; int round, relayed;
      c5 = c; c5.max_peers = 4;
      st = rant_discovery_init(mem,sizeof mem,&c5);
      rant_discovery_update(st, 1000, out, sizeof out);
      n=dc_mk(buf,40,0x04,99,8000,1); dc_feed(st,sa,7400,0,buf,n,1000);     /* direct relay-me */
      n=dc_mk(buf,40,0x04,99,8000,1); dc_feed(st,sa,7400,0,buf,n,900000);   /* still direct */
      t = 1001000;                       /* first announce tick: direct heard 101ms ago */
      rant_discovery_update(st, t, out, sizeof out);
      pn = rant_discovery_poll_relay(st, out, sizeof out);
      ST_CHECK(pn > 0 && out[8]==40, "disc-core: a directly heard relay-me peer is relayed");
      /* the origin dies while other relays' PROXIED announces keep arriving. They keep the
         entry alive but must never keep us relaying it. */
      relayed = 0;
      for (round = 0; round < 3; round++){
          t += 1000000;
          n=dc_mk_ip(buf,40,(uint8_t)(0x08|0x04),99,8000,1,sa);
          dc_feed(st,sb,7400,0,buf,n,t);
          rant_discovery_update(st, t + 1000, out, sizeof out);
          if (rant_discovery_poll_relay(st, out, sizeof out)) relayed = 1;
      }
      ST_CHECK(!relayed && rant_discovery_peer_count(st)==1,
               "disc-core: proxies keep a peer alive but never sustain relaying (count=%u)",
               rant_discovery_peer_count(st));
      /* nor is a direct-stale origin introduced to a newly appearing relay-me peer */
      n=dc_mk(buf,41,0x04,99,8001,1); dc_feed(st,sb,50002,1,buf,n,t + 2000);
      { int saw40=0, k2; RantDiscoveryAddr a2; int ex2;
        for (k2=0;k2<8;k2++){
            pn = rant_discovery_poll_introduce(st, out, sizeof out, &a2, &ex2);
            if (!pn) break;
            if (out[8]==40) saw40=1;
        }
        ST_CHECK(!saw40, "disc-core: a direct-stale peer is not ours to introduce"); }
      /* the proxies stop (every relay's own direct went stale): the ghost now times out */
      dc_down_n=0;
      rant_discovery_update(st, t + 2200000, out, sizeof out);
      ST_CHECK(dc_down_n>=1, "disc-core: the ghost dies once proxies cease (downs=%u)", dc_down_n);
    }

    /* 12. A UNICAST-ONLY RECEIVER: a published port forward rewrites the source to the
       gateway on our own subnet, so a source never overrides a held locator here. */
    { RantDiscoveryCoreConfig c6; RantDiscoverySubnet net; RantDiscoveryAddr a;
      RantDiscoveryPeer v; uint32_t id50, got;
      static const uint8_t gw[4]={10,0,2,2}, real_ip[4]={192,168,1,106};
      static const uint8_t net_ip[4]={10,0,2,0}, net_mask[4]={255,255,255,0};
      c6 = c; c6.max_peers = 4; c6.relay_me = 1;         /* we are the NAT'd node */
      st = rant_discovery_init(mem,sizeof mem,&c6);
      memcpy(net.ip, net_ip, 4); memcpy(net.mask, net_mask, 4);
      rant_discovery_set_local_subnets(st, &net, 1);       /* the container's own subnet */
      rant_discovery_update(st, 1000, out, sizeof out);
      /* (a) an introduction states the peer's true routed locator */
      n=dc_mk_ip(buf,50,0x08,99,5555,1,real_ip); dc_feed(st,sa,7400,0,buf,n,9000);
      id50=dc_up_id;
      /* (b) its direct announce arrives through our port forward: the source is the gateway
         on our own subnet, rank 2 against the real address's rank 1. The locator must not move. */
      n=dc_mk(buf,50,0,99,5555,2); dc_feed(st,gw,33445,1,buf,n,9001);
      { int have = rant_discovery_peer_at(st, 0, &v);
        ST_CHECK(have && memcmp(v.addr.ip,real_ip,4)==0 && v.addr.port==5555,
                 "disc-core: a forwarded source never overrides the held locator (%u.%u.%u.%u:%u)",
                 v.addr.ip[0],v.addr.ip[1],v.addr.ip[2],v.addr.ip[3],v.addr.port); }
      /* (c) ...but it IS the peer's return path: bound as the observed source even for
         an ordinary (non-relay-me) peer, routing data + attribution through the flow */
      memset(&a,0,sizeof a);
      ST_CHECK(rant_discovery_addr_of_id(st, id50, &a) && a.port==33445 && memcmp(a.ip,gw,4)==0,
               "disc-core: a unicast-only node routes every peer via its observed return path (port=%u)",
               a.port);
      got=0;
      ST_CHECK(rant_discovery_id_for_addr(st,gw,4,33445,&got) && got==id50,
               "disc-core: ...and attributes arrivals from that flow to the peer");
    }

    /* 13. DUPLICATE SUPPRESSION: two foreign proxies within one announce interval suppress
       our own emission, our looped echo never counts, and a changed origin re arms it. */
    { RantDiscoveryCoreConfig c7; uint8_t mine[RANT_DISCOVERY_WIRE_MAX];
      size_t mn, pn2;
      c7 = c; c7.max_peers = 4;
      st = rant_discovery_init(mem,sizeof mem,&c7);
      rant_discovery_update(st, 1000, out, sizeof out);
      n=dc_mk(buf,60,0x04,99,8000,1); dc_feed(st,sa,7400,0,buf,n,1000);   /* direct relay-me */
      mn = rant_discovery_poll_relay(st, mine, sizeof mine);
      ST_CHECK(mn > 16 && mine[8]==60 && memcmp(mine + mn - 16, c7.uuid, 16)==0,
               "disc-core: our proxy carries our relayer trailer (n=%u)", (unsigned)mn);
      /* our own echo comes back, plus ONE foreign proxy (old-style, trailer-less) */
      dc_feed(st,sc,7400,0,mine,mn,2000);                                  /* self echo */
      n=dc_mk_ip(buf,60,(uint8_t)(0x08|0x04),99,8000,1,sa);
      dc_feed(st,sb,7400,0,buf,n,2001);                                    /* 1 foreign */
      n=dc_mk(buf,60,0x04,99,8000,1); dc_feed(st,sa,7400,0,buf,n,900000);  /* stay direct-fresh */
      rant_discovery_update(st, 1001000, out, sizeof out);
      ST_CHECK(rant_discovery_poll_relay(st, out, sizeof out) > 0,
               "disc-core: one foreign proxy does not suppress; a self echo never counts");
      /* two foreign proxies within the interval: quorum exists, we sit this one out */
      n=dc_mk_ip(buf,60,(uint8_t)(0x08|0x04),99,8000,1,sa);
      dc_feed(st,sb,7400,0,buf,n,1500000);
      dc_feed(st,sc,7400,0,buf,n,1600000);
      n=dc_mk(buf,60,0x04,99,8000,1); dc_feed(st,sa,7400,0,buf,n,1900000);
      rant_discovery_update(st, 2001000, out, sizeof out);
      ST_CHECK(rant_discovery_poll_relay(st, out, sizeof out) == 0,
               "disc-core: a relay quorum suppresses our periodic emission");
      /* the quorum went quiet: the next tick re-enlists us */
      n=dc_mk(buf,60,0x04,99,8000,1); dc_feed(st,sa,7400,0,buf,n,2900000);
      rant_discovery_update(st, 3001000, out, sizeof out);
      pn2 = rant_discovery_poll_relay(st, out, sizeof out);
      ST_CHECK(pn2 > 0, "disc-core: relay silence re-enlists us at the next tick");
      /* a CHANGED origin bypasses suppression: news beats steady-state thinning */
      n=dc_mk_ip(buf,60,(uint8_t)(0x08|0x04),99,8000,1,sa);
      dc_feed(st,sb,7400,0,buf,n,3200000);
      dc_feed(st,sc,7400,0,buf,n,3200001);
      n=dc_mk(buf,60,0x04,99,8000,2); dc_feed(st,sa,7400,0,buf,n,3300000);  /* blob v2 */
      ST_CHECK(rant_discovery_poll_relay(st, out, sizeof out) > 0,
               "disc-core: a changed origin is relayed at once despite a quorum");
    }
}

/* node core peer lifecycle, sans IO: drive the peer up, down and refused hooks directly
   over a transport with no sockets, clock or platform anywhere. */
static uint32_t nc_up_n, nc_down_n, nc_refused_n, nc_up_id, nc_down_id;
static void nc_event(const RantEvent *ev){
    if      (ev->kind==RANT_PEER_UP)       { nc_up_n++;   nc_up_id=ev->peer; }
    else if (ev->kind==RANT_PEER_DOWN)     { nc_down_n++; nc_down_id=ev->peer; }
    else if (ev->kind==RANT_ERROR && ev->error==RANT_E_PEER_REFUSED){ nc_refused_n++; }
}
/* feed the node core through a real sans IO discovery core: build an announce datagram
   with no overlay so discovery fires events into the node core. flags 0x01 = BYE. */
static size_t nc_dgram(uint8_t *p, uint8_t uid, uint8_t flags, uint16_t dom, uint16_t port,
                       const char *name, uint32_t mver){
    size_t off = RANT_DISCOVERY_META_OFF;
    uint8_t *b = p + off;
    uint8_t nl = name ? (uint8_t)strlen(name) : 0;
    uint16_t ml = (uint16_t)(4u + nl);
    memset(p, 0, off);
    p[0]='u';p[1]='D';p[2]='S';p[3]='C';
    p[4]=(uint8_t)RANT_DISCOVERY_PROTO_VERSION;
    p[5]=flags;
    p[6]=(uint8_t)dom; p[7]=(uint8_t)(dom>>8);
    memset(p+8, uid, 16);                          /* a distinct uuid per uid */
    p[off-6]=(uint8_t)mver; p[off-5]=(uint8_t)(mver>>8);
    p[off-4]=(uint8_t)(mver>>16); p[off-3]=(uint8_t)(mver>>24);
    b[0]=(uint8_t)port; b[1]=(uint8_t)(port>>8); b[2]=0; b[3]=nl;   /* port, no self ip, name_len */
    if (nl) memcpy(b+4, name, nl);
    p[off-2]=(uint8_t)ml; p[off-1]=(uint8_t)(ml>>8);
    return off + ml;
}

/* node core peer lifecycle, fully sans IO: a transport plus a real discovery core, the
   node core delegating its peer table to discovery. */
static void node_core_checks(void){
    static uint8_t tmem[1<<18], cmem[4096], dmem[8192], amem[1<<16];
    uint8_t buf[256], out[RANT_DISCOVERY_WIRE_MAX];
    uint8_t sa[4]={10,0,0,1}, sb[4]={10,0,0,2}, sc[4]={10,0,0,3};
    RantConfig tc; RantTransportState *tr; RantTopicDef ch[1];
    RantDiscoveryCoreConfig dcfg; RantDiscoveryState *st;
    i_RantNodeCoreConfig cc; i_RantNodeCore *nc;
    i_RantNodeDest d; uint32_t id, idA, idB; size_t n;
    /* STATIC allocator over a caller buffer: the embedded no-heap contract */
    static RantAllocator A; A = rant_allocator_static(amem, sizeof amem);

    memset(ch,0,sizeof ch); ch[0].name="nc/topic";
    memset(&tc,0,sizeof tc); tc.topics=ch; tc.n_topics=1; tc.max_peers=2;
    tc.allocator=rant_allocator_alloc; tc.user=&A;
    tr = rant_transport_init(tmem, sizeof tmem, &tc);
    ST_CHECK(tr!=NULL, "node-core: transport init");
    if (!tr) return;

    /* node core first (discovery bound once it exists, exactly like the runtime) */
    memset(&cc,0,sizeof cc);
    cc.transport=tr; cc.n_topics=1; cc.frag_size=1200; cc.on_event=nc_event;
    cc.alloc=rant_allocator_alloc; cc.alloc_user=&A;
    nc = i_rant_node_core_init(cmem, sizeof cmem, &cc);
    ST_CHECK(nc!=NULL, "node-core: init");
    if (!nc) return;

    /* the discovery core whose peer table the node core delegates to: reserve the node's
       per peer scratch via peer_user_bytes and wire its events into the node core */
    memset(&dcfg,0,sizeof dcfg); memset(dcfg.uuid,0xEE,16);
    dcfg.domain_id=99; dcfg.announce_interval_us=1000000; dcfg.peer_timeout_us=1000000; dcfg.max_peers=2;
    dcfg.peer_user_bytes=i_rant_node_core_peer_user_bytes();
    dcfg.on_event=i_rant_node_core_on_disc_event; dcfg.user=nc;
    st = rant_discovery_init(dmem, sizeof dmem, &dcfg);
    ST_CHECK(st!=NULL, "node-core: discovery init");
    if (!st) return;
    i_rant_node_core_bind_discovery(nc, st);
    rant_discovery_update(st, 1000, out, sizeof out);     /* start */

    /* build-meta still works (uses the transport, not any peer table) */
    {   RantBytes mb;
        i_rant_node_core_build_meta(nc);
        mb = i_rant_node_core_meta(nc);
        ST_CHECK(mb.len>=5 && rant_meta_frag(mb)==1200,
                 "node-core: builds overlay (frag=%u)", rant_meta_frag(mb)); }

    /* 1. two peers announce: two ups, discovery assigns the ids, resolve each way */
    nc_up_n=nc_down_n=nc_refused_n=0;
    n=nc_dgram(buf,1,0,99,5001,"nc-self",1); dc_feed(st,sa,7400,0,buf,n,2000); idA=nc_up_id;
    n=nc_dgram(buf,2,0,99,5002,"nc-self",1); dc_feed(st,sb,7400,0,buf,n,2000); idB=nc_up_id;
    ST_CHECK(nc_up_n==2, "node-core: two peers up (ups=%u)", nc_up_n);
    {   RantString pn = i_rant_node_core_peer_name(nc, idA);
        ST_CHECK(pn.data && pn.len==7 && memcmp(pn.data,"nc-self",7)==0,
                 "node-core: peer name learned from announce (%.*s)", (int)pn.len, pn.data?pn.data:"?"); }
    ST_CHECK(i_rant_node_core_resolve(nc,idA,&d) && d.port==5001 && d.ip[3]==1,
             "node-core: peer id resolves to addr (port=%u ip3=%u)", d.port, d.ip[3]);
    ST_CHECK(i_rant_node_core_id_for_addr(nc, sb, 5002, &id) && id==idB,
             "node-core: addr resolves to id (id=%u)", id);

    /* a nameless announce falls back to "unknown-peer", never empty or NULL */
    {   RantString pn;
        n=nc_dgram(buf,1,0,99,5001,NULL,2);     dc_feed(st,sa,7400,0,buf,n,2001);
        pn = i_rant_node_core_peer_name(nc, idA);
        ST_CHECK(pn.data && pn.len==12 && memcmp(pn.data,"unknown-peer",12)==0,
                 "node-core: nameless announce -> unknown-peer (%.*s)", (int)pn.len, pn.data?pn.data:"(null)");
        n=nc_dgram(buf,1,0,99,5001,"nc-self",3); dc_feed(st,sa,7400,0,buf,n,2002); }

    /* 2. a third peer is REFUSED while the table is full of ACTIVE peers, the node forwards it */
    nc_refused_n=0;
    n=nc_dgram(buf,3,0,99,5003,"three",1); dc_feed(st,sc,7400,0,buf,n,2003);
    ST_CHECK(nc_refused_n==1, "node-core: refused forwarded (refused=%u)", nc_refused_n);

    /* 3. silence past the timeout DROPS both: a PEER_DOWN each, but the slots are kept (resolve) */
    nc_down_n=0;
    rant_discovery_update(st, 1003000, out, sizeof out);
    ST_CHECK(nc_down_n==2, "node-core: timeout drops both (downs=%u)", nc_down_n);
    ST_CHECK(i_rant_node_core_resolve(nc,idA,&d)==1, "node-core: dropped peer kept (resolves)");

    /* 4. the same uuid returns: RESUME re fires PEER_UP under the same id */
    nc_up_n=0;
    n=nc_dgram(buf,1,0,99,5001,"nc-self",4); dc_feed(st,sa,7400,0,buf,n,1100000);
    ST_CHECK(nc_up_n==1 && nc_up_id==idA, "node-core: resume re-ups same id (ups=%u id=%u)", nc_up_n, nc_up_id);

    /* 5. GONE (BYE) on the active peer: one PEER_DOWN, and it does not resolve any more */
    nc_down_n=0;
    n=nc_dgram(buf,1,0x01,99,5001,NULL,1); dc_feed(st,sa,7400,0,buf,n,1100001);
    ST_CHECK(nc_down_n==1, "node-core: GONE on active fires down (downs=%u)", nc_down_n);
    ST_CHECK(i_rant_node_core_resolve(nc,idA,&d)==0, "node-core: GONE peer freed (no resolve)");
}

#ifdef RANT_SHM
/* ===================== SHM self-tests (only when built -DRANT_SHM) ========= */

/* (1) the rant_shm mapping module: create/attach by name, write/stamp/read, the
   generation recycle guard, and the seqlock verify tail. */
static void shm_module_checks(void){
    i_RantShmConfig cfg; void *pw, *pr; i_RantShmPool *w, *r;
    uint32_t cap=0, rlen=0; void *cp; const void *rp;
    i_RantShmDesc d, d2; uint8_t wire[RANT_SHM_DESC_WIRE], a[16], b[16];
    const char msg[] = "hello shared memory";
    i_rant_plat_startup();
    memset(&cfg,0,sizeof cfg);
    /* per process segment name: the name is OS global, so a fixed one would let a concurrent
       selftest share this run's segment. Suffixed with the per process domain base. */
#ifdef _WIN32
    snprintf(cfg.name,sizeof cfg.name,"rant-shm-stmod-%u",(unsigned)st_domain_base);
#else
    snprintf(cfg.name,sizeof cfg.name,"/rant-shm-stmod-%u",(unsigned)st_domain_base);
#endif
    cfg.segment_id=0x1234; cfg.chunk_bytes=4096; cfg.n_chunks=4;
    pw=malloc(i_rant_shm_state_bytes()); pr=malloc(i_rant_shm_state_bytes());
    w=i_rant_shm_create(pw,&cfg); r=i_rant_shm_attach(pr,&cfg);
    ST_CHECK(w && r, "shm-mod: create + attach by name");
    if (w && r){
        i_rant_plat_host_uuid(a); i_rant_plat_host_uuid(b);
        ST_CHECK(i_rant_shm_host_match(a,b)==1, "shm-mod: host_uuid stable + matches");
        cp=i_rant_shm_chunk(w,0,&cap); memcpy(cp,msg,sizeof msg);
        i_rant_shm_stamp(w,0,(uint32_t)sizeof msg,&d);
        ST_CHECK(cap==4096 && d.generation==1, "shm-mod: loan + stamp (gen=%llu)", (unsigned long long)d.generation);
        i_rant_shm_desc_encode(&d,wire);
        ST_CHECK(i_rant_shm_desc_decode(&d2,wire,sizeof wire) &&
                 d2.chunk==d.chunk && d2.length==d.length && d2.generation==d.generation,
                 "shm-mod: descriptor wire round-trip");
        rp=i_rant_shm_read(r,&d2,&rlen);
        ST_CHECK(rp && rlen==sizeof msg && memcmp(rp,msg,sizeof msg)==0, "shm-mod: read sees writer bytes");
        ST_CHECK(i_rant_shm_verify(r,&d2)==1, "shm-mod: verify current gen ok");
        { void *cp2=i_rant_shm_chunk(w,0,NULL); i_RantShmDesc dn; uint32_t l;
          memcpy(cp2,"new",4); i_rant_shm_stamp(w,0,4,&dn);
          ST_CHECK(i_rant_shm_read(r,&d2,&l)==NULL, "shm-mod: recycled chunk -> old descriptor refused");
          ST_CHECK(i_rant_shm_verify(r,&d2)==0, "shm-mod: verify recycled gen fails (torn guard)"); }
        i_rant_shm_detach(r); i_rant_shm_detach(w);
    }
    free(pw); free(pr);
    i_rant_plat_cleanup();
}

/* (2) transport core loss and repair over shared memory: a mock on_shm with controllable
   success and a pump that can drop SHM-DATA (spec/testing.md). */
static int shml_ok, shml_recv, shml_lost, shml_drop;
static uint64_t shml_now;
static RantTransportState *shml_W, *shml_R;
static int shml_on_shm(void *u, uint16_t ch, uint32_t from, const uint8_t *desc){
    (void)u;(void)ch;(void)from;(void)desc; if (shml_ok){ shml_recv++; return 1; } return 0;
}
static void shml_on_event(const RantTransportEvent *ev){ if (ev->kind==RANT_TRANSPORT_MSG_LOST) shml_lost++; }
static void shml_pump(int n){
    uint8_t buf[RANT_DGRAM_MAX]; uint32_t to; size_t ol; int i;
    for (i=0;i<n;i++){
        while (rant_transport_poll_send(shml_W,&to,buf,sizeof buf,&ol,shml_now)){
            if (shml_drop>0 && (buf[0]&0x20u)){ shml_drop--; continue; }   /* drop SHM-DATA */
            rant_transport_on_datagram(shml_R, 1u, rant_bytes(buf, ol), shml_now);
        }
        while (rant_transport_poll_send(shml_R,&to,buf,sizeof buf,&ol,shml_now))
            rant_transport_on_datagram(shml_W, 2u, rant_bytes(buf, ol), shml_now);
        shml_now += 30000;   /* 30 ms: past repair_delay (20ms), lets heartbeats fire */
    }
}
static void shml_send(void){
    static unsigned char chunk[2048]; unsigned char desc[RANT_SHM_DESC_WIRE];
    memset(desc,0,sizeof desc); rant_transport_send_shm(shml_W, 0, rant_bytes(chunk, 1000), desc, shml_now);
}
static void shm_loss_checks(void){
    RantTopicDef cw, cr; RantConfig wc, rc; void *mw, *mr; size_t nw, nr;
    RantAllocator wa = rant_allocator_heap(0);
    RantAllocator ra = rant_allocator_heap(0);
    RantQos q; memset(&q,0,sizeof q); q.reliability=RANT_RELIABLE; q.keep_last=8;
    q.heartbeat_us=50000; q.repair_delay_us=20000;
    memset(&cw,0,sizeof cw); cw.name="shmloss"; cw.qos=q; cw.role=RANT_PUB_ONLY;
    memset(&cr,0,sizeof cr); cr.name="shmloss"; cr.qos=q; cr.role=RANT_SUB_ONLY;
    memset(&wc,0,sizeof wc); wc.topics=&cw; wc.n_topics=1; wc.max_peers=2;
    wc.allocator=rant_allocator_alloc; wc.user=&wa;
    memset(&rc,0,sizeof rc); rc.topics=&cr; rc.n_topics=1; rc.max_peers=2;
    rc.allocator=rant_allocator_alloc; rc.user=&ra;
    rc.on_shm=shml_on_shm; rc.on_event=shml_on_event;
    nw=rant_transport_required_memory(&wc); mw=malloc(nw); shml_W=rant_transport_init(mw,nw,&wc);
    nr=rant_transport_required_memory(&rc); mr=malloc(nr); shml_R=rant_transport_init(mr,nr,&rc);
    shml_now=1000000;
    rant_transport_peer_add(shml_W,2u,RANT_FRAG_SIZE); rant_transport_peer_add(shml_R,1u,RANT_FRAG_SIZE);
    st_apply_verified(shml_R, 1u, shml_W);
    st_apply_verified(shml_W, 2u, shml_R);
    rant_transport_peer_set_shm(shml_W,2u,1);
    ST_CHECK(rant_transport_publisher_match_count(shml_W,0)>0, "shm-loss: writer matched reader");
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
    rant_transport_destroy(shml_W); rant_transport_destroy(shml_R); free(mw); free(mr);
    rant_allocator_reset(&wa); rant_allocator_reset(&ra);
}

/* (3) full nodes on loopback: SHM across size classes (byte-exact + shm_tx/rx), and
   the inline fallback for a non-SHM-capable subscriber. */
static int shmn_recv; static size_t shmn_len; static unsigned long shmn_sum;
static void shmn_on_message(const RantMsg *msg){
    const unsigned char *p=(const unsigned char*)msg->data.data; size_t i, len=msg->data.len; unsigned long s=0;
    for(i=0;i<len;i++) s+=p[i];
    shmn_recv++; shmn_len=len; shmn_sum=s;
}
static RantNode *shmn_open(int is_pub, int shm_capable, uint16_t domain, void **mem_out){
    static RantTopicDef ch[2]; static int slot;
    RantTopicDef *d=&ch[slot++ & 1]; RantDiscoveryAddr seed; RantNodeOpts opts;
    size_t cap = 1u<<20; void *mem; RantQos q; memset(&q,0,sizeof q);
    q.reliability=RANT_RELIABLE; q.keep_last=4; q.catch_up=1;
    memset(d,0,sizeof *d); d->name="shmnode"; d->qos=q; d->role=is_pub?RANT_PUB_ONLY:RANT_SUB_ONLY;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&opts,0,sizeof opts); opts.domain=domain;   /* dynamic mode is SHM capable */
    opts.disable_shm = (uint8_t)(!shm_capable);   /* a non-SHM peer forces the inline UDP path */
    opts.discovery.max_peers=4;
    opts.net.multicast_interface="127.0.0.1"; opts.net.seed_peers=&seed; opts.net.n_seed_peers=1;
    mem=malloc(cap); *mem_out=mem;
    return test_node_open(mem, cap, NULL, is_pub?NULL:shmn_on_message, NULL, opts, d, 1);
}
static void shm_node_checks(void){
    static unsigned char buf[6*1024*1024];
    RantNode *P,*S; void *mp,*ms; int i; uint32_t tx=0, rx=0; size_t sizes[3];
    P=shmn_open(1,1,(uint16_t)(ST_DOMAIN+40),&mp); S=shmn_open(0,1,(uint16_t)(ST_DOMAIN+40),&ms);
    ST_CHECK(P&&S, "shm-node: SHM-capable pub + sub open");
    if (P&&S){
        for (i=0;i<800 && rant_node_publisher_match_count(P,0)==0;i++){ rant_node_poll(P,2); rant_node_poll(S,2); }
        ST_CHECK(rant_node_publisher_match_count(P,0)>0, "shm-node: matched");
        /* sizes[0] fits one datagram so it ships inline, not SHM. The two larger ones would
           fragment, so they take the SHM path. */
        sizes[0]=200; sizes[1]=300*1024; sizes[2]=4*1024*1024;
        for (i=0;i<3;i++){
            unsigned long want=0; size_t j; int before=shmn_recv, t;
            for (j=0;j<sizes[i];j++){ buf[j]=(unsigned char)((j*31u+(unsigned)i+1)&0xFF); want+=buf[j]; }
            rant_node_send(P,0,buf,sizes[i]);
            for (t=0;t<500 && shmn_recv==before;t++){ rant_node_poll(P,2); rant_node_poll(S,2); }
            ST_CHECK(shmn_recv==before+1 && shmn_len==sizes[i] && shmn_sum==want,
                     "shm-node: byte-exact %lu bytes", (unsigned long)sizes[i]);
            if (i==0){ tx=0; rant_node_shm_stats(P,&tx,NULL);
                ST_CHECK(tx==0, "shm-node: sub-fragment %lu B went inline, not SHM (tx=%u)",
                         (unsigned long)sizes[i], tx); }
        }
        rant_node_shm_stats(P,&tx,NULL); rant_node_shm_stats(S,NULL,&rx);
        ST_CHECK(tx==2 && rx==2, "shm-node: 2 fragmenting msgs over SHM, small inline (tx=%u rx=%u)", tx, rx);
        rant_node_close(P,1); rant_node_close(S,1);
    }
    free(mp); free(ms);
    /* inline fallback: a non-SHM-capable subscriber forces inline UDP */
    shmn_recv=0;
    P=shmn_open(1,1,78,&mp); S=shmn_open(0,0,78,&ms);
    if (P&&S){
        unsigned long want=0; size_t j; int before, t;
        for (i=0;i<800 && rant_node_publisher_match_count(P,0)==0;i++){ rant_node_poll(P,2); rant_node_poll(S,2); }
        for (j=0;j<50*1024;j++){ buf[j]=(unsigned char)((j*31u+9)&0xFF); want+=buf[j]; }
        before=shmn_recv; rant_node_send(P,0,buf,50*1024);
        for (t=0;t<500 && shmn_recv==before;t++){ rant_node_poll(P,2); rant_node_poll(S,2); }
        ST_CHECK(shmn_recv==before+1 && shmn_sum==want, "shm-node: non-SHM sub -> inline byte-exact");
        tx=0; rant_node_shm_stats(P,&tx,NULL);
        ST_CHECK(tx==0, "shm-node: no SHM used for the non-SHM reader (tx=%u)", tx);
        rant_node_close(P,1); rant_node_close(S,1);
    }
    free(mp); free(ms);
}

#endif /* RANT_SHM */

/* Unit checks for the small pure helpers: the fragment clamp, the send result codes and
 * the shared little endian packing. No sockets, straight against the transport core. */
static void unit_checks(void){
    /* rant_clamp_frag: 0 = default, else clamp into [MIN, MAX] */
    ST_CHECK(rant_clamp_frag(0) == RANT_FRAG_SIZE,
             "clamp: 0 -> default frag (%u)", (unsigned)rant_clamp_frag(0));
    ST_CHECK(rant_clamp_frag(65535) == RANT_FRAG_SIZE_MAX,
             "clamp: above-max -> MAX (%u)", (unsigned)rant_clamp_frag(65535));
    ST_CHECK(rant_clamp_frag(1) >= RANT_FRAG_SIZE_MIN,
             "clamp: tiny -> >= MIN (%u)", (unsigned)rant_clamp_frag(1));

    /* shared little-endian helpers: byte order + round-trip */
    {   uint8_t b[8];
        i_rant_le_w16(b, 0xBEEFu);
        ST_CHECK(b[0]==0xEF && b[1]==0xBE && i_rant_le_r16(b)==0xBEEFu,
                 "bytes: w16/r16 little-endian round-trip");
        i_rant_le_w32(b, 0x01020304u);
        ST_CHECK(b[0]==0x04 && b[3]==0x01 && i_rant_le_r32(b)==0x01020304u,
                 "bytes: w32/r32 little-endian round-trip");
        i_rant_le_w64(b, 0x0102030405060708ull);
        ST_CHECK(b[0]==0x08 && b[7]==0x01 && i_rant_le_r64(b)==0x0102030405060708ull,
                 "bytes: w64/r64 little-endian round-trip");
    }

    /* topic identity: deterministic and name-distinct */
    ST_CHECK(rant_topic_id("alpha") == rant_topic_id("alpha")
             && rant_topic_id("alpha") != rant_topic_id("beta"),
             "topic-id: deterministic and name-distinct");

    /* send result codes on the transport core: the size check precedes the role check, and
       an init with no allocator must refuse (spec/testing.md) */
    {   static uint8_t tmem[1<<16], amem[1<<12];
        RantTopicDef uch[2]; RantConfig tc; RantTransportState *ts; uint8_t buf[128];
        static RantAllocator A; A = rant_allocator_static(amem, sizeof amem);
        memset(uch, 0, sizeof uch);
        uch[0].name = "u/pub"; uch[0].role = RANT_PUBSUB;
        uch[1].name = "u/sub"; uch[1].role = RANT_SUB_ONLY;
        memset(&tc, 0, sizeof tc);
        tc.topics = uch; tc.n_topics = 2; tc.max_peers = 2;
        ST_CHECK(rant_transport_init(tmem, sizeof tmem, &tc) == NULL, "result: init without an allocator refused");
        tc.allocator = rant_allocator_alloc; tc.user = &A;
        ts = rant_transport_init(tmem, sizeof tmem, &tc);
        ST_CHECK(ts != NULL, "result: transport init");
        if (ts){
            size_t wire_cap = 65535u * (size_t)rant_transport_frag(ts);     /* checked before copy */
            memset(buf, 0, sizeof buf);
            ST_CHECK(rant_transport_send(ts, 5, rant_bytes(buf, 16),      0) == RANT_ERR_NO_TOPIC, "result: out-of-range topic -> NO_CHANNEL");
            ST_CHECK(rant_transport_send(ts, 1, rant_bytes(buf, 16),      0) == RANT_ERR_ROLE,         "result: sub-only topic -> ROLE");
            ST_CHECK(rant_transport_send(ts, 0, rant_bytes(buf, wire_cap + 1u), 0) == RANT_ERR_TOO_BIG, "result: past the wire cap -> TOO_BIG");
            ST_CHECK(rant_transport_send(ts, 0, rant_bytes(buf, 16),      0) == RANT_OK,               "result: valid publish -> OK");
        }
    }
}

/* Exercise the staged cleanup paths of rant_node_open: force a failure at a different
 * stage each time, assert NULL, then confirm a normal open still works. */
/* open-failure event sink: capture the RANT_ERROR kind fired during a failing open */
static int of_seen_error;
static void of_on_event(const RantEvent *ev){ if (ev->kind == RANT_ERROR) of_seen_error = (int)ev->error; }

static void open_fail_checks(void){
    static uint8_t mem[1<<20];
    RantNode *n;
    of_seen_error = RANT_E_NONE;

    /* create fail: an over long topic name is rejected by rant_node_create_topic. The node
       opened fine and stays usable. */
    {   static char longname[RANT_TOPIC_NAME_MAX + 8]; RantTopic *c;
        RantAllocator a = rant_allocator_static(mem, sizeof mem);
        memset(longname, 'x', sizeof longname - 1); longname[sizeof longname - 1] = 0;
        n = rant_node_open(&a, NULL, NULL, NULL, &(RantNodeOpts){ .domain=ST_DOMAIN });
        ST_CHECK(n != NULL, "open-fail: node opens for create-fail check");
        c = n ? rant_node_create_topic(n, longname, RANT_PUBSUB, NULL, NULL) : NULL;
        ST_CHECK(c == NULL, "open-fail: over-long topic name -> create_channel NULL");
        if (n) rant_node_close(n, 0);
    }

    /* a non multicast discovery group fails the IGMP join, so the node unwinds through
       fail_sock and rant_last_error(NULL) names MCAST_JOIN with no handle to query */
    {   RantAllocator a = rant_allocator_static(mem, sizeof mem);
        RantEvent err; char line[160];
        n = rant_node_open(&a, NULL, NULL, NULL,
            &(RantNodeOpts){ .domain=ST_DOMAIN, .net={ .discovery_group="1.2.3.4" } });
        ST_CHECK(n == NULL, "open-fail: non-multicast discovery group -> NULL");
        err = rant_last_error(NULL);
        ST_CHECK(err.kind == RANT_ERROR && err.error == RANT_E_MCAST_JOIN,
                 "open-fail: last_error is RANT_E_MCAST_JOIN (%s)", rant_event_str(&err, line, sizeof line));
        if (n) rant_node_close(n, 0);
    }

    /* fail_sock: occupy an ephemeral port, then aim the node's data socket at it. The bind
       takes no reuse, so it collides and last_error names RANT_E_BIND with port and errno. */
    {   i_RantSock occupy;
        i_rant_plat_startup();
        occupy = i_rant_plat_udp_open();
        if (occupy != RANT_SOCK_BAD && i_rant_plat_bind(occupy, 0, 0, 0)){
            uint16_t port = i_rant_plat_local_port(occupy);
            RantAllocator a = rant_allocator_static(mem, sizeof mem);
            RantEvent err; char line[160];
            n = rant_node_open(&a, NULL, NULL, NULL,
                &(RantNodeOpts){ .domain=ST_DOMAIN, .net={ .data_port=port } });
            ST_CHECK(n == NULL, "open-fail: data-port collision -> NULL (fail_sock)");
            err = rant_last_error(NULL);
            ST_CHECK(err.kind == RANT_ERROR && err.error == RANT_E_BIND && err.port == port,
                     "open-fail: last_error is RANT_E_BIND port=%u (%s)", port, rant_event_str(&err, line, sizeof line));
            if (n) rant_node_close(n, 0);
        }
        if (occupy != RANT_SOCK_BAD) i_rant_plat_close(occupy);
        i_rant_plat_cleanup();
    }

    /* an on_event handler also receives the open failure directly (no handle needed): the
       node fires it on the passed-in callback before returning NULL. */
    {   RantAllocator a = rant_allocator_static(mem, sizeof mem);
        n = rant_node_open(&a, NULL, NULL, of_on_event,
            &(RantNodeOpts){ .domain=ST_DOMAIN, .net={ .discovery_group="1.2.3.4" } });
        ST_CHECK(n == NULL, "open-fail: open still returns NULL with on_event set");
        ST_CHECK(of_seen_error == RANT_E_MCAST_JOIN,
                 "open-fail: on_event received the failure (error=%d)", of_seen_error);
        if (n) rant_node_close(n, 0);
    }

    /* after the failed opens a normal open must still succeed (cleanup balanced) */
    {   RantAllocator a = rant_allocator_static(mem, sizeof mem);
        n = rant_node_open(&a, NULL, NULL, NULL, &(RantNodeOpts){ .domain=ST_DOMAIN });
        ST_CHECK(n != NULL, "open-fail: normal open still works after failures");
        if (n) rant_node_close(n, 0);
    }
}

/* An SHM capable node rewraps the transport callbacks to forward the app's user_data, so
 * a transport fired event must reach on_event with that same user_data (spec/testing.md). */
static void *evu_user; static int evu_collisions;
static void evu_on_event(const RantEvent *ev){
    if (ev->kind == RANT_ERROR && ev->error == RANT_E_NAME_COLLISION){ evu_user = ev->user; evu_collisions++; }
}
static void event_user_checks(void){
    static uint8_t mem_w[1<<20], mem_r[1<<20]; static int sentinel;
    const char *A="iuZA9tcJzAG", *B="5wVGxhTCmOC";   /* both to one identity, see collide.c */
    RantTopicDef cw, cr; RantNodeOpts wo, ro; RantNode *w, *r;
    uint8_t payload[16]; int i; memset(payload,0x5A,sizeof payload);
    memset(&cw,0,sizeof cw);
    cw.name=A; cw.role=RANT_PUB_ONLY;
    cw.qos.reliability=RANT_RELIABLE; cw.qos.keep_last=1; cw.qos.catch_up=1;
    cw.qos.max_message_bytes=32; cw.qos.heartbeat_us=50000;
    cr=cw; cr.name=B; cr.role=RANT_SUB_ONLY;
    wo = (RantNodeOpts){ .domain=ST_DOMAIN+5, .discovery={ .max_peers=4 } };
    ro = wo;
    ro.user_data=&sentinel;   /* dynamic mode, so SHM capable */
    evu_user=NULL; evu_collisions=0;
    w=test_node_open(mem_w,sizeof mem_w,NULL,NULL,NULL,wo,&cw,1);
    r=test_node_open(mem_r,sizeof mem_r,NULL,NULL,evu_on_event,ro,&cr,1);
    ST_CHECK(w && r, "event-user: nodes open");
    if (w && r){
        for (i=0;i<120 && evu_collisions==0;i++){ rant_node_send(w,0,payload,16); rant_node_poll(w,0); rant_node_poll(r,20); }
        ST_CHECK(evu_collisions >= 1, "event-user: collision event fired (%d)", evu_collisions);
        ST_CHECK(evu_user == (void*)&sentinel,
                 "event-user: on_event gets user_data not node ptr (got %p want %p)", evu_user, (void*)&sentinel);
        rant_node_close(r,1); rant_node_close(w,1);
    }
}


/* Dynamic growth: creating topics past the reserve relocates the node into a bigger arena.
 * Stream on topic 0 across several grows and assert nothing was lost, duplicated or reordered. */
static int dg_recv[24]; static int dg_seq_ok; static int dg_next0;
static void dg_on_message(const RantMsg *msg){
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
    RantAllocator pa = rant_allocator_heap(0), sa = rant_allocator_heap(0);
    RantNodeOpts po, so; RantNode *P=NULL, *S=NULL; RantTopic *pc0=NULL, *pcN;
    RantTopicOpts co; RantDiscoveryAddr seed; uint8_t payload[8]; int i, t;
    memset(&co,0,sizeof co); co.qos.reliability=RANT_RELIABLE; co.qos.keep_last=32;
    co.qos.catch_up=32; co.qos.heartbeat_us=50000;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=ST_DOMAIN+7; po.max_topics=2; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po;
    for (i=0;i<24;i++) dg_recv[i]=0;
    dg_seq_ok=1; dg_next0=0;
    P = rant_node_open(&pa, "dg-pub", NULL, NULL, &po);
    S = rant_node_open(&sa, "dg-sub", dg_on_message, NULL, &so);
    ST_CHECK(P&&S, "dyn-grow: nodes open (max_topics=2)");
    if (!(P&&S)){ if(P)rant_node_close(P,0); if(S)rant_node_close(S,0); return; }
    pc0 = rant_node_create_topic(P, names[0], RANT_PUB_ONLY, NULL, &co);
    rant_node_create_topic(S, names[0], RANT_SUB_ONLY, NULL, &co);
    ST_CHECK(pc0 != NULL, "dyn-grow: topic 0 created");
    for (t=0;t<800 && rant_topic_match_count(pc0)==0;t++){ rant_node_poll(P,2); rant_node_poll(S,2); }
    ST_CHECK(rant_topic_match_count(pc0)>0, "dyn-grow: topic 0 matched");
    for (i=0;i<5;i++){ payload[0]=(uint8_t)i; rant_topic_send(pc0,rant_bytes(payload,1), NULL); rant_node_poll(P,1); rant_node_poll(S,2); }
    /* create topics 1 to 11 on both: several grows, max_topics 2, 4, 8, 16 */
    for (i=1;i<12;i++){ rant_node_create_topic(P, names[i], RANT_PUB_ONLY, NULL, &co);
                        rant_node_create_topic(S, names[i], RANT_SUB_ONLY, NULL, &co);
                        rant_node_poll(P,1); rant_node_poll(S,1); }
    ST_CHECK(rant_topic_match_count(pc0)>0, "dyn-grow: topic 0 still matched after grows");
    for (i=5;i<10;i++){ payload[0]=(uint8_t)i; rant_topic_send(pc0,rant_bytes(payload,1), NULL); rant_node_poll(P,1); rant_node_poll(S,2); }
    for (t=0;t<400 && dg_recv[0]<10;t++){ rant_node_poll(P,1); rant_node_poll(S,2); }
    ST_CHECK(dg_recv[0]==10, "dyn-grow: all 10 on topic 0 delivered across grows (got %d)", dg_recv[0]);
    ST_CHECK(dg_seq_ok, "dyn-grow: topic 0 in-order, no loss/dup across grows");
    pcN = rant_node_topic(P, 11);                    /* a topic created AFTER a grow */
    for (i=0;i<3;i++){ payload[0]=0xAA; if(pcN) rant_topic_send(pcN,rant_bytes(payload,1), NULL); rant_node_poll(P,1); rant_node_poll(S,2); }
    for (t=0;t<200 && dg_recv[11]<3;t++){ rant_node_poll(P,1); rant_node_poll(S,2); }
    ST_CHECK(dg_recv[11]==3, "dyn-grow: post-grow topic delivers (got %d)", dg_recv[11]);
    rant_node_close(P,1); rant_node_close(S,1);
}

/* (17) QoS: a RANT_RELIABLE subscriber must refuse a best effort publisher, every other
   direction matches. Sans IO transport core: build the interest, apply, read the match. */
static unsigned long qos_incompat_n;
static void qos_on_event(const RantTransportEvent *ev){ if (ev->kind==RANT_TRANSPORT_QOS_INCOMPATIBLE) qos_incompat_n++; }
static void qos_pair(int wrel, int rrel, uint16_t *recv_out, unsigned long *evt_out){
    RantTopicDef cw, cr; RantConfig wc, rc; void *mw, *mr; size_t nw, nr;
    RantTransportState *W, *R; uint16_t pub=0, recv=0;
    RantAllocator wa = rant_allocator_heap(0);
    RantAllocator ra = rant_allocator_heap(0);
    memset(&cw,0,sizeof cw); cw.name="qostopic"; cw.role=RANT_PUB_ONLY;
    cw.qos.reliability=wrel?RANT_RELIABLE:RANT_BEST_EFFORT; cw.qos.keep_last=4;
    memset(&cr,0,sizeof cr); cr.name="qostopic"; cr.role=RANT_SUB_ONLY;
    cr.qos.reliability=rrel?RANT_RELIABLE:RANT_BEST_EFFORT; cr.qos.keep_last=4;
    memset(&wc,0,sizeof wc); wc.topics=&cw; wc.n_topics=1; wc.max_peers=2;
    wc.allocator=rant_allocator_alloc; wc.user=&wa;
    memset(&rc,0,sizeof rc); rc.topics=&cr; rc.n_topics=1; rc.max_peers=2; rc.on_event=qos_on_event;
    rc.allocator=rant_allocator_alloc; rc.user=&ra;
    nw=rant_transport_required_memory(&wc); mw=malloc(nw); W=rant_transport_init(mw,nw,&wc);
    nr=rant_transport_required_memory(&rc); mr=malloc(nr); R=rant_transport_init(mr,nr,&rc);
    rant_transport_peer_add(W,2u,RANT_FRAG_SIZE); rant_transport_peer_add(R,1u,RANT_FRAG_SIZE);
    qos_incompat_n=0;
    st_apply_verified(R, 1u, W);
    rant_transport_peer_match_counts(R,1u,&pub,&recv);
    if (recv_out) *recv_out=recv;
    if (evt_out)  *evt_out=qos_incompat_n;
    rant_transport_destroy(W); rant_transport_destroy(R); free(mw); free(mr);
    rant_allocator_reset(&wa); rant_allocator_reset(&ra);
}
static void qos_match_checks(void){
    uint16_t recv; unsigned long evt;
    qos_pair(0,1,&recv,&evt);   /* best-effort pub, reliable sub: REFUSED */
    ST_CHECK(recv==0, "qos: reliable sub refuses best-effort pub (receive_from=%u)", recv);
    ST_CHECK(evt>=1,  "qos: refusal raised RANT_QOS_INCOMPATIBLE (n=%lu)", evt);
    qos_pair(1,1,&recv,&evt);   /* reliable pub, reliable sub */
    ST_CHECK(recv==1 && evt==0, "qos: reliable sub matches reliable pub (recv=%u evt=%lu)", recv, evt);
    qos_pair(1,0,&recv,&evt);   /* reliable pub, best-effort sub: allowed downgrade */
    ST_CHECK(recv==1 && evt==0, "qos: best-effort sub matches reliable pub (recv=%u evt=%lu)", recv, evt);
    qos_pair(0,0,&recv,&evt);   /* both best-effort */
    ST_CHECK(recv==1 && evt==0, "qos: best-effort sub matches best-effort pub (recv=%u evt=%lu)", recv, evt);
}

/* (17b) flow control: a best effort reader matched to a reliable writer must not count
   toward backpressure. Fill the history ring past keep_last with no acks. */
static int beff_would_evict(int rrel){
    RantTopicDef cw, cr; RantConfig wc, rc; void *mw, *mr; size_t nw, nr;
    RantTransportState *W, *R; uint8_t payload[8]; int i, evict;
    RantAllocator wa = rant_allocator_heap(0);
    RantAllocator ra = rant_allocator_heap(0);
    memset(&cw,0,sizeof cw); cw.name="beff"; cw.role=RANT_PUB_ONLY;
    cw.qos.reliability=RANT_RELIABLE; cw.qos.keep_last=2;
    memset(&cr,0,sizeof cr); cr.name="beff"; cr.role=RANT_SUB_ONLY;
    cr.qos.reliability=rrel?RANT_RELIABLE:RANT_BEST_EFFORT; cr.qos.keep_last=2;
    memset(&wc,0,sizeof wc); wc.topics=&cw; wc.n_topics=1; wc.max_peers=2;
    wc.allocator=rant_allocator_alloc; wc.user=&wa;
    memset(&rc,0,sizeof rc); rc.topics=&cr; rc.n_topics=1; rc.max_peers=2;
    rc.allocator=rant_allocator_alloc; rc.user=&ra;
    nw=rant_transport_required_memory(&wc); mw=malloc(nw); W=rant_transport_init(mw,nw,&wc);
    nr=rant_transport_required_memory(&rc); mr=malloc(nr); R=rant_transport_init(mr,nr,&rc);
    rant_transport_peer_add(W,2u,RANT_FRAG_SIZE); rant_transport_peer_add(R,1u,RANT_FRAG_SIZE);
    st_apply_verified(W, 2u, R);   /* W learns (and verifies) that R subscribes */
    memset(payload,0x5A,sizeof payload);
    for (i=0;i<5;i++) rant_transport_send(W,0,rant_bytes(payload,sizeof payload),1000u+(uint64_t)i);
    evict = rant_transport_send_would_evict(W,0);
    rant_transport_destroy(W); rant_transport_destroy(R); free(mw); free(mr);
    rant_allocator_reset(&wa); rant_allocator_reset(&ra);
    return evict;
}
static void beff_flow_checks(void){
    ST_CHECK(beff_would_evict(0)==0, "flow: best-effort reader never stalls a reliable writer");
    ST_CHECK(beff_would_evict(1)==1, "flow: reliable reader does apply backpressure");
}

/* rant_schema_print is the inverse of compile: print a schema back to DSL, recompile that
   text, and assert an exact wire round-trip plus that the (NULL,0) measure matches. */
static void schema_print_roundtrip(RantAllocator *ma, RantSchema *s, const char *label){
    char buf[1024]; uint32_t need; RantSchema *back;
    if (!s) return;
    need = rant_schema_print(s, NULL, 0);                   /* snprintf-style measure */
    rant_schema_print(s, buf, sizeof buf);
    ST_CHECK(need > 0 && need < sizeof buf && strlen(buf) == (size_t)need,
             "schema-print: %s measures (%u) and matches the written length", label, need);
    back = rant_schema_compile(rant_allocator_alloc, ma, buf, NULL);
    ST_CHECK(back && rant_schema_hash(back) == rant_schema_hash(s),
             "schema-print: %s DSL recompiles to the same wire (hash)", label);
}

/* (17c) the schema DSL: text compiles to the same wire bytes (hence hash) the builder
   emits, layout comes out right, and malformed text fails with a useful position. */
static void schema_dsl_checks(void){
    RantAllocator ma = rant_allocator_heap(0);
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
    RantSchema *txt, *built;
    txt = rant_schema_compile(rant_allocator_alloc, &ma, POSE, NULL);
    ST_CHECK(txt != NULL, "schema-dsl: compiles");
    {   RantSchemaBuilder b = rant_schema_begin(rant_allocator_alloc, &ma, "Pose");
        rant_schema_field(&b, "stamp", RANT_U64);
        rant_schema_field(&b, "x", RANT_F64);
        rant_schema_field(&b, "y", RANT_F64);
        rant_schema_field_array(&b, "uuid", RANT_U8, 16);
        rant_schema_field(&b, "tagCount", RANT_U8);
        rant_schema_begin_struct(&b, "velocity");
        rant_schema_field(&b, "dx", RANT_F32);
        rant_schema_field(&b, "dy", RANT_F32);
        rant_schema_end_struct(&b);
        built = rant_schema_finish(&b);
    }
    ST_CHECK(built != NULL, "schema-dsl: builder twin builds");
    ST_CHECK(txt && built && rant_schema_hash(txt) == rant_schema_hash(built),
             "schema-dsl: text and builder produce the same wire (same hash)");
    schema_print_roundtrip(&ma, txt, "Pose (nested struct + array)");
    if (txt){
        RantSchemaFieldInfo fi;
        ST_CHECK(rant_schema_size(txt) == 8+8+8+16+1+8, "schema-dsl: size %u", rant_schema_size(txt));
        ST_CHECK(rant_schema_field_count(txt) == 8, "schema-dsl: 8 flat fields (6 top + 2 nested)");
        ST_CHECK(rant_schema_field_index(txt, "tagCount") == 4
              && rant_schema_field_index(txt, "velocity.dx") == 6
              && rant_schema_field_index(txt, "dx") == -1,      /* nested needs its path */
                 "schema-dsl: index by name incl. dotted paths");
        ST_CHECK(rant_schema_field_at(txt, 3, &fi) && fi.kind == RANT_ARR
                 && fi.elem == RANT_U8 && fi.count == 16 && fi.offset == 24 && fi.size == 16,
                 "schema-dsl: array field info (off=%u size=%u count=%u)", fi.offset, fi.size, fi.count);
        ST_CHECK(rant_schema_field_at(txt, 5, &fi) && fi.kind == RANT_STRUCT && fi.size == 8
                 && fi.offset == 41 && fi.depth == 0,
                 "schema-dsl: struct field info (off=%u size=%u)", fi.offset, fi.size);
        ST_CHECK(rant_schema_field_at(txt, 7, &fi) && fi.kind == RANT_F32
                 && fi.offset == 45 && fi.depth == 1,
                 "schema-dsl: nested member is flattened (off=%u depth=%u)", fi.offset, fi.depth);
    }
    if (txt){   /* setters: build a message BY NAME, read it back through the getters */
        uint8_t m[49], uuid[16]; int i, ok;
        memset(m, 0xAA, sizeof m);   /* dirty: the set fields must fully determine it */
        for (i = 0; i < 16; i++) uuid[i] = (uint8_t)i;
        ok  = rant_set_uint (m, sizeof m, txt, "stamp", 42);
        ok &= rant_set_f64    (m, sizeof m, txt, "x", 1.5);
        ok &= rant_set_f64    (m, sizeof m, txt, "y", -2.5);
        ok &= rant_set_array(m, sizeof m, txt, "uuid", rant_bytes(uuid, 5));     /* short write */
        ok &= rant_set_uint (m, sizeof m, txt, "tagCount", 300);   /* narrows like a cast */
        ok &= rant_set_f32    (m, sizeof m, txt, "velocity.dy", 7.5f);           /* nested by path */
        ST_CHECK(ok, "schema-dsl: setters accept (incl. nested path)");
        ST_CHECK(rant_get_uint(rant_bytes(m,sizeof m), txt, "stamp") == 42
              && rant_get_f64 (rant_bytes(m,sizeof m), txt, "y") == -2.5
              && rant_get_uint(rant_bytes(m,sizeof m), txt, "tagCount") == (300u & 0xFF)
              && rant_get_f32 (rant_bytes(m,sizeof m), txt, "velocity.dy") == 7.5f,
                 "schema-dsl: getters read the setters back (incl. nested path)");
        {   RantBytes a = rant_get_array(rant_bytes(m,sizeof m), txt, "uuid");
            ST_CHECK(a.len == 16 && a.data[4] == 4 && a.data[5] == 0 && a.data[15] == 0,
                     "schema-dsl: short array write zero-fills the tail");
        }
        {   RantValue v;                              /* reflection access by flat index */
            ST_CHECK(rant_get_value(rant_bytes(m,sizeof m), txt, 7, &v)
                     && v.kind == RANT_F32 && v.v.f == 7.5,
                     "schema-dsl: rant_get_value reads the nested member");
            v.v.u = 9;
            ST_CHECK(rant_set_value(m, sizeof m, txt, 4, &v)
                     && rant_get_uint(rant_bytes(m,sizeof m), txt, "tagCount") == 9,
                     "schema-dsl: rant_set_value writes by index");
        }
        ST_CHECK(!rant_set_uint (m, sizeof m, txt, "x", 1)   /* f64: wrong family */
              && !rant_set_f64    (m, sizeof m, txt, "velocity", 0.0)   /* struct: no setter */
              && !rant_set_array(m, sizeof m, txt, "uuid", rant_bytes(uuid, 17))     /* overflow */
              && !rant_set_uint (m, 8, txt, "x", 1)                               /* short buffer */
              && !rant_set_uint (m, sizeof m, txt, "nope", 1),   /* unknown field */
                 "schema-dsl: bad sets refused");
    }
    {   /* strings: string<cap> and string<cap>[N] are fixed slots of [u16 len][cap bytes] */
        static const char TAGGED[] =
            "Tagged { id: u32, name: string<12>, labels: string<8>[3], meta: { note: string<4> } }";
        RantSchema *ts, *twin;
        ts = rant_schema_compile(rant_allocator_alloc, &ma, TAGGED, NULL);
        ST_CHECK(ts != NULL, "schema-dsl: strings compile");
        {   RantSchemaBuilder b = rant_schema_begin(rant_allocator_alloc, &ma, "Tagged");
            rant_schema_field(&b, "id", RANT_U32);
            rant_schema_field_string(&b, "name", 12);
            rant_schema_field_string_array(&b, "labels", 8, 3);
            rant_schema_begin_struct(&b, "meta");
            rant_schema_field_string(&b, "note", 4);
            rant_schema_end_struct(&b);
            twin = rant_schema_finish(&b);
        }
        ST_CHECK(twin && ts && rant_schema_hash(ts) == rant_schema_hash(twin),
                 "schema-dsl: string text and builder produce the same wire (same hash)");
        schema_print_roundtrip(&ma, ts, "Tagged (capped strings + string array)");
        if (ts){
            RantSchemaFieldInfo fi;
            ST_CHECK(rant_schema_size(ts) == 4 + (2+12) + 3*(2+8) + (2+4),
                     "schema-dsl: string sizes (%u)", rant_schema_size(ts));
            ST_CHECK(rant_schema_field_at(ts, 1, &fi) && fi.kind == RANT_STR
                     && fi.str_cap == 12 && fi.offset == 4 && fi.size == 14,
                     "schema-dsl: string field info (off=%u size=%u cap=%u)",
                     fi.offset, fi.size, fi.str_cap);
            ST_CHECK(rant_schema_field_at(ts, 2, &fi) && fi.kind == RANT_ARR && fi.elem == RANT_STR
                     && fi.count == 3 && fi.str_cap == 8 && fi.size == 30,
                     "schema-dsl: string array field info (size=%u cap=%u)", fi.size, fi.str_cap);
        }
        if (ts){
            uint8_t m[54]; RantString v; int ok;
            ok  = rant_schema_message_default(ts, m, sizeof m);
            ok &= rant_set_uint    (m, sizeof m, ts, "id", 7);
            ok &= rant_set_string(m, sizeof m, ts, "name", rant_string("robot-1", 7));
            ok &= rant_set_string_at(m, sizeof m, ts, "labels", 0, rant_string("fast", 4));
            ok &= rant_set_string_at(m, sizeof m, ts, "labels", 2, rant_string("red", 3));
            ok &= rant_set_string(m, sizeof m, ts, "meta.note", rant_string("ok", 2));
            ST_CHECK(ok, "schema-dsl: string setters accept (incl. nested + indexed)");
            v = rant_get_string(rant_bytes(m,sizeof m), ts, "name");
            ST_CHECK(v.len == 7 && memcmp(v.data, "robot-1", 7) == 0,
                     "schema-dsl: string round-trips");
            v = rant_get_string_at(rant_bytes(m,sizeof m), ts, "labels", 2);
            ST_CHECK(v.len == 3 && memcmp(v.data, "red", 3) == 0,
                     "schema-dsl: string array element round-trips");
            v = rant_get_string_at(rant_bytes(m,sizeof m), ts, "labels", 1);
            ST_CHECK(v.len == 0 && v.data != NULL, "schema-dsl: unset string element is empty");
            v = rant_get_string(rant_bytes(m,sizeof m), ts, "meta.note");
            ST_CHECK(v.len == 2 && memcmp(v.data, "ok", 2) == 0,
                     "schema-dsl: nested string round-trips");
            {   RantValue dv;     /* reflection sees the live bytes + the cap */
                ST_CHECK(rant_get_value(rant_bytes(m,sizeof m), ts, 1, &dv)
                         && dv.kind == RANT_STR && dv.str_cap == 12
                         && dv.bytes.len == 7 && memcmp(dv.bytes.data, "robot-1", 7) == 0,
                         "schema-dsl: rant_get_value yields the live string");
            }
            ST_CHECK(!rant_set_string(m, sizeof m, ts, "name", rant_string("a-name-too-long", 15))
                  && !rant_set_string(m, sizeof m, ts, "id", rant_string("x", 1))   /* not string */
                  && !rant_set_string_at(m, sizeof m, ts, "labels", 3, rant_string("x", 1)),
                     "schema-dsl: bad string sets refused");
            {   /* a hostile length prefix reads back clamped to the cap */
                RantSchemaFieldInfo fi;
                rant_schema_field_at(ts, 1, &fi);
                m[fi.offset] = 0xFF; m[fi.offset + 1] = 0xFF;          /* len = 65535 */
                v = rant_get_string(rant_bytes(m,sizeof m), ts, "name");
                ST_CHECK(v.len == 12, "schema-dsl: hostile string length clamps to cap (%u)",
                         (unsigned)v.len);
            }
        }
        {   RantSchema *r_ok    = rant_schema_compile(rant_allocator_alloc, &ma, "Tagged { name: string<12> }", NULL);
            RantSchema *r_bad = rant_schema_compile(rant_allocator_alloc, &ma, "Tagged { name: string<10> }", NULL);
            ST_CHECK(r_ok && r_bad && ts && rant_schema_subset(r_ok, ts) && !rant_schema_subset(r_bad, ts),
                     "schema-dsl: string subset needs the same cap");
        }
    }
    {   /* variable fields ride the tail as [u32 len] frames in schema order, fixed offsets
           are unaffected */
        static const char VDSL[] =
            "Var { id: u32, note: string, samples: f32[], labels: string<6>[], extras: map, tail: u8 }";
        RantSchema *vs, *twin;
        vs = rant_schema_compile(rant_allocator_alloc, &ma, VDSL, NULL);
        ST_CHECK(vs != NULL, "schema-var: compiles");
        {   RantSchemaBuilder b = rant_schema_begin(rant_allocator_alloc, &ma, "Var");
            rant_schema_field(&b, "id", RANT_U32);
            rant_schema_field_var_string(&b, "note");
            rant_schema_field_var_array(&b, "samples", RANT_F32);
            rant_schema_field_var_string_array(&b, "labels", 6);
            rant_schema_field_map(&b, "extras");
            rant_schema_field(&b, "tail", RANT_U8);
            twin = rant_schema_finish(&b);
        }
        ST_CHECK(twin && vs && rant_schema_hash(vs) == rant_schema_hash(twin),
                 "schema-var: text and builder produce the same wire (same hash)");
        schema_print_roundtrip(&ma, vs, "Var (variable string/array/map)");
        if (vs){
            RantSchemaFieldInfo fi;
            ST_CHECK(rant_schema_size(vs) == 5 && rant_schema_msg_min(vs) == 5 + 4*4,
                     "schema-var: fixed size %u, msg_min %u",
                     rant_schema_size(vs), rant_schema_msg_min(vs));
            ST_CHECK(rant_schema_field_at(vs, 5, &fi) && fi.kind == RANT_U8 && fi.offset == 4,
                     "schema-var: fixed fields pack around the variable ones (off=%u)", fi.offset);
            ST_CHECK(rant_schema_field_at(vs, 1, &fi) && fi.kind == RANT_VSTR
                     && fi.offset == 0 && fi.size == 0,
                     "schema-var: variable field reports no static offset");
            ST_CHECK(rant_schema_field_at(vs, 3, &fi) && fi.kind == RANT_VARR
                     && fi.elem == RANT_STR && fi.str_cap == 6 && fi.count == 0,
                     "schema-var: string<6>[] field info (cap=%u)", fi.str_cap);
        }
        if (vs){
            uint8_t m[256], smp[8], slots[16], tmp[128], big[250];
            uint32_t blen, len; int ok; RantString v; RantBytes a, mb;
            {   int i; for (i = 0; i < 8; i++) smp[i] = (uint8_t)(i + 1); }
            memset(slots, 0, sizeof slots);              /* two empty string<6> slots */
            memset(big, 'x', sizeof big);

            ok = rant_schema_message_default(vs, m, sizeof m);
            ST_CHECK(ok && rant_schema_msg_len(vs, m, sizeof m) == rant_schema_msg_min(vs)
                     && rant_schema_validate(vs, rant_bytes(m, rant_schema_msg_min(vs))),
                     "schema-var: the default message is all empty frames and validates");
            ok  = rant_set_uint(m, sizeof m, vs, "id", 7);
            ok &= rant_set_uint(m, sizeof m, vs, "tail", 9);
            ok &= rant_set_array(m, sizeof m, vs, "samples", rant_bytes(smp, 8));    /* 2 live f32 */
            ok &= rant_set_string(m, sizeof m, vs, "note", rant_string("hello, tail", 11));
            ok &= rant_set_array(m, sizeof m, vs, "labels", rant_bytes(slots, 16));
            ok &= rant_set_string_at(m, sizeof m, vs, "labels", 1, rant_string("red", 3));
            ST_CHECK(ok, "schema-var: variable setters accept (out of schema order)");
            ST_CHECK(rant_get_uint(rant_bytes(m,sizeof m), vs, "id") == 7
                  && rant_get_uint(rant_bytes(m,sizeof m), vs, "tail") == 9,
                     "schema-var: fixed fields intact after tail resizes");
            v = rant_get_string(rant_bytes(m,sizeof m), vs, "note");
            ST_CHECK(v.len == 11 && memcmp(v.data, "hello, tail", 11) == 0,
                     "schema-var: variable string round-trips");
            a = rant_get_array(rant_bytes(m,sizeof m), vs, "samples");
            ST_CHECK(a.len == 8 && memcmp(a.data, smp, 8) == 0,
                     "schema-var: variable array survives an earlier frame's resize");
            v = rant_get_string_at(rant_bytes(m,sizeof m), vs, "labels", 1);
            ST_CHECK(v.len == 3 && memcmp(v.data, "red", 3) == 0,
                     "schema-var: variable string-array element round-trips");
            ST_CHECK(!rant_set_string_at(m, sizeof m, vs, "labels", 2, rant_string("x", 1)),
                     "schema-var: index past the live count refused");
            {   RantValue dv;                        /* reflection sees the live extent */
                ST_CHECK(rant_get_value(rant_bytes(m,sizeof m), vs, 2, &dv)
                         && dv.kind == RANT_VARR && dv.elem == RANT_F32
                         && dv.count == 2 && dv.bytes.len == 8,
                         "schema-var: rant_get_value yields the live element count");
            }

            {   RantMapWriter w = rant_map_begin(tmp, sizeof tmp);       /* the escape hatch */
                rant_map_put_uint(&w, "battery", 87);
                rant_map_put_string(&w, "state", rant_string("docked", 6));
                rant_map_open_array(&w, "temps");
                rant_map_put_f64(&w, NULL, 36.5);
                rant_map_put_int(&w, NULL, -3);
                rant_map_close(&w);
                rant_map_open_map(&w, "pose");
                rant_map_put_f64(&w, "x", 1.5);
                rant_map_close(&w);
                blen = rant_map_finish(&w);
            }
            ST_CHECK(blen > 0 && rant_set_map(m, sizeof m, vs, "extras", rant_bytes(tmp, blen)),
                     "schema-var: map writes and installs");
            mb = rant_get_map(rant_bytes(m,sizeof m), vs, "extras");
            ST_CHECK(mb.data && mb.len == blen && rant_map_count(mb) == 4,
                     "schema-var: map body round-trips (%u entries)", rant_map_count(mb));
            {   RantValue dv, e0, e1;
                ST_CHECK(rant_map_get(mb, "battery", &dv) && dv.kind == RANT_U8 && dv.v.u == 87,
                         "schema-var: map uint stores in the smallest kind");
                ST_CHECK(rant_map_get(mb, "state", &dv) && dv.kind == RANT_VSTR
                         && dv.bytes.len == 6 && memcmp(dv.bytes.data, "docked", 6) == 0,
                         "schema-var: map string");
                ST_CHECK(rant_map_get(mb, "temps", &dv) && dv.kind == RANT_VARR && dv.count == 2
                      && rant_map_array_at(dv.bytes, 0, &e0) && e0.kind == RANT_F64 && e0.v.f == 36.5
                      && rant_map_array_at(dv.bytes, 1, &e1) && e1.kind == RANT_I8 && e1.v.i == -3,
                         "schema-var: map array elements (heterogeneous)");
                ST_CHECK(rant_map_get(mb, "pose", &dv) && dv.kind == RANT_MAP
                      && rant_map_count(dv.bytes) == 1
                      && rant_map_get(dv.bytes, "x", &e0) && e0.v.f == 1.5,
                         "schema-var: nested map recurses");
                ST_CHECK(!rant_map_get(mb, "nope", &dv), "schema-var: missing key misses");
            }
            ST_CHECK(!rant_map_valid(rant_bytes(tmp, blen - 1))
                  && !rant_set_map(m, sizeof m, vs, "extras", rant_bytes(tmp, blen - 1)),
                     "schema-var: truncated map body refused");

            len = rant_schema_msg_len(vs, m, sizeof m);
            ST_CHECK(len == 5u + (4+11) + (4+8) + (4+16) + (4+blen),
                     "schema-var: live length (%u)", len);
            ST_CHECK(rant_schema_validate(vs, rant_bytes(m, len))
                  && !rant_schema_validate(vs, rant_bytes(m, len - 1))
                  && !rant_schema_validate(vs, rant_bytes(m, len + 1)),
                     "schema-var: frames must consume the message exactly");
            ST_CHECK(!rant_set_string(m, sizeof m, vs, "note", rant_string((char *)big, 250)),
                     "schema-var: a frame that would exceed the buffer is refused");
            v = rant_get_string(rant_bytes(m,sizeof m), vs, "note");
            ST_CHECK(v.len == 11, "schema-var: refused set leaves the message untouched");
            ok = rant_set_string(m, sizeof m, vs, "note", rant_string("hi", 2));
            a = rant_get_array(rant_bytes(m,sizeof m), vs, "samples");
            ST_CHECK(ok && a.len == 8 && memcmp(a.data, smp, 8) == 0
                     && rant_get_uint(rant_bytes(m,sizeof m), vs, "tail") == 9,
                     "schema-var: shrinking a frame memmoves the tail intact");

            {   /* subset + rebase: the reader skips frames it does not declare */
                RantSchema *sub = rant_schema_compile(rant_allocator_alloc, &ma,
                    "Var { tail: u8, samples: f32[], extras: map }", NULL);
                RantSchema *bad1 = rant_schema_compile(rant_allocator_alloc, &ma,
                    "Var { note: string<8> }", NULL);     /* capped vs variable */
                RantSchema *bad2 = rant_schema_compile(rant_allocator_alloc, &ma,
                    "Var { samples: f64[] }", NULL);      /* element kind differs */
                ST_CHECK(sub && rant_schema_subset(sub, vs), "schema-var: variable subset matches");
                ST_CHECK(bad1 && !rant_schema_subset(bad1, vs)
                      && bad2 && !rant_schema_subset(bad2, vs),
                         "schema-var: capped-vs-variable and element mismatches refused");
                {   RantSchema *rb = rant_schema_rebase(sub, vs, rant_allocator_alloc, &ma);
                    ST_CHECK(rb != NULL, "schema-var: rebase");
                    if (rb){
                        uint32_t rlen = rant_schema_msg_len(rb, m, sizeof m);
                        ST_CHECK(rlen == rant_schema_msg_len(vs, m, sizeof m)
                              && rant_schema_validate(rb, rant_bytes(m, rlen)),
                                 "schema-var: rebased schema walks the writer's frames");
                        ST_CHECK(rant_get_uint(rant_bytes(m, rlen), rb, "tail") == 9,
                                 "schema-var: rebased fixed offset");
                        a = rant_get_array(rant_bytes(m, rlen), rb, "samples");
                        ST_CHECK(a.len == 8 && memcmp(a.data, smp, 8) == 0,
                                 "schema-var: rebased variable ordinal finds the right frame");
                        mb = rant_get_map(rant_bytes(m, rlen), rb, "extras");
                        ST_CHECK(mb.data && rant_map_count(mb) == 4,
                                 "schema-var: rebased map field reads");
                    }
                }
            }
        }
    }
    {   /* schema-print: two struct levels deep, and a struct followed by more top-level fields */
        RantSchema *deep = rant_schema_compile(rant_allocator_alloc, &ma,
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
            "Pose { x: string[] }",   /* ragged: an array of unbounded strings is a map's job */
            "Pose { x: string[4] }",        /* a fixed string array needs its <cap> */
            "Pose { x: string<0> }",        /* zero cap */
            "Pose { x: string<12 }",        /* missing '>' */
            "Pose { v: { y: u8[] }[2] }",   /* variable field inside an array element */
            "Pose { m: map[3] }",           /* a map has no element form */
            "Pose { m: enum<u8> { A=300 } }",  /* value out of the backing range */
            "Pose { m: enum<f32> { A=0 } }",   /* non-integer backing */
            "Pose { m: enum<u8> A=0 }",        /* missing '{' */
            "Pose { m: enum { A } }"           /* missing <backing> */
        };
        unsigned i, ok = 1;
        for (i = 0; i < sizeof bad / sizeof bad[0]; i++){
            const char *ep = NULL;
            RantSchema *s = rant_schema_compile(rant_allocator_alloc, &ma, bad[i], &ep);
            if (s || !ep || ep < bad[i] || ep > bad[i] + strlen(bad[i])) ok = 0;
        }
        ST_CHECK(ok, "schema-dsl: malformed text rejected with a position");
    }
    {   /* schema enum: a named integer is a fixed field carrying its backing scalar. The name
           table is schema only, so an unknown value stays readable and subset compares the width. */
        static const char *EDSL =
            "Robot { id: u32,"
            " mode: enum<u8> { Idle=0, Running=1, Charging=2, Fault=3 },"
            " step: enum<i8> { Back=-1, Hold, Fwd } }";   /* auto: Hold=0, Fwd=1 */
        RantSchema *es = rant_schema_compile(rant_allocator_alloc, &ma, EDSL, NULL);
        ST_CHECK(es != NULL, "schema-enum: compiles");
        if (es){
            RantSchemaFieldInfo fi; int mi = rant_schema_field_index(es, "mode");
            uint8_t m[32]; RantString nm; int64_t vv; RantValue dv;
            ST_CHECK(rant_schema_field_at(es, (uint16_t)mi, &fi) && fi.kind == RANT_ENUM
                     && fi.elem == RANT_U8 && fi.count == 4 && fi.size == 1,
                     "schema-enum: reflects as ENUM (backing=elem, options=count, size=1)");
            ST_CHECK(rant_schema_size(es) == 4u + 1u + 1u, "schema-enum: fixed size %u", rant_schema_size(es));
            ST_CHECK(rant_schema_enum_count(es, (uint16_t)mi) == 4
                     && rant_schema_enum_variant(es, (uint16_t)mi, 2, &vv, &nm)
                     && vv == 2 && nm.len == 8 && memcmp(nm.data, "Charging", 8) == 0,
                     "schema-enum: variant listing");
            {   int si = rant_schema_field_index(es, "step");    /* signed + auto-increment */
                ST_CHECK(rant_schema_enum_variant(es, (uint16_t)si, 0, &vv, &nm) && vv == -1
                      && rant_schema_enum_variant(es, (uint16_t)si, 2, &vv, &nm) && vv == 1,
                         "schema-enum: signed backing + auto-increment"); }
            ST_CHECK(rant_enum_name_of(es, (uint16_t)mi, 3).len == 5
                     && rant_enum_name_of(es, (uint16_t)mi, 99).data == NULL
                     && rant_enum_value_of(es, (uint16_t)mi, "Running", &vv) && vv == 1
                     && !rant_enum_value_of(es, (uint16_t)mi, "Nope", &vv),
                     "schema-enum: name<->value resolvers");

            rant_schema_message_default(es, m, sizeof m);
            rant_set_uint(m, sizeof m, es, "mode", 1);                 /* by number */
            ST_CHECK(rant_set_enum(m, sizeof m, es, "step", "Fwd")     /* by name */
                     && !rant_set_enum(m, sizeof m, es, "step", "Bad"),
                     "schema-enum: set by name (unknown refused)");
            nm = rant_get_enum(rant_bytes(m, sizeof m), es, "mode");
            ST_CHECK(rant_get_uint(rant_bytes(m,sizeof m), es, "mode") == 1
                     && nm.len == 7 && memcmp(nm.data, "Running", 7) == 0,
                     "schema-enum: read number + label");
            ST_CHECK(rant_get_int(rant_bytes(m,sizeof m), es, "step") == 1,
                     "schema-enum: signed value reads back");
            rant_set_uint(m, sizeof m, es, "mode", 42);                /* unknown value */
            ST_CHECK(rant_get_uint(rant_bytes(m,sizeof m), es, "mode") == 42
                     && rant_get_enum(rant_bytes(m,sizeof m), es, "mode").data == NULL,
                     "schema-enum: an unknown/newer value stays readable, name empty");
            rant_set_uint(m, sizeof m, es, "mode", 2);
            ST_CHECK(rant_get_value(rant_bytes(m,sizeof m), es, (uint16_t)mi, &dv)
                     && dv.kind == RANT_ENUM && dv.elem == RANT_U8 && dv.count == 4 && dv.v.i == 2,
                     "schema-enum: rant_get_value carries the number + backing/options");

            schema_print_roundtrip(&ma, es, "Robot (enum fields)");   /* prints, recompiles */

            {   /* subset: the same width is compatible despite a different option table, a width
                   mismatch is refused */
                RantSchema *rd = rant_schema_compile(rant_allocator_alloc, &ma,
                    "Robot { mode: enum<u8> { Idle=0, Down=7 } }", NULL);   /* fewer options */
                RantSchema *bw = rant_schema_compile(rant_allocator_alloc, &ma,
                    "Robot { mode: enum<u16> { Idle=0 } }", NULL);            /* wrong width */
                ST_CHECK(rd && rant_schema_subset(rd, es), "schema-enum: same-width subset (names advisory)");
                ST_CHECK(bw && !rant_schema_subset(bw, es), "schema-enum: backing-width mismatch refused");
            }
        }
    }
    rant_allocator_reset(&ma);
}

/* (18) announce interest: one positional [u32 hash][u8 flags] entry per topic slot. An
   inactive topic is not yielded but holds its position (spec/testing.md). */
static void schema_advert_checks(void){
    RantAllocator pa = rant_allocator_heap(0);
    RantAllocator sa = rant_allocator_heap(0);
    RantAllocator ma = rant_allocator_heap(0);       /* caller side schema */
    RantNodeOpts po, so; RantNode *P=NULL, *S=NULL; RantTopic *pc;
    RantTopicOpts co; RantDiscoveryAddr seed; RantSchema *sch=NULL;
    uint32_t pose_h = (uint32_t)rant_topic_id("sch/pose");
    uint32_t raw_h  = (uint32_t)rant_topic_id("sch/raw");
    uint32_t late_h = (uint32_t)rant_topic_id("sch/late");
    int t, pose_ok=0, raw_ok=0;
    const RantDiscoveryPeer *peers; uint16_t n_peers=0;

    {   RantSchemaBuilder b = rant_schema_begin(rant_allocator_alloc, &ma, "Pose");
        rant_schema_field(&b, "x", RANT_F64);
        rant_schema_field(&b, "y", RANT_F64);
        rant_schema_field_array(&b, "tags", RANT_U8, 16);
        sch = rant_schema_finish(&b);
    }
    ST_CHECK(sch!=NULL, "announce: schema builds");
    if (!sch) return;

    memset(&co,0,sizeof co); co.qos.keep_last=4;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=ST_DOMAIN+8; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po;
    P = rant_node_open(&pa, "sch-pub", NULL, NULL, &po);
    S = rant_node_open(&sa, "sch-sub", NULL, NULL, &so);
    ST_CHECK(P&&S, "announce: nodes open");
    if (!(P&&S)){ if(P)rant_node_close(P,0); if(S)rant_node_close(S,0); rant_allocator_reset(&ma); return; }
    pc = rant_node_create_topic(P, "sch/pose", RANT_PUB_ONLY, sch, &co);     /* index 0, typed */
    rant_node_create_topic(P, "sch/raw",  RANT_PUB_ONLY, NULL, &co);         /* index 1, raw */
    rant_node_create_topic(S, "sch/pose", RANT_SUB_ONLY, sch, &co);
    ST_CHECK(pc!=NULL, "announce: topics created");
    rant_schema_free(sch, rant_allocator_alloc, &ma);       /* node owns its copy: caller's freed NOW */

    /* a typed-typed match through hash nomination + detail verification */
    for (t=0;t<800 && rant_topic_match_count(pc)==0;t++){ rant_node_poll(P,2); rant_node_poll(S,2); }
    ST_CHECK(rant_topic_match_count(pc)>0, "announce: typed match formed via detail exchange");

    peers = st_peers(S, &n_peers);   /* S's view of P */
    ST_CHECK(peers && n_peers==1, "announce: subscriber sees one peer (%u)", n_peers);
    if (peers && n_peers==1){
        RantInterestIter it; RantTopicEntry tp;
        memset(&it,0,sizeof it);
        while (i_rant_node_peer_interest_next(&peers[0], &it, &tp)){
            if (!tp.is_pub) continue;
            if (tp.index==0 && tp.hash==pose_h && tp.role==RANT_PUB_ONLY) pose_ok=1;
            if (tp.index==1 && tp.hash==raw_h  && tp.role==RANT_PUB_ONLY) raw_ok=1;
        }
        ST_CHECK(pose_ok && raw_ok, "announce: positional indices carry the 32-bit hashes");

        /* the runtime flip flow: an inactive topic holds its position but is not yielded,
           flipping it to publish advertises the same index */
        {   RantTopic *lc = rant_node_create_topic(P, "sch/late", RANT_INACTIVE, NULL, &co);
            ST_CHECK(lc!=NULL, "announce: inactive topic created");
            for (t=0;t<200;t++){ rant_node_poll(P,2); rant_node_poll(S,2); }
            {   RantInterestIter it2; RantTopicEntry tp2; int seen=0;
                peers = st_peers(S, &n_peers);
                memset(&it2,0,sizeof it2);
                while (i_rant_node_peer_interest_next(&peers[0], &it2, &tp2))
                    if (tp2.hash==late_h) seen=1;
                ST_CHECK(!seen, "announce: inactive topic not advertised");
            }
            rant_topic_set_role(lc, RANT_PUB_ONLY);
            for (t=0;t<400;t++){ rant_node_poll(P,2); rant_node_poll(S,2); }
            {   RantInterestIter it2; RantTopicEntry tp2; uint16_t late_alias=0xFFFF;
                peers = st_peers(S, &n_peers);
                memset(&it2,0,sizeof it2);
                while (i_rant_node_peer_interest_next(&peers[0], &it2, &tp2))
                    if (tp2.is_pub && tp2.hash==late_h) late_alias=tp2.index;
                ST_CHECK(late_alias==2, "announce: role flip advertises the held position (index %u)",
                         late_alias);
            }
        }
    }
    rant_node_close(P,1); rant_node_close(S,1);
    rant_allocator_reset(&ma);
}

/* (19) reader side subset binding: a subset subscriber matches with a rebased schema, an
   incompatible one is refused on both sides, a wrong size message is dropped. */
static int      sb_recv, sb_schema_ok;
static uint64_t sb_stamp; static double sb_y;
static unsigned long sb_mismatch_n;
static void sb_on_message(const RantMsg *msg){
    sb_recv++;
    if (!msg->schema) return;
    sb_schema_ok = (rant_schema_size(msg->schema) == 25 && rant_schema_field_count(msg->schema) == 2);
    sb_y     = rant_get_f64 (msg->data, msg->schema, "y");
    sb_stamp = rant_get_uint(msg->data, msg->schema, "stamp");
}
static void sb_on_event(const RantEvent *ev){
    if (ev->kind == RANT_ERROR && ev->error == RANT_E_SCHEMA_MISMATCH) sb_mismatch_n++;
}
static void schema_bind_checks(void){
    RantAllocator pa = rant_allocator_heap(0);
    RantAllocator sa = rant_allocator_heap(0);
    RantAllocator ma = rant_allocator_heap(0);
    RantNodeOpts po, so; RantNode *P=NULL, *S=NULL;
    RantTopic *pc, *pc_bad; RantTopicOpts co; RantDiscoveryAddr seed;
    RantSchema *W, *R, *WB, *RB; int t;
    /* writer: the full Pose. reader: a reordered subset of it */
    W  = rant_schema_compile(rant_allocator_alloc, &ma,
             "Pose { stamp: u64, x: f64, y: f64, tag: u8 }", NULL);      /* 25 B */
    R  = rant_schema_compile(rant_allocator_alloc, &ma,
             "Pose { y: f64, stamp: u64 }", NULL);
    WB = rant_schema_compile(rant_allocator_alloc, &ma, "Bad { v: u64 }", NULL);
    RB = rant_schema_compile(rant_allocator_alloc, &ma, "Bad { v: f64 }", NULL);
    ST_CHECK(W && R && WB && RB, "schema-bind: schemas compile");
    ST_CHECK(W && R && rant_schema_subset(R, W) && !rant_schema_subset(W, R),
             "schema-bind: subset is one-way (reader within writer)");
    if (!(W && R && WB && RB)){ rant_allocator_reset(&ma); return; }

    memset(&co,0,sizeof co); co.qos.keep_last=4;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=ST_DOMAIN+9; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po;
    sb_recv=0; sb_schema_ok=0; sb_stamp=0; sb_y=0.0; sb_mismatch_n=0;
    P = rant_node_open(&pa, "sb-pub", NULL,          sb_on_event, &po);
    S = rant_node_open(&sa, "sb-sub", sb_on_message, sb_on_event, &so);
    ST_CHECK(P&&S, "schema-bind: nodes open");
    if (!(P&&S)){ if(P)rant_node_close(P,0); if(S)rant_node_close(S,0); rant_allocator_reset(&ma); return; }
    pc     = rant_node_create_topic(P, "sb/pose", RANT_PUB_ONLY, W,    &co);
    pc_bad = rant_node_create_topic(P, "sb/bad",  RANT_PUB_ONLY, WB, &co);
    rant_node_create_topic(S, "sb/pose", RANT_SUB_ONLY, R,    &co);
    rant_node_create_topic(S, "sb/bad",  RANT_SUB_ONLY, RB, &co);
    ST_CHECK(pc && pc_bad, "schema-bind: topics created");

    for (t=0;t<800 && rant_topic_match_count(pc)==0;t++){ rant_node_poll(P,2); rant_node_poll(S,2); }
    ST_CHECK(rant_topic_match_count(pc)==1, "schema-bind: subset reader matched");
    ST_CHECK(rant_topic_match_count(pc_bad)==0, "schema-bind: kind-conflict reader refused");
    ST_CHECK(sb_mismatch_n>=1, "schema-bind: refusal surfaced (%lu RANT_E_SCHEMA_MISMATCH)", sb_mismatch_n);

    {   /* publish one Pose packed in the writer's layout. The reader decodes through the
           rebased schema with its own indices */
        uint8_t buf[25]; uint64_t bits; double x=1.5, y=-2.25;
        i_rant_le_w64(buf, 0x1122334455667788ULL);                /* stamp @0 */
        memcpy(&bits,&x,8); i_rant_le_w64(buf+8,    bits);        /* x     @8 */
        memcpy(&bits,&y,8); i_rant_le_w64(buf+16, bits);          /* y     @16 */
        buf[24]=7;                                                /* tag   @24 */
        rant_topic_send(pc, rant_bytes(buf, sizeof buf), NULL);
        for (t=0;t<400 && sb_recv==0;t++){ rant_node_poll(P,1); rant_node_poll(S,2); }
        ST_CHECK(sb_recv==1, "schema-bind: subset message delivered");
        ST_CHECK(sb_schema_ok, "schema-bind: RantMsg.schema is the rebased view (writer size, reader fields)");
        ST_CHECK(sb_y==-2.25 && sb_stamp==0x1122334455667788ULL,
                 "schema-bind: reader indices read the writer's offsets (y=%.2f)", sb_y);
    }
    {   /* a message that does not fit the sender's schema is refused at the sender */
        unsigned long before = sb_mismatch_n;
        uint8_t junk[3] = {1,2,3};
        int rc = rant_topic_send(pc, rant_bytes(junk, sizeof junk), NULL);
        for (t=0;t<50;t++){ rant_node_poll(P,1); rant_node_poll(S,2); }
        ST_CHECK(rc==RANT_ERR_SCHEMA && sb_recv==1 && sb_mismatch_n==before,
                 "schema-bind: wrong-size message refused at the sender (rc=%d recv=%d)", rc, sb_recv);
    }
    rant_node_close(P,1); rant_node_close(S,1);
    rant_allocator_reset(&ma);
}

/* (19a2) a large procedurally built enum, 1024 options with a u16 backing. Both nodes
   define the identical list, so the hashes match and no schema wire is sent. */
#define BE_N 1024
static int be_recv; static int64_t be_val; static char be_label[32];
static void be_on_message(const RantMsg *msg){
    RantString nm; size_t k;
    if (!msg->schema) return;
    be_recv++;
    be_val = rant_get_int(msg->data, msg->schema, "job");
    nm = rant_get_enum(msg->data, msg->schema, "job");
    k = nm.len < sizeof be_label - 1 ? nm.len : sizeof be_label - 1;
    if (nm.data) memcpy(be_label, nm.data, k);
    be_label[k] = '\0';
}
static RantSchema *be_build_schema(RantAllocator *a){
    static char be_names[BE_N][16];
    static RantEnumVariant be_vs[BE_N];
    RantSchemaBuilder b; int i;
    for (i=0;i<BE_N;i++){ sprintf(be_names[i], "job_%04d", i);
                          be_vs[i].value=i; be_vs[i].name=be_names[i]; }
    b = rant_schema_begin(rant_allocator_alloc, a, "Jobs");
    rant_schema_field(&b, "id", RANT_U32);
    rant_schema_field_enum(&b, "job", RANT_U16, be_vs, BE_N);
    return rant_schema_finish(&b);
}
static void schema_bigenum_checks(void){
    RantAllocator pa = rant_allocator_heap(0);
    RantAllocator sa = rant_allocator_heap(0);
    RantAllocator ma = rant_allocator_heap(0);
    RantNodeOpts po, so; RantNode *P=NULL, *S=NULL; RantTopic *pc; RantTopicOpts co;
    RantDiscoveryAddr seed; RantSchema *es; int t, ji;

    es = be_build_schema(&ma);
    ST_CHECK(es!=NULL, "bigenum: 1024-option schema compiles");
    if (!es){ rant_allocator_reset(&ma); return; }
    ji = rant_schema_field_index(es, "job");

    {   RantString nm; int64_t vv; RantSchema *back; RantBytes wire;
        ST_CHECK(rant_schema_enum_count(es,(uint16_t)ji)==BE_N,
                 "bigenum: option count carries past 255 (%u)",
                 rant_schema_enum_count(es,(uint16_t)ji));
        ST_CHECK(rant_schema_enum_variant(es,(uint16_t)ji,1000,&vv,&nm)
                 && vv==1000 && nm.len==8 && memcmp(nm.data,"job_1000",8)==0,
                 "bigenum: high-index variant reflects");
        ST_CHECK(rant_enum_name_of(es,(uint16_t)ji,1023).len==8
                 && rant_enum_value_of(es,(uint16_t)ji,"job_0500",&vv) && vv==500,
                 "bigenum: name<->value at the tail");
        wire = rant_schema_wire(es);
        ST_CHECK(wire.len > RANT_DGRAM_MAX,
                 "bigenum: schema wire exceeds one datagram (%u bytes)", (unsigned)wire.len);
        back = rant_schema_parse(wire.data, wire.len, rant_allocator_alloc, &ma);
        ST_CHECK(back && rant_schema_hash(back)==rant_schema_hash(es)
                 && rant_schema_enum_count(back,(uint16_t)ji)==BE_N,
                 "bigenum: wire round-trips (parse preserves hash + count)");
        if (back) rant_schema_free(back, rant_allocator_alloc, &ma);
    }

    memset(&co,0,sizeof co); co.qos.keep_last=4;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=ST_DOMAIN+10; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po;
    be_recv=0; be_val=-1; be_label[0]='\0';
    P = rant_node_open(&pa, "be-pub", NULL, NULL, &po);
    S = rant_node_open(&sa, "be-sub", be_on_message, NULL, &so);
    ST_CHECK(P&&S, "bigenum: nodes open");
    if (!(P&&S)){ if(P)rant_node_close(P,0); if(S)rant_node_close(S,0);
                  rant_schema_free(es,rant_allocator_alloc,&ma); rant_allocator_reset(&ma); return; }
    pc = rant_node_create_topic(P, "be/jobs", RANT_PUB_ONLY, es, &co);
    rant_node_create_topic(S, "be/jobs", RANT_SUB_ONLY, es, &co);
    ST_CHECK(pc!=NULL, "bigenum: topic created");

    for (t=0;t<800 && rant_topic_match_count(pc)==0;t++){ rant_node_poll(P,2); rant_node_poll(S,2); }
    ST_CHECK(rant_topic_match_count(pc)>0, "bigenum: typed match reached the peer");

    {   uint8_t buf[16];
        rant_schema_message_default(es, buf, sizeof buf);
        rant_set_uint(buf, sizeof buf, es, "id", 42);
        ST_CHECK(rant_set_enum(buf, sizeof buf, es, "job", "job_1000"),
                 "bigenum: set the value by its human name");
        rant_topic_send(pc, rant_bytes(buf, rant_schema_msg_len(es, buf, sizeof buf)), NULL);
        for (t=0;t<400 && be_recv==0;t++){ rant_node_poll(P,1); rant_node_poll(S,2); }
        ST_CHECK(be_recv==1 && be_val==1000 && strcmp(be_label,"job_1000")==0,
                 "bigenum: value delivered + resolved to its name (val=%lld, label=%s)",
                 (long long)be_val, be_label);
    }
    rant_node_close(P,1); rant_node_close(S,1);
    rant_schema_free(es, rant_allocator_alloc, &ma);
    rant_allocator_reset(&ma);
}

/* (19a3) primitive rooted schemas: one bare type, anonymous, addressed by the empty path,
   the same wire in any language (spec/testing.md). Every root kind is covered. */
static const uint64_t SR_HASH_BOOL  = 0xee90234f61d2520bULL;   /* `bool`  canonical wire hash */
static const uint64_t SR_HASH_F32ARR = 0x314844e3386a1fc4ULL;  /* `f32[]` canonical wire hash */

/* one bare-type spelling: compiles, prints back to the identical text, recompiles to the
   same wire, and reflects as a single anonymous field with the expected layout */
static void sr_root_case(RantAllocator *ma, const char *text, uint8_t kind,
                         uint32_t size, uint32_t msg_min){
    RantSchema *s = rant_schema_compile(rant_allocator_alloc, ma, text, NULL);
    RantSchema *back = NULL; RantSchemaFieldInfo fi; char buf[128];
    ST_CHECK(s != NULL, "schema-root: '%s' compiles", text);
    if (!s) return;
    rant_schema_print(s, buf, sizeof buf);
    ST_CHECK(strncmp(buf, text, strlen(text)) == 0 && strcmp(buf + strlen(text), "\n") == 0,
             "schema-root: '%s' prints back bare (got '%s')", text, buf);
    ST_CHECK(rant_schema_print(s, NULL, 0) == (uint32_t)strlen(buf),
             "schema-root: '%s' print measures with (NULL,0)", text);
    back = rant_schema_compile(rant_allocator_alloc, ma, buf, NULL);
    ST_CHECK(back && rant_schema_hash(back) == rant_schema_hash(s),
             "schema-root: '%s' print recompiles to the same wire (hash)", text);
    ST_CHECK(rant_schema_field_count(s) == 1 && rant_schema_field_at(s, 0, &fi)
             && fi.kind == kind && fi.name.len == 0 && fi.name.data != NULL
             && fi.depth == 0 && fi.offset == 0,
             "schema-root: '%s' is one anonymous field (kind=%u)", text, fi.kind);
    ST_CHECK(rant_schema_name(s).len == 0 && rant_schema_field_index(s, "") == 0,
             "schema-root: '%s' root is unnamed, the empty path resolves it", text);
    ST_CHECK(rant_schema_size(s) == size && rant_schema_msg_min(s) == msg_min,
             "schema-root: '%s' layout (size=%u min=%u)", text,
             rant_schema_size(s), rant_schema_msg_min(s));
    if (back) rant_schema_free(back, rant_allocator_alloc, ma);
    rant_schema_free(s, rant_allocator_alloc, ma);
}

static int sr_flag_recv, sr_flag_val, sr_note_recv, sr_samples_recv, sr_map_recv, sr_enum_recv;
static int sr_raw_recv, sr_raw_ok;
static unsigned long sr_mismatch_n;
static char sr_note[32], sr_mode[16];
static float sr_s0, sr_s2; static size_t sr_ns;
static uint64_t sr_battery;
/* one handler for every root kind: the delivered schema's single field says which */
static void sr_on_message(const RantMsg *msg){
    RantSchemaFieldInfo fi;
    if (!msg->schema || !rant_schema_field_at(msg->schema, 0, &fi)) return;
    switch (fi.kind){
        case RANT_BOOL:
            sr_flag_recv++; sr_flag_val = (int)rant_get_uint(msg->data, msg->schema, "");
            break;
        case RANT_VSTR: {
            RantString v = rant_get_string(msg->data, msg->schema, "");
            size_t k = v.len < sizeof sr_note - 1 ? v.len : sizeof sr_note - 1;
            if (v.data) memcpy(sr_note, v.data, k);
            sr_note[k] = '\0'; sr_note_recv++;
            break;
        }
        case RANT_VARR: {
            RantBytes a = rant_get_array(msg->data, msg->schema, "");
            sr_ns = a.len / sizeof(float);
            if (sr_ns >= 3){ memcpy(&sr_s0, a.data, 4); memcpy(&sr_s2, a.data + 8, 4); }
            sr_samples_recv++;
            break;
        }
        case RANT_MAP: {
            RantValue v;
            if (rant_map_get(rant_get_map(msg->data, msg->schema, ""), "battery", &v))
                sr_battery = v.v.u;
            sr_map_recv++;
            break;
        }
        case RANT_ENUM: {
            RantString nm = rant_get_enum(msg->data, msg->schema, "");
            size_t k = nm.len < sizeof sr_mode - 1 ? nm.len : sizeof sr_mode - 1;
            if (nm.data) memcpy(sr_mode, nm.data, k);
            sr_mode[k] = '\0'; sr_enum_recv++;
            break;
        }
        default: break;
    }
}
/* the schema-less reader: everything it knows comes from the sender's schema */
static void sr_on_raw(const RantMsg *msg){
    RantSchemaFieldInfo fi;
    sr_raw_recv++;
    sr_raw_ok = msg->schema && rant_schema_field_count(msg->schema) == 1
             && rant_schema_name(msg->schema).len == 0
             && rant_schema_field_at(msg->schema, 0, &fi) && fi.kind == RANT_BOOL
             && fi.name.len == 0
             && rant_get_uint(msg->data, msg->schema, "") == 1;
}
static void sr_on_event(const RantEvent *ev){
    if (ev->kind == RANT_ERROR && ev->error == RANT_E_SCHEMA_MISMATCH) sr_mismatch_n++;
}
static void schema_root_checks(void){
    RantAllocator pa = rant_allocator_heap(0);
    RantAllocator sa = rant_allocator_heap(0);
    RantAllocator qa = rant_allocator_heap(0);
    RantAllocator ma = rant_allocator_heap(0);
    RantNodeOpts po, so, qo; RantNode *P=NULL, *S=NULL, *Q=NULL;
    RantTopicOpts co; RantDiscoveryAddr seed;
    RantSchema *sb, *su8, *sstr, *sarr, *smap, *senum, *swrap;
    RantTopic *pflag, *pnote, *psamples, *pextras, *pmode, *pbad, *pwrap;
    int t;

    sr_root_case(&ma, "bool",   RANT_BOOL, 1, 1);
    sr_root_case(&ma, "u8",     RANT_U8,     1, 1);
    sr_root_case(&ma, "i16",    RANT_I16,    2, 2);
    sr_root_case(&ma, "u32",    RANT_U32,    4, 4);
    sr_root_case(&ma, "i64",    RANT_I64,    8, 8);
    sr_root_case(&ma, "f32",    RANT_F32,    4, 4);
    sr_root_case(&ma, "f64",    RANT_F64,    8, 8);
    sr_root_case(&ma, "u8[16]", RANT_ARR, 16, 16);
    sr_root_case(&ma, "string<64>",   RANT_STR, 66, 66);
    sr_root_case(&ma, "string<8>[4]", RANT_ARR, 40, 40);
    sr_root_case(&ma, "string", RANT_VSTR, 0, 4);
    sr_root_case(&ma, "f32[]",  RANT_VARR, 0, 4);
    sr_root_case(&ma, "string<8>[]", RANT_VARR, 0, 4);
    sr_root_case(&ma, "map",    RANT_MAP,    0, 4);
    sr_root_case(&ma, "enum<u8> { Idle = 0, Run = 1, Fault = 2 }", RANT_ENUM, 1, 1);

    sb    = rant_schema_compile(rant_allocator_alloc, &ma, "bool", NULL);
    su8   = rant_schema_compile(rant_allocator_alloc, &ma, "u8", NULL);
    sstr  = rant_schema_compile(rant_allocator_alloc, &ma, "string", NULL);
    sarr  = rant_schema_compile(rant_allocator_alloc, &ma, "f32[]", NULL);
    smap  = rant_schema_compile(rant_allocator_alloc, &ma, "map", NULL);
    senum = rant_schema_compile(rant_allocator_alloc, &ma, "enum<u8> { Idle, Run, Fault }", NULL);
    swrap = rant_schema_compile(rant_allocator_alloc, &ma, "Wrap { v: bool }", NULL);
    ST_CHECK(sb && su8 && sstr && sarr && smap && senum && swrap,
             "schema-root: the e2e schemas compile");
    if (!(sb && su8 && sstr && sarr && smap && senum && swrap)){ rant_allocator_reset(&ma); return; }

    /* THE canonical-hash pin: a bare type's wire is its kind alone, so every language's
       `bool` topic hashes to this and cross-language matching costs zero detail bytes. */
    ST_CHECK(rant_schema_hash(sb) == SR_HASH_BOOL,
             "schema-root: `bool` canonical hash %016llx", (unsigned long long)rant_schema_hash(sb));
    ST_CHECK(rant_schema_hash(sarr) == SR_HASH_F32ARR,
             "schema-root: `f32[]` canonical hash %016llx", (unsigned long long)rant_schema_hash(sarr));
    ST_CHECK(rant_schema_wire(sb).len == 3, "schema-root: `bool` wire is 3 bytes (%u)",
             (unsigned)rant_schema_wire(sb).len);

    {   /* the C builder twin of the DSL: begin_value + one unnamed field */
        RantSchemaBuilder b = rant_schema_begin_value(rant_allocator_alloc, &ma);
        RantSchema *twin;
        rant_schema_field(&b, "", RANT_BOOL);
        twin = rant_schema_finish(&b);
        ST_CHECK(twin && rant_schema_hash(twin) == rant_schema_hash(sb),
                 "schema-root: builder value root == compiled `bool` (same hash)");
        if (twin) rant_schema_free(twin, rant_allocator_alloc, &ma);
    }
    {   RantSchemaBuilder b = rant_schema_begin_value(rant_allocator_alloc, &ma);
        RantSchema *twin;
        rant_schema_field_var_array(&b, NULL, RANT_F32);           /* NULL name == "" */
        twin = rant_schema_finish(&b);
        ST_CHECK(twin && rant_schema_hash(twin) == rant_schema_hash(sarr),
                 "schema-root: builder value root == compiled `f32[]` (same hash)");
        if (twin) rant_schema_free(twin, rant_allocator_alloc, &ma);
    }
    {   RantSchemaBuilder b = rant_schema_begin_value(rant_allocator_alloc, &ma);
        rant_schema_field(&b, "x", RANT_BOOL);                   /* a named bare root: refused */
        ST_CHECK(rant_schema_finish(&b) == NULL, "schema-root: builder refuses a named bare root");
    }
    {   RantSchemaBuilder b = rant_schema_begin_value(rant_allocator_alloc, &ma);
        rant_schema_field(&b, "", RANT_BOOL);
        rant_schema_field(&b, "", RANT_U8);                      /* two types: not a bare root */
        ST_CHECK(rant_schema_finish(&b) == NULL, "schema-root: builder refuses two bare fields");
    }
    ST_CHECK(rant_schema_compile(rant_allocator_alloc, &ma, "Temperature: f32", NULL) == NULL
          && rant_schema_compile(rant_allocator_alloc, &ma, "bool bool", NULL) == NULL,
             "schema-root: a named bare root and trailing garbage are compile errors");
    {   /* the wire: a bare root round trips, a named one is an alias, but the name may only
           ride the header, so a named type in root position stays refused */
        RantBytes w = rant_schema_wire(sarr);
        RantSchema *rt = rant_schema_parse(w.data, w.len, rant_allocator_alloc, &ma);
        RantSchema *alias; uint8_t named[6];
        ST_CHECK(rt && rant_schema_hash(rt) == rant_schema_hash(sarr)
                 && rant_schema_field_count(rt) == 1,
                 "schema-root: a bare root parses back from its wire");
        if (rt) rant_schema_free(rt, rant_allocator_alloc, &ma);
        named[0] = (uint8_t)RANT_SCHEMA_WIRE_VERSION;     /* [ver][namelen 1]['x'][BOOL] */
        named[1] = 1; named[2] = 'x'; named[3] = (uint8_t)RANT_BOOL;
        alias = rant_schema_parse(named, 4, rant_allocator_alloc, &ma);
        ST_CHECK(alias && rant_schema_name(alias).len == 1
                 && rant_schema_hash(alias) != rant_schema_hash(sb),
                 "schema-root: a named bare root on the wire is an alias, distinct from `bool`");
        if (alias) rant_schema_free(alias, rant_allocator_alloc, &ma);
        named[1] = 0;                                   /* [ver][0][NAMED][1]['x'][BOOL] */
        named[2] = (uint8_t)RANT_NAMED; named[3] = 1; named[4] = 'x'; named[5] = (uint8_t)RANT_BOOL;
        ST_CHECK(rant_schema_parse(named, sizeof named, rant_allocator_alloc, &ma) == NULL,
                 "schema-root: a NAMED type in root position is refused (the header names it)");
    }

    {   /* empty-path and flat-index-0 access, every kind */
        uint8_t m[128]; RantValue v; RantBytes got; RantString sv;
        float xs[3]; uint8_t body[32]; RantMapWriter mw; uint32_t bl;
        xs[0]=1.5f; xs[1]=2.5f; xs[2]=-3.0f;
        ST_CHECK(rant_schema_message_default(sb, m, sizeof m) && m[0] == 0,
                 "schema-root: bare default is the zero value");
        ST_CHECK(rant_set_uint(m, sizeof m, sb, "", 1)
                 && rant_get_uint(rant_bytes(m, 1), sb, "") == 1
                 && rant_schema_msg_len(sb, m, sizeof m) == 1
                 && rant_schema_validate(sb, rant_bytes(m, 1)),
                 "schema-root: bool set/get through the empty path");
        memset(&v, 0, sizeof v);
        ST_CHECK(rant_get_value(rant_bytes(m, 1), sb, 0, &v) && v.kind == RANT_BOOL && v.v.u == 1,
                 "schema-root: rant_get_value at flat index 0");
        v.v.u = 0;
        ST_CHECK(rant_set_value(m, sizeof m, sb, 0, &v)
                 && rant_get_uint(rant_bytes(m, 1), sb, "") == 0,
                 "schema-root: rant_set_value at flat index 0");
        ST_CHECK(rant_schema_message_default(sarr, m, sizeof m)
                 && rant_set_array(m, sizeof m, sarr, "", rant_bytes(xs, sizeof xs))
                 && rant_schema_msg_len(sarr, m, sizeof m) == 4 + 12,
                 "schema-root: f32[] frame set through the empty path");
        got = rant_get_array(rant_bytes(m, 16), sarr, "");
        ST_CHECK(got.len == 12 && memcmp(got.data, xs, 12) == 0, "schema-root: f32[] reads back");
        ST_CHECK(rant_schema_message_default(sstr, m, sizeof m)
                 && rant_set_string(m, sizeof m, sstr, "", rant_cstr("bare")),
                 "schema-root: string frame set through the empty path");
        sv = rant_get_string(rant_bytes(m, rant_schema_msg_len(sstr, m, sizeof m)), sstr, "");
        ST_CHECK(sv.len == 4 && memcmp(sv.data, "bare", 4) == 0, "schema-root: string reads back");
        mw = rant_map_begin(body, sizeof body);
        rant_map_put_uint(&mw, "battery", 87);
        bl = rant_map_finish(&mw);
        ST_CHECK(rant_schema_message_default(smap, m, sizeof m)
                 && rant_set_map(m, sizeof m, smap, "", rant_bytes(body, bl))
                 && rant_map_count(rant_get_map(rant_bytes(m, rant_schema_msg_len(smap, m, sizeof m)),
                                                smap, "")) == 1,
                 "schema-root: map body set/get through the empty path");
        ST_CHECK(rant_schema_message_default(senum, m, sizeof m)
                 && rant_set_enum(m, sizeof m, senum, "", "Fault")
                 && rant_get_uint(rant_bytes(m, 1), senum, "") == 2
                 && rant_get_enum(rant_bytes(m, 1), senum, "").len == 5,
                 "schema-root: enum set by name, read back as value + name");
    }
    {   /* matching: the roots' types compare like fields, struct vs bare never matches */
        char why[128]; RantSchema *rb;
        ST_CHECK(rant_schema_subset(sb, sb) == 1 && rant_schema_subset(sb, su8) == 0
              && rant_schema_subset(sb, swrap) == 0 && rant_schema_subset(swrap, sb) == 0,
                 "schema-root: bare roots match only their own type");
        rant_schema_subset_why(sb, su8, why, sizeof why);
        ST_CHECK(strcmp(why, "root: reader bool, writer u8") == 0,
                 "schema-root: subset_why names both roots ('%s')", why);
        rant_schema_subset_why(swrap, sb, why, sizeof why);
        ST_CHECK(strcmp(why, "root: reader struct 'Wrap', writer bool") == 0,
                 "schema-root: subset_why explains struct vs bare ('%s')", why);
        rb = rant_schema_rebase(sarr, sarr, rant_allocator_alloc, &ma);
        ST_CHECK(rb && rant_schema_field_count(rb) == 1 && rant_schema_msg_min(rb) == 4,
                 "schema-root: rebase of a bare root stays valid");
        if (rb) rant_schema_free(rb, rant_allocator_alloc, &ma);
    }

    /* ---- end to end: one topic per root kind, plus the two refusals ---- */
    memset(&co,0,sizeof co); co.qos.keep_last=4; co.qos.reliability=RANT_RELIABLE;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=ST_DOMAIN+11; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po; qo=po;
    sr_flag_recv=sr_flag_val=sr_note_recv=sr_samples_recv=sr_map_recv=sr_enum_recv=0;
    sr_raw_recv=sr_raw_ok=0; sr_mismatch_n=0; sr_ns=0; sr_battery=0;
    sr_note[0]='\0'; sr_mode[0]='\0';
    P = rant_node_open(&pa, "sr-pub", NULL,          sr_on_event, &po);
    S = rant_node_open(&sa, "sr-sub", sr_on_message, sr_on_event, &so);
    Q = rant_node_open(&qa, "sr-raw", sr_on_raw,     NULL,        &qo);
    ST_CHECK(P&&S&&Q, "schema-root: nodes open");
    if (!(P&&S&&Q)){ if(P)rant_node_close(P,0); if(S)rant_node_close(S,0); if(Q)rant_node_close(Q,0);
                     rant_allocator_reset(&ma); return; }
    pflag    = rant_node_create_topic(P, "sr/flag",    RANT_PUB_ONLY, sb,      &co);
    pnote    = rant_node_create_topic(P, "sr/note",    RANT_PUB_ONLY, sstr,    &co);
    psamples = rant_node_create_topic(P, "sr/samples", RANT_PUB_ONLY, sarr,    &co);
    pextras  = rant_node_create_topic(P, "sr/extras",  RANT_PUB_ONLY, smap,    &co);
    pmode    = rant_node_create_topic(P, "sr/mode",    RANT_PUB_ONLY, senum, &co);
    pbad     = rant_node_create_topic(P, "sr/bad",     RANT_PUB_ONLY, sb,      &co);
    pwrap    = rant_node_create_topic(P, "sr/wrap",    RANT_PUB_ONLY, swrap, &co);
    rant_node_create_topic(S, "sr/flag",    RANT_SUB_ONLY, sb,      &co);
    rant_node_create_topic(S, "sr/note",    RANT_SUB_ONLY, sstr,    &co);
    rant_node_create_topic(S, "sr/samples", RANT_SUB_ONLY, sarr,    &co);
    rant_node_create_topic(S, "sr/extras",  RANT_SUB_ONLY, smap,    &co);
    rant_node_create_topic(S, "sr/mode",    RANT_SUB_ONLY, senum, &co);
    rant_node_create_topic(S, "sr/bad",     RANT_SUB_ONLY, su8,     &co);   /* bool writer: refused */
    rant_node_create_topic(S, "sr/wrap",    RANT_SUB_ONLY, sb,      &co);   /* refused: a struct */
    rant_node_create_topic(Q, "sr/flag",    RANT_SUB_ONLY, NULL,    &co);   /* raw reader */
    ST_CHECK(pflag && pnote && psamples && pextras && pmode && pbad && pwrap,
             "schema-root: topics created");

    for (t=0;t<1200 && (rant_topic_match_count(pflag)==0 || rant_topic_match_count(pmode)==0
                        || rant_topic_match_count(pextras)==0);t++){
        rant_node_poll(P,2); rant_node_poll(S,2); rant_node_poll(Q,2);
    }
    ST_CHECK(rant_topic_match_count(pflag)==2, "schema-root: bool topic matched typed + raw readers (%d)",
             rant_topic_match_count(pflag));
    ST_CHECK(rant_topic_match_count(pnote)==1 && rant_topic_match_count(psamples)==1
             && rant_topic_match_count(pextras)==1 && rant_topic_match_count(pmode)==1,
             "schema-root: string/f32[]/map/enum roots matched");
    ST_CHECK(rant_topic_match_count(pbad)==0 && rant_topic_match_count(pwrap)==0,
             "schema-root: bool-vs-u8 and struct-vs-bare readers refused");
    ST_CHECK(sr_mismatch_n>=2, "schema-root: both refusals surfaced (%lu RANT_E_SCHEMA_MISMATCH)",
             sr_mismatch_n);

    {   uint8_t m[128]; float xs[3]; uint8_t body[32]; RantMapWriter mw; uint32_t bl;
        xs[0]=1.5f; xs[1]=2.5f; xs[2]=-3.0f;
        rant_schema_message_default(sb, m, sizeof m);
        rant_set_uint(m, sizeof m, sb, "", 1);
        rant_topic_send(pflag, rant_bytes(m, 1), NULL);
        rant_schema_message_default(sstr, m, sizeof m);
        rant_set_string(m, sizeof m, sstr, "", rant_cstr("bare-string"));
        rant_topic_send(pnote, rant_bytes(m, rant_schema_msg_len(sstr, m, sizeof m)), NULL);
        rant_schema_message_default(sarr, m, sizeof m);
        rant_set_array(m, sizeof m, sarr, "", rant_bytes(xs, sizeof xs));
        rant_topic_send(psamples, rant_bytes(m, rant_schema_msg_len(sarr, m, sizeof m)), NULL);
        mw = rant_map_begin(body, sizeof body);
        rant_map_put_uint(&mw, "battery", 87);
        bl = rant_map_finish(&mw);
        rant_schema_message_default(smap, m, sizeof m);
        rant_set_map(m, sizeof m, smap, "", rant_bytes(body, bl));
        rant_topic_send(pextras, rant_bytes(m, rant_schema_msg_len(smap, m, sizeof m)), NULL);
        rant_schema_message_default(senum, m, sizeof m);
        rant_set_enum(m, sizeof m, senum, "", "Fault");
        rant_topic_send(pmode, rant_bytes(m, 1), NULL);
        for (t=0;t<800 && (sr_flag_recv==0 || sr_note_recv==0 || sr_samples_recv==0
                           || sr_map_recv==0 || sr_enum_recv==0 || sr_raw_recv==0);t++){
            rant_node_poll(P,1); rant_node_poll(S,2); rant_node_poll(Q,2);
        }
    }
    ST_CHECK(sr_flag_recv==1 && sr_flag_val==1, "schema-root: bool delivered (recv=%d val=%d)",
             sr_flag_recv, sr_flag_val);
    ST_CHECK(sr_note_recv==1 && strcmp(sr_note,"bare-string")==0,
             "schema-root: string root delivered ('%s')", sr_note);
    ST_CHECK(sr_samples_recv==1 && sr_ns==3 && sr_s0==1.5f && sr_s2==-3.0f,
             "schema-root: f32[] root delivered (%u elems)", (unsigned)sr_ns);
    ST_CHECK(sr_map_recv==1 && sr_battery==87, "schema-root: map root delivered (battery=%llu)",
             (unsigned long long)sr_battery);
    ST_CHECK(sr_enum_recv==1 && strcmp(sr_mode,"Fault")==0,
             "schema-root: enum root delivered + resolved to its name ('%s')", sr_mode);
    ST_CHECK(sr_raw_recv==1 && sr_raw_ok,
             "schema-root: schema-less reader decodes through the sender's bare schema");

    rant_node_close(P,1); rant_node_close(S,1); rant_node_close(Q,1);
    rant_allocator_reset(&ma);
}

/* (19a4) the schema wire, pure: named types, alias roots, struct element arrays, variable
   members at any depth, and the static stride refusals. Everything round trips print. */
static RantAllocator sv_ma;
static RantSchema *sv(const char *text){                      /* compile, or NULL */
    return rant_schema_compile(rant_allocator_alloc, &sv_ma, text, NULL);
}
static void sv_free(RantSchema *s){ if (s) rant_schema_free(s, rant_allocator_alloc, &sv_ma); }
/* rant_schema_subset over two texts: 1 yes, 0 refused, -1 a text failed to compile */
static int sv_sub(const char *sub, const char *pub){
    RantSchema *a = sv(sub), *b = sv(pub); int r = -1;
    if (a && b) r = rant_schema_subset(a, b);
    sv_free(a); sv_free(b);
    return r;
}
static int sv_roundtrip(const char *text){                  /* print, recompile, the same wire */
    RantSchema *a = sv(text), *b; char buf[2048]; int ok;
    if (!a) return 0;
    rant_schema_print(a, buf, sizeof buf);
    b = sv(buf);
    ok = b && rant_schema_hash(b) == rant_schema_hash(a)
           && rant_schema_print(a, NULL, 0) == (uint32_t)strlen(buf);
    sv_free(b); sv_free(a);
    return ok;
}
static void schema_v8_checks(void){
    sv_ma = rant_allocator_heap(0);

    /* ---- named types NARROW: unnamed reads named, named demands the same name ---- */
    {   RantSchema *c = sv("Celsius = f32"), *f = sv("Fahrenheit = f32"), *bare = sv("f32");
        ST_CHECK(c && f && bare, "schema-v8: alias roots compile");
        if (c && f && bare){
            ST_CHECK(rant_schema_name(c).len == 7 && rant_schema_hash(c) != rant_schema_hash(f)
                     && rant_schema_hash(c) != rant_schema_hash(bare),
                     "schema-v8: an alias root carries its name and its own identity");
            ST_CHECK(rant_schema_subset(bare, c) == 1 && rant_schema_subset(c, bare) == 0
                     && rant_schema_subset(c, f) == 0 && rant_schema_subset(c, c) == 1,
                     "schema-v8: alias roots narrow (unnamed reads named, never the reverse)");
            ST_CHECK(rant_schema_size(c) == 4,
                     "schema-v8: an alias costs no message bytes (%u)", rant_schema_size(c));
        }
        sv_free(c); sv_free(f); sv_free(bare);
    }
    ST_CHECK(sv_sub("A { t: f32 }", "Celsius = f32\nA { t: Celsius }") == 1,
             "schema-v8: an unnamed field reads a named writer field (unwrap is free)");
    ST_CHECK(sv_sub("Celsius = f32\nA { t: Celsius }", "A { t: f32 }") == 0,
             "schema-v8: a named field refuses an unnamed writer field");
    ST_CHECK(sv_sub("Celsius = f32\nA { t: Celsius }", "Fahrenheit = f32\nA { t: Fahrenheit }") == 0,
             "schema-v8: two different names over one shape never cross-wire");
    ST_CHECK(sv_sub("Celsius = f32\nA { t: Celsius }", "Celsius = f32\nA { t: Celsius, u: u8 }") == 1,
             "schema-v8: subset stays top-level under named fields");
    ST_CHECK(sv_sub("A { v: { a: f32 } }", "A { v: { a: f32, b: f32 } }") == 0,
             "schema-v8: a nested struct still compares exactly");
    ST_CHECK(sv_sub("A { m: enum<u8> { P } }", "A { m: enum<u8> { Q, R } }") == 1
             && sv_sub("A { m: enum<u8> { P } }", "A { m: enum<u16> { P } }") == 0,
             "schema-v8: enum option names stay advisory, the backing width gates");
    ST_CHECK(sv_sub("A { x: f32 }", "B { x: f32 }") == 0,
             "schema-v8: struct root names stay strict-equal");

    /* ---- struct-element arrays: one element-0 template, indexed paths ---- */
    {   RantSchema *s = sv("A { p: Float3[3], q: { m: i16 }[] }");
        RantSchemaFieldInfo fi, fx;
        uint8_t m[256]; RantBytes b; int xi;
        ST_CHECK(s != NULL, "schema-v8: struct arrays compile");
        if (s){
            ST_CHECK(rant_schema_field_count(s) == 6,
                     "schema-v8: a struct array flattens its element once (%u fields)",
                     rant_schema_field_count(s));
            rant_schema_field_at(s, 0, &fi);
            rant_schema_field_at(s, 1, &fx);
            ST_CHECK(fi.kind == RANT_ARR && fi.elem == RANT_STRUCT && fi.count == 3
                     && fi.elem_size == 12 && fi.size == 36 && fi.elem_name.len == 6,
                     "schema-v8: Float3[3] reports element kind, stride and name");
            ST_CHECK(fx.depth == 1 && fx.arr_parent == 0 && fx.offset == 0,
                     "schema-v8: its members point back at the array (arr_parent=%u)", fx.arr_parent);
            ST_CHECK(rant_schema_size(s) == 36, "schema-v8: the fixed array packs inline (%u)",
                     rant_schema_size(s));
            xi = rant_schema_field_index(s, "p[2].z");
            ST_CHECK(xi == rant_schema_field_index(s, "p.z") && xi > 0,
                     "schema-v8: an indexed path resolves to the element template");
            ST_CHECK(rant_schema_message_default(s, m, sizeof m), "schema-v8: default message");
            ST_CHECK(rant_set_f32(m, sizeof m, s, "p[2].z", 7.25f)
                     && rant_set_f32(m, sizeof m, s, "p[0].x", 1.5f)
                     && rant_set_f32(m, sizeof m, s, "p[3].z", 1.0f) == 0,
                     "schema-v8: fixed-array element writes land, out of range refuses");
            ST_CHECK(rant_set_array_count(m, sizeof m, s, "q", 4)
                     && rant_set_int(m, sizeof m, s, "q[3].m", -9)
                     && rant_set_int(m, sizeof m, s, "q[4].m", 1) == 0,
                     "schema-v8: a variable struct array grows, then bounds its elements");
            b = rant_bytes(m, rant_schema_msg_len(s, m, sizeof m));
            ST_CHECK(rant_get_f32(b, s, "p[2].z") == 7.25f && rant_get_f32(b, s, "p[0].x") == 1.5f
                     && rant_get_f32(b, s, "p[1].x") == 0.0f,
                     "schema-v8: fixed-array elements read back independently");
            ST_CHECK(rant_get_int(b, s, "q[3].m") == -9 && rant_get_array_count(b, s, "q") == 4
                     && rant_get_array_count(b, s, "p") == 3,
                     "schema-v8: variable-array elements read back, counts report live");
            ST_CHECK(rant_schema_validate(s, b), "schema-v8: the struct-array message validates");
            {   RantValue v; int ok;
                memset(&v, 0, sizeof v); v.kind = RANT_F32; v.v.f = 4.5;
                ok = rant_set_value_at(m, sizeof m, s, (uint16_t)xi, 1, &v)
                     && rant_get_value_at(b, s, (uint16_t)xi, 1, &v) && v.v.f == 4.5
                     && rant_array_count_at(b, s, 0) == 3;
                ST_CHECK(ok, "schema-v8: reflection get/set_value_at index an element");
            }
        }
        sv_free(s);
    }

    /* ---- variable members at depth: declaration nests, storage does not ---- */
    {   RantSchema *s = sv("A { hdr: { n: u32, note: string, k: u32 }, tail: u8[] }");
        RantSchemaFieldInfo fi; uint8_t m[256]; RantBytes b;
        ST_CHECK(s != NULL, "schema-v8: a variable member inside a struct compiles");
        if (s){
            ST_CHECK(rant_schema_field_at(s, 0, &fi) && fi.kind == RANT_STRUCT && fi.size == 8
                     && rant_schema_size(s) == 8 && rant_schema_msg_min(s) == 16,
                     "schema-v8: the struct's size is its FIXED part; the frame is top-level");
            rant_schema_message_default(s, m, sizeof m);
            ST_CHECK(rant_set_uint(m, sizeof m, s, "hdr.n", 7)
                     && rant_set_uint(m, sizeof m, s, "hdr.k", 9)
                     && rant_set_string(m, sizeof m, s, "hdr.note", rant_cstr("deep"))
                     && rant_set_array(m, sizeof m, s, "tail", rant_bytes("abc", 3)),
                     "schema-v8: nested variable member sets");
            b = rant_bytes(m, rant_schema_msg_len(s, m, sizeof m));
            ST_CHECK(rant_get_uint(b, s, "hdr.n") == 7 && rant_get_uint(b, s, "hdr.k") == 9
                     && rant_get_string(b, s, "hdr.note").len == 4
                     && rant_get_array(b, s, "tail").len == 3 && rant_schema_validate(s, b),
                     "schema-v8: nested variable member reads back, message validates");
        }
        sv_free(s);
    }
    {   RantSchema *s = sv("A { a: string, g: { b: string, c: string }, d: string }");
        uint8_t m[256]; RantBytes b;
        ST_CHECK(s != NULL, "schema-v8: frames at mixed depths compile");
        if (s){
            rant_schema_message_default(s, m, sizeof m);
            rant_set_string(m, sizeof m, s, "a", rant_cstr("1"));
            rant_set_string(m, sizeof m, s, "g.b", rant_cstr("22"));
            rant_set_string(m, sizeof m, s, "g.c", rant_cstr("333"));
            rant_set_string(m, sizeof m, s, "d", rant_cstr("4444"));
            b = rant_bytes(m, rant_schema_msg_len(s, m, sizeof m));
            ST_CHECK(rant_get_string(b, s, "a").len == 1 && rant_get_string(b, s, "g.b").len == 2
                     && rant_get_string(b, s, "g.c").len == 3 && rant_get_string(b, s, "d").len == 4,
                     "schema-v8: frames sit in depth-first declaration order");
        }
        sv_free(s);
    }

    /* ---- rebase across nested frames and struct arrays ---- */
    {   RantSchema *pub = sv("A { pad: u64, v: { a: f32, note: string }, p: Float3[2], t: string }");
        RantSchema *sub = sv("A { v: { a: f32, note: string }, t: string }");
        RantSchema *rb = NULL; uint8_t m[256]; RantBytes b;
        if (pub && sub) rb = rant_schema_rebase(sub, pub, rant_allocator_alloc, &sv_ma);
        ST_CHECK(rb != NULL, "schema-v8: rebase of a nested/array subset");
        if (rb){
            rant_schema_message_default(pub, m, sizeof m);
            rant_set_f32(m, sizeof m, pub, "v.a", 3.5f);
            rant_set_string(m, sizeof m, pub, "v.note", rant_cstr("nn"));
            rant_set_string(m, sizeof m, pub, "t", rant_cstr("tt"));
            b = rant_bytes(m, rant_schema_msg_len(pub, m, sizeof m));
            ST_CHECK(rant_schema_validate(rb, b) && rant_get_f32(b, rb, "v.a") == 3.5f
                     && rant_get_string(b, rb, "v.note").len == 2
                     && rant_get_string(b, rb, "t").len == 2,
                     "schema-v8: the rebased reader walks the writer's frames");
        }
        sv_free(rb); sv_free(pub); sv_free(sub);
    }

    /* ---- refusals: an array element's size must be static, and stay one level deep ---- */
    {   static const char *bad[] = {
            "A { p: { n: string }[4] }",        /* a variable member inside an element   */
            "A { p: Image[2] }",                /* a standard type that has one          */
            "A { p: f32[2][3] }",               /* an array of arrays                    */
            "A { p: Uuid[2] }",                 /* a named alias that IS an array        */
            "A { p: { q: { r: f32 }[2] }[2] }", /* a struct array inside an element      */
            "A { m: enum<u8> { X }[2] }",       /* enum elements would hide the backing  */
            "Float3 = { x: f32 }\nA { p: Float3 }",  /* a reserved name, a wrong shape   */
            "Transform = { x: f32 }",           /* likewise as the last definition       */
            "A { p: Nope }",                    /* an unknown type word                  */
            "A { x: f32 }\nB { y: f32 }",       /* two roots                             */
            "Temperature: f32"                  /* the old typedef spelling stays an error */
        };
        unsigned i, ok = 1;
        for (i = 0; i < sizeof bad / sizeof bad[0]; i++){
            const char *ep = NULL;
            RantSchema *s = rant_schema_compile(rant_allocator_alloc, &sv_ma, bad[i], &ep);
            if (s){ ok = 0; sv_free(s); }
            else if (!ep) ok = 0;
        }
        ST_CHECK(ok, "schema-v8: every malformed/misplaced form is refused with a position");
    }
    {   RantSchema *a = sv("Float3 = { x: f32, y: f32, z: f32 }\nA { p: Float3 }");
        RantSchema *b = sv("Pose { x: f32 }");
        RantSchema *c = sv("Color { r: f32, g: f32, b: f32, a: f32 }");
        ST_CHECK(a != NULL, "schema-v8: redefining a standard type IDENTICALLY is allowed");
        ST_CHECK(b != NULL && c != NULL,
                 "schema-v8: a plain root name is not reserved (reflection names its class)");
        sv_free(a); sv_free(b); sv_free(c);
    }

    /* ---- hostile wire ---- */
    {   uint8_t w[8]; RantSchema *s;
        w[0] = (uint8_t)RANT_SCHEMA_WIRE_VERSION; w[1] = 0;
        w[2] = (uint8_t)RANT_NAMED; w[3] = 1; w[4] = 'A'; w[5] = (uint8_t)RANT_NAMED;
        w[6] = 1; w[7] = 'B';
        ST_CHECK(rant_schema_parse(w, 8, rant_allocator_alloc, &sv_ma) == NULL,
                 "schema-v8: a doubly-wrapped NAMED is refused");
        w[2] = (uint8_t)RANT_NAMED; w[3] = 0; w[4] = (uint8_t)RANT_BOOL;
        ST_CHECK(rant_schema_parse(w, 5, rant_allocator_alloc, &sv_ma) == NULL,
                 "schema-v8: a NAMED with an empty name is refused");
        w[2] = (uint8_t)RANT_ARR; w[3] = 2; w[4] = 0; w[5] = (uint8_t)RANT_VSTR;
        ST_CHECK(rant_schema_parse(w, 6, rant_allocator_alloc, &sv_ma) == NULL,
                 "schema-v8: a variable array element is refused on the wire");
        w[5] = (uint8_t)RANT_NAMED; w[6] = 1; w[7] = 'A';
        s = rant_schema_parse(w, 8, rant_allocator_alloc, &sv_ma);
        ST_CHECK(s == NULL, "schema-v8: a truncated NAMED element is refused");
        sv_free(s);
    }

    /* ---- print: named definitions hoist, dependencies first, and recompile exactly ---- */
    ST_CHECK(sv_roundtrip("Uuid = u8[16]") && sv_roundtrip("Transform") && sv_roundtrip("Image"),
             "schema-v8: alias, composite and variable standard roots round-trip");
    ST_CHECK(sv_roundtrip("A { x: f32, v: { a: u8, b: string<4> }, p: Float3[3],"
                          "    q: { m: i16 }[2], s: string, e: enum<i16> { N = -1, Z } }"),
             "schema-v8: a mixed schema round-trips through print");
    ST_CHECK(sv_roundtrip("Cloud { at: Transform, when: Timestamp, pts: Float3[], meta: map }"),
             "schema-v8: nested standard types round-trip");
    {   RantSchema *s = sv("Cloud { at: Transform, pts: Float3[] }");
        char buf[1024]; const char *d3, *q, *ps;
        if (s){
            rant_schema_print(s, buf, sizeof buf);
            d3 = strstr(buf, "Double3 ="); q = strstr(buf, "Quaternion ="); ps = strstr(buf, "Transform =");
            ST_CHECK(d3 && q && ps && d3 < ps && q < ps,
                     "schema-v8: print emits each named type once, dependencies first");
        }
        sv_free(s);
    }

    /* ---- compile_env: another schema's root name becomes a type word ---- */
    {   RantSchema *w = sv("Widget { id: u32, tag: Color }");
        const RantSchema *env[1]; RantSchema *p = NULL;
        env[0] = w;
        if (w) p = rant_schema_compile_env(rant_allocator_alloc, &sv_ma,
                                           "Panel { w: Widget, more: Widget[2] }", env, 1, NULL);
        ST_CHECK(p != NULL, "schema-v8: compile_env resolves an environment schema by root name");
        if (p){
            ST_CHECK(rant_schema_size(p) == 24 && rant_schema_field_index(p, "w.tag.r") >= 0
                     && rant_schema_field_index(p, "more[1].id") >= 0,
                     "schema-v8: the environment type nests and arrays like any other");
        }
        ST_CHECK(sv("Panel { w: Widget }") == NULL,
                 "schema-v8: without the environment that name does not resolve");
        sv_free(p); sv_free(w);
    }
    rant_allocator_reset(&sv_ma);
}

/* (19a5) the standard type library: names always in scope, golden bytes and hash, shape
   verifying recognition, and an end to end pair publishing Transform and Image. */
static volatile long sd_pose_recv = 0, sd_img_recv = 0, sd_bad_recv = 0, sd_mismatch = 0;
static double sd_px = 0.0, sd_qw = 0.0;
static unsigned sd_iw = 0, sd_ih = 0, sd_ifmt = 0; static size_t sd_ilen = 0;
static void sd_on_message(const RantMsg *m){
    if (!m->schema) return;
    if (rant_get_f64(m->data, m->schema, "translation.x") != 0.0 ||
        rant_schema_field_index(m->schema, "rotation.w") >= 0){
        if (rant_schema_field_index(m->schema, "translation.x") >= 0){
            sd_px = rant_get_f64(m->data, m->schema, "translation.x");
            sd_qw = rant_get_f64(m->data, m->schema, "rotation.w");
            sd_pose_recv++;
            return;
        }
    }
    if (rant_schema_field_index(m->schema, "width") >= 0){
        sd_iw = (unsigned)rant_get_uint(m->data, m->schema, "width");
        sd_ih = (unsigned)rant_get_uint(m->data, m->schema, "height");
        sd_ifmt = (unsigned)rant_get_uint(m->data, m->schema, "format");
        sd_ilen = rant_get_array(m->data, m->schema, "data").len;
        sd_img_recv++;
    }
}
static void sd_on_bad(const RantMsg *m){ (void)m; sd_bad_recv++; }
static void sd_on_event(const RantEvent *ev){
    if (ev->kind == RANT_ERROR && ev->error == RANT_E_SCHEMA_MISMATCH) sd_mismatch++;
}
static void stdtypes_checks(void){
    RantAllocator ma = rant_allocator_heap(0);
    RantAllocator pa = rant_allocator_heap(0);
    RantAllocator sa = rant_allocator_heap(0);
    RantNodeOpts po, so; RantNode *P = NULL, *S = NULL;
    RantTopicOpts co; RantDiscoveryAddr seed;
    RantTopic *ppose, *pimg, *pbad; RantSchema *SPose, *SImg, *SBad, *SBadSub;
    int t;

    /* the whole roster compiles by name alone, and each recognizes itself */
    {   int i, ok = 1, rec = 1;
        for (i = 1; i < (int)RANT_STD_COUNT; i++){
            RantSchema *s = rant_std_schema((RantStdType)i, rant_allocator_alloc, &ma);
            RantString n;
            if (!s){ ok = 0; continue; }
            n = rant_schema_name(s);
            if (n.len != strlen(rant_std_name((RantStdType)i))) ok = 0;
            if (rant_std_recognize(s, rant_allocator_alloc, &ma) != (RantStdType)i) rec = 0;
            rant_schema_free(s, rant_allocator_alloc, &ma);
        }
        ST_CHECK(ok, "stdtypes: every standard type compiles from its name alone");
        ST_CHECK(rec, "stdtypes: every standard type recognizes itself");
    }
    /* GOLDEN BYTES: the canonical wire and hash every wrapper mirrors. Change these only
       with a deliberate wire bump. */
    {   RantSchema *s = rant_std_schema(RANT_STD_FLOAT3, rant_allocator_alloc, &ma);
        static const uint8_t want[] = {
            8, 6, 'F','l','o','a','t','3', 12, 3,
            1, 'x', 8,  1, 'y', 8,  1, 'z', 8
        };
        RantBytes w = rant_schema_wire(s);
        ST_CHECK(s && w.len == sizeof want && memcmp(w.data, want, sizeof want) == 0,
                 "stdtypes: Float3 golden wire (%u bytes)", (unsigned)w.len);
        ST_CHECK(s && rant_schema_hash(s) == 0x04aa9469cd08b1ddULL,
                 "stdtypes: Float3 golden hash %016llx",
                 (unsigned long long)rant_schema_hash(s));
        ST_CHECK(s && rant_schema_size(s) == 12, "stdtypes: Float3 is 12 message bytes");
        if (s) rant_schema_free(s, rant_allocator_alloc, &ma);
    }
    /* recognition verifies the SHAPE, so a peer's same-named impostor never converts */
    {   RantSchema *good = rant_schema_compile(rant_allocator_alloc, &ma,
            "A { at: Transform, id: Uuid, pts: Float3[], plain: f32 }", NULL);
        RantSchema *bad = rant_schema_compile(rant_allocator_alloc, &ma,
            "Uuid = u8[16]\nB { id: Uuid }", NULL);
        RantSchema *lie = rant_schema_parse(bad ? rant_schema_wire(bad).data : NULL,
                                            bad ? rant_schema_wire(bad).len : 0,
                                            rant_allocator_alloc, &ma);
        ST_CHECK(good && bad && lie, "stdtypes: recognition fixtures compile");
        if (good){
            ST_CHECK(rant_std_recognize_field(good, (uint16_t)rant_schema_field_index(good, "at"),
                                              rant_allocator_alloc, &ma) == RANT_STD_TRANSFORM
                     && rant_std_recognize_field(good, (uint16_t)rant_schema_field_index(good, "id"),
                                              rant_allocator_alloc, &ma) == RANT_STD_UUID,
                     "stdtypes: a named field recognizes by name AND shape");
            ST_CHECK(rant_std_recognize_elem(good, (uint16_t)rant_schema_field_index(good, "pts"),
                                             rant_allocator_alloc, &ma) == RANT_STD_FLOAT3,
                     "stdtypes: an array's ELEMENT recognizes");
            ST_CHECK(rant_std_recognize_field(good, (uint16_t)rant_schema_field_index(good, "plain"),
                                              rant_allocator_alloc, &ma) == RANT_STD_NONE
                     && rant_std_recognize(good, rant_allocator_alloc, &ma) == RANT_STD_NONE,
                     "stdtypes: an anonymous field and a user root are not standard types");
        }
        if (good) rant_schema_free(good, rant_allocator_alloc, &ma);
        if (bad)  rant_schema_free(bad,    rant_allocator_alloc, &ma);
        if (lie)  rant_schema_free(lie,    rant_allocator_alloc, &ma);
    }
    {   /* a hand-built wire that calls itself Uuid but is 15 bytes: refused by shape */
        uint8_t w[10]; RantSchema *s;
        w[0] = (uint8_t)RANT_SCHEMA_WIRE_VERSION; w[1] = 4;
        w[2] = 'U'; w[3] = 'u'; w[4] = 'i'; w[5] = 'd';
        w[6] = (uint8_t)RANT_ARR; w[7] = 15; w[8] = 0; w[9] = (uint8_t)RANT_U8;
        s = rant_schema_parse(w, sizeof w, rant_allocator_alloc, &ma);
        ST_CHECK(s && rant_std_recognize(s, rant_allocator_alloc, &ma) == RANT_STD_NONE,
                 "stdtypes: a same-named wrong-shaped peer type is NOT recognized");
        if (s) rant_schema_free(s, rant_allocator_alloc, &ma);
    }
    /* the C mirrors line up with the wire, byte for byte */
    {   RantSchema *sp = rant_std_schema(RANT_STD_TRANSFORM, rant_allocator_alloc, &ma);
        RantTransform p = rant_transform_identity(); uint8_t m[sizeof(RantTransform)];
        p.translation = rant_double3(1.0, 2.0, 3.0);
        memcpy(m, &p, sizeof p);
        ST_CHECK(sp && sizeof(RantTransform) == rant_schema_size(sp),
                 "stdtypes: sizeof(RantTransform) == the wire size (%u)",
                 sp ? rant_schema_size(sp) : 0);
        if (sp){
            RantBytes b = rant_bytes(m, sizeof m);
            ST_CHECK(rant_get_f64(b, sp, "translation.y") == 2.0
                     && rant_get_f64(b, sp, "rotation.w") == 1.0,
                     "stdtypes: a memcpy'd RantTransform reads back through the schema");
        }
        if (sp) rant_schema_free(sp, rant_allocator_alloc, &ma);
    }
    {   RantColor c = rant_color_from_hex(0x11223344u);
        RantDouble3 v = rant_quaternion_rotate(rant_quaternion(0.0, 0.0, 1.0, 0.0),
                                               rant_double3(1.0, 0.0, 0.0));
        double len = rant_double3_length(rant_double3(3.0, 4.0, 0.0));
        ST_CHECK(c.r == 0x11 && c.g == 0x22 && c.b == 0x33 && c.a == 0x44
                 && rant_color_to_hex(c) == 0x11223344u,
                 "stdtypes: Color hex round-trips as 0xRRGGBBAA");
        ST_CHECK(len == 5.0, "stdtypes: vector length without libm (%g)", len);
        ST_CHECK(v.x < -0.999 && v.x > -1.001 && v.y < 1e-9 && v.y > -1e-9,
                 "stdtypes: a 180 degree rotation about z flips x (%g, %g)", v.x, v.y);
    }
    {   /* the two values that need a platform, hence the node runtime */
        RantTimestamp t0 = rant_timestamp_now();
        RantUuid u1, u2;
        rant_uuid_new(&u1); rant_uuid_new(&u2);
        ST_CHECK(t0 > 1600000000000000LL,       /* past 2020 in microseconds */
                 "stdtypes: rant_timestamp_now is Unix-epoch microseconds (%lld)",
                 (long long)t0);
        ST_CHECK(!rant_uuid_is_nil(u1) && memcmp(u1.bytes, u2.bytes, 16) != 0
                 && (u1.bytes[6] & 0xF0u) == 0x40u && (u1.bytes[8] & 0xC0u) == 0x80u,
                 "stdtypes: rant_uuid_new makes distinct RFC 4122 version-4 ids");
    }

    /* ---- end to end: Transform and Image over two nodes, plus a shape impostor refused ---- */
    SPose = rant_std_schema(RANT_STD_TRANSFORM, rant_allocator_alloc, &ma);
    SImg  = rant_std_schema(RANT_STD_IMAGE, rant_allocator_alloc, &ma);
    SBad  = rant_schema_compile(rant_allocator_alloc, &ma, "Twist", NULL);
    SBadSub = rant_std_schema(RANT_STD_TRANSFORM, rant_allocator_alloc, &ma);
    ST_CHECK(SPose && SImg && SBad && SBadSub, "stdtypes: e2e schemas compile");
    if (!(SPose && SImg && SBad && SBadSub)){ rant_allocator_reset(&ma); return; }

    memset(&co,0,sizeof co); co.qos.keep_last=4; co.qos.reliability=RANT_RELIABLE;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=ST_DOMAIN+23; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po;
    sd_pose_recv=sd_img_recv=sd_bad_recv=sd_mismatch=0;
    P = rant_node_open(&pa, "sd-pub", NULL,          sd_on_event, &po);
    S = rant_node_open(&sa, "sd-sub", sd_on_message, sd_on_event, &so);
    ST_CHECK(P&&S, "stdtypes: nodes open");
    if (!(P&&S)){ if(P)rant_node_close(P,0); if(S)rant_node_close(S,0); rant_allocator_reset(&ma); return; }
    ppose = rant_node_create_topic(P, "sd/pose", RANT_PUB_ONLY, SPose, &co);
    pimg  = rant_node_create_topic(P, "sd/img",  RANT_PUB_ONLY, SImg,    &co);
    pbad  = rant_node_create_topic(P, "sd/bad",  RANT_PUB_ONLY, SBad,    &co);
    rant_node_create_topic(S, "sd/pose", RANT_SUB_ONLY, SPose, &co);
    rant_node_create_topic(S, "sd/img",  RANT_SUB_ONLY, SImg,    &co);
    rant_node_create_topic(S, "sd/bad",  RANT_SUB_ONLY, SBadSub, &co);     /* Twist writer: refused */
    ST_CHECK(ppose && pimg && pbad, "stdtypes: topics created");

    for (t=0;t<1200 && (rant_topic_match_count(ppose)==0 || rant_topic_match_count(pimg)==0);t++){
        rant_node_poll(P,2); rant_node_poll(S,2);
    }
    ST_CHECK(rant_topic_match_count(ppose)==1 && rant_topic_match_count(pimg)==1,
             "stdtypes: Transform and Image topics matched");
    ST_CHECK(rant_topic_match_count(pbad)==0,
             "stdtypes: a Transform reader refuses a Twist writer");
    ST_CHECK(sd_mismatch >= 1, "stdtypes: the refusal surfaced (%ld)", sd_mismatch);

    {   uint8_t m[sizeof(RantTransform)]; RantTransform p = rant_transform_identity();
        uint8_t *img; uint32_t need; size_t px = 64u*48u*3u;
        p.translation = rant_double3(4.5, -1.25, 9.0);
        memcpy(m, &p, sizeof p);
        rant_topic_send(ppose, rant_bytes(m, sizeof p), NULL);
        need = rant_schema_msg_min(SImg) + (uint32_t)px;
        img = (uint8_t *)i_rant_plat_realloc(NULL, need);
        if (img){
            size_t i;
            uint8_t *pix = (uint8_t *)i_rant_plat_realloc(NULL, px);
            rant_schema_message_default(SImg, img, need);
            rant_set_uint(img, need, SImg, "width", 64);
            rant_set_uint(img, need, SImg, "height", 48);
            rant_set_uint(img, need, SImg, "stride", 0);
            rant_set_enum(img, need, SImg, "format", "Rgb8");
            if (pix){
                for (i = 0; i < px; i++) pix[i] = (uint8_t)i;
                rant_set_array(img, need, SImg, "data", rant_bytes(pix, px));
                i_rant_plat_realloc(pix, 0);
            }
            rant_topic_send(pimg, rant_bytes(img, rant_schema_msg_len(SImg, img, need)), NULL);
            i_rant_plat_realloc(img, 0);
        }
        for (t=0;t<1200 && (sd_pose_recv==0 || sd_img_recv==0);t++){
            rant_node_poll(P,1); rant_node_poll(S,2);
        }
    }
    ST_CHECK(sd_pose_recv==1 && sd_px==4.5 && sd_qw==1.0,
             "stdtypes: Transform delivered (recv=%ld x=%g w=%g)", sd_pose_recv, sd_px, sd_qw);
    ST_CHECK(sd_img_recv==1 && sd_iw==64 && sd_ih==48 && sd_ifmt==(unsigned)RANT_IMAGE_RGB8
             && sd_ilen==64u*48u*3u,
             "stdtypes: Image delivered whole (%ux%u fmt=%u %u bytes)",
             sd_iw, sd_ih, sd_ifmt, (unsigned)sd_ilen);
    ST_CHECK(sd_bad_recv==0, "stdtypes: the refused topic delivered nothing");
    rant_node_close(S,1); rant_node_close(P,1);
    rant_allocator_reset(&ma); rant_allocator_reset(&pa); rant_allocator_reset(&sa);
}

/* (19b) the pairwise detail codec, sans IO: request build, the stateless responder, schema
   wire only on a hash mismatch, entry boundary truncation, malformed input rejected whole. */
static void detail_codec_checks(void){
    static uint8_t tmem[1<<18];
    RantAllocator ma = rant_allocator_heap(0);
    RantConfig tc; RantTransportState *tr; RantTopicDef ch[3];
    RantMetaSchema schemas[3]; RantSchema *S;
    RantDetailWant wants[4];
    uint8_t req[256], resp[1024], out2[1024];
    size_t rl, need, len;

    S = rant_schema_compile(rant_allocator_alloc, &ma, "Pose { stamp: u64, x: f64 }", NULL);
    memset(ch,0,sizeof ch);
    ch[0].name="dt/typed"; ch[1].name="dt/raw";
    ch[2].name="dt/off"; ch[2].role=RANT_INACTIVE;
    memset(&tc,0,sizeof tc); tc.topics=ch; tc.n_topics=3; tc.max_peers=2;
    tc.allocator=rant_allocator_alloc; tc.user=&ma;
    tr = rant_transport_init(tmem, sizeof tmem, &tc);
    ST_CHECK(tr!=NULL && S!=NULL, "detail: transport + schema ready");
    if (!tr || !S){ rant_allocator_reset(&ma); return; }
    memset(schemas,0,sizeof schemas);
    schemas[0].hash = rant_schema_hash(S); schemas[0].wire = rant_schema_wire(S);

    /* request four indices: typed (requester untyped: hash 0), raw, INACTIVE, unknown */
    wants[0].index=0; wants[0].schema_hash=0;
    wants[1].index=1; wants[1].schema_hash=0;
    wants[2].index=2; wants[2].schema_hash=0;
    wants[3].index=9; wants[3].schema_hash=0;
    rl = rant_detail_req_build(77, 5, wants, 4, req, sizeof req);
    ST_CHECK(rl == 14u+4u*10u, "detail: req builds (%u bytes)", (unsigned)rl);
    ST_CHECK(rant_detail_kind(rant_bytes(req,rl))==RANT_DETAIL_REQ
          && rant_detail_domain(rant_bytes(req,rl))==77
          && rant_detail_meta_version(rant_bytes(req,rl))==5,
             "detail: req header round-trips (kind/domain/version)");
    ST_CHECK(rant_detail_req_build(77,5,wants,4,req,20)==0, "detail: req refuses a short buffer");

    need = rant_transport_detail_resp_size(tr, schemas, rant_bytes(req,rl));
    len  = rant_transport_detail_respond(tr, schemas, 9, rant_bytes(req,rl), resp, sizeof resp);
    ST_CHECK(need==len && len>14u, "detail: resp_size == respond, byte for byte (%u)", (unsigned)len);
    ST_CHECK(rant_detail_kind(rant_bytes(resp,len))==RANT_DETAIL_RESP
          && rant_detail_domain(rant_bytes(resp,len))==77
          && rant_detail_meta_version(rant_bytes(resp,len))==9,
             "detail: resp header carries the responder version");
    {   RantDetailIter it; RantDetail d; int n=0, ok_typed=0, ok_raw=0;
        RantBytes wire = rant_bytes(NULL, 0);
        memset(&it,0,sizeof it);
        while (rant_detail_next(rant_bytes(resp,len), &it, &d)){
            n++;
            if (d.index==0){
                ok_typed = d.name.len==8 && memcmp(d.name.data,"dt/typed",8)==0
                        && d.schema_hash==rant_schema_hash(S)
                        && d.schema_wire.len==rant_schema_wire(S).len;
                wire = d.schema_wire;
            }
            if (d.index==1)
                ok_raw = d.name.len==6 && memcmp(d.name.data,"dt/raw",6)==0
                      && d.schema_hash==0 && d.schema_wire.len==0;
        }
        ST_CHECK(n==2, "detail: advertised indices answered, INACTIVE + unknown skipped (n=%d)", n);
        ST_CHECK(ok_typed, "detail: typed entry carries name + hash + inlined wire");
        ST_CHECK(ok_raw, "detail: raw entry carries name, no schema");
        {   RantSchema *P2 = wire.len ? rant_schema_parse(wire.data, wire.len,
                                                          rant_allocator_alloc, &ma) : NULL;
            ST_CHECK(P2 && rant_schema_hash(P2)==rant_schema_hash(S),
                     "detail: inlined wire parses back to the same identity");
        }
    }

    /* identical hash: hash-only entry, zero wire bytes (identical wire is implied) */
    wants[0].schema_hash = rant_schema_hash(S);
    rl  = rant_detail_req_build(77, 5, wants, 2, req, sizeof req);
    len = rant_transport_detail_respond(tr, schemas, 9, rant_bytes(req,rl), resp, sizeof resp);
    {   RantDetailIter it; RantDetail d; int hash_only=0; memset(&it,0,sizeof it);
        while (rant_detail_next(rant_bytes(resp,len), &it, &d))
            if (d.index==0) hash_only = d.schema_wire.len==0 && d.schema_hash==rant_schema_hash(S);
        ST_CHECK(hash_only, "detail: identical hash rides hash-only (no wire)");
    }

    /* a cap one byte short of full truncates at an entry boundary: still parseable,
       holding exactly the leading entries that fit (the requester re-asks for the rest) */
    wants[0].schema_hash = 0;
    rl = rant_detail_req_build(77, 5, wants, 2, req, sizeof req);
    {   size_t full = rant_transport_detail_resp_size(tr, schemas, rant_bytes(req,rl));
        size_t cut  = rant_transport_detail_respond(tr, schemas, 9, rant_bytes(req,rl), out2, full-1);
        RantDetailIter it; RantDetail d; int n=0; uint16_t first=0xFFFF;
        memset(&it,0,sizeof it);
        while (rant_detail_next(rant_bytes(out2,cut), &it, &d)){ if (!n) first=d.index; n++; }
        ST_CHECK(cut>0 && cut<full && n==1 && first==0,
                 "detail: truncation stops at an entry boundary (paging: %d/%d entries)", n, 2);
    }

    /* malformed input is rejected wholesale, never partially trusted */
    ST_CHECK(rant_transport_detail_respond(tr, schemas, 9, rant_bytes(req, rl-1), out2, sizeof out2)==0,
             "detail: truncated req rejected");
    ST_CHECK(rant_transport_detail_respond(tr, schemas, 9, rant_bytes(resp, len), out2, sizeof out2)==0,
             "detail: a RESP fed to the responder is refused (kind gate)");
    {   uint8_t junk[32]; memset(junk, 0x5A, sizeof junk);
        ST_CHECK(rant_detail_kind(rant_bytes(junk, sizeof junk))==0
              && rant_detail_kind(rant_bytes(req, 4))==0,
                 "detail: non-detail bytes yield kind 0");
    }
    rant_allocator_reset(&ma);
}

/* (19b2) detail paging never IP fragments and never wedges: a response fits one datagram
   and the requester pages the rest (spec/testing.md). */
static void detail_paging_checks(void){
#define DP_N 40
    RantAllocator ma = rant_allocator_heap(0);
    RantConfig tc; RantTransportState *tr; RantMetaSchema schemas[DP_N]; RantSchema *S;
    RantDetailWant wants[DP_N]; uint8_t req[512], buf[RANT_DGRAM_MAX + 8];
    char names[DP_N][RANT_TOPIC_NAME_MAX + 1];
    int done[DP_N], i, rounds, resolved, max_page = 0;

    S = rant_schema_compile(rant_allocator_alloc, &ma, "Pose { stamp: u64, x: f64, y: f64 }", NULL);
    memset(&tc, 0, sizeof tc);
    tc.topics = NULL; tc.n_topics = DP_N; tc.max_peers = 2; tc.allocator = rant_allocator_alloc; tc.user = &ma;
    { size_t need = rant_transport_required_memory(&tc);         /* dynamic reserve mode */
      void *mem = rant_allocator_alloc(&ma, NULL, need);
      tr = mem ? rant_transport_init(mem, need, &tc) : NULL; }
    ST_CHECK(tr != NULL && S != NULL, "detail-paging: transport + schema ready");
    if (!tr || !S){ rant_allocator_reset(&ma); return; }

    memset(schemas, 0, sizeof schemas);
    for (i = 0; i < DP_N; i++){                          /* long names so entries are fat */
        RantTopicDef d; memset(&d, 0, sizeof d);
        snprintf(names[i], sizeof names[i],
                 "paging.detail.regression.topic.with.a.long.name.%02d", i);
        d.name = names[i]; d.role = RANT_PUB_ONLY;
        rant_transport_topic_define(tr, (uint16_t)i, &d);
        schemas[i].hash = rant_schema_hash(S); schemas[i].wire = rant_schema_wire(S);
    }

    /* the requester's paging loop: ask for the unresolved indices, take one page, mark what
       it carried, repeat. Exactly what rant_transport_detail_wants drives live. */
    memset(done, 0, sizeof done);
    for (rounds = 0, resolved = 0; resolved < DP_N && rounds < DP_N; rounds++){
        uint16_t nw = 0; size_t rl, page, len; RantDetailIter it; RantDetail dd; int got = 0;
        for (i = 0; i < DP_N; i++) if (!done[i]){ wants[nw].index = (uint16_t)i; wants[nw].schema_hash = 0; nw++; }
        rl = rant_detail_req_build(3, 1, wants, nw, req, sizeof req);
        if (!rl) break;
        page = rant_transport_detail_resp_size(tr, schemas, rant_bytes(req, rl));
        if (page > (size_t)max_page) max_page = (int)page;
        if (page > RANT_DGRAM_MAX){ ST_CHECK(0, "detail-paging: a page exceeded one datagram (%u)", (unsigned)page); break; }
        len = rant_transport_detail_respond(tr, schemas, 1, rant_bytes(req, rl), buf, page);
        memset(&it, 0, sizeof it);
        while (rant_detail_next(rant_bytes(buf, len), &it, &dd))
            if (dd.index < DP_N && !done[dd.index]){ done[dd.index] = 1; resolved++; got++; }
        if (!got) break;                                 /* no forward progress: wedged */
    }
    ST_CHECK(resolved == DP_N, "detail-paging: all %d entries resolved (%d)", DP_N, resolved);
    ST_CHECK(rounds > 1, "detail-paging: it actually paged (%d single-datagram rounds)", rounds);
    ST_CHECK(max_page > 0 && max_page <= RANT_DGRAM_MAX,
             "detail-paging: every page stayed within one datagram (max=%u)", (unsigned)max_page);
    rant_allocator_reset(&ma);

    /* (b) force first: one topic whose schema wire alone exceeds a datagram. respond must
       emit exactly that whole entry, never a header only reply that re asks forever. */
    {   RantAllocator mb = rant_allocator_heap(0);
        RantSchemaBuilder b = rant_schema_begin(rant_allocator_alloc, &mb, "Big");
        RantTransportState *t2; RantConfig c2; RantMetaSchema sc; RantDetailWant w; RantSchema *B;
        RantTopicDef d; uint8_t rq[64], *big; size_t need2, rl2, page2, len2; int k;
        for (k = 0; k < 200; k++){ char fn[16]; snprintf(fn, sizeof fn, "field%03d", k); rant_schema_field(&b, fn, RANT_F64); }
        B = rant_schema_finish(&b);
        ST_CHECK(B && rant_schema_wire(B).len > RANT_DGRAM_MAX,
                 "detail-paging: built a schema wire over one datagram (%u)",
                 (unsigned)(B ? rant_schema_wire(B).len : 0));
        memset(&c2, 0, sizeof c2);
        c2.topics = NULL; c2.n_topics = 1; c2.max_peers = 1; c2.allocator = rant_allocator_alloc; c2.user = &mb;
        need2 = rant_transport_required_memory(&c2);
        t2 = B ? rant_transport_init(rant_allocator_alloc(&mb, NULL, need2), need2, &c2) : NULL;
        if (t2){
            memset(&d, 0, sizeof d); d.name = "big/schema"; d.role = RANT_PUB_ONLY;
            rant_transport_topic_define(t2, 0, &d);
            memset(&sc, 0, sizeof sc); sc.hash = rant_schema_hash(B); sc.wire = rant_schema_wire(B);
            w.index = 0; w.schema_hash = 0;   /* requester untyped: forces the wire inline */
            rl2 = rant_detail_req_build(3, 1, &w, 1, rq, sizeof rq);
            page2 = rant_transport_detail_resp_size(t2, &sc, rant_bytes(rq, rl2));
            ST_CHECK(page2 > RANT_DGRAM_MAX, "detail-paging: the oversize entry is measured whole (%u)", (unsigned)page2);
            big = (uint8_t*)rant_allocator_alloc(&mb, NULL, page2 + 8);
            len2 = big ? rant_transport_detail_respond(t2, &sc, 1, rant_bytes(rq, rl2), big, page2) : 0;
            {   RantDetailIter it; RantDetail dd; int n = 0; uint64_t h = 0;
                memset(&it, 0, sizeof it);
                while (big && rant_detail_next(rant_bytes(big, len2), &it, &dd)){ n++; h = dd.schema_hash; }
                ST_CHECK(n == 1 && h == rant_schema_hash(B),
                         "detail-paging: the lone oversize entry rides its own page (n=%d)", n);
            }
        } else ST_CHECK(0, "detail-paging: force-first transport ready");
        rant_allocator_reset(&mb);
    }
#undef DP_N
}

/* (19c) live uDTL routing: a DETAIL_REQ at the data socket is answered to its source even
   from a bare socket. Duplicates are idempotent, wrong domain and garbage are ignored. */
static void detail_live_checks(void){
    RantAllocator pa = rant_allocator_heap(0);
    RantAllocator sa = rant_allocator_heap(0);
    RantAllocator ma = rant_allocator_heap(0);
    RantNodeOpts po, so; RantNode *P=NULL, *S=NULL; RantTopicOpts co; RantDiscoveryAddr seed;
    RantSchema *W; RantTopic *pc;
    i_RantSock q = RANT_SOCK_BAD;
    uint8_t pip[4]={0,0,0,0}; uint16_t pport=0; uint32_t pversion=0;
    /* the oracle, copied out of the peer view BEFORE any further poll invalidates it */
    uint16_t oalias[4]; char oname[4][64]; uint8_t onlen[4]; uint64_t ohash[4]; uint16_t nw=0;
    uint16_t dom = ST_DOMAIN+10;
    int t;

    W = rant_schema_compile(rant_allocator_alloc, &ma, "Pose { stamp: u64, x: f64 }", NULL);
    memset(&co,0,sizeof co); co.qos.keep_last=2;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=dom; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po;
    P = rant_node_open(&pa, "dt-pub", NULL, NULL, &po);
    S = rant_node_open(&sa, "dt-sub", st_on_message, NULL, &so);
    ST_CHECK(P && S && W, "detail-live: nodes open");
    if (!(P && S && W)){
        if (P) rant_node_close(P,0); if (S) rant_node_close(S,0);
        rant_allocator_reset(&ma); return;
    }
    pc = rant_node_create_topic(P, "dt/pose",  RANT_PUB_ONLY, W,      &co);
    rant_node_create_topic(P, "dt/plain", RANT_PUB_ONLY, NULL, &co);
    rant_node_create_topic(S, "dt/pose",  RANT_SUB_ONLY, W,      &co);
    for (t=0;t<800 && rant_topic_match_count(pc)==0;t++){ rant_node_poll(P,2); rant_node_poll(S,2); }
    ST_CHECK(pc && rant_topic_match_count(pc)==1, "detail-live: matched");

    /* the oracle: the publisher's locator, version, and advertised topics as its v9
       blob (held by the subscriber) states them */
    {   uint16_t cnt=0, k; const RantDiscoveryPeer *ps = st_peers(S, &cnt);
        const RantDiscoveryPeer *pp = NULL;
        for (k=0;k<cnt;k++) if (ps[k].name.len==6 && memcmp(ps[k].name.data,"dt-pub",6)==0) pp=&ps[k];
        ST_CHECK(pp!=NULL, "detail-live: publisher in the peer view");
        if (pp){
            RantInterestIter it; RantTopicEntry tp;
            memcpy(pip, pp->addr.ip, 4); pport = pp->addr.port; pversion = pp->meta_version;
            memset(&it,0,sizeof it);
            /* v10 entries carry 32-bit hashes only: bind each announced entry to the
               topic we created on P by hash, and expect the RESP to fill in the rest */
            while (nw<4 && i_rant_node_peer_interest_next(pp, &it, &tp)){
                if (!tp.is_pub) continue;
                oalias[nw] = tp.index;
                if (tp.hash == (uint32_t)rant_topic_id("dt/pose")){
                    onlen[nw]=7; memcpy(oname[nw],"dt/pose",7);  ohash[nw]=rant_schema_hash(W);
                } else if (tp.hash == (uint32_t)rant_topic_id("dt/plain")){
                    onlen[nw]=8; memcpy(oname[nw],"dt/plain",8); ohash[nw]=0;
                } else continue;
                nw++;
            }
        }
    }
    ST_CHECK(nw==2 && pport!=0, "detail-live: oracle holds both pub topics (nw=%u)", nw);

    /* observer mode (opts.fetch_details): a topic-less node greedily fetches every
       peer topic's name + schema and serves the rant_node_peer_topic_* queries */
    {   RantAllocator oa = rant_allocator_heap(0);
        RantNodeOpts oo = po; RantNode *O;
        oo.fetch_details = 1;
        O = rant_node_open(&oa, "dt-obs", st_on_message, NULL, &oo);
        ST_CHECK(O != NULL, "detail-obs: observer node opens");
        if (O){
            uint32_t pid = 0;
            int have_pose = 0, have_plain = 0, pose_typed = 0, plain_raw = 0;
            for (t=0;t<800;t++){
                uint16_t cnt=0, k; const RantDiscoveryPeer *ops;
                RantIter it; RantEntityInfo ei;
                rant_node_poll(O,2); rant_node_poll(P,1); rant_node_poll(S,1);
                ops = st_peers(O, &cnt);
                pid = 0;
                for (k=0;k<cnt;k++)
                    if (ops[k].name.len==6 && memcmp(ops[k].name.data,"dt-pub",6)==0) pid = ops[k].id;
                if (!pid) continue;
                have_pose = have_plain = 0;
                memset(&it,0,sizeof it);
                while (rant_node_entities_next(O, pid, &it, &ei)){
                    if (ei.name.len==7 && memcmp(ei.name.data,"dt/pose",7)==0){
                        have_pose = 1;
                        pose_typed = ei.schema && ei.schema_hash==rant_schema_hash(W)
                                     && rant_schema_hash(ei.schema)==ei.schema_hash;
                    }
                    if (ei.name.len==8 && memcmp(ei.name.data,"dt/plain",8)==0){
                        have_plain = 1; plain_raw = !ei.schema && ei.schema_hash==0;
                    }
                }
                if (have_pose && have_plain) break;
            }
            ST_CHECK(have_pose && have_plain, "detail-obs: greedy cache resolves both topic names");
            ST_CHECK(pose_typed, "detail-obs: typed topic's schema cached parsed (hash matches)");
            ST_CHECK(plain_raw, "detail-obs: raw topic cached untyped");
            /* the subscriber is authoritative about its schema too: the observer fetches a SUB_ONLY
               topic's schema exactly like a publisher's */
            {   uint32_t sid = 0; int sub_typed = 0;
                for (t=0;t<800;t++){
                    uint16_t cnt=0, k; const RantDiscoveryPeer *ops;
                    RantIter it; RantEntityInfo ei;
                    rant_node_poll(O,2); rant_node_poll(P,1); rant_node_poll(S,1);
                    ops = st_peers(O, &cnt);
                    sid = 0;
                    for (k=0;k<cnt;k++)
                        if (ops[k].name.len==6 && memcmp(ops[k].name.data,"dt-sub",6)==0) sid = ops[k].id;
                    if (!sid) continue;
                    memset(&it,0,sizeof it);
                    while (rant_node_entities_next(O, sid, &it, &ei))
                        if (ei.schema && ei.schema_hash==rant_schema_hash(W)
                            && rant_schema_hash(ei.schema)==ei.schema_hash) sub_typed = 1;
                    if (sub_typed) break;
                }
                ST_CHECK(sub_typed,
                         "detail-obs: subscriber-only topic's schema fetched (subscriber authoritative)");
            }
            /* observe then subscribe, the explorer's flow: the greedy fetch dissolved these indices
               against a topic less node, so creating the topic must re pend and re verify them */
            {   RantTopic *osub = rant_node_create_topic(O, "dt/pose", RANT_SUB_ONLY, W, &co);
                unsigned long a0 = st_any;
                ST_CHECK(osub != NULL, "detail-obs: late subscribe topic created");
                for (t=0;t<800 && rant_topic_match_count(pc)<2;t++){
                    rant_node_poll(O,2); rant_node_poll(P,1); rant_node_poll(S,1);
                }
                ST_CHECK(rant_topic_match_count(pc)==2,
                         "detail-obs: observe-then-subscribe re-verifies and matches (%u readers)",
                         rant_topic_match_count(pc));
                {   uint8_t pose[16]; memset(pose, 0x33, sizeof pose);   /* stamp + x */
                    rant_topic_send(pc, rant_bytes(pose, sizeof pose), NULL);
                    for (t=0;t<400 && st_any < a0+2;t++){
                        rant_node_poll(P,1); rant_node_poll(S,1); rant_node_poll(O,2);
                    }
                    ST_CHECK(st_any >= a0+2, "detail-obs: late subscriber receives (got %lu new)",
                             st_any - a0);
                }
            }
            rant_node_close(O, 1);
        }
    }

    q = i_rant_plat_udp_open();
    if (q != RANT_SOCK_BAD){ if (!i_rant_plat_bind(q, 0, 0, 0)){ i_rant_plat_close(q); q = RANT_SOCK_BAD; } }
    ST_CHECK(q != RANT_SOCK_BAD, "detail-live: raw requester socket");
    if (q != RANT_SOCK_BAD && nw==2 && pport){
        uint8_t req[128], r1[2048], r2[2048]; size_t rl; int n1=-1, n2=-1;
        RantDetailWant wants[4]; uint16_t k;
        i_rant_plat_set_nonblock(q);
        for (k=0;k<nw;k++){ wants[k].index=oalias[k]; wants[k].schema_hash=0; }
        rl = rant_detail_req_build(dom, pversion, wants, nw, req, sizeof req);

        /* a wrong-domain request is ignored (the node core's domain gate) */
        {   uint8_t bad[128]; size_t bl = rant_detail_req_build((uint16_t)(dom+1), pversion,
                                                                wants, nw, bad, sizeof bad);
            i_rant_plat_send(q, bad, bl, pip, pport);
            for (t=0;t<50;t++){ rant_node_poll(P,1); rant_node_poll(S,1);
                                if (i_rant_plat_recv(q, r1, sizeof r1, NULL, NULL) > 0){ n1=1; break; } }
            ST_CHECK(n1<0, "detail-live: wrong-domain request ignored");
        }

        /* the real request, re-sent like a real requester until answered */
        n1 = -1;
        for (t=0;t<400 && n1<=0;t++){
            if ((t & 63)==0) i_rant_plat_send(q, req, rl, pip, pport);
            rant_node_poll(P,2); rant_node_poll(S,1);
            n1 = i_rant_plat_recv(q, r1, sizeof r1, NULL, NULL);
        }
        ST_CHECK(n1>0, "detail-live: response reached the request's source socket");
        if (n1>0){
            RantBytes rb = rant_bytes(r1, (size_t)n1);
            RantDetailIter it; RantDetail d; int n=0, names_ok=1, hashes_ok=1, wire_ok=0;
            ST_CHECK(rant_detail_kind(rb)==RANT_DETAIL_RESP && rant_detail_domain(rb)==dom
                  && rant_detail_meta_version(rb)==pversion,
                     "detail-live: header matches the announced version (%u)", pversion);
            memset(&it,0,sizeof it);
            while (rant_detail_next(rb, &it, &d)){
                for (k=0;k<nw;k++) if (oalias[k]==d.index) break;
                if (k==nw){ names_ok=0; continue; }
                if (d.name.len!=onlen[k] || memcmp(d.name.data, oname[k], onlen[k])!=0) names_ok=0;
                if (d.schema_hash != ohash[k]) hashes_ok=0;
                if (ohash[k] && d.schema_wire.len){
                    RantSchema *ps2 = rant_schema_parse(d.schema_wire.data, d.schema_wire.len,
                                                        rant_allocator_alloc, &ma);
                    if (ps2 && rant_schema_hash(ps2)==ohash[k]) wire_ok=1;
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
                if ((t & 63)==0) i_rant_plat_send(q, req, rl, pip, pport);
                rant_node_poll(P,2); rant_node_poll(S,1);
                {   int r = i_rant_plat_recv(q, got==0?r1:r2, sizeof r1, NULL, NULL);
                    if (r>0){ if (got==0) n1=r; else n2=r; got++; } }
            }
            ST_CHECK(n1>0 && n1==n2 && memcmp(r1,r2,(size_t)n1)==0,
                     "detail-live: duplicate requests answer byte-identically");
        }

        /* a RESP and garbage uDTL bytes at the node are ignored, it still answers */
        if (n1>0) i_rant_plat_send(q, r1, (size_t)n1, pip, pport);
        {   uint8_t junk[6]={'u','D','T','L',0x7F,0x00};
            i_rant_plat_send(q, junk, sizeof junk, pip, pport); }
        n2 = -1;
        for (t=0;t<400 && n2<=0;t++){
            if ((t & 63)==0) i_rant_plat_send(q, req, rl, pip, pport);
            rant_node_poll(P,2); rant_node_poll(S,1);
            n2 = i_rant_plat_recv(q, r2, sizeof r2, NULL, NULL);
        }
        ST_CHECK(n2>0, "detail-live: node still answers after RESP/garbage datagrams");
    }
    if (q != RANT_SOCK_BAD) i_rant_plat_close(q);
    rant_node_close(P,1); rant_node_close(S,1);
    rant_allocator_reset(&ma);
}

/* The threaded phases: service threads, condvar flow control and the waker. Phases 20 to
 * 24 are catalogued in spec/testing.md. */
#ifdef RANT_THREADS

static void sw_sleep_ms(int ms);   /* defined with the sweep helpers below */

#define TH_SENDERS 3
#define TH_MSGS    1000u

static volatile unsigned long th_recv, th_order_bad, th_lost, th_evicted_evt;
static unsigned long th_next_seq[TH_SENDERS];   /* only the sub's service thread writes */

static void th_on_message(const RantMsg *m){
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
static void th_on_event(const RantEvent *ev){
    if (ev->kind == RANT_MSG_LOST) th_lost += (unsigned long)ev->lost_count;
    else if (ev->kind == RANT_ERROR && ev->error == RANT_E_EVICTED_UNSENT) th_evicted_evt++;
}

typedef struct { RantTopic *ch; uint32_t id; } th_sender_arg;
static void th_sender(void *arg){
    th_sender_arg *a = (th_sender_arg*)arg;
    uint8_t buf[8]; uint32_t i;
    for (i=0;i<TH_MSGS;i++){
        buf[0]=(uint8_t)a->id; buf[1]=(uint8_t)(a->id>>8); buf[2]=(uint8_t)(a->id>>16); buf[3]=(uint8_t)(a->id>>24);
        buf[4]=(uint8_t)i; buf[5]=(uint8_t)(i>>8); buf[6]=(uint8_t)(i>>16); buf[7]=(uint8_t)(i>>24);
        rant_topic_send(a->ch, rant_bytes(buf, sizeof buf), NULL);
    }
}

/* phase 23 state: node B echoes every request onto its reply topic from the
 * callback, and probes the forbidden reentrant calls exactly once */
static RantNode *th_echo_b;
static RantTopic *th_echo_rep;
static volatile unsigned long th_echo_replies;
static volatile int th_cb_send_rc = -100, th_cb_create_refused = -1, th_cb_setrole_rc = -100;
static void th_echo_b_on_msg(const RantMsg *m){
    th_cb_send_rc = rant_topic_send(th_echo_rep, m->data, NULL);
    if (th_cb_create_refused < 0){
        th_cb_create_refused = (rant_node_create_topic(th_echo_b, "th/na", RANT_PUBSUB, NULL, NULL) == NULL);
        th_cb_setrole_rc = rant_topic_set_role(th_echo_rep, RANT_PUB_ONLY);
    }
}
static void th_echo_a_on_msg(const RantMsg *m){ (void)m; th_echo_replies++; }

/* phase 24: hammer a reliable topic until told to stop */
static volatile int th_hammer_stop;
static void th_hammer(void *arg){
    uint8_t buf[8]; memset(buf, 0x77, sizeof buf);
    while (!th_hammer_stop) rant_topic_send((RantTopic*)arg, rant_bytes(buf, sizeof buf), NULL);
}

static void threaded_checks(void){
    static uint8_t dummy[1];
    uint8_t payload[16]; unsigned i;
    memset(payload, 0x33, sizeof payload);

    /* 20. RELIABLE: exactly-once ordered delivery under multithreaded send */
    { RantTopicDef cd[1]; RantNodeOpts o; RantNode *w, *r;
      memset(cd, 0, sizeof cd);
      cd[0].name = "th/rel"; cd[0].role = RANT_PUB_ONLY;
      cd[0].qos.reliability = RANT_RELIABLE; cd[0].qos.keep_last = 8;
      cd[0].qos.max_message_bytes = 32; cd[0].qos.heartbeat_us = 50000;
      cd[0].qos.backpressure_wait_us = 500000;
      memset(&o, 0, sizeof o);
      o.domain = ST_DOMAIN+4; o.disable_shm = 1; o.discovery.max_peers = 4;
      w = test_node_open(dummy, 0, "th-pub", NULL, th_on_event, o, cd, 1);
      cd[0].role = RANT_SUB_ONLY;
      r = test_node_open(dummy, 0, "th-sub", th_on_message, th_on_event, o, cd, 1);
      ST_CHECK(w && r, "threaded: nodes open");
      if (w && r){
          th_recv = th_order_bad = th_lost = th_evicted_evt = 0;
          memset(th_next_seq, 0, sizeof th_next_seq);
          /* ST_CHECK evaluates its condition twice: keep side effects out of it */
          { int s1 = rant_node_start(w), s2 = rant_node_start(r), again, poll_rc;
            again = rant_node_start(w);
            poll_rc = rant_node_poll(w, 0);
            ST_CHECK(s1 == RANT_OK && s2 == RANT_OK, "threaded: service threads start");
            ST_CHECK(again == RANT_ERR_STATE, "threaded: double start refused");
            ST_CHECK(poll_rc == RANT_ERR_STATE, "threaded: foreign poll refused while started");
            ST_CHECK(rant_node_is_started(w) == 1, "threaded: is_started");
          }
          /* the service threads own discovery + matching: no polling from here on */
          { uint64_t end = i_rant_plat_now_us() + 5000000u;
            while (rant_node_publisher_match_count(w, 0) == 0 && i_rant_plat_now_us() < end)
                sw_sleep_ms(5); }
          ST_CHECK(rant_node_publisher_match_count(w, 0) == 1, "threaded: match formed by the services");
          {   i_RantThread th[TH_SENDERS]; th_sender_arg ta[TH_SENDERS]; uint32_t t;
              for (t=0;t<TH_SENDERS;t++){
                  ta[t].ch = rant_node_topic(w, 0); ta[t].id = t;
                  i_rant_plat_thread_start(&th[t], th_sender, &ta[t]);
              }
              for (t=0;t<TH_SENDERS;t++) i_rant_plat_thread_join(&th[t]);
          }
          { int drained = rant_node_drain(w, 0, 10000);
            ST_CHECK(drained == 1, "threaded: drain completes"); }
          { uint64_t end = i_rant_plat_now_us() + 3000000u;      /* delivered before acked, settle */
            while (th_recv < (unsigned long)TH_SENDERS*TH_MSGS && i_rant_plat_now_us() < end)
                sw_sleep_ms(5); }
          rant_node_stop(r); rant_node_stop(w);                    /* join: counters now settled */
          ST_CHECK(th_recv == (unsigned long)TH_SENDERS*TH_MSGS,
                   "threaded: exactly-once delivery (%lu, want %lu)",
                   th_recv, (unsigned long)TH_SENDERS*TH_MSGS);
          ST_CHECK(th_order_bad == 0, "threaded: per-thread order kept (bad=%lu)", th_order_bad);
          ST_CHECK(th_lost == 0, "threaded: no loss (lost=%lu)", th_lost);
          ST_CHECK(th_evicted_evt == 0 && rant_node_evicted_unsent(w) == 0,
                   "threaded: no unsent eviction (evt=%lu cnt=%u)",
                   th_evicted_evt, rant_node_evicted_unsent(w));
          rant_node_close(r, 1); rant_node_close(w, 1);
      }
    }

    /* 21 + 21b + 22. BURST / HOSTILE / WAKER on one best-effort pair */
    { RantTopicDef cd[1]; RantNodeOpts o; RantNode *w, *r;
      memset(cd, 0, sizeof cd);
      cd[0].name = "th/burst"; cd[0].role = RANT_PUB_ONLY;
      cd[0].qos.keep_last = 4;   /* best-effort */
      cd[0].qos.max_message_bytes = 32; cd[0].qos.heartbeat_us = 50000;
      memset(&o, 0, sizeof o);
      o.domain = ST_DOMAIN+5; o.disable_shm = 1; o.discovery.max_peers = 4;
      w = test_node_open(dummy, 0, "th-bpub", NULL, th_on_event, o, cd, 1);
      cd[0].role = RANT_SUB_ONLY;
      r = test_node_open(dummy, 0, "th-bsub", th_on_message, th_on_event, o, cd, 1);
      ST_CHECK(w && r, "burst: nodes open");
      if (w && r){
          th_recv = th_lost = th_evicted_evt = 0;
          rant_node_start(w); rant_node_start(r);
          { uint64_t end = i_rant_plat_now_us() + 5000000u;
            while (rant_node_publisher_match_count(w, 0) == 0 && i_rant_plat_now_us() < end)
                sw_sleep_ms(5); }

          /* 21. 64 back-to-back sends, keep_last 4: every one must reach the wire, since
             each send transmits before it returns */
          for (i=0;i<64;i++) rant_node_send(w, 0, payload, sizeof payload);
          { uint64_t end = i_rant_plat_now_us() + 3000000u;
            while (th_recv < 64 && i_rant_plat_now_us() < end) sw_sleep_ms(5); }
          ST_CHECK(th_recv == 64, "burst: all 64 delivered past a depth-4 ring (%lu)", th_recv);
          ST_CHECK(rant_node_evicted_unsent(w) == 0, "burst: nothing evicted unsent (%u)",
                   rant_node_evicted_unsent(w));

          /* 22. WAKER: quiesce, then one send must land well inside the announce capped sleep.
             Typical is sub ms, the margin absorbs a loaded CI box losing the CPU. */
          sw_sleep_ms(300);
          { unsigned long r0 = th_recv; uint64_t t0 = i_rant_plat_now_us(), dt;
            rant_node_send(w, 0, payload, sizeof payload);
            while (th_recv == r0 && i_rant_plat_now_us() - t0 < 1000000u) { /* spin */ }
            dt = i_rant_plat_now_us() - t0;
            ST_CHECK(th_recv == r0+1 && dt < 400000u,
                     "waker: idle-node send delivered in %.1f ms", dt/1000.0);
          }

#ifdef _WIN32
          /* 21b. HOSTILE: transport TX forced to would block. The first pass parks one datagram in
             tx_hold, past that the burst overwrites unsent history, which must surface as evicted. */
          { unsigned long r0 = th_recv; uint32_t e0 = rant_node_evicted_unsent(w);
            uint32_t evicted;
            g_tx_block_data = 1;
            for (i=0;i<64;i++) rant_node_send(w, 0, payload, sizeof payload);
            evicted = rant_node_evicted_unsent(w) - e0;
            ST_CHECK(evicted >= 1, "hostile: blocked TX surfaces RANT_E_EVICTED_UNSENT (%u)", evicted);
            g_tx_block_data = 0;
            /* the service retries the held datagram and drains the ring on its next pass, which
               the announce cadence bounds */
            { uint64_t end = i_rant_plat_now_us() + 3000000u;
              while (th_recv - r0 + evicted < 64 && i_rant_plat_now_us() < end) sw_sleep_ms(10); }
            ST_CHECK(th_recv - r0 + (unsigned long)evicted == 64,
                     "hostile: every send delivered or accounted (recv=%lu evicted=%u)",
                     th_recv - r0, evicted);
          }
#endif
          /* lock/unlock smoke: bracket a peer-view read while the services run */
          { uint16_t cnt = 0;
            rant_node_lock(w);
            (void)st_peers(w, &cnt);
            rant_node_unlock(w);
            ST_CHECK(cnt >= 1, "lock: bracketed peer view reads (%u peers)", cnt);
          }
          rant_node_close(r, 1); rant_node_close(w, 1);
      }
    }

    /* 22b. CAPPED: a rate capped subscriber holds samples back on its lane, which must never
       slow a depth 1 publisher. A send that waits for the lane's tick takes 20 ms here. */
    { RantTopicDef cd[1]; RantNodeOpts o; RantNode *w, *r; uint8_t payload[16];
      memset(payload, 0, sizeof payload);
      memset(cd, 0, sizeof cd);
      cd[0].name = "th/capped"; cd[0].role = RANT_PUB_ONLY;
      cd[0].qos.keep_last = 1; cd[0].qos.max_message_bytes = 32;
      memset(&o, 0, sizeof o);
      o.domain = ST_DOMAIN+12; o.disable_shm = 1; o.discovery.max_peers = 4;
      w = test_node_open(dummy, 0, "th-cpub", NULL, th_on_event, o, cd, 1);
      cd[0].role = RANT_SUB_ONLY; cd[0].qos.max_rate_hz = 50;
      r = test_node_open(dummy, 0, "th-csub", th_on_message, th_on_event, o, cd, 1);
      ST_CHECK(w && r, "capped: nodes open");
      if (w && r){
          uint64_t worst = 0; int i;
          th_recv = 0;
          rant_node_start(w); rant_node_start(r);
          { uint64_t end = i_rant_plat_now_us() + 5000000u;
            while (rant_node_publisher_match_count(w, 0) == 0 && i_rant_plat_now_us() < end)
                sw_sleep_ms(5); }
          sw_sleep_ms(300);                     /* the rate section rides an announce */
          for (i=0;i<200;i++){
              uint64_t t0 = i_rant_plat_now_us(), dt;
              rant_node_send(w, 0, payload, sizeof payload);
              dt = i_rant_plat_now_us() - t0;
              if (dt > worst) worst = dt;
              sw_sleep_ms(1);
          }
          ST_CHECK(worst < 10000u, "capped: no send waits for the capped lane (worst %.2f ms)",
                   worst/1000.0);
          ST_CHECK(th_recv >= 2 && th_recv < 100, "capped: the subscriber is still paced (%lu of 200)",
                   th_recv);
          ST_CHECK(rant_node_evicted_unsent(w) == 0, "capped: a held back sample is not an eviction (%u)",
                   rant_node_evicted_unsent(w));
          rant_node_close(r, 1); rant_node_close(w, 1);
      }
    }

    /* 23. REENTRANT: echo from the callback, forbidden calls refuse loudly. The ring must be
       deeper than the request burst since a reentrant send never waits for a TX pass. */
    { RantTopicDef ca[2], cb[2]; RantNodeOpts o; RantNode *a, *b;
      memset(ca, 0, sizeof ca);
      ca[0].name = "th/req"; ca[0].role = RANT_PUB_ONLY;
      ca[0].qos.reliability = RANT_RELIABLE; ca[0].qos.keep_last = 16;
      ca[0].qos.max_message_bytes = 32; ca[0].qos.heartbeat_us = 50000;
      ca[1] = ca[0]; ca[1].name = "th/rep"; ca[1].role = RANT_SUB_ONLY;
      memcpy(cb, ca, sizeof ca);
      cb[0].role = RANT_SUB_ONLY; cb[1].role = RANT_PUB_ONLY;
      memset(&o, 0, sizeof o);
      o.domain = ST_DOMAIN+6; o.disable_shm = 1; o.discovery.max_peers = 4;
      a = test_node_open(dummy, 0, "th-echo-a", th_echo_a_on_msg, NULL, o, ca, 2);
      b = test_node_open(dummy, 0, "th-echo-b", th_echo_b_on_msg, NULL, o, cb, 2);
      ST_CHECK(a && b, "reentrant: nodes open");
      if (a && b){
          th_echo_b = b; th_echo_rep = rant_node_topic(b, 1);
          th_echo_replies = 0; th_cb_send_rc = -100; th_cb_create_refused = -1; th_cb_setrole_rc = -100;
          rant_node_start(a); rant_node_start(b);
          { uint64_t end = i_rant_plat_now_us() + 5000000u;
            while ((rant_node_publisher_match_count(a, 0) == 0 || rant_node_publisher_match_count(b, 1) == 0)
                   && i_rant_plat_now_us() < end)
                sw_sleep_ms(5); }
          for (i=0;i<10;i++) rant_node_send(a, 0, payload, sizeof payload);
          { uint64_t end = i_rant_plat_now_us() + 3000000u;
            while (th_echo_replies < 10 && i_rant_plat_now_us() < end) sw_sleep_ms(5); }
          rant_node_stop(b); rant_node_stop(a);
          ST_CHECK(th_echo_replies == 10, "reentrant: callback send echoes (%lu/10)", th_echo_replies);
          ST_CHECK(th_cb_send_rc == RANT_OK, "reentrant: callback send returns RANT_OK (%d)", th_cb_send_rc);
          ST_CHECK(th_cb_create_refused == 1, "reentrant: callback create_topic refused");
          ST_CHECK(th_cb_setrole_rc == RANT_ERR_STATE, "reentrant: callback set_role refused (%d)", th_cb_setrole_rc);
          rant_node_close(b, 1); rant_node_close(a, 1);
      }
    }

    /* 24. STOP-UNDER-LOAD: hammer threads parked in the backpressure wait while stop
       broadcasts them loose, repeated under a watchdog. A hang here is the deadlock detector. */
    { uint64_t phase_t0 = i_rant_plat_now_us(); int iter;
      for (iter=0; iter<3; iter++){
          RantTopicDef cd[1]; RantNodeOpts o; RantNode *w, *r;
          i_RantThread h1, h2;
          memset(cd, 0, sizeof cd);
          cd[0].name = "th/stop"; cd[0].role = RANT_PUB_ONLY;
          cd[0].qos.reliability = RANT_RELIABLE; cd[0].qos.keep_last = 4;
          cd[0].qos.max_message_bytes = 32; cd[0].qos.heartbeat_us = 50000;
          cd[0].qos.backpressure_wait_us = 300000;
          memset(&o, 0, sizeof o);
          o.domain = (uint16_t)(ST_DOMAIN+7+iter); o.disable_shm = 1; o.discovery.max_peers = 4;
          w = test_node_open(dummy, 0, "th-hpub", NULL, NULL, o, cd, 1);
          cd[0].role = RANT_SUB_ONLY;
          r = test_node_open(dummy, 0, "th-hsub", NULL, NULL, o, cd, 1);
          if (!w || !r){ ST_CHECK(0, "stop-load: nodes open (iter %d)", iter); break; }
          { uint64_t end = i_rant_plat_now_us() + 5000000u;     /* match, then silence the reader */
            while (rant_node_publisher_match_count(w, 0) == 0 && i_rant_plat_now_us() < end)
                st_pump(w, r, 10); }
          rant_node_start(w);                      /* reader stays unpolled: never acks */
          th_hammer_stop = 0;
          i_rant_plat_thread_start(&h1, th_hammer, rant_node_topic(w, 0));
          i_rant_plat_thread_start(&h2, th_hammer, rant_node_topic(w, 0));
          sw_sleep_ms(50);                         /* hammers now parked in the wait */
          rant_node_stop(w);                       /* broadcasts the waiters loose */
          th_hammer_stop = 1;
          i_rant_plat_thread_join(&h1);
          i_rant_plat_thread_join(&h2);
          rant_node_close(r, 0);
          rant_node_close(w, 0);
      }
      ST_CHECK(i_rant_plat_now_us() - phase_t0 < 30000000u,
               "stop-load: 3 stop-under-fire cycles, no hang (%.1f s)",
               (i_rant_plat_now_us() - phase_t0)/1e6);
    }
}
#endif /* RANT_THREADS */

/* The consumer queue phases Q1 to Q5 over one manually pumped pair, a pub only A and a
 * sub only B (spec/testing.md). */
static unsigned long qc_dispatched;
static void qc_on_message(const RantMsg *msg){ (void)msg; qc_dispatched++; }

static void queue_checks(void){
    static uint8_t mem_a[1], mem_b[1];
    uint8_t payload[512];
    RantTopicDef ca[4], cb[4];
    RantNodeOpts ao, bo;
    RantNode *a, *b;
    RantTopic *lazy, *be, *rel, *disp;
    RantMsg m;
    int i, r, got;
    memset(ca, 0, sizeof ca); memset(payload, 0, sizeof payload); memset(&m, 0, sizeof m);
    ca[0].name="q/lazy"; ca[0].role=RANT_PUB_ONLY;
    ca[0].qos.reliability=RANT_RELIABLE; ca[0].qos.keep_last=8; ca[0].qos.heartbeat_us=20000;
    ca[1].name="q/be";   ca[1].role=RANT_PUB_ONLY;                    /* best-effort */
    ca[1].qos.keep_last=8;
    ca[2].name="q/rel";  ca[2].role=RANT_PUB_ONLY;
    ca[2].qos.reliability=RANT_RELIABLE; ca[2].qos.keep_last=16;      /* holds the whole burst */
    ca[2].qos.heartbeat_us=20000; ca[2].qos.repair_delay_us=5000;
    ca[3].name="q/disp"; ca[3].role=RANT_PUB_ONLY;
    ca[3].qos.reliability=RANT_RELIABLE; ca[3].qos.keep_last=8; ca[3].qos.heartbeat_us=20000;
    memcpy(cb, ca, sizeof ca);
    for (i=0;i<4;i++) cb[i].role=RANT_SUB_ONLY;
    cb[1].qos.queue_bytes=4096;    /* hard consumer cap: forces overwrite-oldest */
    cb[2].qos.queue_bytes=2048;    /* holds ~4 of the 400 B messages: forces parking */
    cb[3].qos.queue_bytes=65536;   /* queued from creation: the dispatch tests */
    ao = (RantNodeOpts){ .domain=ST_DOMAIN+4, .discovery={ .max_peers=4 } };
    bo = ao;
    st_gap_calls[0]=st_gap_calls[1]=st_gap_calls[2]=st_gap_calls[3]=0;
    a = test_node_open(mem_a, sizeof mem_a, "qa-node", NULL, NULL, ao, ca, 4);
    b = test_node_open(mem_b, sizeof mem_b, "qb-node", qc_on_message, st_on_event, bo, cb, 4);
    ST_CHECK(a && b, "queue: nodes open");
    if (!a || !b){ if (a) rant_node_close(a,0); if (b) rant_node_close(b,0); return; }
    lazy = rant_node_topic(b, 0); be     = rant_node_topic(b, 1);
    rel  = rant_node_topic(b, 2); disp = rant_node_topic(b, 3);

    { uint64_t end = i_rant_plat_now_us()+5000000u;      /* all four matches first */
      while (i_rant_plat_now_us()<end &&
             (rant_node_publisher_match_count(a,0)<1 || rant_node_publisher_match_count(a,1)<1 ||
              rant_node_publisher_match_count(a,2)<1 || rant_node_publisher_match_count(a,3)<1))
          st_pump(a,b,10); }
    ST_CHECK(rant_node_publisher_match_count(a,0)==1 && rant_node_publisher_match_count(a,2)==1,
             "queue: matches formed");

    /* Q1: the first take enables the queue, a timeout take pumps the loop itself */
    r = rant_topic_take(lazy, &m, 0);
    ST_CHECK(r == 0, "queue: first take is empty (rc=%d) and enables queued delivery", r);
    qc_dispatched = 0;
    for (i=0;i<5;i++){ put32(payload,(uint32_t)i); rant_node_send(a, 0, payload, 64); }
    got = 0;
    { uint64_t end = i_rant_plat_now_us()+5000000u;
      while (got<5 && i_rant_plat_now_us()<end){
          rant_node_poll(a, 0);
          if (rant_topic_take(lazy, &m, 50) == 1){      /* waits by pumping b's own loop */
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

    /* Q2: best-effort at a hard cap overwrites oldest, fires RANT_MSG_LOST, keeps newest */
    { uint32_t msgs=0, bytes=0, cap=0, dropped=0, last=0;
      for (i=0;i<60;i++){ put32(payload,(uint32_t)i); rant_node_send(a, 1, payload, 256); st_pump(a,b,1); }
      st_pump(a,b,50);
      rant_topic_queue_stats(be, &msgs, &bytes, &cap, &dropped);
      ST_CHECK(cap==4096 && dropped>0 && msgs>0,
               "queue: BE cap held, oldest dropped (cap=%u msgs=%u dropped=%u)", cap, msgs, dropped);
      got=0;
      while (rant_topic_take(be, &m, 0)==1){ last=get32(m.data.data); got++; }
      ST_CHECK(got>0 && last==59u, "queue: newest survive a BE overflow (got=%d last=%u)", got, last);
      ST_CHECK(st_gap_calls[1]>0, "queue: BE queue loss fired RANT_MSG_LOST (%lu)", st_gap_calls[1]);
    }

    /* Q3: reliable plus a tiny queue parks instead of losing. A slow take loop still
       receives everything in order via unpark and the normal repair machinery */
    got=0;
    for (i=0;i<12;i++){ put32(payload,(uint32_t)i); rant_node_send(a, 2, payload, 400); st_pump(a,b,2); }
    st_pump(a,b,30);
    ST_CHECK(rant_node_drain(a, 2, 0)==0, "queue: parked reader withholds acks (writer not drained)");
    { uint64_t end=i_rant_plat_now_us()+8000000u;
      while (got<12 && i_rant_plat_now_us()<end){
          if (rant_topic_take(rel, &m, 20)==1){
              if ((int)get32(m.data.data)!=got) break;
              got++;
          }
          rant_node_poll(a, 0);
      } }
    ST_CHECK(got==12, "queue: reliable park loses nothing, in order (%d/12)", got);
    { RantRepairStats rs; rant_node_repair_stats(b, 2, &rs);
      ST_CHECK(rs.msgs_skipped==0 && st_gap_calls[2]==0,
               "queue: no skip during park (skipped=%llu lost_events=%lu)",
               (unsigned long long)rs.msgs_skipped, st_gap_calls[2]); }
    st_pump(a,b,50);
    ST_CHECK(rant_node_drain(a, 2, 2000)==1, "queue: writer fully acked once drained");

    /* Q4: dispatch runs the node's on_message on the calling thread */
    qc_dispatched=0;
    for (i=0;i<3;i++){ put32(payload,(uint32_t)i); rant_node_send(a, 3, payload, 64); }
    { uint64_t end=i_rant_plat_now_us()+5000000u; uint32_t msgs=0;
      while (msgs<3 && i_rant_plat_now_us()<end){ st_pump(a,b,10); rant_topic_queue_stats(disp,&msgs,NULL,NULL,NULL); } }
    r = rant_topic_dispatch(disp, 0, 0);
    ST_CHECK(r==3 && qc_dispatched==3, "queue: dispatch runs the callback here (r=%d cb=%lu)", r, qc_dispatched);
    r = rant_node_dispatch(b, 0, 0);
    ST_CHECK(r==0, "queue: node dispatch finds nothing left (%d)", r);

#ifdef RANT_THREADS
    /* Q5: take alongside service threads (the cv-wait path). ST_CHECK evaluates its
       condition twice, so the side-effecting starts run outside it. */
    r = (rant_node_start(a)==RANT_OK && rant_node_start(b)==RANT_OK);
    ST_CHECK(r, "queue: services start");
    for (i=0;i<40;i++){
        put32(payload,(uint32_t)i);
        rant_node_send(a, 3, payload, 64);
        if (rant_topic_take(disp, &m, 2000)!=1 || (int)get32(m.data.data)!=i) break;
    }
    ST_CHECK(i==40, "queue: threaded take (cv wait) delivers 40 in order (%d)", i);
    rant_node_stop(b); rant_node_stop(a);
#endif

    rant_node_close(b, 1);
    rant_node_close(a, 1);
}

/* The callback queue phases (spec/testing.md): a pub only A and a sub only B whose topics
 * cq/x and cq/y share one queue while cq/in stays inline. */
static RantNode  *cq_b;
static RantQueue *cq_q;
static RantTopic *cq_x, *cq_y;
static unsigned long cq_seen[3];
static uint32_t   cq_order[32]; static int cq_n_order;
static int        cq_nested_rc, cq_retire_self_rc, cq_retire_other_rc, cq_close_rc, cq_inline_rc;
static uint64_t   cq_thread;
static int        cq_ev_n; static RantErrorKind cq_ev_error; static char cq_ev_topic[64];
static void cq_on_event(const RantEvent *ev){
    st_on_event(ev);
    cq_ev_n++;
    if (ev->kind == RANT_ERROR){
        cq_ev_error = ev->error;
        cq_ev_topic[0] = 0;
        if (ev->topic_name){ strncpy(cq_ev_topic, ev->topic_name, sizeof cq_ev_topic - 1); cq_ev_topic[sizeof cq_ev_topic - 1] = 0; }
    }
}
static void cq_on_message(const RantMsg *m){
    uint32_t v = get32(m->data.data);
    if (m->topic_index < 3) cq_seen[m->topic_index]++;
    if (cq_n_order < 32) cq_order[cq_n_order++] = v;
#ifdef RANT_THREADS
    cq_thread = i_rant_plat_thread_id();
#endif
    if (m->topic_index == 2){                      /* inline: the lock is held by this thread */
        cq_inline_rc = rant_queue_dispatch(cq_q, 0, 0);
        return;
    }
    if (v == 1){                                    /* the first y record: the refusals */
        cq_nested_rc      = rant_queue_dispatch(cq_q, 0, 0);
        cq_retire_self_rc = rant_topic_retire(cq_y);
        cq_close_rc       = rant_node_close(cq_b, 0);
    }
    if (v == 5) cq_retire_other_rc = rant_topic_retire(cq_y);   /* from x's callback, y idle */
}

static void callback_queue_checks(void){
    static uint8_t mem_a[1], mem_b[1];
    uint8_t payload[64];
    RantTopicDef ca[3];
    RantNodeOpts ao, bo;
    RantNode *a, *b;
    RantTopic *in;
    RantTopicOpts co;
    uint32_t waiting = 0, dropped = 0;
    int i, r;
    memset(ca, 0, sizeof ca); memset(payload, 0, sizeof payload);
    ca[0].name="cq/x";  ca[0].role=RANT_PUB_ONLY; ca[0].qos.reliability=RANT_RELIABLE;
    ca[0].qos.keep_last=8; ca[0].qos.heartbeat_us=20000;
    ca[1] = ca[0]; ca[1].name="cq/y";
    ca[2] = ca[0]; ca[2].name="cq/in";
    ao = (RantNodeOpts){ .domain=ST_DOMAIN+5, .discovery={ .max_peers=4 } };
    bo = ao; bo.max_topics = 3;
    a = test_node_open(mem_a, sizeof mem_a, "cqa-node", NULL, NULL, ao, ca, 3);
    b = test_node_open(mem_b, sizeof mem_b, "cqb-node", cq_on_message, cq_on_event, bo, NULL, 0);
    ST_CHECK(a && b, "cqueue: nodes open");
    if (!a || !b){ if (a) rant_node_close(a,0); if (b) rant_node_close(b,0); return; }
    cq_b = b;
    cq_q = rant_node_create_queue(b);
    ST_CHECK(cq_q != NULL, "cqueue: queue created");
    memset(&co, 0, sizeof co); co.qos = ca[0].qos; co.queue = cq_q;
    cq_x = rant_node_create_topic(b, "cq/x", RANT_SUB_ONLY, NULL, &co);
    cq_y = rant_node_create_topic(b, "cq/y", RANT_SUB_ONLY, NULL, &co);
    co.queue = NULL;
    in = rant_node_create_topic(b, "cq/in", RANT_SUB_ONLY, NULL, &co);
    ST_CHECK(cq_x && cq_y && in, "cqueue: topics created (x=%d y=%d in=%d)", !!cq_x, !!cq_y, !!in);
    if (!cq_x || !cq_y || !in){ rant_node_close(b,0); rant_node_close(a,0); return; }

    /* a queue of another node is refused loudly */
    co.queue = cq_q;
    ST_CHECK(rant_node_create_topic(a, "cq/foreign", RANT_SUB_ONLY, NULL, &co) == NULL
             && rant_last_error(a).error == RANT_E_STATE,
             "cqueue: another node's queue refused with STATE (%d)", (int)rant_last_error(a).error);
    /* the queue budget */
    for (i = 1; i < RANT_QUEUES_MAX; i++) if (!rant_node_create_queue(b)) break;
    ST_CHECK(i == RANT_QUEUES_MAX && rant_node_create_queue(b) == NULL
             && rant_last_error(b).error == RANT_E_STATE,
             "cqueue: the %dth queue is refused with STATE (made %d)", RANT_QUEUES_MAX + 1, i);

    { uint64_t end = i_rant_plat_now_us()+5000000u;
      while (i_rant_plat_now_us()<end &&
             (rant_node_publisher_match_count(a,0)<1 || rant_node_publisher_match_count(a,1)<1 ||
              rant_node_publisher_match_count(a,2)<1))
          st_pump(a,b,10); }
    ST_CHECK(rant_node_publisher_match_count(a,0)==1 && rant_node_publisher_match_count(a,1)==1
             && rant_node_publisher_match_count(a,2)==1, "cqueue: matches formed");

    /* CQ1: six sends in a known arrival order, x0 y1 x2 in3 y4 x5. The inline one fires
       during the poll, the queued ones wait */
    memset(cq_seen, 0, sizeof cq_seen); cq_n_order = 0; cq_inline_rc = 1;
    {   static const uint16_t on[6] = { 0, 1, 0, 2, 1, 0 };
        for (i=0;i<6;i++){ put32(payload,(uint32_t)i); rant_node_send(a, on[i], payload, 64); st_pump(a,b,5); }
    }
    st_pump(a,b,30);
    rant_queue_stats(cq_q, &waiting, &dropped);
    ST_CHECK(cq_seen[2]==1 && cq_seen[0]==0 && cq_seen[1]==0 && waiting==5 && dropped==0,
             "cqueue: inline fired in the poll, 5 parked (in=%lu x=%lu y=%lu waiting=%u)",
             cq_seen[2], cq_seen[0], cq_seen[1], waiting);
#ifdef RANT_THREADS
    ST_CHECK(cq_inline_rc == RANT_ERR_STATE, "cqueue: dispatch from an inline callback is refused (%d)", cq_inline_rc);
#endif

    /* CQ2: a capped dispatch runs the two oldest, across topics, in arrival order */
    cq_n_order = 0;
    r = rant_queue_dispatch(cq_q, 2, 0);
    ST_CHECK(r==2 && cq_n_order==2 && cq_order[0]==0 && cq_order[1]==1,
             "cqueue: max 2 ran x0 then y1 (r=%d got %u %u)", r, cq_order[0], cq_order[1]);
    ST_CHECK(cq_nested_rc == RANT_ERR_STATE, "cqueue: nested dispatch refused (%d)", cq_nested_rc);
    ST_CHECK(cq_retire_self_rc == RANT_ERR_STATE, "cqueue: retire from its own callback refused (%d)", cq_retire_self_rc);
    ST_CHECK(cq_close_rc == RANT_ERR_STATE, "cqueue: close from a dispatched callback refused (%d)", cq_close_rc);
    r = rant_queue_dispatch(cq_q, 0, 0);
    ST_CHECK(r==3 && cq_n_order==5 && cq_order[2]==2 && cq_order[3]==4 && cq_order[4]==5,
             "cqueue: the rest ran in arrival order (r=%d got %u %u %u)", r, cq_order[2], cq_order[3], cq_order[4]);
    ST_CHECK(cq_retire_other_rc == RANT_OK, "cqueue: retire of an idle sibling from a callback (%d)", cq_retire_other_rc);
    r = rant_queue_dispatch(cq_q, 0, 0);
    ST_CHECK(r==0, "cqueue: nothing left (%d)", r);
    { uint64_t t0 = i_rant_plat_now_us();
      r = rant_queue_dispatch(cq_q, 0, 30);
      ST_CHECK(r==0 && i_rant_plat_now_us()-t0 >= 25000u, "cqueue: a timed wait pumps and returns 0 (%d)", r); }

    /* CQ3: y comes back on the queue in its retired slot, and a retire from outside drops
       what it had not run yet */
    co.queue = cq_q;
    cq_y = rant_node_create_topic(b, "cq/y", RANT_SUB_ONLY, NULL, &co);
    ST_CHECK(cq_y != NULL, "cqueue: y recreated on the queue");
    /* a must process the hole and the recreate before the first send: the lane re forms
       future only, so a message committed to the old reader is a late subscriber's loss */
    st_pump(a,b,300);
    for (i=0;i<3;i++){ put32(payload,100u+(uint32_t)i); rant_node_send(a, 1, payload, 64); st_pump(a,b,5); }
    { uint64_t end = i_rant_plat_now_us()+3000000u;         /* the reformed lane may still repair */
      do { st_pump(a,b,10); rant_queue_stats(cq_q, &waiting, NULL); } while (waiting<3 && i_rant_plat_now_us()<end); }
    r = cq_y ? rant_topic_retire(cq_y) : -1;
    ST_CHECK(waiting==3 && r==RANT_OK, "cqueue: 3 parked, retire drops them (waiting=%u rc=%d)", waiting, r);
    rant_queue_stats(cq_q, &waiting, NULL);
    ST_CHECK(waiting==0, "cqueue: nothing waiting after the retire (%u)", waiting);

#ifdef RANT_THREADS
    /* CQ4: under service threads a timed dispatch wakes on arrival and runs the callback on
       the caller's thread. ST_CHECK evaluates twice, so the starts run outside it. */
    r = (rant_node_start(a)==RANT_OK && rant_node_start(b)==RANT_OK);
    ST_CHECK(r, "cqueue: services start");
    cq_n_order = 0; cq_thread = 0;
    for (i=0;i<20;i++){
        put32(payload,200u+(uint32_t)i);
        rant_node_send(a, 0, payload, 64);
        if (rant_queue_dispatch(cq_q, 0, 2000) < 1 || cq_order[cq_n_order-1] != 200u+(uint32_t)i) break;
    }
    ST_CHECK(i==20 && cq_thread == i_rant_plat_thread_id(),
             "cqueue: threaded dispatch delivers 20 in order on this thread (%d)", i);
    rant_node_stop(b); rant_node_stop(a);
#endif
    /* CQ5: events park on the event queue, last_error stays current, the strings are
       copied, and NULL drops what is parked */
    r = rant_node_set_event_queue(b, cq_q);
    ST_CHECK(r==RANT_OK, "cqueue: event queue set (%d)", r);
    cq_ev_n = 0; cq_ev_error = RANT_E_NONE;
    ST_CHECK(rant_node_create_topic(b, "@bad", RANT_SUB_ONLY, NULL, NULL)==NULL
             && rant_last_error(b).error==RANT_E_BAD_NAME && cq_ev_n==0,
             "cqueue: error event parked, last_error current (n=%d err=%d)", cq_ev_n, (int)rant_last_error(b).error);
    rant_queue_stats(cq_q, &waiting, NULL);
    r = rant_queue_dispatch(cq_q, 0, 0);
    ST_CHECK(waiting==1 && r==1 && cq_ev_n==1 && cq_ev_error==RANT_E_BAD_NAME && strcmp(cq_ev_topic, "@bad")==0,
             "cqueue: event dispatched with its copied name (r=%d n=%d name=%s)", r, cq_ev_n, cq_ev_topic);
    (void)rant_node_create_topic(b, "@bad2", RANT_SUB_ONLY, NULL, NULL);
    rant_queue_stats(cq_q, &waiting, NULL);
    r = rant_node_set_event_queue(b, NULL);
    ST_CHECK(waiting==1 && r==RANT_OK && cq_ev_n==1, "cqueue: NULL drops the parked event (rc=%d n=%d)", r, cq_ev_n);
    rant_queue_stats(cq_q, &waiting, NULL);
    (void)rant_node_create_topic(b, "@bad3", RANT_SUB_ONLY, NULL, NULL);
    ST_CHECK(waiting==0 && cq_ev_n==2, "cqueue: events inline again (waiting=%u n=%d)", waiting, cq_ev_n);

    rant_node_close(b, 1);
    rant_node_close(a, 1);
}

/* The callback queue pattern phases (spec/testing.md): a provider P and a caller C, each
 * with one queue, so every pattern callback parks and runs at a dispatch. */
static RantQueue    *cp_pq, *cp_cq;
static RantFunction *cp_def, *cp_tdef;
static int      cp_req_runs, cp_req_retire_rc;
static uint64_t cp_req_thread, cp_token;
static int      cp_rsp_done, cp_cancel_calls, cp_prog_n, cp_prog_first_empty;
static int      cp_change_n, cp_write_n;
static uint32_t cp_rsp_val, cp_prog_last, cp_change_val, cp_write_val, cp_write_src;
static RantCallStatus cp_rsp_status;
static uint32_t cp_sum(const RantRequest *req){
    return req->data.len >= 8 ? i_rant_le_r32(req->data.data) + i_rant_le_r32(req->data.data + 4) : 0u;
}
static void cp_add_handler(RantRequest *req, void *user){
    uint8_t out[4]; (void)user;
    cp_req_runs++;
#ifdef RANT_THREADS
    cp_req_thread = i_rant_plat_thread_id();
#endif
    cp_req_retire_rc = rant_function_retire(cp_def);   /* its own dispatched callback: refused */
    i_rant_le_w32(out, cp_sum(req));
    rant_request_reply(req, rant_bytes(out, 4));
}
static void cp_inline_handler(RantRequest *req, void *user){
    uint8_t out[4]; (void)user;
    i_rant_le_w32(out, cp_sum(req));
    rant_request_reply(req, rant_bytes(out, 4));
}
static void cp_on_reply(const RantResponse *r){
    cp_rsp_done++; cp_rsp_status = r->status;
    cp_rsp_val = r->data.len >= 4 ? i_rant_le_r32(r->data.data) : 0u;
}
static void cp_task_handler(RantRequest *req, void *user){ (void)user; cp_token = rant_request_defer(req); }
static void cp_on_cancel(uint64_t token, void *user){ (void)token; (void)user; cp_cancel_calls++; }
static void cp_on_progress(const RantProgress *p){
    if (cp_prog_n == 0) cp_prog_first_empty = (p->data.len == 0);
    cp_prog_n++;
    if (p->data.len >= 4) cp_prog_last = i_rant_le_r32(p->data.data);
}
static void cp_on_change(const RantVariableUpdate *u, void *user){
    (void)user; cp_change_n++;
    cp_change_val = u->value.len >= 4 ? i_rant_le_r32(u->value.data) : 0u;
}
static void cp_on_write(const RantVariableUpdate *u, void *user){
    (void)user; cp_write_n++; cp_write_src = u->source;
    cp_write_val = u->value.len >= 4 ? i_rant_le_r32(u->value.data) : 0u;
}
/* pumps both nodes until the queue holds a record, up to about a second */
static uint32_t cp_wait_parked(RantNode *P, RantNode *C, RantQueue *q){
    uint32_t waiting = 0; int t;
    for (t = 0; t < 500; t++){ st_pump(P, C, 2); rant_queue_stats(q, &waiting, NULL); if (waiting) break; }
    return waiting;
}

static void callback_pattern_checks(void){
    RantAllocator pa = rant_allocator_heap(0);
    RantAllocator ca = rant_allocator_heap(0);
    RantNodeOpts po, co; RantNode *P=NULL, *C=NULL; RantDiscoveryAddr seed;
    RantFunction *cadd, *ctask, *cnone, *pinl, *cinl;
    RantVariable *pvar, *cvar;
    uint32_t waiting, id = 0; int t, r, r2;
    uint8_t req[8], val[4];
    uint16_t dom = ST_DOMAIN+40;

    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=dom; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    co=po;
    P = rant_node_open(&pa, "cq-prov", NULL, NULL, &po);
    C = rant_node_open(&ca, "cq-call", NULL, NULL, &co);
    ST_CHECK(P && C, "cpat: nodes open");
    if (!(P && C)){ if(P)rant_node_close(P,0); if(C)rant_node_close(C,0); return; }
    cp_pq = rant_node_create_queue(P); cp_cq = rant_node_create_queue(C);
    i_rant_le_w32(val, 7);
    cp_def  = rant_node_create_function_definition(P, "cq/add", NULL, NULL, cp_add_handler, NULL,
                                                   &(RantFunctionOpts){ .queue = cp_pq });
    cadd    = rant_node_create_remote_function(C, "cq/add", NULL, NULL, &(RantFunctionOpts){ .queue = cp_cq });
    cp_tdef = rant_node_create_task_definition(P, "cq/job", NULL, NULL, NULL, cp_task_handler, NULL,
                                               &(RantTaskOpts){ .queue = cp_pq });
    ctask   = rant_node_create_remote_task(C, "cq/job", NULL, NULL, NULL, &(RantTaskOpts){ .queue = cp_cq });
    cnone   = rant_node_create_remote_function(C, "cq/none", NULL, NULL,
                                               &(RantFunctionOpts){ .queue = cp_cq, .timeout_us = 150000u });
    pinl    = rant_node_create_function_definition(P, "cq/inl", NULL, NULL, cp_inline_handler, NULL, NULL);
    cinl    = rant_node_create_remote_function(C, "cq/inl", NULL, NULL, &(RantFunctionOpts){ .queue = cp_cq });
    pvar    = rant_node_create_variable_definition(P, "cq/var", NULL,
                                                   &(RantVariableOpts){ .queue = cp_pq, .initial = rant_bytes(val, 4) });
    cvar    = rant_node_create_remote_variable(C, "cq/var", NULL, &(RantVariableOpts){ .queue = cp_cq });
    ST_CHECK(cp_pq && cp_cq && cp_def && cadd && cp_tdef && ctask && cnone && pinl && cinl && pvar && cvar,
             "cpat: queues and handles created");
    if (!(cp_pq && cp_cq && cp_def && cadd && cp_tdef && ctask && cnone && pinl && cinl && pvar && cvar)){
        rant_node_close(C,0); rant_node_close(P,0); return;
    }
    rant_function_on_cancel(cp_tdef, cp_on_cancel, NULL);
    rant_variable_on_write(pvar, cp_on_write, NULL);
    for (t=0;t<2000 && (rant_function_match_count(cadd)==0 || rant_function_match_count(ctask)==0
                        || rant_function_match_count(cinl)==0 || !rant_variable_get(cvar, NULL));t++)
        st_pump(P,C,2);
    ST_CHECK(rant_function_match_count(cadd)==1 && rant_variable_get(cvar, NULL),
             "cpat: providers matched, the initial value cached (%d)", rant_function_match_count(cadd));

    /* CP1: the request parks at the provider, the reply parks at the caller */
    i_rant_le_w32(req,2); i_rant_le_w32(req+4,3);
    cp_req_runs=0; cp_rsp_done=0; cp_req_retire_rc=1;
    r = rant_function_call_async(cadd, rant_bytes(req,8), cp_on_reply, NULL, NULL);
    waiting = cp_wait_parked(P, C, cp_pq);
    ST_CHECK(r==RANT_OK && waiting==1 && cp_req_runs==0,
             "cpat: request parked at the provider (waiting=%u runs=%d)", waiting, cp_req_runs);
    r = rant_queue_dispatch(cp_pq, 0, 0);
    ST_CHECK(r==1 && cp_req_runs==1 && cp_req_retire_rc==RANT_ERR_STATE,
             "cpat: dispatch ran the handler, its own retire refused (r=%d rc=%d)", r, cp_req_retire_rc);
#ifdef RANT_THREADS
    ST_CHECK(cp_req_thread == i_rant_plat_thread_id(), "cpat: handler ran on the dispatching thread");
#endif
    waiting = cp_wait_parked(P, C, cp_cq);
    ST_CHECK(waiting==1 && cp_rsp_done==0, "cpat: reply parked at the caller (waiting=%u done=%d)", waiting, cp_rsp_done);
    r = rant_queue_dispatch(cp_cq, 0, 0);
    ST_CHECK(r==1 && cp_rsp_done==1 && cp_rsp_status==RANT_CALL_OK && cp_rsp_val==5,
             "cpat: dispatched reply add(2,3)=5 (r=%d st=%d val=%u)", r, cp_rsp_status, cp_rsp_val);

#ifdef RANT_THREADS
    /* CP2: the blocking call completes inline through a queued rsp channel, nothing parks */
    {   RantResponse out;
        rant_node_start(P);      /* the inline provider answers on its own thread */
        i_rant_le_w32(req,4); i_rant_le_w32(req+4,5);
        r = rant_function_call(cinl, rant_bytes(req,8), &out, 2000, NULL);
        rant_node_stop(P);
        rant_queue_stats(cp_cq, &waiting, NULL);
        ST_CHECK(r==1 && out.status==RANT_CALL_OK && out.data.len==4 && i_rant_le_r32(out.data.data)==9 && waiting==0,
                 "cpat: blocking call completes inline on a queued handle (r=%d st=%d waiting=%u)", r, out.status, waiting);
    }
#endif

    /* CP3: a synthesized outcome parks like a delivered one */
    cp_rsp_done=0;
    r = rant_function_call_async(cnone, rant_bytes(NULL,0), cp_on_reply, NULL, NULL);
    waiting = cp_wait_parked(P, C, cp_cq);
    ST_CHECK(r==RANT_OK && waiting==1 && cp_rsp_done==0, "cpat: NO_PROVIDER outcome parked (waiting=%u)", waiting);
    r = rant_queue_dispatch(cp_cq, 0, 0);
    ST_CHECK(r==1 && cp_rsp_done==1 && cp_rsp_status==RANT_CALL_NO_PROVIDER,
             "cpat: NO_PROVIDER dispatched (r=%d st=%d)", r, cp_rsp_status);

    /* CP4: a task: RUNNING, progress, the cancel and the terminal outcome each park */
    cp_token=0; cp_prog_n=0; cp_prog_first_empty=0; cp_cancel_calls=0; cp_rsp_done=0; id=0;
    r = rant_function_call_async(ctask, rant_bytes(NULL,0), cp_on_reply, NULL,
                                 &(RantCallOpts){ .on_progress = cp_on_progress, .id_out = &id });
    waiting = cp_wait_parked(P, C, cp_pq);
    r2 = rant_queue_dispatch(cp_pq, 0, 0);          /* the handler defers, RUNNING goes out */
    ST_CHECK(r==RANT_OK && waiting==1 && r2==1 && cp_token!=0, "cpat: task request dispatched and deferred (r=%d)", r2);
    waiting = cp_wait_parked(P, C, cp_cq);
    ST_CHECK(waiting==1 && cp_prog_n==0, "cpat: RUNNING parked (waiting=%u)", waiting);
    r = rant_queue_dispatch(cp_cq, 0, 0);
    ST_CHECK(r==1 && cp_prog_n==1 && cp_prog_first_empty, "cpat: RUNNING dispatched as empty progress (n=%d)", cp_prog_n);
    i_rant_le_w32(val, 42);
    r = rant_function_progress(cp_tdef, cp_token, rant_bytes(val,4));
    waiting = cp_wait_parked(P, C, cp_cq);
    ST_CHECK(r==RANT_OK && waiting==1 && cp_prog_n==1, "cpat: progress parked (waiting=%u)", waiting);
    r = rant_queue_dispatch(cp_cq, 0, 0);
    ST_CHECK(r==1 && cp_prog_n==2 && cp_prog_last==42, "cpat: progress dispatched (last=%u)", cp_prog_last);
    r = rant_function_cancel(ctask, id);
    waiting = cp_wait_parked(P, C, cp_pq);
    ST_CHECK(r==RANT_OK && waiting==1 && cp_cancel_calls==0 && rant_function_cancelled(cp_tdef, cp_token)==1,
             "cpat: cancel flag set at receipt, on_cancel parked (rc=%d waiting=%u)", r, waiting);
    r = rant_queue_dispatch(cp_pq, 0, 0);
    ST_CHECK(r==1 && cp_cancel_calls==1, "cpat: on_cancel dispatched (%d)", cp_cancel_calls);
    r = rant_function_complete(cp_tdef, cp_token, RANT_CALL_CANCELLED, NULL, rant_bytes(NULL,0));
    waiting = cp_wait_parked(P, C, cp_cq);
    r2 = rant_queue_dispatch(cp_cq, 0, 0);
    ST_CHECK(r==RANT_OK && waiting==1 && r2==1 && cp_rsp_done==1 && cp_rsp_status==RANT_CALL_CANCELLED,
             "cpat: terminal CANCELLED dispatched (rc=%d st=%d)", r, cp_rsp_status);

    /* CP5: variables: the replay parks, the owner applies a remote write at receipt and
       notifies at dispatch, the remote caches the new value before its own dispatch */
    cp_change_n=0;
    rant_variable_on_change(cvar, cp_on_change, NULL);
    rant_queue_stats(cp_cq, &waiting, NULL);
    ST_CHECK(waiting==1 && cp_change_n==0, "cpat: on_change replay parked (waiting=%u)", waiting);
    r = rant_queue_dispatch(cp_cq, 0, 0);
    ST_CHECK(r==1 && cp_change_n==1 && cp_change_val==7, "cpat: replay dispatched with the value (val=%u)", cp_change_val);
    i_rant_le_w32(val, 8); cp_write_n=0;
    r = rant_variable_set(cvar, rant_bytes(val,4));
    waiting = cp_wait_parked(P, C, cp_pq);
    {   RantBytes cur; int has = rant_variable_get(pvar, &cur);
        ST_CHECK(r==RANT_OK && waiting==1 && cp_write_n==0 && has && cur.len==4 && i_rant_le_r32(cur.data)==8,
                 "cpat: owner applied the write at receipt, on_write parked (waiting=%u)", waiting); }
    r = rant_queue_dispatch(cp_pq, 0, 0);
    ST_CHECK(r==1 && cp_write_n==1 && cp_write_val==8 && cp_write_src!=0,
             "cpat: on_write dispatched with the write's value and source (val=%u src=%u)", cp_write_val, cp_write_src);
    waiting = cp_wait_parked(P, C, cp_cq);
    {   RantBytes cur; rant_variable_get(cvar, &cur);
        ST_CHECK(waiting==1 && cp_change_n==1 && cur.len==4 && i_rant_le_r32(cur.data)==8,
                 "cpat: remote cached 8 at receipt, on_change parked (waiting=%u)", waiting); }
    r = rant_queue_dispatch(cp_cq, 0, 0);
    ST_CHECK(r==1 && cp_change_n==2 && cp_change_val==8, "cpat: on_change dispatched (val=%u)", cp_change_val);

    /* CP6: a retire with a parked reply answers it CANCELLED inline and drops its records */
    i_rant_le_w32(req,1); i_rant_le_w32(req+4,1);
    cp_rsp_done=0;
    rant_function_call_async(cadd, rant_bytes(req,8), cp_on_reply, NULL, NULL);
    (void)cp_wait_parked(P, C, cp_pq);
    (void)rant_queue_dispatch(cp_pq, 0, 0);
    waiting = cp_wait_parked(P, C, cp_cq);
    r = rant_function_retire(cadd);
    ST_CHECK(waiting==1 && r==RANT_OK && cp_rsp_done==1 && cp_rsp_status==RANT_CALL_CANCELLED,
             "cpat: retire settles the parked reply CANCELLED (rc=%d done=%d st=%d)", r, cp_rsp_done, cp_rsp_status);
    rant_queue_stats(cp_cq, &waiting, NULL);
    ST_CHECK(waiting==0, "cpat: the retired handle's records are gone (%u)", waiting);

    rant_node_close(C, 1);
    rant_node_close(P, 1);
}

/* The function phases: a provider node and a caller node in one process
 * (spec/testing.md). */
static volatile int   pf_reply_done;
static RantCallStatus pf_reply_status;
static uint32_t         pf_reply_val;
static char             pf_reply_msg[RANT_CALL_MSG_MAX + 1]; static size_t pf_reply_msg_len;
static void pf_on_reply(const RantResponse *r){
    pf_reply_status = r->status;
    pf_reply_val = r->data.len>=4 ? i_rant_le_r32(r->data.data) : 0;
    pf_reply_msg_len = r->message.len <= RANT_CALL_MSG_MAX ? r->message.len : RANT_CALL_MSG_MAX;
    if (pf_reply_msg_len) memcpy(pf_reply_msg, r->message.data, pf_reply_msg_len);
    pf_reply_msg[pf_reply_msg_len] = 0;
    pf_reply_done = 1;
}
static int pf_calls;
static void pf_add_handler(RantRequest *req, void *user){
    uint8_t out[4];
    uint32_t v = req->data.len>=4 ? i_rant_le_r32(req->data.data) : 0;
    (void)user; pf_calls++;
    i_rant_le_w32(out, v+1);
    rant_request_reply(req, rant_bytes(out,4));
}
static void pf_empty_handler(RantRequest *req, void *user){ (void)req;(void)user; pf_calls++; }
/* fail with an over long message and a structured payload: pins the wire carried
 * APP_ERROR text, the truncating cap, and that data still rides beside the message */
static void pf_fail_handler(RantRequest *req, void *user){
    char big[301]; int i; uint8_t out[4]; (void)user; pf_calls++;
    for (i=0;i<300;i++) big[i] = (char)('a' + i%26);
    big[300] = 0;
    i_rant_le_w32(out, 13);
    rant_request_fail(req, big, rant_bytes(out,4));
}
/* second caller's reply capture (two-caller directed-isolation test) */
static volatile int   pf_reply2_done;
static RantCallStatus pf_reply2_status;
static uint32_t         pf_reply2_val;
static void pf_on_reply2(const RantResponse *r){
    pf_reply2_status = r->status;
    pf_reply2_val = r->data.len>=4 ? i_rant_le_r32(r->data.data) : 0;
    pf_reply2_done = 1;
}
/* burst capture: counts completions, sums returned values, flags any non-OK */
static volatile int pf_burst_done; static uint32_t pf_burst_sum; static int pf_burst_bad;
static RantCallStatus pf_burst_last_bad;
static void pf_on_reply_burst(const RantResponse *r){
    if (r->status == RANT_CALL_OK && r->data.len>=4) pf_burst_sum += i_rant_le_r32(r->data.data);
    else { pf_burst_bad++; pf_burst_last_bad = r->status; }
    pf_burst_done++;
}
static volatile uint64_t pf_defer_token;
static void pf_defer_handler(RantRequest *req, void *user){ (void)user; pf_calls++; pf_defer_token = rant_request_defer(req); }
/* variable on_change / on_write capture */
typedef struct { int n; uint32_t val, seq, source; uint8_t forced; } PfVarEvt;
static void pf_on_var_update(const RantVariableUpdate *u, void *user){
    PfVarEvt *e = (PfVarEvt*)user;
    e->n++; e->val = u->value.len>=4 ? i_rant_le_r32(u->value.data) : 0;
    e->seq = u->write_seq; e->source = u->source; e->forced = u->forced;
}
/* cancel-at-close capture: a call pending at close must get exactly one CANCELLED outcome */
static volatile int pf_cancel_count; static RantCallStatus pf_cancel_status;
static void pf_on_cancel(const RantResponse *r){ pf_cancel_status = r->status; pf_cancel_count++; }
/* retire from inside a callback must be refused (the role flip would rematch mid-delivery) */
static int pf_retire_in_cb_rc;
static void pf_on_var_retire_attempt(const RantVariableUpdate *u, void *user){
    (void)user;
    pf_retire_in_cb_rc = rant_variable_retire(u->variable);
}
/* reentrant on_write: the first write of 100 immediately re-sets to 200, once */
static RantVariable *pf_reent_var; static int pf_reent_done;
static void pf_on_var_reenter(const RantVariableUpdate *u, void *user){
    (void)user;
    if (!pf_reent_done && u->value.len>=4 && i_rant_le_r32(u->value.data)==100){
        uint8_t nb[4];
        pf_reent_done = 1;
        i_rant_le_w32(nb, 200);
        rant_variable_set(pf_reent_var, rant_bytes(nb,4));
    }
}

/* a write burst from inside a callback: every set after the first runs reentrant, so the
 * value channel's ring is the only thing between the burst and silent loss */
#define PF_VBURST_N 8
static RantVariable *pf_vburst_var; static int pf_vburst_ran;
static void pf_on_var_burst(const RantVariableUpdate *u, void *user){
    uint8_t b[4]; uint32_t i;
    (void)u; (void)user;
    if (pf_vburst_ran) return;
    pf_vburst_ran = 1;
    for (i=1;i<=PF_VBURST_N;i++){ i_rant_le_w32(b,i); rant_variable_set(pf_vburst_var, rant_bytes(b,4)); }
}
static uint32_t pf_vburst_seen;   /* bit per value observed at the remote */
static void pf_on_var_burst_rx(const RantVariableUpdate *u, void *user){
    uint32_t v;
    (void)user;
    if (u->value.len < 4) return;
    v = i_rant_le_r32(u->value.data);
    if (v>=1 && v<=PF_VBURST_N) pf_vburst_seen |= 1u << (v-1);
}

static void pf_pump(RantNode *a, RantNode *b, int ms){
    uint64_t end = i_rant_plat_now_us() + (uint64_t)ms*1000u;
    while (i_rant_plat_now_us() < end){ rant_node_poll(a,2); rant_node_poll(b,2); }
}

static void patterns_checks(void){
    RantAllocator pa = rant_allocator_heap(0);
    RantAllocator ca = rant_allocator_heap(0);
    RantNodeOpts po, co; RantNode *P=NULL, *C=NULL; RantDiscoveryAddr seed;
    RantFunction *prov, *call_add, *pe, *ce, *pd, *cd, *pnh, *cnh, *ghost, *pfm, *cfm;
    uint16_t dom = ST_DOMAIN+20; int t;

    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=dom; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    co=po;
    co.fetch_details = 1;   /* C doubles as the reflection observer: full entity names */
    P = rant_node_open(&pa, "fn-prov", NULL, NULL, &po);
    C = rant_node_open(&ca, "fn-call", NULL, NULL, &co);
    ST_CHECK(P && C, "patterns: nodes open");
    if (!(P && C)){ if(P)rant_node_close(P,0); if(C)rant_node_close(C,0); return; }

    prov = rant_node_create_function_definition(P, "add",  NULL, NULL, pf_add_handler,   NULL, NULL);
    call_add = rant_node_create_remote_function(C, "add",  NULL, NULL, NULL);
    pe   = rant_node_create_function_definition(P, "noop", NULL, NULL, pf_empty_handler, NULL, NULL);
    ce   = rant_node_create_remote_function(C, "noop", NULL, NULL, NULL);
    pd   = rant_node_create_function_definition(P, "defr", NULL, NULL, pf_defer_handler, NULL, NULL);
    cd   = rant_node_create_remote_function(C, "defr", NULL, NULL, NULL);
    pnh  = rant_node_create_function_definition(P, "nohd", NULL, NULL, NULL /* no handler */, NULL, NULL);
    cnh  = rant_node_create_remote_function(C, "nohd", NULL, NULL, NULL);
    ghost= rant_node_create_remote_function(C, "ghost",NULL, NULL, &(RantFunctionOpts){ .timeout_us = 150000u });
    pfm  = rant_node_create_function_definition(P, "failm", NULL, NULL, pf_fail_handler, NULL, NULL);
    cfm  = rant_node_create_remote_function(C, "failm", NULL, NULL, NULL);
    ST_CHECK(prov&&call_add&&pe&&ce&&pd&&cd&&pnh&&cnh&&ghost&&pfm&&cfm, "patterns: functions created/opened");
    (void)pfm;
    (void)pe;(void)ce;

    for (t=0;t<2000 && rant_function_match_count(call_add)==0;t++) pf_pump(P,C,2);
    ST_CHECK(rant_function_match_count(call_add)==1, "patterns: provider matched (%d)",
             rant_function_match_count(call_add));

    /* settle solicits and blocks until every live peer answered. The peer must answer while
       we block, so P runs its service thread for the duration. */
    { int sr;
      rant_node_start(P);
      sr = rant_node_settle(C, 3000);
      rant_node_stop(P);
      ST_CHECK(sr == 1, "patterns: rant_node_settle settles (%d)", sr); }

    /* async call: add(41) gives 42, status OK */
    { uint8_t req[4]; i_rant_le_w32(req,41); pf_reply_done=0;
      rant_function_call_async(call_add, rant_bytes(req,4), pf_on_reply, NULL, NULL);
      for (t=0;t<800 && !pf_reply_done;t++) pf_pump(P,C,2);
      ST_CHECK(pf_reply_done && pf_reply_status==RANT_CALL_OK && pf_reply_val==42,
               "patterns: async add(41)=42 OK (done=%d st=%d val=%u)", pf_reply_done, pf_reply_status, pf_reply_val);
      ST_CHECK(pf_reply_msg_len==0, "patterns: OK response message empty (len=%u)",
               (unsigned)pf_reply_msg_len); }

    /* fail with a message: APP_ERROR text rides the response header (truncated at the
       255 cap, never refused) beside a structured payload */
    { size_t i; int msg_ok;
      pf_reply_done=0;
      rant_function_call_async(cfm, rant_bytes(NULL,0), pf_on_reply, NULL, NULL);
      for (t=0;t<800 && !pf_reply_done;t++) pf_pump(P,C,2);
      msg_ok = pf_reply_msg_len == RANT_CALL_MSG_MAX;
      for (i=0; msg_ok && i<pf_reply_msg_len; i++)
          if (pf_reply_msg[i] != (char)('a' + i%26)) msg_ok = 0;
      ST_CHECK(pf_reply_done && pf_reply_status==RANT_CALL_APP_ERROR,
               "patterns: fail -> APP_ERROR (done=%d st=%d)", pf_reply_done, pf_reply_status);
      ST_CHECK(msg_ok, "patterns: 300-byte message truncated to cap, content intact (len=%u)",
               (unsigned)pf_reply_msg_len);
      ST_CHECK(pf_reply_val==13, "patterns: structured payload rides beside the message (val=%u)",
               pf_reply_val); }

    /* empty ack: the handler returns without replying, auto OK with an empty payload */
    { pf_reply_done=0; pf_calls=0;
      rant_function_call_async(ce, rant_bytes(NULL,0), pf_on_reply, NULL, NULL);
      for (t=0;t<800 && !pf_reply_done;t++) pf_pump(P,C,2);
      ST_CHECK(pf_reply_done && pf_reply_status==RANT_CALL_OK && pf_reply_val==0,
               "patterns: empty-ack auto OK (done=%d st=%d)", pf_reply_done, pf_reply_status); }

    /* deferred: handler defers, we complete later with a value */
    { pf_reply_done=0; pf_defer_token=0;
      rant_function_call_async(cd, rant_bytes(NULL,0), pf_on_reply, NULL, NULL);
      for (t=0;t<800 && !pf_defer_token;t++) pf_pump(P,C,2);
      ST_CHECK(pf_defer_token!=0, "patterns: handler deferred (token=%llu)", (unsigned long long)pf_defer_token);
      { uint8_t out[4]; i_rant_le_w32(out,99);
        rant_function_complete(pd, pf_defer_token, RANT_CALL_OK, "deferred done", rant_bytes(out,4)); }
      for (t=0;t<800 && !pf_reply_done;t++) pf_pump(P,C,2);
      ST_CHECK(pf_reply_done && pf_reply_status==RANT_CALL_OK && pf_reply_val==99,
               "patterns: deferred completion delivers 99 (done=%d val=%u)", pf_reply_done, pf_reply_val);
      ST_CHECK(pf_reply_msg_len==13 && memcmp(pf_reply_msg,"deferred done",13)==0,
               "patterns: complete() message carried on OK (\"%s\")", pf_reply_msg); }

    /* no handler: the provider has a NULL on_request, NO_HANDLER */
    { pf_reply_done=0;
      rant_function_call_async(cnh, rant_bytes(NULL,0), pf_on_reply, NULL, NULL);
      for (t=0;t<800 && !pf_reply_done;t++) pf_pump(P,C,2);
      ST_CHECK(pf_reply_done && pf_reply_status==RANT_CALL_NO_HANDLER,
               "patterns: no-handler -> NO_HANDLER (done=%d st=%d)", pf_reply_done, pf_reply_status);
      ST_CHECK(pf_reply_msg_len==10 && memcmp(pf_reply_msg,"no handler",10)==0,
               "patterns: empty wire message defaults to status text (\"%s\")", pf_reply_msg); }

    /* no provider for "ghost": the deadline synthesizes NO_PROVIDER, never TIMEOUT */
    { pf_reply_done=0;
      rant_function_call_async(ghost, rant_bytes(NULL,0), pf_on_reply, NULL, NULL);
      for (t=0;t<400 && !pf_reply_done;t++) pf_pump(P,C,2);
      ST_CHECK(pf_reply_done && pf_reply_status==RANT_CALL_NO_PROVIDER,
               "patterns: no-provider -> NO_PROVIDER (done=%d st=%d)", pf_reply_done, pf_reply_status);
      ST_CHECK(pf_reply_msg_len==11 && memcmp(pf_reply_msg,"no provider",11)==0,
               "patterns: synthesized outcome carries status text (\"%s\")", pf_reply_msg); }

    /* call before the match forms: a fresh function pair called immediately. The request
       must queue and flush when the provider matches, never drop into a timeout. */
    { RantFunction *pe2, *ce2; int cr; uint8_t req[4];
      pe2 = rant_node_create_function_definition(P, "early", NULL, NULL, pf_add_handler, NULL, NULL);
      ce2 = rant_node_create_remote_function(C, "early", NULL, NULL, NULL);
      ST_CHECK(pe2 && ce2, "patterns: early function pair created");
      i_rant_le_w32(req, 6); pf_reply_done = 0;
      cr = rant_function_call_async(ce2, rant_bytes(req,4), pf_on_reply, NULL, NULL);
      ST_CHECK(cr==RANT_OK, "patterns: early call accepted (%d)", cr);
      for (t=0;t<2000 && !pf_reply_done;t++) pf_pump(P,C,2);
      ST_CHECK(pf_reply_done && pf_reply_status==RANT_CALL_OK && pf_reply_val==7,
               "patterns: early call flushed on match -> 7 (done=%d st=%d val=%u)",
               pf_reply_done, pf_reply_status, pf_reply_val); }

    /* two callers answered in one provider tick: both replies commit back to back before any
       TX runs, and each caller must receive its own, since call ids are per caller counters */
    { RantAllocator c2a = rant_allocator_heap(0);
      RantNodeOpts c2o = co; RantNode *C2 = rant_node_open(&c2a, "fn-call2", NULL, NULL, &c2o);
      RantFunction *call2 = C2 ? rant_node_create_remote_function(C2, "add", NULL, NULL, NULL) : NULL;
      ST_CHECK(C2 && call2, "patterns: second caller open");
      if (C2 && call2){
          uint8_t r1[4], r2[4]; int i2;
          for (t=0;t<2000 && rant_function_match_count(call2)==0;t++){ pf_pump(P,C,1); rant_node_poll(C2,1); }
          ST_CHECK(rant_function_match_count(call2)==1, "patterns: second caller matched");
          /* park both requests at the provider before it polls once */
          i_rant_le_w32(r1,100); i_rant_le_w32(r2,200);
          pf_reply_done=0; pf_reply2_done=0;
          rant_function_call_async(call_add, rant_bytes(r1,4), pf_on_reply,      NULL, NULL);
          rant_function_call_async(call2,      rant_bytes(r2,4), pf_on_reply2, NULL, NULL);
          rant_node_poll(C,0); rant_node_poll(C2,0);          /* flush both requests out */
          for (i2=0;i2<800 && !(pf_reply_done && pf_reply2_done);i2++){
              rant_node_poll(P,2); rant_node_poll(C,2); rant_node_poll(C2,2);
          }
          ST_CHECK(pf_reply_done && pf_reply_status==RANT_CALL_OK && pf_reply_val==101,
                   "patterns: caller1 got ITS reply (done=%d st=%d val=%u)",
                   pf_reply_done, pf_reply_status, pf_reply_val);
          ST_CHECK(pf_reply2_done && pf_reply2_status==RANT_CALL_OK && pf_reply2_val==201,
                   "patterns: caller2 got ITS reply (done=%d st=%d val=%u)",
                   pf_reply2_done, pf_reply2_status, pf_reply2_val);
          rant_node_close(C2,1);
      } else if (C2) rant_node_close(C2,1);
      rant_allocator_reset(&c2a); }

#ifdef RANT_THREADS
    /* a burst past keep_last with the provider on its own service thread: the caller's sends
       engage backpressure without the pattern layer holding the node lock, every call completes */
    { uint8_t req[4]; int i2; uint32_t expect_sum=0;
      rant_node_start(P);
      pf_burst_done=0; pf_burst_sum=0; pf_burst_bad=0;
      for (i2=0;i2<14;i2++){
          int cr;   /* hoisted: ST_CHECK evaluates its condition twice */
          i_rant_le_w32(req,(uint32_t)(1000+i2)); expect_sum += (uint32_t)(1000+i2+1);
          cr = rant_function_call_async(call_add, rant_bytes(req,4), pf_on_reply_burst, NULL, NULL);
          ST_CHECK(cr==RANT_OK, "patterns: burst call %d accepted (%d)", i2, cr);
      }
      for (t=0;t<2000 && pf_burst_done<14;t++) rant_node_poll(C,2);
      ST_CHECK(pf_burst_done==14 && pf_burst_bad==0 && pf_burst_sum==expect_sum,
               "patterns: burst 14/keep_last 10 all OK (done=%d bad=%d sum=%u want=%u)",
               pf_burst_done, pf_burst_bad, pf_burst_sum, expect_sum);
      ST_CHECK(rant_node_evicted_unsent(C)==0, "patterns: burst evicted nothing (%u)",
               rant_node_evicted_unsent(C));
      rant_node_stop(P); }

    /* a sync call that times out locally, then the deferred reply lands late: the pending
       entry must be unlinked, the late reply dropped, and a later sync call still works */
    { RantResponse rep; int rc;
      rant_node_start(P);
      pf_defer_token=0;
      rc = rant_function_call(cd, rant_bytes(NULL,0), &rep, 120, NULL);
      ST_CHECK(rc==0 && rep.status==RANT_CALL_TIMEOUT,
               "patterns: sync local timeout (rc=%d st=%d)", rc, rep.status);
      ST_CHECK(rep.message.len==7 && memcmp(rep.message.data,"timeout",7)==0,
               "patterns: sync timeout carries status text");
      for (t=0;t<400 && !pf_defer_token;t++) rant_node_poll(C,2);
      ST_CHECK(pf_defer_token!=0, "patterns: deferred token arrived");
      if (pf_defer_token){ uint8_t out[4]; i_rant_le_w32(out,7);
          rant_function_complete(pd, pf_defer_token, RANT_CALL_OK, NULL, rant_bytes(out,4)); }
      for (t=0;t<200;t++) rant_node_poll(C,2);     /* late reply arrives: must be dropped safely */
      pf_reply_done=0;
      { uint8_t req[4]; i_rant_le_w32(req,60);
        rc = rant_function_call(call_add, rant_bytes(req,4), &rep, 1000, NULL); }
      ST_CHECK(rc==1 && rep.status==RANT_CALL_OK && rep.data.len>=4 && i_rant_le_r32(rep.data.data)==61,
               "patterns: sync works after late-reply drop (rc=%d st=%d)", rc, rep.status);
      rant_node_stop(P); }
#endif

    /* ---- variables: catch-up, convergence, read-only refusal, force/absorb/unforce ---- */
    { RantVariable *ov, *av; RantBytes gv; uint8_t b[4];
      i_rant_le_w32(b,20);
      ov = rant_node_create_variable_definition(P, "temp", NULL, &(RantVariableOpts){ .initial=rant_bytes(b,4), .allow_force=1 });
      av = rant_node_create_remote_variable(C, "temp", NULL, NULL);
      ST_CHECK(ov && av, "var: created/opened");
      for (t=0;t<2000 && rant_variable_match_count(ov)==0;t++) pf_pump(P,C,2);
      ST_CHECK(rant_variable_match_count(ov)==1, "var: accessor matched (%d)", rant_variable_match_count(ov));
      for (t=0;t<600 && !rant_variable_get(av,&gv);t++) pf_pump(P,C,2);
      ST_CHECK(rant_variable_get(av,&gv) && gv.len==4 && i_rant_le_r32(gv.data)==20, "var: catch_up initial=20");
      i_rant_le_w32(b,25); rant_variable_set(ov, rant_bytes(b,4));
      for (t=0;t<600;t++){ pf_pump(P,C,2); if (rant_variable_get(av,&gv)&&gv.len==4&&i_rant_le_r32(gv.data)==25) break; }
      ST_CHECK(rant_variable_get(av,&gv)&&i_rant_le_r32(gv.data)==25, "var: owner set converges (25)");
      i_rant_le_w32(b,30);
      { int sr = rant_variable_set(av, rant_bytes(b,4)); ST_CHECK(sr==RANT_OK, "var: accessor set ok (%d)", sr); }
      for (t=0;t<600;t++){ pf_pump(P,C,2); if (rant_variable_get(ov,&gv)&&gv.len==4&&i_rant_le_r32(gv.data)==30) break; }
      ST_CHECK(rant_variable_get(ov,&gv)&&i_rant_le_r32(gv.data)==30, "var: accessor set applied at owner (30)");
      i_rant_le_w32(b,99); rant_variable_force(ov, rant_bytes(b,4));
      for (t=0;t<600;t++){ pf_pump(P,C,2); if (rant_variable_forced(av)&&rant_variable_get(av,&gv)&&i_rant_le_r32(gv.data)==99) break; }
      ST_CHECK(rant_variable_forced(av)&&rant_variable_get(av,&gv)&&i_rant_le_r32(gv.data)==99,
               "var: force visible at accessor (99, forced)");
      i_rant_le_w32(b,50); rant_variable_set(av, rant_bytes(b,4));         /* absorbed into the shadow */
      pf_pump(P,C,80);
      ST_CHECK(rant_variable_get(av,&gv)&&i_rant_le_r32(gv.data)==99, "var: set absorbed while forced (still 99)");
      rant_variable_unforce(ov);
      for (t=0;t<600;t++){ pf_pump(P,C,2); if (!rant_variable_forced(av)&&rant_variable_get(av,&gv)&&i_rant_le_r32(gv.data)==50) break; }
      ST_CHECK(!rant_variable_forced(av)&&rant_variable_get(av,&gv)&&i_rant_le_r32(gv.data)==50,
               "var: unforce restores latest absorbed (50)"); }
    { RantVariable *ro_o, *ro_a; uint8_t b[4]; int sr; i_rant_le_w32(b,7);
      ro_o = rant_node_create_variable_definition(P, "rovar", NULL, &(RantVariableOpts){ .initial=rant_bytes(b,4), .access=RANT_VAR_READONLY });
      ro_a = rant_node_create_remote_variable(C, "rovar", NULL, NULL);
      ST_CHECK(ro_o&&ro_a, "var: read-only created/opened");
      for (t=0;t<600;t++) pf_pump(P,C,2);
      i_rant_le_w32(b,8); sr = rant_variable_set(ro_a, rant_bytes(b,4));
      ST_CHECK(sr==RANT_ERR_ROLE, "var: read-only set refused (%d)", sr); }
    { RantVariable *orphan = rant_node_create_remote_variable(C, "nobody-owns-this", NULL, NULL);
      uint8_t b[4]; int sr; i_rant_le_w32(b,1);
      ST_CHECK(orphan != NULL, "var: orphan accessor opens");
      sr = orphan ? rant_variable_set(orphan, rant_bytes(b,4)) : 0;
      ST_CHECK(sr==RANT_ERR_NO_TOPIC, "var: set with no owner -> NO_TOPIC (%d)", sr); }
    { /* typed variable, remote force and unforce: the op only unforce must pass the schema
         gate through the empty payload exemption, and remote sets while forced absorb */
      RantAllocator ma = rant_allocator_heap(0);
      RantSchema *ts = rant_schema_compile(rant_allocator_alloc, &ma, "T { v: u32 }", NULL);
      RantVariable *to, *ta; RantBytes gv; uint8_t b[4]; int fr, ur;
      ST_CHECK(ts != NULL, "var: typed schema compiles");
      i_rant_le_w32(b,10);
      to = rant_node_create_variable_definition(P, "ttemp", ts, &(RantVariableOpts){ .initial=rant_bytes(b,4), .allow_force=1 });
      ta = rant_node_create_remote_variable(C, "ttemp", ts, NULL);
      ST_CHECK(to && ta, "var: typed created/opened");
      for (t=0;t<2000 && rant_variable_match_count(to)==0;t++) pf_pump(P,C,2);
      for (t=0;t<600 && !rant_variable_get(ta,&gv);t++) pf_pump(P,C,2);
      i_rant_le_w32(b,777); fr = rant_variable_force(ta, rant_bytes(b,4));         /* remote force */
      ST_CHECK(fr==RANT_OK, "var: typed remote force sent (%d)", fr);
      for (t=0;t<600;t++){ pf_pump(P,C,2); if (rant_variable_forced(ta)&&rant_variable_get(ta,&gv)&&i_rant_le_r32(gv.data)==777) break; }
      ST_CHECK(rant_variable_forced(ta)&&rant_variable_get(ta,&gv)&&i_rant_le_r32(gv.data)==777,
               "var: typed force pins (777, forced)");
      ur = rant_variable_unforce(ta);     /* remote, op only payload */
      ST_CHECK(ur==RANT_OK, "var: typed remote unforce sent (%d)", ur);
      for (t=0;t<600;t++){ pf_pump(P,C,2); if (!rant_variable_forced(ta)) break; }
      ST_CHECK(!rant_variable_forced(ta), "var: typed remote unforce applies (op-only payload)");
      rant_allocator_reset(&ma); }
    { /* on_change and on_write: change dedup, every write, replay at registration, force
         transitions, absorbed writes stay silent, and the source peer stamp */
      PfVarEvt oc, ow, ac, aw;   /* owner / accessor, change / write */
      RantVariable *eo, *ea; uint8_t b[4]; int rr;
      memset(&oc,0,sizeof oc); memset(&ow,0,sizeof ow); memset(&ac,0,sizeof ac); memset(&aw,0,sizeof aw);
      i_rant_le_w32(b,5);
      eo = rant_node_create_variable_definition(P, "evar", NULL, &(RantVariableOpts){ .initial=rant_bytes(b,4), .allow_force=1 });
      ea = rant_node_create_remote_variable(C, "evar", NULL, NULL);
      ST_CHECK(eo && ea, "var: event pair created");
      /* registering AFTER the initial value exists replays it once, immediately */
      rr = rant_variable_on_change(eo, pf_on_var_update, &oc);
      ST_CHECK(rr==RANT_OK && oc.n==1 && oc.val==5,
               "var: on_change replays current at registration (n=%d val=%u)", oc.n, oc.val);
      rant_variable_on_write(eo, pf_on_var_update, &ow);
      ST_CHECK(ow.n==0, "var: on_write does not replay (writes are events, not state)");
      rant_variable_on_change(ea, pf_on_var_update, &ac);
      rant_variable_on_write(ea, pf_on_var_update, &aw);
      for (t=0;t<2000 && ac.n==0;t++) pf_pump(P,C,2);   /* catch-up: the first value fires both */
      ST_CHECK(ac.n==1 && aw.n==1 && ac.val==5,
               "var: accessor first value fires (change=%d write=%d val=%u)", ac.n, aw.n, ac.val);
      ST_CHECK(ac.source!=0, "var: accessor source = the owner peer (%u)", ac.source);
      i_rant_le_w32(b,6); rant_variable_set(eo, rant_bytes(b,4));         /* local set, source 0 */
      ST_CHECK(oc.n==2 && ow.n==1 && oc.val==6 && oc.source==0,
               "var: local set fires inline on the caller (change=%d write=%d src=%u)", oc.n, ow.n, oc.source);
      rant_variable_set(eo, rant_bytes(b,4));                   /* byte-identical re-set */
      ST_CHECK(oc.n==2 && ow.n==2 && ow.seq==oc.seq+1,
               "var: identical re-set fires write only, seq advances (change=%d write=%d)", oc.n, ow.n);
      for (t=0;t<600 && aw.n<3;t++) pf_pump(P,C,2);
      ST_CHECK(aw.n==3 && ac.n==2 && ac.val==6,
               "var: accessor saw 3 writes / 2 changes (w=%d c=%d val=%u)", aw.n, ac.n, ac.val);
      rant_variable_force(eo, rant_bytes(b,4));       /* same bytes: the forced flip is a change */
      ST_CHECK(oc.n==3 && oc.forced==1 && ow.n==3,
               "var: force with identical bytes still a change (n=%d forced=%u)", oc.n, oc.forced);
      for (t=0;t<600 && ac.n<3;t++) pf_pump(P,C,2);
      ST_CHECK(ac.n==3 && ac.forced==1, "var: accessor sees the forced flip (n=%d forced=%u)", ac.n, ac.forced);
      i_rant_le_w32(b,44); rant_variable_set(eo, rant_bytes(b,4));         /* absorbed into the shadow */
      pf_pump(P,C,40);
      ST_CHECK(oc.n==3 && ow.n==3, "var: absorbed write fires nothing (c=%d w=%d)", oc.n, ow.n);
      rant_variable_unforce(eo);
      ST_CHECK(oc.n==4 && !oc.forced && oc.val==44 && ow.n==4,
               "var: unforce fires with the restored value (n=%d val=%u)", oc.n, oc.val);
      for (t=0;t<600 && ac.n<4;t++) pf_pump(P,C,2);
      ST_CHECK(ac.n==4 && !ac.forced && ac.val==44,
               "var: accessor unforce change (n=%d val=%u forced=%u)", ac.n, ac.val, ac.forced);
      i_rant_le_w32(b,70);
      { int sr = rant_variable_set(ea, rant_bytes(b,4)); ST_CHECK(sr==RANT_OK, "var: event remote set ok (%d)", sr); }
      for (t=0;t<600 && oc.n<5;t++) pf_pump(P,C,2);
      ST_CHECK(oc.n==5 && oc.val==70 && oc.source!=0,
               "var: remote set fires at the owner with the setter's peer (src=%u)", oc.source);
      rant_variable_on_change(eo, NULL, NULL); rant_variable_on_write(eo, NULL, NULL);
      i_rant_le_w32(b,71); rant_variable_set(eo, rant_bytes(b,4));
      ST_CHECK(oc.n==5 && ow.n==5, "var: cleared callbacks stay silent (c=%d w=%d)", oc.n, ow.n);
      for (t=0;t<600 && ac.n<5;t++) pf_pump(P,C,2);   /* drain the echoes before the captures die */
      rant_variable_on_change(ea, NULL, NULL); rant_variable_on_write(ea, NULL, NULL); }

#ifdef RANT_THREADS
    /* sync call: the provider answers from its own service thread while the caller's
       sync loop drives its node */
    { RantResponse rep; int rc; uint8_t req[4]; i_rant_le_w32(req,7);
      rant_node_start(P);
      rc = rant_function_call(call_add, rant_bytes(req,4), &rep, 1000, NULL);
      ST_CHECK(rc==1 && rep.status==RANT_CALL_OK && rep.data.len>=4 && i_rant_le_r32(rep.data.data)==8,
               "patterns: sync add(7)=8 (rc=%d st=%d)", rc, rep.status);
      rant_node_stop(P); }
#endif

    /* reflection: the entity walk folds P's channels. P hosts 5 functions and 3 variables,
       and the peer walk from C and P's local walk must both yield exactly those. */
    for (t=0;t<200;t++) pf_pump(P,C,2);   /* let any straggling detail fetches settle */
    { const RantDiscoveryPeer *ps; uint16_t pc = 0; uint32_t pid = 0;
      ps = st_peers(C, &pc);
      if (ps && pc) pid = ps[0].id;
      { RantIter eit; RantEntityInfo ei;
        int fns=0,vars=0,tops=0,ats=0,inc=0,temp_rw=0,rovar_ro=0,temp_forceable=0,rovar_forceable=0; size_t k;
        memset(&eit,0,sizeof eit);
        while (rant_node_entities_next(C, pid, &eit, &ei)){
            switch (ei.kind){
            case RANT_ENTITY_FUNCTION: fns++; break;
            case RANT_ENTITY_VARIABLE:
                vars++;
                if (ei.name.len==4 && !memcmp(ei.name.data,"temp",4)) { temp_rw  = ei.writable; temp_forceable = ei.forceable; }
                if (ei.name.len==5 && !memcmp(ei.name.data,"rovar",5)){ rovar_ro = !ei.writable; rovar_forceable = ei.forceable; }
                break;
            default: tops++; break;
            }
            inc += ei.incomplete;
            for (k=0;k<ei.name.len;k++) if (ei.name.data[k]=='@') ats++;
        }
        ST_CHECK(fns==6 && vars==4 && tops==0,
                 "reflect: peer entities fold (fn=%d var=%d top=%d)", fns, vars, tops);
        ST_CHECK(ats==0 && inc==0, "reflect: no internals leak (@bytes=%d incomplete=%d)", ats, inc);
        ST_CHECK(temp_rw==1 && rovar_ro==1, "reflect: writability (temp rw=%d, rovar ro=%d)", temp_rw, rovar_ro);
        ST_CHECK(temp_forceable==1 && rovar_forceable==0,
                 "reflect: forceability (temp allow_force=%d, rovar=%d)", temp_forceable, rovar_forceable); }
      { RantIter eit; RantEntityInfo ei; int fns=0,vars=0,tops=0,temp_forceable=0;
        memset(&eit,0,sizeof eit);
        while (rant_node_entities_next(P, RANT_SELF, &eit, &ei)){
            switch (ei.kind){
            case RANT_ENTITY_FUNCTION: fns++; break;
            case RANT_ENTITY_VARIABLE:
                vars++;
                if (ei.name.len==4 && !memcmp(ei.name.data,"temp",4)) temp_forceable = ei.forceable;
                break;
            default: tops++; break;
            } }
        ST_CHECK(fns==6 && vars==4 && tops==0,
                 "reflect: local entities (fn=%d var=%d top=%d)", fns, vars, tops);
        ST_CHECK(temp_forceable==1, "reflect: local forceability (temp allow_force=%d)", temp_forceable); } }

    { /* a reentrant set inside on_write commits to transport history before the outer set's
         deferred send, so the owner must skip the stale outer publish (spec/patterns.md) */
      RantVariable *ro, *ra; uint8_t b[4]; RantBytes cur;
      ro = rant_node_create_variable_definition(P, "rvar", NULL, NULL);
      ra = rant_node_create_remote_variable(C, "rvar", NULL, NULL);
      ST_CHECK(ro && ra, "var: reentrancy pair created");
      pf_reent_var = ro; pf_reent_done = 0;
      rant_variable_on_write(ro, pf_on_var_reenter, NULL);
      i_rant_le_w32(b,100);
      rant_variable_set(ro, rant_bytes(b,4));
      ST_CHECK(pf_reent_done, "var: reentrant set ran inside on_write");
      { RantBytes ov; int have = rant_variable_get(ro, &ov);
        ST_CHECK(have && ov.len>=4 && i_rant_le_r32(ov.data)==200,
                 "var: owner store holds the newest (200)"); }
      for (t=0;t<2000;t++){ pf_pump(P,C,2);
          if (rant_variable_get(ra, &cur) && cur.len>=4 && i_rant_le_r32(cur.data)==200) break; }
      { int have = rant_variable_get(ra, &cur);
        ST_CHECK(have && cur.len>=4 && i_rant_le_r32(cur.data)==200,
                 "var: remote reached the newest value"); }
      pf_pump(P,C,60);   /* nothing stale may follow: the owner skipped the outer publish */
      { int have = rant_variable_get(ra, &cur);
        ST_CHECK(have && cur.len>=4 && i_rant_le_r32(cur.data)==200,
                 "var: stale outer publish skipped, remote stays newest (%u)",
                 (have && cur.len>=4) ? i_rant_le_r32(cur.data) : 0u); }
      rant_variable_on_write(ro, NULL, NULL); }

    { /* the ring is the repair window, not the replay window: a reliable write from a callback
         cannot wait for a TX pass, so every write of the burst must fit the value channel's history */
      RantVariable *bo, *ba; uint8_t b[4]; uint32_t ev0, want = (1u<<PF_VBURST_N) - 1u;
      bo = rant_node_create_variable_definition(P, "bvar", NULL, NULL);
      ba = rant_node_create_remote_variable(C, "bvar", NULL, NULL);
      ST_CHECK(bo && ba, "var: burst pair created");
      pf_vburst_var = bo; pf_vburst_ran = 0; pf_vburst_seen = 0;
      rant_variable_on_write(ba, pf_on_var_burst_rx, NULL);
      for (t=0;t<2000 && rant_variable_match_count(bo)==0;t++) pf_pump(P,C,2);
      ST_CHECK(rant_variable_match_count(bo)==1, "var: burst remote matched (%d)",
               rant_variable_match_count(bo));
      ev0 = rant_node_evicted_unsent(P);
      rant_variable_on_write(bo, pf_on_var_burst, NULL);
      i_rant_le_w32(b,0); rant_variable_set(bo, rant_bytes(b,4));         /* fires the burst inline */
      ST_CHECK(pf_vburst_ran, "var: burst ran inside on_write");
      for (t=0;t<2000 && pf_vburst_seen!=want;t++) pf_pump(P,C,2);
      ST_CHECK(pf_vburst_seen==want, "var: every reentrant write reached the remote (0x%02x of 0x%02x)",
               pf_vburst_seen, want);
      ST_CHECK(rant_node_evicted_unsent(P)==ev0, "var: burst evicted nothing unsent (%u)",
               rant_node_evicted_unsent(P) - ev0);
      rant_variable_on_write(bo, NULL, NULL); rant_variable_on_write(ba, NULL, NULL); }

    { /* a variable is state: a burst far past the ring never waits on a reader that is not
         acking, and the reader still ends on the newest value (spec/patterns.md) */
      RantVariable *lo, *la; uint8_t b[4]; RantBytes cur; uint64_t t0, el; uint32_t i2;
      lo = rant_node_create_variable_definition(P, "lvar", NULL, NULL);
      la = rant_node_create_remote_variable(C, "lvar", NULL, NULL);
      ST_CHECK(lo && la, "var: latest pair created");
      for (t=0;t<2000 && rant_variable_match_count(lo)==0;t++) pf_pump(P,C,2);
      t0 = i_rant_plat_now_us();
      for (i2=0;i2<200;i2++){ i_rant_le_w32(b,1000u+i2); rant_variable_set(lo, rant_bytes(b,4)); }
      el = i_rant_plat_now_us() - t0;
      ST_CHECK(el < 500000u, "var: 200 writes past a silent reader never wait (%.1f ms)", el/1000.0);
      for (t=0;t<2000;t++){ pf_pump(P,C,2);
          if (rant_variable_get(la, &cur) && cur.len>=4 && i_rant_le_r32(cur.data)==1199u) break; }
      { int have = rant_variable_get(la, &cur);
        ST_CHECK(have && cur.len>=4 && i_rant_le_r32(cur.data)==1199u,
                 "var: the reader ends on the newest value (%u)",
                 (have && cur.len>=4) ? i_rant_le_r32(cur.data) : 0u); } }

    { /* provider side reply burst: an inline reply is a reentrant send, so the rsp ring is the
         only thing between a batch drained in one pass and lost replies (spec/testing.md) */
      uint8_t req[4]; int i2; uint32_t expect_sum, ev0;
      RantFunction *deep_p, *deep_c;
      pf_burst_done=0; pf_burst_sum=0; pf_burst_bad=0; expect_sum=0;
      for (i2=0;i2<8;i2++){
          i_rant_le_w32(req,(uint32_t)(2000+i2)); expect_sum += (uint32_t)(2000+i2+1);
          rant_function_call_async(call_add, rant_bytes(req,4), pf_on_reply_burst, NULL, NULL);
      }
      for (i2=0;i2<50;i2++) rant_node_poll(C,0);     /* park every request: P has not polled */
      ev0 = rant_node_evicted_unsent(P);
      rant_node_poll(P,20);                          /* ONE pass: drain 8, reply inline to each */
      for (t=0;t<2000 && pf_burst_done<8;t++) pf_pump(P,C,2);
      ST_CHECK(pf_burst_done==8 && pf_burst_bad==0 && pf_burst_sum==expect_sum,
               "patterns: 8 inline replies in one pass all returned (done=%d bad=%d sum=%u want=%u)",
               pf_burst_done, pf_burst_bad, pf_burst_sum, expect_sum);
      ST_CHECK(rant_node_evicted_unsent(P)==ev0, "patterns: reply burst evicted nothing unsent (%u)",
               rant_node_evicted_unsent(P) - ev0);

      deep_p = rant_node_create_function_definition(P, "deep", NULL, NULL, pf_add_handler, NULL,
                              &(RantFunctionOpts){ .keep_last = 32 });
      /* the caller needs the depth too: its req ring holds the burst while the provider has
         not polled, and at the default depth calls past it stall in backpressure */
      deep_c = rant_node_create_remote_function(C, "deep", NULL, NULL,
                              &(RantFunctionOpts){ .keep_last = 32 });
      ST_CHECK(deep_p && deep_c, "patterns: deep-ring function pair created");
      /* both lanes must be up: the caller's req match alone would let the first replies
         commit before the provider's rsp lane exists, and rsp carries no catch_up */
      for (t=0;t<2000 && !(rant_function_match_count(deep_c) && rant_function_match_count(deep_p));t++)
          pf_pump(P,C,2);
      ST_CHECK(rant_function_match_count(deep_c) && rant_function_match_count(deep_p),
               "patterns: deep-ring pair matched both ways (c=%d p=%d)",
               rant_function_match_count(deep_c), rant_function_match_count(deep_p));
      pf_pump(P,C,50);
      { int before = pf_calls; (void)before;
      pf_burst_done=0; pf_burst_sum=0; pf_burst_bad=0; expect_sum=0;
      for (i2=0;i2<24;i2++){
          i_rant_le_w32(req,(uint32_t)(3000+i2)); expect_sum += (uint32_t)(3000+i2+1);
          rant_function_call_async(deep_c, rant_bytes(req,4), pf_on_reply_burst, NULL, NULL);
      }
      for (i2=0;i2<80;i2++) rant_node_poll(C,0);
      rant_node_poll(P,20);
      for (t=0;t<2000 && pf_burst_done<24;t++) pf_pump(P,C,2);
      ST_CHECK(pf_burst_done==24 && pf_burst_bad==0 && pf_burst_sum==expect_sum,
               "patterns: keep_last=32 carries a 24-reply pass (handled=%d done=%d bad=%d laststatus=%d sum=%u want=%u)",
               pf_calls - before, pf_burst_done, pf_burst_bad, (int)pf_burst_last_bad, pf_burst_sum, expect_sum); } }

    /* a call still pending when the node closes gets one synthesized CANCELLED outcome */
    { RantFunction *never = rant_node_create_remote_function(C, "never-served", NULL, NULL,
                              &(RantFunctionOpts){ .timeout_us = 60000000u });
      ST_CHECK(never != NULL, "patterns: cancel-at-close remote created");
      pf_cancel_count = 0; pf_cancel_status = RANT_CALL_OK;
      if (never) rant_function_call_async(never, rant_bytes(NULL,0), pf_on_cancel, NULL, NULL); }

    rant_node_close(P,0); rant_node_close(C,0);
    ST_CHECK(pf_cancel_count==1 && pf_cancel_status==RANT_CALL_CANCELLED,
             "patterns: pending call cancelled at close (n=%d status=%d)",
             pf_cancel_count, (int)pf_cancel_status);
    rant_allocator_reset(&pa); rant_allocator_reset(&ca);
}

/* The task smoke phase (19e7): defer, RUNNING first progress, cancel both ways, no_cancel
 * refused locally, an inline reply and the bare return APP_ERROR. The matrix is 19e8. */
static volatile int   tk_reply_done;
static RantCallStatus tk_reply_status;
static uint32_t         tk_reply_val;
static char             tk_reply_msg[RANT_CALL_MSG_MAX + 1]; static size_t tk_reply_msg_len;
static void tk_on_reply(const RantResponse *r){
    tk_reply_status = r->status;
    tk_reply_val = r->data.len>=4 ? i_rant_le_r32(r->data.data) : 0;
    tk_reply_msg_len = r->message.len <= RANT_CALL_MSG_MAX ? r->message.len : RANT_CALL_MSG_MAX;
    if (tk_reply_msg_len) memcpy(tk_reply_msg, r->message.data, tk_reply_msg_len);
    tk_reply_msg[tk_reply_msg_len] = 0;
    tk_reply_done = 1;
}
/* progress capture: the RUNNING ack is the empty first update, payloads must ascend */
static int tk_prog_n, tk_prog_first_empty, tk_prog_in_order;
static uint32_t tk_prog_last_val, tk_prog_call_id;
static void tk_on_progress(const RantProgress *p){
    if (tk_prog_n == 0) tk_prog_first_empty = (p->data.len == 0);
    if (p->data.len >= 4){
        uint32_t v = i_rant_le_r32(p->data.data);
        if (v <= tk_prog_last_val) tk_prog_in_order = 0;
        tk_prog_last_val = v;
    }
    tk_prog_call_id = p->call_id;
    tk_prog_n++;
}
static volatile uint64_t tk_token;
static void tk_defer_handler(RantRequest *req, void *user){ (void)user; tk_token = rant_request_defer(req); }
static void tk_inline_handler(RantRequest *req, void *user){
    uint8_t out[4]; (void)user;
    i_rant_le_w32(out, req->data.len>=4 ? i_rant_le_r32(req->data.data)+1 : 0);
    rant_request_reply(req, rant_bytes(out,4));
}
static void tk_bare_handler(RantRequest *req, void *user){ (void)req; (void)user; }
static volatile int tk_cancel_fired; static volatile uint64_t tk_cancel_token;
static void tk_on_cancel_cb(uint64_t token, void *user){ (void)user; tk_cancel_token = token; tk_cancel_fired = 1; }

static void task_checks(void){
    RantAllocator pa = rant_allocator_heap(0);
    RantAllocator ca = rant_allocator_heap(0);
    RantNodeOpts po, co; RantNode *P=NULL, *C=NULL; RantDiscoveryAddr seed;
    RantFunction *pxfer, *cxfer, *plong, *clong, *pnc, *cnc, *pinl, *cinl, *pbare, *cbare;
    uint16_t dom = ST_DOMAIN+32; int t;

    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=dom; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    co=po;
    P = rant_node_open(&pa, "task-prov", NULL, NULL, &po);
    C = rant_node_open(&ca, "task-call", NULL, NULL, &co);
    ST_CHECK(P && C, "task: nodes open");
    if (!(P && C)){ if(P)rant_node_close(P,0); if(C)rant_node_close(C,0); return; }

    pxfer = rant_node_create_task_definition(P, "xfer", NULL, NULL, NULL, tk_defer_handler, NULL, NULL);
    cxfer = rant_node_create_remote_task(C, "xfer", NULL, NULL, NULL, NULL);
    plong = rant_node_create_task_definition(P, "long", NULL, NULL, NULL, tk_defer_handler, NULL, NULL);
    clong = rant_node_create_remote_task(C, "long", NULL, NULL, NULL, NULL);
    pnc   = rant_node_create_task_definition(P, "nocan", NULL, NULL, NULL, tk_defer_handler, NULL,
                                             &(RantTaskOpts){ .no_cancel = 1 });
    cnc   = rant_node_create_remote_task(C, "nocan", NULL, NULL, NULL, NULL);
    pinl  = rant_node_create_task_definition(P, "inl", NULL, NULL, NULL, tk_inline_handler, NULL, NULL);
    cinl  = rant_node_create_remote_task(C, "inl", NULL, NULL, NULL, NULL);
    pbare = rant_node_create_task_definition(P, "bare", NULL, NULL, NULL, tk_bare_handler, NULL, NULL);
    cbare = rant_node_create_remote_task(C, "bare", NULL, NULL, NULL, NULL);
    ST_CHECK(pxfer&&cxfer&&plong&&clong&&pnc&&cnc&&pinl&&cinl&&pbare&&cbare, "task: pairs created");

    for (t=0;t<2000 && rant_function_match_count(cxfer)==0;t++) pf_pump(P,C,2);
    ST_CHECK(rant_function_match_count(cxfer)==1, "task: provider matched (%d)",
             rant_function_match_count(cxfer));

    /* defer + 3 progress updates + terminal OK: RUNNING fires on_progress first (empty),
       payloads arrive in order, id_out names the call */
    { uint8_t req[4], pb[4]; uint32_t id = 0; int rc, i2;
      i_rant_le_w32(req, 5);
      tk_reply_done=0; tk_token=0;
      tk_prog_n=0; tk_prog_first_empty=0; tk_prog_in_order=1; tk_prog_last_val=0; tk_prog_call_id=0;
      rc = rant_function_call_async(cxfer, rant_bytes(req,4), tk_on_reply, NULL,
              &(RantCallOpts){ .on_progress = tk_on_progress, .id_out = &id });
      ST_CHECK(rc==RANT_OK && id!=0, "task: call accepted, id_out filled (rc=%d id=%u)", rc, id);
      for (t=0;t<2000 && !tk_token;t++) pf_pump(P,C,2);
      ST_CHECK(tk_token!=0, "task: handler deferred");
      for (t=0;t<800 && tk_prog_n<1;t++) pf_pump(P,C,2);
      ST_CHECK(tk_prog_n>=1 && tk_prog_first_empty,
               "task: RUNNING fired on_progress first, empty (n=%d empty=%d)",
               tk_prog_n, tk_prog_first_empty);
      for (i2=1;i2<=3;i2++){
          int pr;   /* each update lands before the next is sent, pinning arrival order */
          i_rant_le_w32(pb, (uint32_t)(100*i2));
          pr = rant_function_progress(pxfer, tk_token, rant_bytes(pb,4));
          ST_CHECK(pr==RANT_OK, "task: progress %d sent (%d)", i2, pr);
          for (t=0;t<400 && tk_prog_n < 1+i2;t++) pf_pump(P,C,2);
      }
      i_rant_le_w32(pb, 777);
      rc = rant_function_complete(pxfer, tk_token, RANT_CALL_OK, NULL, rant_bytes(pb,4));
      ST_CHECK(rc==RANT_OK, "task: complete OK sent (%d)", rc);
      for (t=0;t<800 && !tk_reply_done;t++) pf_pump(P,C,2);
      ST_CHECK(tk_reply_done && tk_reply_status==RANT_CALL_OK && tk_reply_val==777,
               "task: terminal OK (done=%d st=%d val=%u)", tk_reply_done, tk_reply_status, tk_reply_val);
      ST_CHECK(tk_prog_n==4 && tk_prog_in_order && tk_prog_last_val==300 && tk_prog_call_id==id,
               "task: 3 payloads in order after RUNNING (n=%d order=%d last=%u call=%u want=%u)",
               tk_prog_n, tk_prog_in_order, tk_prog_last_val, tk_prog_call_id, id);
      { int sr = rant_function_cancelled(pxfer, tk_token);     /* completed: the token is stale */
        ST_CHECK(sr==RANT_ERR_STATE, "task: stale token refused (%d)", sr); } }

    /* cancel honored: long task, rant_function_cancel, the definition sees the flag +
       on_cancel, completes CANCELLED, the caller receives it */
    { uint32_t id = 0; int rc;
      tk_reply_done=0; tk_token=0; tk_cancel_fired=0; tk_cancel_token=0;
      rant_function_on_cancel(plong, tk_on_cancel_cb, NULL);
      rc = rant_function_call_async(clong, rant_bytes(NULL,0), tk_on_reply, NULL,
              &(RantCallOpts){ .id_out = &id });
      ST_CHECK(rc==RANT_OK, "task: long call accepted (%d)", rc);
      for (t=0;t<2000 && !tk_token;t++) pf_pump(P,C,2);
      ST_CHECK(tk_token!=0, "task: long deferred");
      { int sr = rant_function_cancelled(plong, tk_token);
        ST_CHECK(sr==0, "task: not yet cancelled (%d)", sr); }
      rc = rant_function_cancel(clong, id);
      ST_CHECK(rc==RANT_OK, "task: cancel sent (%d)", rc);
      for (t=0;t<800 && !tk_cancel_fired;t++) pf_pump(P,C,2);
      ST_CHECK(tk_cancel_fired && tk_cancel_token==tk_token,
               "task: on_cancel fired with the token (fired=%d)", tk_cancel_fired);
      { int sr = rant_function_cancelled(plong, tk_token);
        ST_CHECK(sr==1, "task: cancelled flag set (%d)", sr); }
      rc = rant_function_complete(plong, tk_token, RANT_CALL_CANCELLED, "stopped", rant_bytes(NULL,0));
      ST_CHECK(rc==RANT_OK, "task: complete CANCELLED sent (%d)", rc);
      for (t=0;t<800 && !tk_reply_done;t++) pf_pump(P,C,2);
      ST_CHECK(tk_reply_done && tk_reply_status==RANT_CALL_CANCELLED
               && tk_reply_msg_len==7 && memcmp(tk_reply_msg,"stopped",7)==0,
               "task: caller sees wire CANCELLED \"stopped\" (st=%d msg=\"%s\")",
               tk_reply_status, tk_reply_msg); }

    /* .no_cancel: the caller's cancel refuses locally (RANT_ERR_ROLE), nothing reaches the
       definition, and the task still completes normally */
    { uint32_t id = 0; int rc, cr;
      tk_reply_done=0; tk_token=0; tk_cancel_fired=0;
      rant_function_on_cancel(pnc, tk_on_cancel_cb, NULL);
      rc = rant_function_call_async(cnc, rant_bytes(NULL,0), tk_on_reply, NULL,
              &(RantCallOpts){ .id_out = &id });
      for (t=0;t<2000 && !tk_token;t++) pf_pump(P,C,2);
      ST_CHECK(rc==RANT_OK && tk_token!=0, "task: no_cancel call running (rc=%d)", rc);
      cr = rant_function_cancel(cnc, id);
      ST_CHECK(cr==RANT_ERR_ROLE, "task: cancel refused locally with RANT_ERR_ROLE (%d)", cr);
      pf_pump(P,C,60);
      { int sr = rant_function_cancelled(pnc, tk_token);
        ST_CHECK(!tk_cancel_fired && sr==0,
                 "task: nothing reached the definition (fired=%d flag=%d)", tk_cancel_fired, sr); }
      cr = rant_function_complete(pnc, tk_token, RANT_CALL_OK, NULL, rant_bytes(NULL,0));
      for (t=0;t<800 && !tk_reply_done;t++) pf_pump(P,C,2);
      ST_CHECK(cr==RANT_OK && tk_reply_done && tk_reply_status==RANT_CALL_OK,
               "task: no_cancel task completes OK (st=%d)", tk_reply_status); }

    /* a task handler answering immediately via rant_request_reply works like a function */
    { uint8_t req[4]; int rc; i_rant_le_w32(req, 41);
      tk_reply_done=0;
      rc = rant_function_call_async(cinl, rant_bytes(req,4), tk_on_reply, NULL, NULL);
      ST_CHECK(rc==RANT_OK, "task: inline call accepted (%d)", rc);
      for (t=0;t<2000 && !tk_reply_done;t++) pf_pump(P,C,2);
      ST_CHECK(tk_reply_done && tk_reply_status==RANT_CALL_OK && tk_reply_val==42,
               "task: inline reply -> 42 OK (st=%d val=%u)", tk_reply_status, tk_reply_val); }

    /* bare return: a task's return is not its answer */
    { int rc;
      tk_reply_done=0;
      rc = rant_function_call_async(cbare, rant_bytes(NULL,0), tk_on_reply, NULL, NULL);
      ST_CHECK(rc==RANT_OK, "task: bare call accepted (%d)", rc);
      for (t=0;t<2000 && !tk_reply_done;t++) pf_pump(P,C,2);
      ST_CHECK(tk_reply_done && tk_reply_status==RANT_CALL_APP_ERROR,
               "task: bare return synthesizes APP_ERROR (st=%d)", tk_reply_status);
      ST_CHECK(strcmp(tk_reply_msg, "handler returned no result")==0,
               "task: synthesized message (\"%s\")", tk_reply_msg); }

    rant_node_close(P,0); rant_node_close(C,0);
    rant_allocator_reset(&pa); rant_allocator_reset(&ca);
}

/* The task matrix (19e8): concurrent caller demux, overlapping calls, provider selection,
 * peer loss, retire and close mid run, third party cancel, churn and reflection. */

/* pump a node set (NULL entries skipped, e.g. a mid-phase closed provider) */
static void txm_pump(RantNode **ns, int n, int ms){
    uint64_t end = i_rant_plat_now_us() + (uint64_t)ms*1000u;
    while (i_rant_plat_now_us() < end){
        int i, first = 1;
        for (i=0;i<n;i++) if (ns[i]){ rant_node_poll(ns[i], first ? 1 : 0); first = 0; }
    }
}

/* caller-side progress capture (rides RantCallOpts.progress_user) */
typedef struct {
    int n, first_empty, payloads, wrong_tag, misattr;
    uint32_t want_tag;        /* nonzero: every payload must carry this tag */
    uint32_t tags_seen;       /* bitmask of payload tags */
    uint32_t vals[4]; int nvals;   /* first payload values, arrival order */
    uint32_t call_of_tag[8];  /* nonzero: a payload of tag i must carry this call_id */
    const RantSchema *ps;     /* prg schema (NULL = raw le32 payload) */
} TxmProg;
static void txm_on_prog(const RantProgress *p){
    TxmProg *c = (TxmProg*)p->user;
    if (c->n == 0) c->first_empty = (p->data.len == 0);
    c->n++;
    if (!p->data.len) return;   /* the RUNNING ack */
    c->payloads++;
    {   uint32_t tag, v;
        if (c->ps){ tag = (uint32_t)rant_get_uint(p->data, c->ps, "tag");
                    v   = (uint32_t)rant_get_uint(p->data, c->ps, "v"); }
        else      { tag = 0; v = p->data.len >= 4 ? i_rant_le_r32(p->data.data) : 0; }
        if (tag < 32) c->tags_seen |= 1u << tag;
        if (c->want_tag && tag != c->want_tag) c->wrong_tag++;
        if (tag < 8 && c->call_of_tag[tag] && c->call_of_tag[tag] != p->call_id) c->misattr++;
        if (c->nvals < 4) c->vals[c->nvals++] = v;
    }
}
/* file-transfer capture: chunks must arrive complete and strictly ascending (1..n) */
static int txm_file_n, txm_file_seq_ok;
static void txm_on_file_prog(const RantProgress *p){
    if (!p->data.len) return;
    { uint32_t v = p->data.len >= 4 ? i_rant_le_r32(p->data.data) : 0;
      if (v != (uint32_t)txm_file_n + 1) txm_file_seq_ok = 0;
      txm_file_n++; }
}
/* caller-side response capture (rides the call's user pointer) */
typedef struct {
    volatile int done; RantCallStatus status; uint32_t provider;
    uint32_t tag, v, raw32;
    char msg[64]; size_t msg_len;
    const RantSchema *rs;       /* rsp schema (NULL = raw le32 payload) */
} TxmRsp;
static void txm_on_rsp(const RantResponse *r){
    TxmRsp *c = (TxmRsp*)r->user;
    c->status = r->status; c->provider = r->provider;
    if (r->data.len >= 4) c->raw32 = i_rant_le_r32(r->data.data);
    if (c->rs && r->schema){ c->tag = (uint32_t)rant_get_uint(r->data, c->rs, "tag");
                             c->v   = (uint32_t)rant_get_uint(r->data, c->rs, "v"); }
    c->msg_len = r->message.len < sizeof c->msg - 1 ? r->message.len : sizeof c->msg - 1;
    if (c->msg_len) memcpy(c->msg, r->message.data, c->msg_len);
    c->msg[c->msg_len] = 0;
    c->done = 1;
}
/* provider handlers */
static volatile uint64_t txm_tok[8];   /* mix: live token per request tag */
static void txm_mix_handler(RantRequest *req, void *user){
    uint32_t tag = req->schema ? (uint32_t)rant_get_uint(req->data, req->schema, "tag") : 0;
    (void)user;
    if (tag < 8) txm_tok[tag] = rant_request_defer(req);
}
static void txm_defer_handler(RantRequest *req, void *user){
    *(volatile uint64_t*)user = rant_request_defer(req);
}
static int txm_sel_p, txm_sel_p2;
static void txm_sel_handler(RantRequest *req, void *user){
    ++*(int*)user;
    rant_request_reply(req, rant_bytes(NULL,0));
}
static volatile uint64_t txm_cancel_tok; static volatile int txm_cancel_n;
static void txm_on_cancel(uint64_t token, void *user){ (void)user; txm_cancel_tok = token; txm_cancel_n++; }
static volatile uint64_t txm_file_tok, txm_ret_tok, txm_shut_tok, txm_churn_tok, txm_drop_tok;
/* raw @prg tap (the explorer's progress view): 8-byte [caller_lo][call_id] header */
static int txm_tap_n, txm_tap_bad_hdr, txm_tap_nlo;
static uint32_t txm_tap_lo[4];
static void txm_tap_on_msg(void *user, const RantMsg *msg){
    (void)user;
    if (msg->header.len != 8){ txm_tap_bad_hdr++; return; }
    { uint32_t lo = i_rant_le_r32(msg->header.data); int i;
      for (i=0;i<txm_tap_nlo;i++) if (txm_tap_lo[i]==lo) break;
      if (i==txm_tap_nlo && txm_tap_nlo<4) txm_tap_lo[txm_tap_nlo++] = lo; }
    txm_tap_n++;
}
static uint32_t txm_peer_by_name(RantNode *n, const char *name){
    uint16_t cnt, i; size_t nl = strlen(name);
    const RantDiscoveryPeer *ps = st_peers(n, &cnt);
    if (!ps) return 0;
    for (i=0;i<cnt;i++)
        if (ps[i].name.len==nl && memcmp(ps[i].name.data,name,nl)==0) return ps[i].id;
    return 0;
}
static RantBytes txm_enc(const RantSchema *s, uint32_t tag, uint32_t v, int has_v,
                         uint8_t *buf, size_t cap){
    rant_schema_message_default(s, buf, cap);
    rant_set_uint(buf, cap, s, "tag", tag);
    if (has_v) rant_set_uint(buf, cap, s, "v", v);
    return rant_bytes(buf, rant_schema_size(s));
}

static void taskx_checks(void){
    RantAllocator ap    = rant_allocator_heap(0);
    RantAllocator ap2 = rant_allocator_heap(0);
    RantAllocator ac1 = rant_allocator_heap(0);
    RantAllocator ac2 = rant_allocator_heap(0);
    RantAllocator ax    = rant_allocator_heap(0);
    RantAllocator ma    = rant_allocator_heap(0);
    RantNodeOpts o; RantDiscoveryAddr seed;
    RantNode *P, *P2, *C1, *C2, *X, *ns[5];
    RantSchema *req_s, *prg_s, *rsp_s;
    RantFunction *pmix, *pnoc, *pfile, *psel, *pret, *pret2, *psel2, *pshut;
    RantFunction *cmix1, *csel, *cfile, *cret, *cret2, *cshut, *cmix2, *xmix;
    RantTopic *xtap, *xreq;
    uint32_t p2_id_c1, p_id_x;
    int t;

    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&o,0,sizeof o); o.domain=(uint16_t)(ST_DOMAIN+34); o.discovery.max_peers=6;
    o.net.multicast_interface="127.0.0.1"; o.net.seed_peers=&seed; o.net.n_seed_peers=1;
    P  = rant_node_open(&ap,    "txm-prov",  NULL, NULL, &o);
    P2 = rant_node_open(&ap2, "txm-prov2", NULL, NULL, &o);
    { RantNodeOpts o1 = o; o1.fetch_details = 1;     /* C1 doubles as the reflection observer */
      C1 = rant_node_open(&ac1, "txm-c1", NULL, NULL, &o1); }
    C2 = rant_node_open(&ac2, "txm-c2",  NULL, NULL, &o);
    X  = rant_node_open(&ax,    "txm-obs", NULL, NULL, &o);
    ST_CHECK(P && P2 && C1 && C2 && X, "taskx: nodes open");
    if (!(P && P2 && C1 && C2 && X)){
        if(P)rant_node_close(P,0); if(P2)rant_node_close(P2,0); if(C1)rant_node_close(C1,0);
        if(C2)rant_node_close(C2,0); if(X)rant_node_close(X,0);
        return;
    }
    ns[0]=P; ns[1]=P2; ns[2]=C1; ns[3]=C2; ns[4]=X;

    /* three DISTINCT schemas so a crossed req/prg/rsp plumb cannot hide */
    req_s = rant_schema_compile(rant_allocator_alloc, &ma, "TmReq { tag: u32 }", NULL);
    prg_s = rant_schema_compile(rant_allocator_alloc, &ma, "TmPrg { tag: u32, v: u32 }", NULL);
    rsp_s = rant_schema_compile(rant_allocator_alloc, &ma, "TmRsp { tag: u32, v: u32, note: u32 }", NULL);
    ST_CHECK(req_s && prg_s && rsp_s, "taskx: schemas compiled");

    /* X's raw wire, the explorer's recipe. A normal remote handle on top of it is refused,
       since a live same name twin would only be shadowed. */
    { RantTopicOpts topt; memset(&topt,0,sizeof topt);
      topt.qos.reliability = RANT_RELIABLE; topt.qos.keep_last = 4;
      txm_tap_n=0; txm_tap_bad_hdr=0; txm_tap_nlo=0;
      xtap = i_rant_node_create_pattern_topic(X, "mix@prg", RANT_SUB_ONLY, prg_s, &topt,
                          RANT_KIND_TASK_PRG, 8, 0, 0, txm_tap_on_msg, NULL);
      xreq = i_rant_node_create_pattern_topic(X, "mix@req", RANT_PUB_ONLY, req_s, &topt,
                          RANT_KIND_TASK_REQ, 5, 0, 0, NULL, NULL);
      xmix = rant_node_create_remote_task(X, "mix", req_s, prg_s, rsp_s, NULL);
      ST_CHECK(xtap && xreq && xmix==NULL && rant_last_error(X).error==RANT_E_NAME_COLLISION,
               "taskx: raw tap + raw req created, a same name remote refused (%d)",
               (int)rant_last_error(X).error); }

    pmix  = rant_node_create_task_definition(P, "mix", req_s, prg_s, rsp_s, txm_mix_handler, NULL, NULL);
    pnoc  = rant_node_create_task_definition(P, "noc", NULL, NULL, NULL, NULL, NULL,
                                             &(RantTaskOpts){ .no_cancel = 1, .exclusive = 1 });
    pfile = rant_node_create_task_definition(P, "file", NULL, NULL, NULL, txm_defer_handler,
                                             (void*)&txm_file_tok, NULL);
    psel  = rant_node_create_task_definition(P, "sel", NULL, NULL, NULL, txm_sel_handler,
                                             &txm_sel_p, &(RantTaskOpts){ .multi = 1 });
    pret  = rant_node_create_task_definition(P, "ret", NULL, NULL, NULL, txm_defer_handler,
                                             (void*)&txm_ret_tok, NULL);
    pret2 = rant_node_create_task_definition(P, "ret2", NULL, NULL, NULL, txm_defer_handler,
                                             (void*)&txm_ret_tok, NULL);
    psel2 = rant_node_create_task_definition(P2, "sel", NULL, NULL, NULL, txm_sel_handler,
                                             &txm_sel_p2, &(RantTaskOpts){ .multi = 1 });
    pshut = rant_node_create_task_definition(P2, "shut", NULL, NULL, NULL, txm_defer_handler,
                                             (void*)&txm_shut_tok, NULL);
    cmix1 = rant_node_create_remote_task(C1, "mix", req_s, prg_s, rsp_s, NULL);
    csel  = rant_node_create_remote_task(C1, "sel", NULL, NULL, NULL, &(RantTaskOpts){ .multi = 1 });
    cfile = rant_node_create_remote_task(C1, "file", NULL, NULL, NULL, NULL);
    cret  = rant_node_create_remote_task(C1, "ret", NULL, NULL, NULL, NULL);
    cret2 = rant_node_create_remote_task(C1, "ret2", NULL, NULL, NULL, NULL);
    cshut = rant_node_create_remote_task(C1, "shut", NULL, NULL, NULL, NULL);
    cmix2 = rant_node_create_remote_task(C2, "mix", req_s, prg_s, rsp_s, NULL);
    ST_CHECK(pmix&&pnoc&&pfile&&psel&&pret&&pret2&&psel2&&pshut&&cmix1&&csel&&cfile&&cret&&cret2
             &&cshut&&cmix2, "taskx: entities created");
    rant_function_on_cancel(pmix, txm_on_cancel, NULL);

    for (t=0;t<3000 && !(rant_function_match_count(cmix1)==1 && rant_function_match_count(cmix2)==1
           && rant_function_match_count(csel)==2 && rant_function_match_count(cfile)==1
           && rant_function_match_count(cret)==1 && rant_function_match_count(cret2)==1
           && rant_function_match_count(cshut)==1
           && i_rant_topic_source_match_count(xtap)>=1 && rant_topic_match_count(xreq)>=1);t++)
        txm_pump(ns,5,2);
    ST_CHECK(rant_function_match_count(cmix1)==1 && rant_function_match_count(cmix2)==1
             && rant_function_match_count(csel)==2 && i_rant_topic_source_match_count(xtap)>=1
             && rant_topic_match_count(xreq)>=1,
             "taskx: full mesh matched (mix=%d/%d sel=%d tap=%d raw=%d)",
             rant_function_match_count(cmix1), rant_function_match_count(cmix2),
             rant_function_match_count(csel), i_rant_topic_source_match_count(xtap),
             rant_topic_match_count(xreq));
    p2_id_c1 = txm_peer_by_name(C1, "txm-prov2");
    p_id_x   = txm_peer_by_name(X,  "txm-prov");
    ST_CHECK(p2_id_c1 && p_id_x, "taskx: provider peer ids resolved (%u/%u)", p2_id_c1, p_id_x);

    /* 1. CONCURRENT CALLER DEMUX: C1 and C2 both fire their first call, so both call ids are
       1. P defers both with distinct payloads and each caller must see only its own. */
    { static TxmProg g1, g2; static TxmRsp r1, r2;
      uint8_t b1[64], b2[64]; uint32_t id1=0, id2=0; int rc1, rc2;
      memset(&g1,0,sizeof g1); memset(&g2,0,sizeof g2);
      memset(&r1,0,sizeof r1); memset(&r2,0,sizeof r2);
      g1.ps = prg_s; g1.want_tag = 1; g2.ps = prg_s; g2.want_tag = 2;
      r1.rs = rsp_s; r2.rs = rsp_s;
      txm_tok[1]=0; txm_tok[2]=0;
      rc1 = rant_function_call_async(cmix1, txm_enc(req_s,1,0,0,b1,sizeof b1), txm_on_rsp, &r1,
              &(RantCallOpts){ .on_progress = txm_on_prog, .progress_user = &g1, .id_out = &id1 });
      rc2 = rant_function_call_async(cmix2, txm_enc(req_s,2,0,0,b2,sizeof b2), txm_on_rsp, &r2,
              &(RantCallOpts){ .on_progress = txm_on_prog, .progress_user = &g2, .id_out = &id2 });
      ST_CHECK(rc1==RANT_OK && rc2==RANT_OK && id1==1 && id2==1,
               "taskx: first calls collide on call_id 1 (rc=%d/%d id=%u/%u)", rc1, rc2, id1, id2);
      for (t=0;t<2000 && !(txm_tok[1] && txm_tok[2]);t++) txm_pump(ns,5,2);
      ST_CHECK(txm_tok[1] && txm_tok[2], "taskx: both requests deferred");
      for (t=0;t<800 && !(g1.n>=1 && g2.n>=1);t++) txm_pump(ns,5,2);
      ST_CHECK(g1.n>=1 && g1.first_empty && g2.n>=1 && g2.first_empty,
               "taskx: both RUNNING acks fired on_progress first, empty (%d/%d)", g1.n, g2.n);
      { static const uint32_t seq[4][2] = { {1,10},{2,11},{1,20},{2,21} };
        uint8_t pb[64]; int i2, w1=0, w2=0, prs=1;
        for (i2=0;i2<4;i2++){
            uint32_t tg = seq[i2][0], v = seq[i2][1];
            if (rant_function_progress(pmix, txm_tok[tg],
                                       txm_enc(prg_s,tg,v,1,pb,sizeof pb)) != RANT_OK) prs = 0;
            if (tg==1) w1++; else w2++;
            for (t=0;t<400 && !((tg==1?g1.payloads:g2.payloads) >= (tg==1?w1:w2));t++)
                txm_pump(ns,5,2);
        }
        ST_CHECK(prs, "taskx: interleaved progress sends accepted"); }
      ST_CHECK(g1.payloads==2 && g1.wrong_tag==0 && g1.tags_seen==(1u<<1)
               && g1.vals[0]==10 && g1.vals[1]==20,
               "taskx: C1 saw ONLY its own payloads (n=%d wrong=%u seen=%#x v=%u,%u)",
               g1.payloads, g1.wrong_tag, g1.tags_seen, g1.vals[0], g1.vals[1]);
      ST_CHECK(g2.payloads==2 && g2.wrong_tag==0 && g2.tags_seen==(1u<<2)
               && g2.vals[0]==11 && g2.vals[1]==21,
               "taskx: C2 saw ONLY its own payloads (n=%d wrong=%u seen=%#x v=%u,%u)",
               g2.payloads, g2.wrong_tag, g2.tags_seen, g2.vals[0], g2.vals[1]);
      { int c1r = rant_function_complete(pmix, txm_tok[1], RANT_CALL_OK, NULL,
                                         txm_enc(rsp_s,1,100,1,b1,sizeof b1));
        int c2r = rant_function_complete(pmix, txm_tok[2], RANT_CALL_OK, NULL,
                                         txm_enc(rsp_s,2,200,1,b2,sizeof b2));
        ST_CHECK(c1r==RANT_OK && c2r==RANT_OK, "taskx: both completes sent (%d/%d)", c1r, c2r); }
      for (t=0;t<800 && !(r1.done && r2.done);t++) txm_pump(ns,5,2);
      ST_CHECK(r1.done && r1.status==RANT_CALL_OK && r1.tag==1 && r1.v==100,
               "taskx: C1 terminal routed (st=%d tag=%u v=%u)", r1.status, r1.tag, r1.v);
      ST_CHECK(r2.done && r2.status==RANT_CALL_OK && r2.tag==2 && r2.v==200,
               "taskx: C2 terminal routed (st=%d tag=%u v=%u)", r2.status, r2.tag, r2.v);
      /* 8. the raw tap observed both callers' progress with distinct caller_lo values. X's
         normal remote handle has no outstanding call, so nothing can fire progress on it. */
      txm_pump(ns,5,20);
      { uint32_t lo1 = i_rant_le_r32(i_rant_node_uuid(C1)), lo2 = i_rant_le_r32(i_rant_node_uuid(C2));
        int has1=0, has2=0, i2;
        for (i2=0;i2<txm_tap_nlo;i2++){ if (txm_tap_lo[i2]==lo1) has1=1; if (txm_tap_lo[i2]==lo2) has2=1; }
        ST_CHECK(txm_tap_n>=4 && txm_tap_bad_hdr==0 && txm_tap_nlo==2 && has1 && has2 && lo1!=lo2,
                 "taskx: raw tap saw both callers' caller_lo (n=%d bad=%d nlo=%d)",
                 txm_tap_n, txm_tap_bad_hdr, txm_tap_nlo); } }

    /* 2. two OVERLAPPING calls from ONE caller: two live tokens on P, interleaved
       progress attributed per call_id, both terminals */
    { static TxmProg g; static TxmRsp r3, r4;
      uint8_t b[64]; uint32_t id3=0, id4=0;
      memset(&g,0,sizeof g); memset(&r3,0,sizeof r3); memset(&r4,0,sizeof r4);
      g.ps = prg_s; r3.rs = rsp_s; r4.rs = rsp_s;
      txm_tok[3]=0; txm_tok[4]=0;
      rant_function_call_async(cmix2, txm_enc(req_s,3,0,0,b,sizeof b), txm_on_rsp, &r3,
          &(RantCallOpts){ .on_progress = txm_on_prog, .progress_user = &g, .id_out = &id3 });
      rant_function_call_async(cmix2, txm_enc(req_s,4,0,0,b,sizeof b), txm_on_rsp, &r4,
          &(RantCallOpts){ .on_progress = txm_on_prog, .progress_user = &g, .id_out = &id4 });
      ST_CHECK(id3 && id4 && id3!=id4, "taskx: overlapping ids distinct (%u/%u)", id3, id4);
      g.call_of_tag[3]=id3; g.call_of_tag[4]=id4;
      for (t=0;t<2000 && !(txm_tok[3] && txm_tok[4]);t++) txm_pump(ns,5,2);
      ST_CHECK(txm_tok[3] && txm_tok[4], "taskx: two live tokens on the provider");
      { static const uint32_t seq[4][2] = { {3,30},{4,40},{3,31},{4,41} }; int i2;
        for (i2=0;i2<4;i2++){
            (void)rant_function_progress(pmix, txm_tok[seq[i2][0]],
                                         txm_enc(prg_s,seq[i2][0],seq[i2][1],1,b,sizeof b));
            for (t=0;t<400 && g.payloads < i2+1;t++) txm_pump(ns,5,2);
        } }
      ST_CHECK(g.payloads==4 && g.misattr==0 && g.tags_seen==((1u<<3)|(1u<<4)),
               "taskx: interleaved progress attributed per call_id (n=%d misattr=%u)",
               g.payloads, g.misattr);
      rant_function_complete(pmix, txm_tok[3], RANT_CALL_OK, NULL, txm_enc(rsp_s,3,300,1,b,sizeof b));
      rant_function_complete(pmix, txm_tok[4], RANT_CALL_APP_ERROR, "worked anyway",
                             txm_enc(rsp_s,4,400,1,b,sizeof b));
      for (t=0;t<800 && !(r3.done && r4.done);t++) txm_pump(ns,5,2);
      ST_CHECK(r3.done && r3.status==RANT_CALL_OK && r3.tag==3 && r3.v==300,
               "taskx: first overlapped terminal (st=%d tag=%u)", r3.status, r3.tag);
      ST_CHECK(r4.done && r4.status==RANT_CALL_APP_ERROR && r4.tag==4 && r4.v==400
               && strcmp(r4.msg,"worked anyway")==0,
               "taskx: second overlapped terminal, message + data (st=%d msg=\"%s\")",
               r4.status, r4.msg); }

    /* 3. PROVIDER SELECTION: an explicit RantCallOpts.provider reaches exactly the chosen
       provider, provider 0 auto directs at the oldest matched */
    { static TxmRsp r; int before;
      memset(&r,0,sizeof r);
      txm_sel_p=0; txm_sel_p2=0;
      rant_function_call_async(csel, rant_bytes(NULL,0), txm_on_rsp, &r,
          &(RantCallOpts){ .provider = p2_id_c1 });
      for (t=0;t<800 && !r.done;t++) txm_pump(ns,5,2);
      txm_pump(ns,5,20);   /* the unchosen provider must stay silent */
      ST_CHECK(r.done && r.status==RANT_CALL_OK && r.provider==p2_id_c1
               && txm_sel_p2==1 && txm_sel_p==0,
               "taskx: explicit provider reached only P2 (p=%d p2=%d prov=%u)",
               txm_sel_p, txm_sel_p2, r.provider);
      before = txm_sel_p + txm_sel_p2;
      memset(&r,0,sizeof r);
      rant_function_call_async(csel, rant_bytes(NULL,0), txm_on_rsp, &r, NULL);
      for (t=0;t<800 && !r.done;t++) txm_pump(ns,5,2);
      txm_pump(ns,5,20);
      ST_CHECK(r.done && r.status==RANT_CALL_OK && txm_sel_p + txm_sel_p2 == before+1,
               "taskx: provider 0 auto-directed at ONE provider (p=%d p2=%d)",
               txm_sel_p, txm_sel_p2); }

    /* 7. THIRD-PARTY CANCEL, the explorer path: X crafts the raw CANCEL op naming C1's live
       call. The provider's on_cancel fires for exactly that call, C2's call is untouched. */
    { static TxmProg g5, g6; static TxmRsp r5, r6;
      uint8_t b[64]; uint32_t id5=0, id6=0;
      memset(&g5,0,sizeof g5); memset(&g6,0,sizeof g6);
      memset(&r5,0,sizeof r5); memset(&r6,0,sizeof r6);
      g5.ps=prg_s; g6.ps=prg_s; r5.rs=rsp_s; r6.rs=rsp_s;
      txm_tok[5]=0; txm_tok[6]=0; txm_cancel_n=0; txm_cancel_tok=0;
      rant_function_call_async(cmix1, txm_enc(req_s,5,0,0,b,sizeof b), txm_on_rsp, &r5,
          &(RantCallOpts){ .on_progress = txm_on_prog, .progress_user = &g5, .id_out = &id5 });
      rant_function_call_async(cmix2, txm_enc(req_s,6,0,0,b,sizeof b), txm_on_rsp, &r6,
          &(RantCallOpts){ .on_progress = txm_on_prog, .progress_user = &g6, .id_out = &id6 });
      for (t=0;t<2000 && !(txm_tok[5] && txm_tok[6]);t++) txm_pump(ns,5,2);
      ST_CHECK(txm_tok[5] && txm_tok[6] && id5!=0, "taskx: victim + bystander calls running");
      { uint8_t hdr[5], pl[4]; int sr;
        i_rant_le_w32(hdr, id5); hdr[4] = 1;     /* op 1 = CANCEL */
        i_rant_le_w32(pl, i_rant_le_r32(i_rant_node_uuid(C1)));         /* the victim's caller_lo */
        sr = i_rant_topic_send_to(xreq, p_id_x, rant_bytes(hdr,5), rant_bytes(pl,4));
        ST_CHECK(sr==RANT_OK, "taskx: raw third-party cancel sent (%d)", sr); }
      for (t=0;t<800 && !txm_cancel_n;t++) txm_pump(ns,5,2);
      ST_CHECK(txm_cancel_n==1 && txm_cancel_tok==txm_tok[5],
               "taskx: on_cancel fired for the victim call (n=%d)", txm_cancel_n);
      { int c5 = rant_function_cancelled(pmix, txm_tok[5]);
        int c6 = rant_function_cancelled(pmix, txm_tok[6]);
        ST_CHECK(c5==1 && c6==0, "taskx: only the victim's flag set (%d/%d)", c5, c6); }
      rant_function_complete(pmix, txm_tok[5], RANT_CALL_CANCELLED, "stopped", rant_bytes(NULL,0));
      rant_function_complete(pmix, txm_tok[6], RANT_CALL_OK, NULL, txm_enc(rsp_s,6,600,1,b,sizeof b));
      for (t=0;t<800 && !(r5.done && r6.done);t++) txm_pump(ns,5,2);
      ST_CHECK(r5.done && r5.status==RANT_CALL_CANCELLED && strcmp(r5.msg,"stopped")==0,
               "taskx: victim caller sees CANCELLED (st=%d msg=\"%s\")", r5.status, r5.msg);
      ST_CHECK(r6.done && r6.status==RANT_CALL_OK && r6.v==600,
               "taskx: bystander call unaffected (st=%d v=%u)", r6.status, r6.v); }

    /* 5. RETIRE MID-RUN through both delivery paths: the wire CANCELLED, and the caller side
       severed lane backstop when the announce arrives first (spec/testing.md). */
    { static TxmProg g; static TxmRsp r;
      uint8_t b[8]; uint32_t id=0; uint64_t dead;
      memset(&g,0,sizeof g); memset(&r,0,sizeof r);
      txm_ret_tok = 0;
      rant_function_call_async(cret, rant_bytes(NULL,0), txm_on_rsp, &r,
          &(RantCallOpts){ .on_progress = txm_on_prog, .progress_user = &g, .id_out = &id });
      for (t=0;t<2000 && !(txm_ret_tok && g.n>=1);t++) txm_pump(ns,5,2);
      ST_CHECK(txm_ret_tok && g.n>=1, "taskx: retire victim RUNNING");
      dead = txm_ret_tok;
      { int rr = rant_function_retire(pret);
        ST_CHECK(rr==RANT_OK, "taskx: definition retired mid-run (%d)", rr); }
      pret = NULL;
      for (t=0;t<400 && !r.done;t++) rant_node_poll(C1, 2);     /* caller first: the wire path */
      ST_CHECK(r.done && r.status==RANT_CALL_CANCELLED && strcmp(r.msg,"provider retired")==0,
               "taskx: caller got wire CANCELLED \"provider retired\" (st=%d msg=\"%s\")",
               r.done ? (int)r.status : -1, r.msg);
      i_rant_le_w32(b, 0);
      { int a = rant_function_progress (pmix, dead, rant_bytes(b,4));
        int c = rant_function_cancelled(pmix, dead);
        int d = rant_function_complete (pmix, dead, RANT_CALL_OK, NULL, rant_bytes(NULL,0));
        ST_CHECK(a==RANT_ERR_STATE && c==RANT_ERR_STATE && d==RANT_ERR_STATE,
                 "taskx: dead token refused everywhere (%d/%d/%d)", a, c, d); }
      /* round 2: provider pumped first, so the retire announce beats the wire reply */
      memset(&g,0,sizeof g); memset(&r,0,sizeof r);
      txm_ret_tok = 0;
      rant_function_call_async(cret2, rant_bytes(NULL,0), txm_on_rsp, &r,
          &(RantCallOpts){ .on_progress = txm_on_prog, .progress_user = &g, .id_out = &id });
      for (t=0;t<2000 && !(txm_ret_tok && g.n>=1);t++) txm_pump(ns,5,2);
      ST_CHECK(txm_ret_tok && g.n>=1, "taskx: second retire victim RUNNING");
      { int rr = rant_function_retire(pret2);
        ST_CHECK(rr==RANT_OK, "taskx: second definition retired (%d)", rr); }
      pret2 = NULL;
      for (t=0;t<800 && !r.done;t++) txm_pump(ns,5,2);   /* P polls first each pass */
      ST_CHECK(r.done && r.status==RANT_CALL_CANCELLED && strcmp(r.msg,"provider retired")==0,
               "taskx: severed lane still resolves CANCELLED (st=%d msg=\"%s\")",
               r.done ? (int)r.status : -1, r.msg); }

    /* 6. CLOSE MID-RUN: the provider closes with the call RUNNING. No BYE is sent, so the
       outcome can only be the close hook's CANCELLED "node closing". */
    { static TxmProg g; static TxmRsp r;
      uint32_t id=0;
      memset(&g,0,sizeof g); memset(&r,0,sizeof r);
      txm_shut_tok = 0;
      rant_function_call_async(cshut, rant_bytes(NULL,0), txm_on_rsp, &r,
          &(RantCallOpts){ .on_progress = txm_on_prog, .progress_user = &g, .id_out = &id });
      for (t=0;t<2000 && !(txm_shut_tok && g.n>=1);t++) txm_pump(ns,5,2);
      ST_CHECK(txm_shut_tok && g.n>=1, "taskx: close victim RUNNING");
      rant_node_close(P2, 0);
      ns[1] = NULL; P2 = NULL;
      for (t=0;t<800 && !r.done;t++) txm_pump(ns,5,2);
      ST_CHECK(r.done && r.status==RANT_CALL_CANCELLED && strcmp(r.msg,"node closing")==0,
               "taskx: caller resolved at provider close (st=%d msg=\"%s\")",
               r.done ? (int)r.status : -1, r.msg); }

    /* 9. FILE-TRANSFER SHAPE: 50 reliable 4 byte chunks as progress, one per app loop pass,
       then the terminal summary. Reliable progress loses nothing and stays in order. */
    { static TxmRsp r; uint8_t b[8]; uint32_t id=0; int i2, sent_ok=1;
      memset(&r,0,sizeof r);
      txm_file_tok=0; txm_file_n=0; txm_file_seq_ok=1;
      rant_function_call_async(cfile, rant_bytes(NULL,0), txm_on_rsp, &r,
          &(RantCallOpts){ .on_progress = txm_on_file_prog, .id_out = &id });
      for (t=0;t<2000 && !txm_file_tok;t++) txm_pump(ns,5,2);
      ST_CHECK(txm_file_tok!=0, "taskx: transfer deferred");
      for (i2=1;i2<=50;i2++){
          i_rant_le_w32(b, (uint32_t)i2);
          if (rant_function_progress(pfile, txm_file_tok, rant_bytes(b,4)) != RANT_OK) sent_ok = 0;
          for (t=0;t<400 && txm_file_n < i2;t++) txm_pump(ns,5,2);
      }
      ST_CHECK(sent_ok, "taskx: every chunk send accepted");
      ST_CHECK(txm_file_n==50 && txm_file_seq_ok,
               "taskx: 50 chunks, strictly ascending (n=%d ok=%d)", txm_file_n, txm_file_seq_ok);
      i_rant_le_w32(b, 50);
      rant_function_complete(pfile, txm_file_tok, RANT_CALL_OK, "done", rant_bytes(b,4));
      for (t=0;t<800 && !r.done;t++) txm_pump(ns,5,2);
      ST_CHECK(r.done && r.status==RANT_CALL_OK && r.raw32==50 && strcmp(r.msg,"done")==0,
               "taskx: terminal summary after the chunks (st=%d n=%u)", r.status, r.raw32); }

    /* 10. TASK CHURN: retire remote and definition, re-create the SAME name, and a full
       defer + progress + complete round trip works again, three cycles */
    { int cyc;
      for (cyc=0; cyc<3; cyc++){
          RantFunction *d, *c; static TxmProg g; static TxmRsp r;
          uint8_t b[8]; uint32_t id=0;
          memset(&g,0,sizeof g); memset(&r,0,sizeof r);
          txm_churn_tok = 0;
          d = rant_node_create_task_definition(P, "churn", NULL, NULL, NULL,
                  txm_defer_handler, (void*)&txm_churn_tok, NULL);
          c = rant_node_create_remote_task(C1, "churn", NULL, NULL, NULL, NULL);
          ST_CHECK(d && c, "taskx: churn cycle %d pair created", cyc);
          if (!(d && c)) break;
          for (t=0;t<2000 && rant_function_match_count(c)==0;t++) txm_pump(ns,5,2);
          rant_function_call_async(c, rant_bytes(NULL,0), txm_on_rsp, &r,
              &(RantCallOpts){ .on_progress = txm_on_prog, .progress_user = &g, .id_out = &id });
          for (t=0;t<2000 && !txm_churn_tok;t++) txm_pump(ns,5,2);
          if (txm_churn_tok){
              i_rant_le_w32(b, (uint32_t)(cyc+1));
              (void)rant_function_progress(d, txm_churn_tok, rant_bytes(b,4));
              for (t=0;t<400 && g.payloads<1;t++) txm_pump(ns,5,2);
              i_rant_le_w32(b, (uint32_t)(100+cyc));
              rant_function_complete(d, txm_churn_tok, RANT_CALL_OK, NULL, rant_bytes(b,4));
          }
          for (t=0;t<800 && !r.done;t++) txm_pump(ns,5,2);
          ST_CHECK(r.done && r.status==RANT_CALL_OK && r.raw32==(uint32_t)(100+cyc)
                   && g.payloads==1,
                   "taskx: churn cycle %d round trip (st=%d v=%u prg=%d)",
                   cyc, r.done ? (int)r.status : -1, r.raw32, g.payloads);
          rant_function_retire(c); rant_function_retire(d);
      } }

    /* 11. REFLECTION: from C1, one RANT_ENTITY_TASK per task name on P, attrs decoded, the
       three schema slots plumbed distinctly. The retired names must be gone. */
    { uint32_t pid = txm_peer_by_name(C1, "txm-prov");
      int tries, tasks=0, others=0, ats=0, inc=0;
      int have_mix=0, have_sel=0, have_noc=0, have_file=0;
      RantEntityInfo mix_e, sel_e, noc_e, file_e;
      memset(&mix_e,0,sizeof mix_e); memset(&sel_e,0,sizeof sel_e);
      memset(&noc_e,0,sizeof noc_e); memset(&file_e,0,sizeof file_e);
      ST_CHECK(pid!=0, "taskx: provider peer visible at C1");
      for (tries=0; tries<400; tries++){   /* let straggling detail fetches settle */
          RantIter it; RantEntityInfo ei; size_t k;
          tasks=others=ats=inc=0; have_mix=have_sel=have_noc=have_file=0;
          memset(&it,0,sizeof it);
          while (rant_node_entities_next(C1, pid, &it, &ei)){
              if (ei.kind==RANT_ENTITY_TASK) tasks++; else others++;
              inc += ei.incomplete;
              for (k=0;k<ei.name.len;k++) if (ei.name.data[k]=='@') ats++;
              if (ei.name.len==3 && !memcmp(ei.name.data,"mix",3)){ mix_e=ei; have_mix=1; }
              if (ei.name.len==3 && !memcmp(ei.name.data,"sel",3)){ sel_e=ei; have_sel=1; }
              if (ei.name.len==3 && !memcmp(ei.name.data,"noc",3)){ noc_e=ei; have_noc=1; }
              if (ei.name.len==4 && !memcmp(ei.name.data,"file",4)){ file_e=ei; have_file=1; }
          }
          if (tasks==4 && others==0 && inc==0 && have_mix && have_sel && have_noc && have_file
              && mix_e.schema && mix_e.rsp_schema && mix_e.progress_schema
              && noc_e.exclusive) break;
          txm_pump(ns,5,2);
      }
      ST_CHECK(tasks==4 && others==0 && ats==0 && inc==0,
               "taskx: reflect: one task entity per name, folded (tasks=%d others=%d @=%d inc=%d)",
               tasks, others, ats, inc);
      ST_CHECK(have_mix && mix_e.provides && mix_e.cancellable==1 && mix_e.exclusive==0
               && mix_e.multi==0,
               "taskx: reflect: mix attrs default (cancel=%d excl=%d multi=%d)",
               mix_e.cancellable, mix_e.exclusive, mix_e.multi);
      ST_CHECK(have_noc && noc_e.cancellable==0 && noc_e.exclusive==1,
               "taskx: reflect: .no_cancel + .exclusive mirrored (cancel=%d excl=%d)",
               noc_e.cancellable, noc_e.exclusive);
      ST_CHECK(have_sel && sel_e.multi==1 && sel_e.cancellable==1,
               "taskx: reflect: .multi mirrored (multi=%d cancel=%d)", sel_e.multi, sel_e.cancellable);
      ST_CHECK(have_mix && mix_e.schema && mix_e.rsp_schema && mix_e.progress_schema
               && mix_e.schema_hash==rant_schema_hash(req_s)
               && mix_e.rsp_schema_hash==rant_schema_hash(rsp_s)
               && mix_e.progress_schema_hash==rant_schema_hash(prg_s),
               "taskx: reflect: req/prg/rsp schemas plumbed distinctly "
               "(req %llx/%llx prg %llx/%llx rsp %llx/%llx)",
               (unsigned long long)mix_e.schema_hash, (unsigned long long)rant_schema_hash(req_s),
               (unsigned long long)mix_e.progress_schema_hash, (unsigned long long)rant_schema_hash(prg_s),
               (unsigned long long)mix_e.rsp_schema_hash, (unsigned long long)rant_schema_hash(rsp_s));
      ST_CHECK(have_file && file_e.cancellable==1,
               "taskx: reflect: untyped task cancellable by default (%d)", file_e.cancellable); }

    rant_node_close(P,0); rant_node_close(C1,0); rant_node_close(C2,0); rant_node_close(X,0);
    rant_allocator_reset(&ap); rant_allocator_reset(&ap2); rant_allocator_reset(&ac1);
    rant_allocator_reset(&ac2); rant_allocator_reset(&ax);

    /* 4. PEER LOSS MID-RUN on a short timeout pair: RUNNING dropped the deadline, then the
       provider stops pumping. The caller's reap must answer PEER_LOST. */
    { RantAllocator apd = rant_allocator_heap(0);
      RantAllocator acd = rant_allocator_heap(0);
      RantNodeOpts od; RantNode *PD, *CD, *pair[2];
      RantFunction *pd, *cd; static TxmProg g; static TxmRsp r;
      uint32_t id=0; uint64_t give_up;
      memset(&od,0,sizeof od); od.domain=(uint16_t)(ST_DOMAIN+36); od.discovery.max_peers=4;
      od.discovery.announce_interval_us=100000; od.discovery.peer_timeout_us=600000;
      od.net.multicast_interface="127.0.0.1"; od.net.seed_peers=&seed; od.net.n_seed_peers=1;
      PD = rant_node_open(&apd, "txm-drop-p", NULL, NULL, &od);
      CD = rant_node_open(&acd, "txm-drop-c", NULL, NULL, &od);
      ST_CHECK(PD && CD, "taskx: drop pair open");
      if (PD && CD){
          txm_drop_tok = 0;
          pd = rant_node_create_task_definition(PD, "goner", NULL, NULL, NULL,
                  txm_defer_handler, (void*)&txm_drop_tok, NULL);
          cd = rant_node_create_remote_task(CD, "goner", NULL, NULL, NULL,
                  &(RantTaskOpts){ .timeout_us = 300000 });
          ST_CHECK(pd && cd, "taskx: drop pair created");
          memset(&g,0,sizeof g); memset(&r,0,sizeof r);
          pair[0]=PD; pair[1]=CD;
          for (t=0;t<2000 && rant_function_match_count(cd)==0;t++) txm_pump(pair,2,2);
          rant_function_call_async(cd, rant_bytes(NULL,0), txm_on_rsp, &r,
              &(RantCallOpts){ .on_progress = txm_on_prog, .progress_user = &g, .id_out = &id });
          for (t=0;t<2000 && !(txm_drop_tok && g.n>=1);t++) txm_pump(pair,2,2);
          ST_CHECK(txm_drop_tok && g.n>=1, "taskx: drop victim RUNNING (deadline dropped)");
          /* the provider goes silent: only the caller's discovery reap can answer now */
          give_up = i_rant_plat_now_us() + 3000000u;
          while (!r.done && i_rant_plat_now_us() < give_up) rant_node_poll(CD, 5);
          ST_CHECK(r.done && r.status==RANT_CALL_PEER_LOST,
                   "taskx: RUNNING call fails PEER_LOST on the drop, not TIMEOUT (st=%d)",
                   r.done ? (int)r.status : -1);
      }
      if (PD) rant_node_close(PD,0);
      if (CD) rant_node_close(CD,0);
      rant_allocator_reset(&apd); rant_allocator_reset(&acd); }

    rant_allocator_reset(&ma);
}

/* The duplicate authority diagnostic (19e2): one provider per function and one owner per
 * variable, the conflict detected off the announce interest on both rivals. */
static void dup_on_event(const RantEvent *ev){
    if (ev->kind == RANT_ERROR && ev->error == RANT_E_DUPLICATE_AUTHORITY)
        ++*(int*)ev->user;
}
static void dup_authority_checks(void){
    RantAllocator aa = rant_allocator_heap(0);
    RantAllocator ba = rant_allocator_heap(0);
    RantNodeOpts ao, bo; RantNode *A, *B; RantDiscoveryAddr seed;
    int a_dups = 0, b_dups = 0, t;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&ao,0,sizeof ao); ao.domain=ST_DOMAIN+21; ao.discovery.max_peers=4;
    ao.net.multicast_interface="127.0.0.1"; ao.net.seed_peers=&seed; ao.net.n_seed_peers=1;
    bo=ao;
    ao.user_data=&a_dups; bo.user_data=&b_dups;
    A = rant_node_open(&aa, "dup-a", NULL, dup_on_event, &ao);
    B = rant_node_open(&ba, "dup-b", NULL, dup_on_event, &bo);
    ST_CHECK(A && B, "dup: nodes open");
    if (!(A && B)){ if(A)rant_node_close(A,0); if(B)rant_node_close(B,0); return; }

    { RantVariable *va = rant_node_create_variable_definition(A, "dupvar", NULL, NULL);
      RantFunction *fa = rant_node_create_function_definition(A, "dupfn", NULL, NULL, pf_empty_handler, NULL, NULL);
      ST_CHECK(va && fa, "dup: first authorities created");
      /* wait until B holds A's interest: the create-time sweep's precondition */
      { uint32_t want = (uint32_t)rant_topic_id("dupvar"); int seen = 0;
        for (t=0;t<2000 && !seen;t++){
            const RantDiscoveryPeer *ps; uint16_t pc;
            pf_pump(A,B,2);
            ps = st_peers(B,&pc);
            if (ps && pc){ RantInterestIter it; RantTopicEntry e; memset(&it,0,sizeof it);
                while (i_rant_node_peer_interest_next(&ps[0], &it, &e))
                    if (e.hash==want){ seen=1; break; } }
        }
        ST_CHECK(seen, "dup: B holds A's interest"); }
      ST_CHECK(a_dups==0 && b_dups==0, "dup: quiet before the rival (a=%d b=%d)", a_dups, b_dups);

      { RantVariable *vb = rant_node_create_variable_definition(B, "dupvar", NULL, NULL);
        ST_CHECK(vb!=NULL, "dup: rival owner creates (diagnostic, not a refusal)");
        ST_CHECK(b_dups==1, "dup: rival owner detected at create (%d)", b_dups); }
      { RantFunction *fb = rant_node_create_function_definition(B, "dupfn", NULL, NULL, pf_empty_handler, NULL, NULL);
        ST_CHECK(fb!=NULL, "dup: rival provider creates");
        ST_CHECK(b_dups==2, "dup: rival provider detected at create (%d)", b_dups); }
      for (t=0;t<2000 && a_dups<2;t++) pf_pump(A,B,2);
      ST_CHECK(a_dups==2, "dup: first authority sees the rival's announce (%d)", a_dups);

      /* dedup + negative: repeated announces re-report nothing, and an accessor pairing
         with an owner is the LEGAL shape (sub side, no authority claim), never flagged */
      { RantVariable *acc    = rant_node_create_remote_variable(B, "solo", NULL, NULL);
        RantVariable *solo = rant_node_create_variable_definition(A, "solo", NULL, NULL);
        ST_CHECK(acc && solo, "dup: solo owner + accessor create");
        for (t=0;t<150;t++) pf_pump(A,B,2);
        ST_CHECK(a_dups==2 && b_dups==2,
                 "dup: once per (entity, peer); accessor never fires (a=%d b=%d)", a_dups, b_dups); } }

    rant_node_close(A,0); rant_node_close(B,0);
    rant_allocator_reset(&aa); rant_allocator_reset(&ba);
}

/* The pattern retire lifecycle (19e3): a second same name handle is shadowed while the
 * first lives, and retire parks the predecessor so a re created handle receives. */
static void retire_checks(void){
    RantAllocator aa = rant_allocator_heap(0);
    RantAllocator ba = rant_allocator_heap(0);
    RantNodeOpts ao, bo; RantNode *A, *B; RantDiscoveryAddr seed;
    int t;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&ao,0,sizeof ao); ao.domain=ST_DOMAIN+22; ao.discovery.max_peers=4;
    ao.net.multicast_interface="127.0.0.1"; ao.net.seed_peers=&seed; ao.net.n_seed_peers=1;
    bo=ao;
    A = rant_node_open(&aa, "retire-a", NULL, NULL, &ao);
    B = rant_node_open(&ba, "retire-b", NULL, NULL, &bo);
    ST_CHECK(A && B, "retire: nodes open");
    if (!(A && B)){ if(A)rant_node_close(A,0); if(B)rant_node_close(B,0); return; }

    /* variable: retire + recreate the accessor, then the definition */
    { RantVariable *def, *acc, *acc2; RantBytes gv; uint8_t b[4]; int rr;
      i_rant_le_w32(b, 20);
      def = rant_node_create_variable_definition(A, "dial", NULL,
                &(RantVariableOpts){ .initial = rant_bytes(b,4) });
      acc = rant_node_create_remote_variable(B, "dial", NULL, NULL);
      ST_CHECK(def && acc, "retire: variable pair created");
      for (t=0;t<2000 && !rant_variable_get(acc,&gv);t++) pf_pump(A,B,2);
      ST_CHECK(rant_variable_get(acc,&gv) && gv.len==4 && i_rant_le_r32(gv.data)==20,
               "retire: first accessor got the value (20)");
      rr = rant_variable_retire(acc);
      ST_CHECK(rr==RANT_OK, "retire: accessor retired (%d)", rr);
      acc2 = rant_node_create_remote_variable(B, "dial", NULL, NULL);
      ST_CHECK(acc2 != NULL, "retire: successor accessor created");
      for (t=0;t<2000 && !rant_variable_get(acc2,&gv);t++) pf_pump(A,B,2);
      ST_CHECK(rant_variable_get(acc2,&gv) && gv.len==4 && i_rant_le_r32(gv.data)==20,
               "retire: successor RECEIVES where a live twin would be shadowed (20)");
      i_rant_le_w32(b, 33);
      { int sr = rant_variable_set(acc2, rant_bytes(b,4));
        ST_CHECK(sr==RANT_OK, "retire: successor set accepted (%d)", sr); }
      for (t=0;t<2000;t++){ pf_pump(A,B,2);
          if (rant_variable_get(def,&gv)&&gv.len==4&&i_rant_le_r32(gv.data)==33) break; }
      ST_CHECK(rant_variable_get(def,&gv)&&gv.len==4&&i_rant_le_r32(gv.data)==33,
               "retire: successor write reached the definition (33)");
      /* definition retire + recreate: the restart-shaped recipe (the old channel must
         stop matching-and-refusing so the successor can serve) */
      rr = rant_variable_retire(def);
      ST_CHECK(rr==RANT_OK, "retire: definition retired (%d)", rr);
      i_rant_le_w32(b, 77);
      def = rant_node_create_variable_definition(A, "dial", NULL,
                &(RantVariableOpts){ .initial = rant_bytes(b,4) });
      ST_CHECK(def != NULL, "retire: successor definition created");
      for (t=0;t<2000;t++){ pf_pump(A,B,2);
          if (rant_variable_get(acc2,&gv)&&gv.len==4&&i_rant_le_r32(gv.data)==77) break; }
      ST_CHECK(rant_variable_get(acc2,&gv)&&gv.len==4&&i_rant_le_r32(gv.data)==77,
               "retire: successor definition serves the accessor (77)");
      /* retire from inside a callback: refused, handle stays valid (on_change replays
         inline at registration, so the attempt runs right here on this thread) */
      pf_retire_in_cb_rc = 1234;
      rant_variable_on_change(def, pf_on_var_retire_attempt, NULL);
      ST_CHECK(pf_retire_in_cb_rc==RANT_ERR_STATE,
               "retire: refused from a callback (%d)", pf_retire_in_cb_rc);
      rant_variable_on_change(def, NULL, NULL);
      { RantBytes chk; ST_CHECK(rant_variable_get(def,&chk)==1,
               "retire: handle survives the refused attempt"); } }

    /* function: an outstanding call cancels at retire, a retired then recreated remote calls */
    { RantFunction *fdef, *rem, *rem2, *never; uint8_t req[4]; int rr;
      fdef = rant_node_create_function_definition(A, "calc", NULL, NULL, pf_add_handler, NULL, NULL);
      rem  = rant_node_create_remote_function(B, "calc", NULL, NULL, NULL);
      ST_CHECK(fdef && rem, "retire: function pair created");
      for (t=0;t<2000 && rant_function_match_count(rem)==0;t++) pf_pump(A,B,2);
      never = rant_node_create_remote_function(B, "retire-never", NULL, NULL,
                &(RantFunctionOpts){ .timeout_us = 60000000u });
      ST_CHECK(never != NULL, "retire: never-served remote created");
      pf_cancel_count = 0; pf_cancel_status = RANT_CALL_OK;
      if (never){
          rant_function_call_async(never, rant_bytes(NULL,0), pf_on_cancel, NULL, NULL);
          rr = rant_function_retire(never);
          ST_CHECK(rr==RANT_OK && pf_cancel_count==1 && pf_cancel_status==RANT_CALL_CANCELLED,
                   "retire: outstanding call cancelled (rc=%d n=%d st=%d)",
                   rr, pf_cancel_count, (int)pf_cancel_status); }
      rr = rant_function_retire(rem);
      ST_CHECK(rr==RANT_OK, "retire: remote function retired (%d)", rr);
      rem2 = rant_node_create_remote_function(B, "calc", NULL, NULL, NULL);
      ST_CHECK(rem2 != NULL, "retire: successor remote created");
      pf_reply_done = 0;
      i_rant_le_w32(req, 41);
      rant_function_call_async(rem2, rant_bytes(req,4), pf_on_reply, NULL, NULL);
      for (t=0;t<2000 && !pf_reply_done;t++) pf_pump(A,B,2);
      ST_CHECK(pf_reply_done && pf_reply_status==RANT_CALL_OK && pf_reply_val==42,
               "retire: successor remote calls (done=%d st=%d val=%u)",
               pf_reply_done, pf_reply_status, pf_reply_val); }

    rant_node_close(A,0); rant_node_close(B,0);
    rant_allocator_reset(&aa); rant_allocator_reset(&ba);
}

static volatile int loud_reply_n;
static volatile int loud_reply_status;
static void loud_on_reply(const RantResponse *r){ loud_reply_status = (int)r->status; loud_reply_n++; }
static int loud_active_peers(RantNode *n){
    RantIter it; RantPeerInfo p; int c = 0;
    memset(&it, 0, sizeof it);
    while (rant_node_peers_next(n, &it, &p)) if (p.liveness == RANT_PEER_ACTIVE) c++;
    return c;
}

/* Loud refusals (19e9): a refused create says why and keeps its strings, a live same name
 * twin is refused, a typed topic refuses bytes its schema cannot read, an accessor's
 * force against an owner without allow_force is refused with nothing sent, and the
 * blocking call and the variable wait work under a service thread. */
static void loud_checks(void){
    RantAllocator aa = rant_allocator_heap(0);
    RantAllocator ba = rant_allocator_heap(0);
    RantNodeOpts ao, bo; RantNode *A, *B; RantDiscoveryAddr seed; RantEvent le;
    RantSchema *pose; char txt[192]; int t;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&ao,0,sizeof ao); ao.domain=ST_DOMAIN+23; ao.discovery.max_peers=4;
    ao.net.multicast_interface="127.0.0.1"; ao.net.seed_peers=&seed; ao.net.n_seed_peers=1;
    ao.match_wait_ms = -1;      /* the sends below reach nobody on purpose: no wait */
    bo=ao;
    A = rant_node_open(&aa, "loud-a", NULL, NULL, &ao);
    B = rant_node_open(&ba, "loud-b", NULL, NULL, &bo);
    ST_CHECK(A && B, "loud: nodes open");
    if (!(A && B)){ if(A)rant_node_close(A,0); if(B)rant_node_close(B,0); return; }

    le = rant_last_error(A);
    rant_event_str(&le, txt, sizeof txt);
    ST_CHECK(le.kind==RANT_ERROR && le.error==RANT_E_NONE && strcmp(txt,"no error")==0,
             "loud: a fresh node's last error reads \"no error\" (%s)", txt);
    ST_CHECK(rant_node_create_topic(A, "a@b", RANT_PUBSUB, NULL, NULL)==NULL
             && rant_last_error(A).error==RANT_E_BAD_NAME,
             "loud: '@' in a plain topic name refused as BAD_NAME (%d)", (int)rant_last_error(A).error);
    le = rant_last_error(A);
    ST_CHECK(le.topic_name && strcmp(le.topic_name,"a@b")==0,
             "loud: the last error keeps the name (%s)", le.topic_name ? le.topic_name : "null");

    pose = rant_schema_compile(rant_heap_realloc, NULL, "LoudPose { x: f32, y: f32 }", NULL);
    ST_CHECK(pose != NULL, "loud: schema compiled");
    {   RantTopic *tp = rant_node_create_topic(A, "loud/pose", RANT_PUB_ONLY, pose, NULL);
        RantTopic *twin = rant_node_create_topic(A, "loud/pose", RANT_SUB_ONLY, pose, NULL);
        RantTopic *parked;
        uint8_t junk[3] = {1,2,3}, good[8] = {0};
        int rc_junk, rc_good;
        ST_CHECK(tp && twin==NULL && rant_last_error(A).error==RANT_E_NAME_COLLISION,
                 "loud: a live same name topic is refused as NAME_COLLISION (%d)", (int)rant_last_error(A).error);
        parked = rant_node_create_topic(A, "loud/pose", RANT_INACTIVE, pose, NULL);
        ST_CHECK(parked != NULL, "loud: an INACTIVE twin is still accepted (the QoS switch)");
        le = rant_last_error(A);
        rant_event_str(&le, txt, sizeof txt);
        ST_CHECK(le.peer==0 && strstr(txt,"already created")!=NULL,
                 "loud: the collision text names this node (%s)", txt);
        rc_junk = tp ? rant_topic_send(tp, rant_bytes(junk,3), NULL) : 0;
        rc_good = tp ? rant_topic_send(tp, rant_bytes(good,8), NULL) : 0;
        ST_CHECK(rc_junk==RANT_ERR_SCHEMA, "loud: bytes the schema cannot read refused with RANT_ERR_SCHEMA (%d)", rc_junk);
        ST_CHECK(rc_good==RANT_OK, "loud: a well formed message still sends (%d)", rc_good);
    }
    {   /* pattern twins and the accessor force gate */
        RantVariable *v1, *v2, *acc, *knob, *kacc; RantBytes gv; uint8_t b[4]; int rc;
        i_rant_le_w32(b, 20);
        v1 = rant_node_create_variable_definition(A, "loud/dial", NULL,
                 &(RantVariableOpts){ .initial = rant_bytes(b,4) });
        v2 = rant_node_create_variable_definition(A, "loud/dial", NULL, NULL);
        ST_CHECK(v1 && v2==NULL && rant_last_error(A).error==RANT_E_NAME_COLLISION,
                 "loud: a live same name variable definition is refused (%d)", (int)rant_last_error(A).error);
        knob = rant_node_create_variable_definition(A, "loud/knob", NULL,
                 &(RantVariableOpts){ .initial = rant_bytes(b,4), .allow_force = 1 });
        acc  = rant_node_create_remote_variable(B, "loud/dial", NULL, NULL);
        kacc = rant_node_create_remote_variable(B, "loud/knob", NULL, NULL);
        ST_CHECK(knob && acc && kacc, "loud: variable pairs created");
        for (t=0;t<2000 && !(rant_variable_get(acc,&gv) && rant_variable_get(kacc,&gv));t++) pf_pump(A,B,2);
        ST_CHECK(rant_variable_get(acc,&gv) && rant_variable_get(kacc,&gv), "loud: accessors got the values");
        i_rant_le_w32(b, 99);
        rc = rant_variable_force(acc, rant_bytes(b,4));
        ST_CHECK(rc==RANT_ERR_ROLE, "loud: force against an owner without allow_force refused ROLE (%d)", rc);
        rc = rant_variable_unforce(acc);
        ST_CHECK(rc==RANT_ERR_ROLE, "loud: unforce likewise (%d)", rc);
        for (t=0;t<200;t++) pf_pump(A,B,2);
        ST_CHECK(v1 && !rant_variable_forced(v1), "loud: nothing reached the owner");
        rc = rant_variable_force(kacc, rant_bytes(b,4));
        ST_CHECK(rc==RANT_OK, "loud: force against a forceable owner sends (%d)", rc);
        for (t=0;t<2000 && !(knob && rant_variable_forced(knob));t++) pf_pump(A,B,2);
        ST_CHECK(knob && rant_variable_forced(knob), "loud: the forceable owner is forced");
    }
    {   /* a queued call: an unrelated peer leaving is not its loss, its deadline ends it
           NO_PROVIDER, and the blocking form reports that outcome */
        RantAllocator ca = rant_allocator_heap(0); RantNodeOpts co = ao; RantNode *C;
        RantFunction *nobody; RantResponse rep; int rc;
        C = rant_node_open(&ca, "loud-c", NULL, NULL, &co);
        nobody = rant_node_create_remote_function(B, "loud/nobody", NULL, NULL,
                     &(RantFunctionOpts){ .timeout_us = 1500000u });
        ST_CHECK(C && nobody, "loud: third node and never served remote created");
        for (t=0;t<3000 && loud_active_peers(B) < 2;t++){ pf_pump(A,B,1); if (C) rant_node_poll(C,1); }
        ST_CHECK(loud_active_peers(B) >= 2, "loud: B sees both peers (%d)", loud_active_peers(B));
        loud_reply_n = 0; loud_reply_status = -1;
        rc = nobody ? rant_function_call_async(nobody, rant_bytes(NULL,0), loud_on_reply, NULL, NULL) : -99;
        ST_CHECK(rc==RANT_OK, "loud: call queued with no provider (%d)", rc);
        if (C){ rant_node_close(C,1); C = NULL; }
        /* bounded by the clock, not by passes: a coarse timer makes a 2 ms poll take 15 */
        { uint64_t end = i_rant_plat_now_us() + 400000u; while (i_rant_plat_now_us() < end) pf_pump(A,B,1); }
        ST_CHECK(loud_active_peers(B) == 1 && loud_reply_n == 0,
                 "loud: the unrelated peer left and the queued call still waits (peers=%d n=%d)",
                 loud_active_peers(B), loud_reply_n);
        for (t=0;t<4000 && loud_reply_n == 0;t++) pf_pump(A,B,1);
        ST_CHECK(loud_reply_n == 1 && loud_reply_status == (int)RANT_CALL_NO_PROVIDER,
                 "loud: the queued call ended NO_PROVIDER at its deadline (n=%d st=%d)",
                 loud_reply_n, loud_reply_status);
        memset(&rep, 0, sizeof rep);
        rc = nobody ? rant_function_call(nobody, rant_bytes(NULL,0), &rep, -1, NULL) : -99;
        ST_CHECK(rc==1 && rep.status==RANT_CALL_NO_PROVIDER && rep.message.len==11
                 && memcmp(rep.message.data, "no provider", 11)==0,
                 "loud: the blocking call reports NO_PROVIDER (rc=%d st=%d)", rc, (int)rep.status);
        rant_allocator_reset(&ca);
    }
#ifdef RANT_THREADS
    {   /* the blocking forms under a service thread: sleep on its progress, no refusal */
        RantFunction *fdef, *rem; RantVariable *late, *lacc; RantResponse rep;
        uint8_t req[4], b[4]; int rc, s1, s2;
        fdef = rant_node_create_function_definition(A, "loud/add", NULL, NULL, pf_add_handler, NULL, NULL);
        rem  = rant_node_create_remote_function(B, "loud/add", NULL, NULL, NULL);
        lacc = rant_node_create_remote_variable(B, "loud/late", NULL, NULL);
        ST_CHECK(fdef && rem && lacc, "loud: threaded pairs created");
        s1 = rant_node_start(A); s2 = rant_node_start(B);
        ST_CHECK(s1==RANT_OK && s2==RANT_OK, "loud: service threads start");
        i_rant_le_w32(req, 41);
        memset(&rep, 0, sizeof rep);
        rc = rem ? rant_function_call(rem, rant_bytes(req,4), &rep, 5000, NULL) : -99;
        ST_CHECK(rc==1 && rep.status==RANT_CALL_OK && rep.data.len==4 && i_rant_le_r32(rep.data.data)==42,
                 "loud: blocking call answers under a service thread (rc=%d st=%d)", rc, (int)rep.status);
        i_rant_le_w32(b, 5);
        late = rant_node_create_variable_definition(A, "loud/late", NULL,
                   &(RantVariableOpts){ .initial = rant_bytes(b,4) });
        rc = (late && lacc) ? rant_variable_wait(lacc, 5000) : -99;
        ST_CHECK(rc==1, "loud: variable wait returns under a service thread (%d)", rc);
        rant_node_stop(A); rant_node_stop(B);
    }
#endif
    if (pose) rant_schema_free(pose, rant_heap_realloc, NULL);
    rant_node_close(A,0); rant_node_close(B,0);
    rant_allocator_reset(&aa); rant_allocator_reset(&ba);
}

/* The dropped peer reflection gate (19e4): the entity walk must refuse dropped peers
 * unless include_dropped is set, or observers grow ghost entities after a restart. */
static void reflect_dropped_checks(void){
    RantAllocator aa = rant_allocator_heap(0);
    RantAllocator ba = rant_allocator_heap(0);
    RantNodeOpts ao, bo; RantNode *A, *B; RantDiscoveryAddr seed;
    RantVariable *def; uint32_t pid = 0; int t;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&ao,0,sizeof ao); ao.domain=ST_DOMAIN+23; ao.discovery.max_peers=4;
    ao.net.multicast_interface="127.0.0.1"; ao.net.seed_peers=&seed; ao.net.n_seed_peers=1;
    ao.discovery.announce_interval_us = 100000;   /* fast cadence so the drop is quick */
    ao.discovery.peer_timeout_us      = 500000;
    bo=ao;
    bo.fetch_details = 1;   /* B is the observer: names + schemas in the detail cache */
    A = rant_node_open(&aa, "ghost-host", NULL, NULL, &ao);
    B = rant_node_open(&ba, "observer",   NULL, NULL, &bo);
    ST_CHECK(A && B, "ghost: nodes open");
    if (!(A && B)){ if(A)rant_node_close(A,0); if(B)rant_node_close(B,0); return; }

    def = rant_node_create_variable_definition(A, "gdial", NULL, NULL);
    ST_CHECK(def != NULL, "ghost: variable definition created");
    { int ents = 0;
      for (t=0;t<2000 && !ents;t++){
          RantIter eit; RantEntityInfo ei; const RantDiscoveryPeer *ps; uint16_t pc;
          pf_pump(A,B,2);
          ps = st_peers(B, &pc);
          if (!(ps && pc)) continue;
          pid = ps[0].id;
          memset(&eit,0,sizeof eit);
          while (rant_node_entities_next(B, pid, &eit, &ei)) ents++;
      }
      ST_CHECK(ents == 1, "ghost: live peer enumerates its entity (%d)", ents); }

    rant_node_close(A, 0);     /* silent death, no BYE: B must DROP (not forget) the peer */
    { uint64_t end = i_rant_plat_now_us() + 1200000u;     /* > peer_timeout */
      while (i_rant_plat_now_us() < end) rant_node_poll(B, 5); }
    { const RantDiscoveryPeer *ps; uint16_t pc, i; int dropped = 0;
      ps = st_peers(B, &pc);
      for (i=0; ps && i<pc; i++)
          if (ps[i].id == pid && ps[i].liveness == RANT_PEER_DROPPED) dropped = 1;
      ST_CHECK(dropped, "ghost: peer is DROPPED yet still listed (by design)"); }
    { RantIter eit; RantEntityInfo ei; int ents = 0, mesh = 0;
      memset(&eit,0,sizeof eit);
      while (rant_node_entities_next(B, pid, &eit, &ei)) ents++;
      ST_CHECK(ents == 1, "ghost: the per-node walk serves the dropped peer's last view (%d)", ents);
      memset(&eit,0,sizeof eit);
      while (rant_node_mesh_next(B, &eit, &ei))
          if (ei.name.len==5 && !memcmp(ei.name.data,"gdial",5)) mesh++;
      ST_CHECK(mesh == 0, "ghost: the mesh walk never counts a dropped peer (%d)", mesh); }

    rant_node_close(B,0);
    rant_allocator_reset(&aa); rant_allocator_reset(&ba);
}

/* The variable set match wait (19e5): a fresh accessor's first write rides the send path's
 * match wait, so NO_TOPIC means the owner is genuinely absent. */
static void varwait_checks(void){
    RantAllocator aa = rant_allocator_heap(0);
    RantAllocator ba = rant_allocator_heap(0);
    RantNodeOpts ao, bo; RantNode *P, *C; RantDiscoveryAddr seed;
    RantVariable *vd; int t;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&ao,0,sizeof ao); ao.domain=ST_DOMAIN+31; ao.discovery.max_peers=4;
    ao.net.multicast_interface="127.0.0.1"; ao.net.seed_peers=&seed; ao.net.n_seed_peers=1;
    bo=ao;
    P = rant_node_open(&aa, "vw-owner",    NULL, NULL, &ao);
    C = rant_node_open(&ba, "vw-accessor", NULL, NULL, &bo);
    ST_CHECK(P && C, "varwait: nodes open");
    if (!(P && C)){ if(P)rant_node_close(P,0); if(C)rant_node_close(C,0); return; }

    { uint8_t b[4]; i_rant_le_w32(b, 20);
      vd = rant_node_create_variable_definition(P, "vw", NULL,
               &(RantVariableOpts){ .initial = rant_bytes(b,4) });
      ST_CHECK(vd != NULL, "varwait: definition created"); }

    /* C must hold P's announce nominating vw before the accessor exists, since candidates
       come from the cached blob. Verdicts are not yet fetched. */
    { uint32_t want = (uint32_t)rant_topic_id("vw"); int seen = 0;
      for (t=0;t<2000 && !seen;t++){
          const RantDiscoveryPeer *ps; uint16_t pc;
          pf_pump(P,C,2);
          ps = st_peers(C,&pc);
          if (ps && pc){ RantInterestIter it; RantTopicEntry e; memset(&it,0,sizeof it);
              while (i_rant_node_peer_interest_next(&ps[0], &it, &e))
                  if (e.hash==want){ seen=1; break; } }
      }
      ST_CHECK(seen, "varwait: C holds P's interest"); }

    /* From here P answers only from its service thread and C is driven solely by the wait
       inside the set call. Create the accessor and write immediately. */
    rant_node_start(P);
    { RantVariable *va = rant_node_create_remote_variable(C, "vw", NULL, NULL);
      uint8_t b[4]; int sr; RantBytes gv;
      ST_CHECK(va != NULL, "varwait: accessor created");
      i_rant_le_w32(b, 5);
      sr = rant_variable_set(va, rant_bytes(b,4));
      ST_CHECK(sr==RANT_OK, "varwait: first write waits out the forming match (%d)", sr);
      rant_node_stop(P);
      for (t=0;t<2000;t++){ pf_pump(P,C,2);
          if (rant_variable_get(vd,&gv)&&gv.len==4&&i_rant_le_r32(gv.data)==5) break; }
      ST_CHECK(rant_variable_get(vd,&gv)&&gv.len==4&&i_rant_le_r32(gv.data)==5,
               "varwait: the write reached the owner (5)"); }

    /* converged verdicts stay instant: an owner nobody offers fails NO_TOPIC without
       consuming the wait bound. Let the post open gather latch first, about 400 ms. */
    pf_pump(P,C,400);
    { RantVariable *orphan = rant_node_create_remote_variable(C, "vw-nobody", NULL, NULL);
      uint8_t b[4]; int sr; uint64_t t0, dt_ms;
      ST_CHECK(orphan != NULL, "varwait: orphan accessor created");
      i_rant_le_w32(b, 1);
      t0 = i_rant_plat_now_us();
      sr = orphan ? rant_variable_set(orphan, rant_bytes(b,4)) : 0;
      dt_ms = (i_rant_plat_now_us() - t0) / 1000u;
      ST_CHECK(sr==RANT_ERR_NO_TOPIC, "varwait: absent owner still NO_TOPIC (%d)", sr);
      ST_CHECK(dt_ms < 250, "varwait: converged verdict is instant (%u ms)", (unsigned)dt_ms); }

    rant_node_close(P,0); rant_node_close(C,0);
    rant_allocator_reset(&aa); rant_allocator_reset(&ba);
}

/* The retire and reuse churn soak (19e6): retire and create cycles reuse slots, so
 * nothing grows over sustained churn (spec/testing.md lists the case matrix). */
static volatile unsigned long ch_c_recv, ch_u_recv, ch_t_recv, ch_c_mismatch;
static volatile uint32_t ch_c_last;
static uint32_t ch_seq;
static void ch_on_message(const RantMsg *m){
    const char *who = (const char *)m->user;
    if (!who || m->data.len < 4) return;
    if      (who[0]=='C'){ ch_c_recv++; ch_c_last = i_rant_le_r32(m->data.data); }
    else if (who[0]=='U') ch_u_recv++;
    else if (who[0]=='T') ch_t_recv++;
}
static void ch_on_event(const RantEvent *ev){
    if (ev->kind == RANT_ERROR && ev->error == RANT_E_SCHEMA_MISMATCH
        && ev->user && ((const char*)ev->user)[0]=='C') ch_c_mismatch++;
}
static void ch_pump(RantNode **ns, int n, int ms){
    uint64_t end = i_rant_plat_now_us() + (uint64_t)ms*1000u;
    while (i_rant_plat_now_us() < end){
        int i;
        for (i=0;i<n;i++) if (ns[i]) rant_node_poll(ns[i], 1);
    }
}
static void ch_send(RantTopic *beat, int nbytes){
    uint8_t pb[8];
    memset(pb, 0, sizeof pb);
    i_rant_le_w32(pb, ++ch_seq);
    rant_topic_send(beat, rant_bytes(pb, (size_t)nbytes), NULL);
}

static void churn_checks(void){
    RantAllocator ma = rant_allocator_heap(0);
    RantAllocator pa = rant_allocator_heap(0);
    RantAllocator ca = rant_allocator_heap(0);
    RantAllocator ua = rant_allocator_heap(0);
    RantNodeOpts po, co2, uo; RantNode *P, *C, *U; RantDiscoveryAddr seed;
    RantSchema *G1, *G2, *G3;
    RantTopic *beat, *csub; RantTopicOpts topts;
    RantNode *nodes[4];
    uint16_t base_idx, c_idx, p_hi0=0, c_hi0=0;
    uint32_t p_int0=0, c_int0=0;
    size_t p_mem0=0, c_mem0=0, u_mem0=0, mem=0, peak=0; uint64_t calls=0;
    int t, cyc, idx_ok=1;

    ch_c_recv = ch_u_recv = ch_t_recv = ch_c_mismatch = 0;
    ch_c_last = 0; ch_seq = 100;
    G1 = rant_schema_compile(rant_allocator_alloc, &ma, "Beat { v: u32 }", NULL);
    G2 = rant_schema_compile(rant_allocator_alloc, &ma, "Beat { w: u32 }", NULL);
    G3 = rant_schema_compile(rant_allocator_alloc, &ma, "Beat { v: u32, extra: u32 }", NULL);
    ST_CHECK(G1 && G2 && G3, "churn: schemas compile");
    if (!(G1 && G2 && G3)){ rant_allocator_reset(&ma); return; }

    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=ST_DOMAIN+33; po.discovery.max_peers=6;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    po.discovery.announce_interval_us = 100000;   /* fast cadence: rebinds re-verify quickly */
    co2=po; uo=po;
    po.user_data=(void*)"P"; co2.user_data=(void*)"C"; uo.user_data=(void*)"U";
    P = rant_node_open(&pa, "churn-pub",     NULL,          ch_on_event, &po);
    C = rant_node_open(&ca, "churn-typed",   ch_on_message, ch_on_event, &co2);
    U = rant_node_open(&ua, "churn-untyped", ch_on_message, ch_on_event, &uo);
    ST_CHECK(P && C && U, "churn: nodes open");
    if (!(P && C && U)){
        if(P)rant_node_close(P,0); if(C)rant_node_close(C,0); if(U)rant_node_close(U,0);
        rant_allocator_reset(&pa); rant_allocator_reset(&ca); rant_allocator_reset(&ua);
        rant_allocator_reset(&ma); return;
    }
    nodes[0]=P; nodes[1]=C; nodes[2]=U; nodes[3]=NULL;

    memset(&topts,0,sizeof topts);
    topts.qos.reliability = RANT_RELIABLE; topts.qos.catch_up = 1;
    beat = rant_node_create_topic(P, "beat", RANT_PUB_ONLY, G1, &topts);
    csub = rant_node_create_topic(C, "beat", RANT_SUB_ONLY, G1, &topts);
    (void)rant_node_create_topic(U, "beat", RANT_SUB_ONLY, NULL, &topts);
    ST_CHECK(beat && csub, "churn: survivor topics created");
    base_idx = rant_topic_index(beat); c_idx = rant_topic_index(csub);

    /* warmup: first delivery, then one identical retire/create cycle so every lazy
       allocation (lane pool, detail buffers, peer maps) is in the baseline */
    ch_send(beat, 4);
    for (t=0;t<4000 && (ch_c_last != ch_seq || ch_u_recv == 0);t++) ch_pump(nodes,3,2);
    ST_CHECK(ch_c_last == ch_seq && ch_u_recv > 0, "churn: warmup delivery (typed + untyped)");
    ST_CHECK(rant_topic_retire(beat) == RANT_OK, "churn: warmup retire");
    beat = rant_node_create_topic(P, "beat", RANT_PUB_ONLY, G1, &topts);
    ST_CHECK(beat && rant_topic_index(beat) == base_idx,
             "churn: identical re-create REUSES the slot (idx %u)", base_idx);
    ch_send(beat, 4);
    for (t=0;t<4000 && ch_c_last != ch_seq;t++) ch_pump(nodes,3,2);
    ST_CHECK(ch_c_last == ch_seq, "churn: delivery across the warmup cycle");
    {   /* saturate the @rant/log/error keep_last ring, which mirrors every retype refusal: its
           16 slots fill once and plateau, so the baselines must include them at full size */
        char big[200]; int k;
        memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0;
        for (k=0;k<20;k++){
            rant_node_log(P, RANT_LOG_ERROR, "%s", big);
            rant_node_log(C, RANT_LOG_ERROR, "%s", big);
            rant_node_log(U, RANT_LOG_ERROR, "%s", big);
        }
    }
    rant_node_mem_stats(P, &p_mem0, &peak, &calls);
    rant_node_mem_stats(C, &c_mem0, &peak, &calls);
    rant_node_mem_stats(U, &u_mem0, &peak, &calls);
    p_hi0 = i_rant_node_topic_count(P); c_hi0 = i_rant_node_topic_count(C);
    p_int0 = rant_transport_interest_size(P->transport);
    c_int0 = rant_transport_interest_size(C->transport);

    /* A: identical binding churn. Every cycle must reuse the slot, relink with no round
       trip, and deliver. Nothing may grow. */
    { int deliver_ok = 1;
      for (cyc=0;cyc<25;cyc++){
          unsigned long u0 = ch_u_recv;
          if (rant_topic_retire(beat) != RANT_OK){ idx_ok = 0; break; }
          beat = rant_node_create_topic(P, "beat", RANT_PUB_ONLY, G1, &topts);
          if (!beat || rant_topic_index(beat) != base_idx){ idx_ok = 0; break; }
          ch_send(beat, 4);
          for (t=0;t<4000 && (ch_c_last != ch_seq || ch_u_recv == u0);t++) ch_pump(nodes,3,2);
          if (ch_c_last != ch_seq || ch_u_recv == u0){ deliver_ok = 0; break; }
      }
      ST_CHECK(idx_ok, "churn-A: 25 identical cycles reuse the slot");
      ST_CHECK(deliver_ok, "churn-A: every cycle delivered (typed + untyped)");
      rant_node_mem_stats(P, &mem, &peak, &calls);
      ST_CHECK(mem <= p_mem0 + 1024, "churn-A: publisher memory plateaus (%u -> %u)",
               (unsigned)p_mem0, (unsigned)mem);
      rant_node_mem_stats(C, &mem, &peak, &calls);
      ST_CHECK(mem <= c_mem0 + 1024, "churn-A: subscriber memory plateaus (%u -> %u)",
               (unsigned)c_mem0, (unsigned)mem);
      ST_CHECK(i_rant_node_topic_count(P) == p_hi0, "churn-A: topic table did not grow");
      ST_CHECK(rant_transport_interest_size(P->transport) == p_int0,
               "churn-A: announce size unchanged (%u)", p_int0); }

    /* B: retype churn. The publisher rebinds the slot to an incompatible schema: the typed
       subscriber refuses loudly, the untyped one follows, then the subscriber adopts. */
    { int deliver_ok = 1, u_ok = 1, mm_ok = 1, iso_ok = 1;
      uint32_t p_int1 = 0, c_int1 = 0;
      size_t p_memb = 0, c_memb = 0;   /* re-baselined once the retype caches warm */
      for (cyc=0;cyc<10;cyc++){
          RantSchema *ns = (cyc & 1) ? G1 : G2;
          unsigned long mm0 = ch_c_mismatch, u0 = ch_u_recv, c0 = ch_c_recv;
          if (rant_topic_retire(beat) != RANT_OK){ idx_ok = 0; break; }
          beat = rant_node_create_topic(P, "beat", RANT_PUB_ONLY, ns, &topts);
          if (!beat || rant_topic_index(beat) != base_idx){ idx_ok = 0; break; }
          ch_send(beat, 4);
          for (t=0;t<4000 && (ch_u_recv == u0 || ch_c_mismatch == mm0);t++) ch_pump(nodes,3,2);
          if (ch_u_recv == u0) u_ok = 0;
          if (ch_c_mismatch == mm0) mm_ok = 0;
          if (ch_c_recv != c0) iso_ok = 0;   /* never a cross-schema delivery */
          if (rant_topic_retire(csub) != RANT_OK){ idx_ok = 0; break; }
          csub = rant_node_create_topic(C, "beat", RANT_SUB_ONLY, ns, &topts);
          if (!csub || rant_topic_index(csub) != c_idx){ idx_ok = 0; break; }
          for (t=0;t<4000 && ch_c_last != ch_seq;t++) ch_pump(nodes,3,2);
          if (ch_c_last != ch_seq){ deliver_ok = 0; break; }
          if (cyc == 0){ p_int1 = rant_transport_interest_size(P->transport);
                         c_int1 = rant_transport_interest_size(C->transport); }
          if (cyc == 1){ rant_node_mem_stats(P, &p_memb, &peak, &calls);
                         rant_node_mem_stats(C, &c_memb, &peak, &calls); }
      }
      ST_CHECK(idx_ok, "churn-B: retype cycles keep reusing both slots");
      ST_CHECK(mm_ok, "churn-B: typed subscriber refuses each retype loudly");
      ST_CHECK(iso_ok, "churn-B: no cross-schema delivery to the typed subscriber");
      ST_CHECK(u_ok, "churn-B: untyped subscriber follows every shape");
      ST_CHECK(deliver_ok, "churn-B: adopted re-create receives the current value (catch-up)");
      ST_CHECK(p_int1 == p_int0 + 3 && rant_transport_interest_size(P->transport) == p_int1,
               "churn-B: announce grows once by the 3 B gen entry, then holds (%u -> %u)",
               p_int0, p_int1);
      ST_CHECK(c_int1 == c_int0 + 3 && rant_transport_interest_size(C->transport) == c_int1,
               "churn-B: subscriber announce likewise (%u -> %u)", c_int0, c_int1);
      /* the plateau is judged against the warmed baseline two full retype round trips in,
         after the one time allocations, and the 8 cycles after it must add nothing */
      rant_node_mem_stats(P, &mem, &peak, &calls);
      ST_CHECK(mem <= p_memb + 512, "churn-B: publisher memory plateaus (%u -> %u)",
               (unsigned)p_memb, (unsigned)mem);
      rant_node_mem_stats(C, &mem, &peak, &calls);
      ST_CHECK(mem <= c_memb + 512, "churn-B: subscriber memory plateaus (%u -> %u)",
               (unsigned)c_memb, (unsigned)mem); }

    /* C: compatible retype. The publisher rebinds to a SUPERSET of the subscriber's
       schema: the typed subscriber keeps receiving with no action and no refusal. */
    { unsigned long mm0 = ch_c_mismatch;
      ST_CHECK(rant_topic_retire(beat) == RANT_OK, "churn-C: retire before the superset rebind");
      beat = rant_node_create_topic(P, "beat", RANT_PUB_ONLY, G3, &topts);
      ST_CHECK(beat && rant_topic_index(beat) == base_idx, "churn-C: superset re-create reuses the slot");
      ch_send(beat, 8);
      for (t=0;t<4000 && ch_c_last != ch_seq;t++) ch_pump(nodes,3,2);
      ST_CHECK(ch_c_last == ch_seq, "churn-C: subset-compatible rebind keeps the typed subscriber");
      ST_CHECK(ch_c_mismatch == mm0, "churn-C: no refusal fired for the compatible rebind"); }

    /* D: transient node waves. Odd waves bring a rival same name publisher of another shape,
       even waves a late joining subscriber. Survivor state must plateau across all of it. */
    { int wave_ok = 1, cross_ok = 1, late_ok = 1, live_ok = 1;
      size_t p_mem1 = 0, c_mem1 = 0, u_mem1 = 0;
      for (cyc=0;cyc<6;cyc++){
          RantAllocator ta = rant_allocator_heap(0);
          RantNodeOpts to2 = po; RantNode *T; RantTopic *tt;
          unsigned long u0 = ch_u_recv, t0c = ch_t_recv;
          to2.user_data = (void*)"T";
          T = rant_node_open(&ta, "churn-transient", ch_on_message, ch_on_event, &to2);
          if (!T){ wave_ok = 0; rant_allocator_reset(&ta); break; }
          nodes[3] = T;
          if (cyc & 1){
              tt = rant_node_create_topic(T, "beat", RANT_PUB_ONLY, G2, &topts);
              if (!tt){ wave_ok = 0; }
              else {
                  for (t=0;t<4000 && rant_topic_match_count(tt) < 1;t++) ch_pump(nodes,4,2);
                  {   uint8_t pb[4]; i_rant_le_w32(pb, 0xBADBEEFu);     /* must never reach C */
                      rant_topic_send(tt, rant_bytes(pb, 4), NULL); }
                  for (t=0;t<4000 && ch_u_recv == u0;t++) ch_pump(nodes,4,2);
                  if (ch_u_recv == u0) wave_ok = 0;
                  ch_send(beat, 8);   /* the survivor stream runs beside the rival */
                  for (t=0;t<4000 && ch_c_last != ch_seq;t++) ch_pump(nodes,4,2);
                  if (ch_c_last != ch_seq) cross_ok = 0;
              }
          } else {
              tt = rant_node_create_topic(T, "beat", RANT_SUB_ONLY, NULL, &topts);
              if (!tt) wave_ok = 0;
              else {
                  for (t=0;t<4000 && ch_t_recv == t0c;t++) ch_pump(nodes,4,2);
                  if (ch_t_recv == t0c) late_ok = 0;   /* catch-up hands it the current value */
              }
          }
          rant_node_close(T, 1);
          nodes[3] = NULL;
          rant_allocator_reset(&ta);
          ch_send(beat, 8);   /* survivors deliver between waves */
          for (t=0;t<4000 && ch_c_last != ch_seq;t++) ch_pump(nodes,3,2);
          if (ch_c_last != ch_seq) live_ok = 0;
          if (cyc == 0){
              rant_node_mem_stats(P, &p_mem1, &peak, &calls);
              rant_node_mem_stats(C, &c_mem1, &peak, &calls);
              rant_node_mem_stats(U, &u_mem1, &peak, &calls);
          }
      }
      ST_CHECK(wave_ok, "churn-D: every wave node joined and exchanged data");
      ST_CHECK(cross_ok, "churn-D: rival same-name publisher never cross-wires the typed stream");
      ST_CHECK(late_ok, "churn-D: late joiners catch up to the current value");
      ST_CHECK(live_ok, "churn-D: survivor subscriptions deliver through every wave");
      rant_node_mem_stats(P, &mem, &peak, &calls);
      ST_CHECK(mem <= p_mem1 + 512, "churn-D: publisher memory plateaus over peer churn (%u -> %u)",
               (unsigned)p_mem1, (unsigned)mem);
      rant_node_mem_stats(C, &mem, &peak, &calls);
      ST_CHECK(mem <= c_mem1 + 512, "churn-D: typed subscriber memory plateaus (%u -> %u)",
               (unsigned)c_mem1, (unsigned)mem);
      rant_node_mem_stats(U, &mem, &peak, &calls);
      ST_CHECK(mem <= u_mem1 + 512, "churn-D: untyped subscriber memory plateaus (%u -> %u)",
               (unsigned)u_mem1, (unsigned)mem);
      ST_CHECK(i_rant_node_topic_count(P) == p_hi0 && i_rant_node_topic_count(C) == c_hi0,
               "churn-D: topic tables never grew"); }

    rant_node_close(P,0); rant_node_close(C,0); rant_node_close(U,0);
    rant_allocator_reset(&pa); rant_allocator_reset(&ca); rant_allocator_reset(&ua);
    rant_allocator_reset(&ma);
}

/* The match wait checks (19f): a first send racing the announce and detail cycle, a
 * disabled wait dropping loudly, and writer authoritative delivery (spec/testing.md). */
static volatile unsigned long mw_recv, mw_lost, mw_unmatched;
static char mw_last[64];
static void mw_on_message(const RantMsg *m){
    size_t c = m->data.len < sizeof mw_last - 1 ? m->data.len : sizeof mw_last - 1;
    memcpy(mw_last, m->data.data, c); mw_last[c] = 0;
    mw_recv++;
}
static void mw_on_event(const RantEvent *ev){
    if (ev->kind == RANT_MSG_LOST) mw_lost++;
    if (ev->kind == RANT_ERROR && ev->error == RANT_E_UNMATCHED_SEND) mw_unmatched++;
}

static void matchwait_checks(void){
    RantDiscoveryAddr seed; int t;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;

#ifdef _WIN32
    /* (a) WRITER-AUTHORITATIVE HEAL: B never receives DETAIL_RESPs, so it cannot verify A
       while A matches B. Unblocking lets B verify and repair must deliver the original sample. */
    { /* per-process fixed port, like the domain base: concurrent selftests must not
         collide on the bind (the phase needs it fixed only to intercept by port) */
      const uint16_t MW_PORT = (uint16_t)(40000u + st_domain_base % 20000u);
      RantAllocator aa = rant_allocator_heap(0);
      RantAllocator ba = rant_allocator_heap(0);
      RantNodeOpts ao, bo; RantNode *A, *B; RantTopic *at=NULL, *bt=NULL;
      memset(&bo,0,sizeof bo); bo.domain=ST_DOMAIN+24; bo.disable_shm=1; bo.discovery.max_peers=4;
      bo.discovery.announce_interval_us=200000;
      bo.net.multicast_interface="127.0.0.1"; bo.net.seed_peers=&seed; bo.net.n_seed_peers=1;
      ao=bo; bo.net.data_port=MW_PORT;
      mw_recv=0; mw_lost=0; mw_last[0]=0;
      B = rant_node_open(&ba, "mw-b", mw_on_message, mw_on_event, &bo);
      ST_CHECK(B!=NULL, "matchwait: sub node opens (is port %d free?)", (int)MW_PORT);
      if (B) bt = rant_node_create_topic(B, "mw-auth", RANT_SUB_ONLY, NULL,
                      &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE } });
      g_tx_block_detail_resp_port = MW_PORT;   /* B stays PENDING on every candidate */
      A = rant_node_open(&aa, "mw-a", NULL, NULL, &ao);
      at = A ? rant_node_create_topic(A, "mw-auth", RANT_PUB_ONLY, NULL,
                      &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE, .keep_last=8,
                                               .heartbeat_us=50000 } }) : NULL;
      ST_CHECK(A && at && bt, "matchwait: nodes + topics up");
      if (A && B && at && bt){
          int r;
          for (t=0;t<3000 && rant_topic_match_count(at)==0;t++){ rant_node_poll(A,2); rant_node_poll(B,2); }
          ST_CHECK(rant_topic_match_count(at)==1,
                   "matchwait: writer side matched while B's responses are blocked (%d)",
                   rant_topic_match_count(at));
          ST_CHECK(rant_topic_pending_count(bt) > 0,
                   "matchwait: reader side still PENDING (%d)", rant_topic_pending_count(bt));
          r = rant_topic_send(at, rant_bytes("authoritative", 13), NULL);
          ST_CHECK(r==RANT_OK, "matchwait: matched send commits (%d)", r);
          for (t=0;t<150;t++){ rant_node_poll(A,2); rant_node_poll(B,2); }
          ST_CHECK(mw_recv==0, "matchwait: unverified reader drops the DATA (recv=%lu)", mw_recv);
          g_tx_block_detail_resp_port = 0;      /* B's next re-ask verifies A */
          for (t=0;t<3000 && mw_recv==0;t++){ rant_node_poll(A,2); rant_node_poll(B,2); }
          ST_CHECK(mw_recv==1 && strcmp(mw_last,"authoritative")==0,
                   "matchwait: repair delivers the pre-verify sample (recv=%lu '%s')", mw_recv, mw_last);
          ST_CHECK(mw_lost==0, "matchwait: healed with no MSG_LOST (%lu)", mw_lost);
          ST_CHECK(rant_topic_pending_count(at)==0 && rant_topic_ready(at)==1,
                   "matchwait: probes converge (pending=%d ready=%d)",
                   rant_topic_pending_count(at), rant_topic_ready(at));
      }
      g_tx_block_detail_resp_port = 0;
      if (A) rant_node_close(A,1);
      if (B) rant_node_close(B,1);
      rant_allocator_reset(&aa); rant_allocator_reset(&ba);
    }

    /* (a2) THE WINDOW: A never receives B's DETAIL_RESPs, so B stays a candidate forever. The
       wait bridges A's own coming up only: one window after the create, then no send waits,
       and a subscriber that appears later never blocks a send at all. */
    { const uint16_t MW_PORT2 = (uint16_t)(40000u + (st_domain_base + 1u) % 20000u);
      RantAllocator aa = rant_allocator_heap(0);
      RantAllocator ba = rant_allocator_heap(0);
      RantNodeOpts ao, bo; RantNode *A, *B; RantTopic *poison=NULL, *late=NULL;
      memset(&bo,0,sizeof bo); bo.domain=ST_DOMAIN+13; bo.disable_shm=1; bo.discovery.max_peers=4;
      bo.discovery.announce_interval_us=200000;
      bo.net.multicast_interface="127.0.0.1"; bo.net.seed_peers=&seed; bo.net.n_seed_peers=1;
      ao=bo; ao.net.data_port=MW_PORT2; ao.match_wait_ms=300;
      mw_unmatched=0;
      B = rant_node_open(&ba, "mw-wb", mw_on_message, mw_on_event, &bo);
      if (B) (void)rant_node_create_topic(B, "mw-poison", RANT_SUB_ONLY, NULL, NULL);
      if (B) rant_node_start(B);
      g_tx_block_detail_resp_port = MW_PORT2;
      A = rant_node_open(&aa, "mw-wa", NULL, mw_on_event, &ao);
      ST_CHECK(A && B, "matchwait: window pair up (is port %d free?)", (int)MW_PORT2);
      if (A && B){
          uint64_t t0, first, worst = 0;
          poison = rant_node_create_topic(A, "mw-poison", RANT_PUB_ONLY, NULL, NULL);
          late   = rant_node_create_topic(A, "mw-late", RANT_PUB_ONLY, NULL, NULL);
          t0 = i_rant_plat_now_us();
          (void)rant_topic_send(poison, rant_bytes("x",1), NULL);
          first = i_rant_plat_now_us() - t0;
          ST_CHECK(first >= 100000u && first < 600000u,
                   "matchwait: the first send waits out the window once (%.0f ms)", first/1000.0);
          for (t=0;t<5;t++){
              uint64_t s0 = i_rant_plat_now_us(), dt;
              (void)rant_topic_send(poison, rant_bytes("x",1), NULL);
              dt = i_rant_plat_now_us() - s0;
              if (dt > worst) worst = dt;
              rant_node_poll(A, 2);
          }
          ST_CHECK(worst < 50000u, "matchwait: no later send waits on the unresolved peer (worst %.1f ms)",
                   worst/1000.0);
          ST_CHECK(rant_topic_match_count(poison)==0, "matchwait: the peer never resolved (%d)",
                   rant_topic_match_count(poison));
          /* a subscriber to mw-late appears long after A created it */
          (void)rant_node_create_topic(B, "mw-late", RANT_SUB_ONLY, NULL, NULL);
          { uint64_t end = i_rant_plat_now_us() + 500000u;
            while (i_rant_plat_now_us() < end) rant_node_poll(A, 2); }    /* A hears the candidate */
          t0 = i_rant_plat_now_us();
          (void)rant_topic_send(late, rant_bytes("x",1), NULL);
          first = i_rant_plat_now_us() - t0;
          ST_CHECK(first < 50000u, "matchwait: a late subscriber never blocks a send (%.1f ms)",
                   first/1000.0);
      }
      g_tx_block_detail_resp_port = 0;
      if (B) rant_node_stop(B);
      if (A) rant_node_close(A,1);
      if (B) rant_node_close(B,1);
      rant_allocator_reset(&aa); rant_allocator_reset(&ba);
    }
#endif

#ifdef RANT_THREADS
    /* (b) FIRST SEND against the forming match: a catch_up 0 publish fired right after
       create_topic must wait for the present subscriber's match, commit and deliver. */
    { RantAllocator aa = rant_allocator_heap(0);
      RantAllocator ba = rant_allocator_heap(0);
      RantNodeOpts o; RantNode *A, *B; RantTopic *at=NULL, *bt=NULL;
      memset(&o,0,sizeof o); o.domain=ST_DOMAIN+25; o.disable_shm=1; o.discovery.max_peers=4;
      o.net.multicast_interface="127.0.0.1"; o.net.seed_peers=&seed; o.net.n_seed_peers=1;
      mw_recv=0; mw_lost=0; mw_unmatched=0; mw_last[0]=0;
      B = rant_node_open(&ba, "mw-sub", mw_on_message, mw_on_event, &o);
      bt = B ? rant_node_create_topic(B, "mw-first", RANT_SUB_ONLY, NULL,
                   &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE } }) : NULL;
      ST_CHECK(B && bt, "matchwait: first-send sub up");
      if (B) rant_node_start(B);
      A = rant_node_open(&aa, "mw-pub", NULL, mw_on_event, &o);
      at = A ? rant_node_create_topic(A, "mw-first", RANT_PUB_ONLY, NULL,
                   &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE } }) : NULL;
      ST_CHECK(A && at, "matchwait: first-send pub up");
      if (A && B && at && bt){
          int r = rant_topic_send(at, rant_bytes("first", 5), NULL);   /* immediately: the race */
          ST_CHECK(r==RANT_OK, "matchwait: racing first send commits (%d)", r);
          ST_CHECK(rant_topic_match_count(at) > 0,
                   "matchwait: ...after waiting out the match (%d)", rant_topic_match_count(at));
          for (t=0;t<1500 && mw_recv==0;t++) rant_node_poll(A,2);
          ST_CHECK(mw_recv==1 && strcmp(mw_last,"first")==0,
                   "matchwait: subscriber got the racing first send (recv=%lu '%s')", mw_recv, mw_last);
          ST_CHECK(mw_unmatched==0, "matchwait: no timeout diagnostic (%lu)", mw_unmatched);
          /* a topic NOBODY consumes must not stall once the gather has settled: the
             converged state is memoized, so this send early-outs as it always did */
          for (t=0;t<300;t++) rant_node_poll(A,2);     /* let the post-open gather settle */
          { RantTopic *v = rant_node_create_topic(A, "mw-void", RANT_PUB_ONLY, NULL, NULL);
            uint64_t t0, el;
            ST_CHECK(v!=NULL, "matchwait: void topic up");
            t0 = i_rant_plat_now_us();
            r = v ? rant_topic_send(v, rant_bytes("x",1), NULL) : -1;
            el = i_rant_plat_now_us() - t0;
            ST_CHECK(r==RANT_OK && el < 250000u,
                     "matchwait: no-consumer send returns fast (%d, %luus)", r, (unsigned long)el); }
      }
      if (B) rant_node_stop(B);
      if (A) rant_node_close(A,1);
      if (B) rant_node_close(B,1);
      rant_allocator_reset(&aa); rant_allocator_reset(&ba);
    }
#endif

    /* (c) CONTROL, the wait disabled: the same racing send commits at once to zero
       subscribers and is gone, but RANT_E_UNMATCHED_SEND fires on the way out. */
    { RantAllocator aa = rant_allocator_heap(0);
      RantAllocator ba = rant_allocator_heap(0);
      RantNodeOpts o, ao; RantNode *A, *B; RantTopic *at=NULL, *bt=NULL;
      memset(&o,0,sizeof o); o.domain=ST_DOMAIN+26; o.disable_shm=1; o.discovery.max_peers=4;
      o.net.multicast_interface="127.0.0.1"; o.net.seed_peers=&seed; o.net.n_seed_peers=1;
      ao=o; ao.match_wait_ms=-1;
      mw_recv=0; mw_unmatched=0;
      B = rant_node_open(&ba, "mw-sub2", mw_on_message, NULL, &o);
      bt = B ? rant_node_create_topic(B, "mw-off", RANT_SUB_ONLY, NULL,
                   &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE } }) : NULL;
      A = rant_node_open(&aa, "mw-off-pub", NULL, mw_on_event, &ao);
      at = A ? rant_node_create_topic(A, "mw-off", RANT_PUB_ONLY, NULL,
                   &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE } }) : NULL;
      ST_CHECK(A && B && at && bt, "matchwait: knob-off pair up");
      if (A && B && at && bt){
          int r = rant_topic_send(at, rant_bytes("gone", 4), NULL);   /* no wait: commits to zero */
          ST_CHECK(r==RANT_OK && mw_unmatched==1,
                   "matchwait: disabled wait drops LOUDLY (r=%d events=%lu)", r, mw_unmatched);
          for (t=0;t<2000 && rant_topic_match_count(at)==0;t++){ rant_node_poll(A,2); rant_node_poll(B,2); }
          ST_CHECK(rant_topic_match_count(at)==1, "matchwait: the match still forms after (%d)",
                   rant_topic_match_count(at));
          for (t=0;t<150;t++){ rant_node_poll(A,2); rant_node_poll(B,2); }
          ST_CHECK(mw_recv==0, "matchwait: the dropped send never resurrects (recv=%lu)", mw_recv);
      }
      if (A) rant_node_close(A,1);
      if (B) rant_node_close(B,1);
      rant_allocator_reset(&aa); rant_allocator_reset(&ba);
    }
}

/* The relay phase: discovery for a node that cannot multicast. U opens unicast_only with
 * one seed, R's data port, and every U to B fact arrives through R's proxied announces. */
static unsigned long rly_recv = 0;
static char rly_last[64];
static void rly_on_message(const RantMsg *msg){
    size_t k = msg->data.len < sizeof rly_last - 1 ? msg->data.len : sizeof rly_last - 1;
    memcpy(rly_last, msg->data.data, k); rly_last[k] = '\0';
    rly_recv++;
}
/* is `name` an ACTIVE peer of n? (the peer view is discovery's own, valid until the next poll) */
static int rly_sees(RantNode *n, const char *name){
    uint16_t c = 0, i; size_t nl = strlen(name);
    const RantDiscoveryPeer *p = st_peers(n, &c);
    for (i=0;i<c;i++)
        if (p[i].liveness == RANT_PEER_ACTIVE && p[i].name.len == nl &&
            memcmp(p[i].name.data, name, nl) == 0) return 1;
    return 0;
}
static void rly_pump(RantNode *a, RantNode *b, RantNode *c, int iters){
    int t;
    for (t=0;t<iters;t++){
        if (a) rant_node_poll(a, 2);
        if (b) rant_node_poll(b, 2);
        if (c) rant_node_poll(c, 2);
    }
}
static void relay_checks(void){
    /* R's data port is fixed so U can seed exactly one address (per-process, like the
       matchwait port: concurrent selftests must not collide on the bind) */
    const uint16_t R_PORT = (uint16_t)(20000u + st_domain_base % 15000u);
    RantDiscoveryAddr seed; int t;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4; seed.port=R_PORT;

    /* (a) INTRODUCTION + DATA, then the relay dies */
    { RantAllocator ua = rant_allocator_heap(0);
      RantAllocator ra = rant_allocator_heap(0);
      RantAllocator ba = rant_allocator_heap(0);
      RantNodeOpts uo, ro, bo; RantNode *U=NULL, *R=NULL, *B=NULL;
      RantTopic *ut=NULL, *bt=NULL;
      memset(&ro,0,sizeof ro); ro.domain=ST_DOMAIN+27; ro.disable_shm=1; ro.discovery.max_peers=4;
      ro.discovery.announce_interval_us=200000;
      ro.net.multicast_interface="127.0.0.1";
      ro.net.data_port=R_PORT;                 /* the one address anybody ever states */
      bo=ro; bo.net.data_port=0;
      uo=ro; uo.net.data_port=0;
      uo.net.unicast_only=1;                    /* no group join, no group sends, RELAY_ME */
      uo.net.seed_peers=&seed; uo.net.n_seed_peers=1;
      rly_recv=0; rly_last[0]='\0';

      R = rant_node_open(&ra, "rly-relay", NULL, NULL, &ro);
      ST_CHECK(R!=NULL, "relay: relay node opens (is port %d free?)", (int)R_PORT);
      B = rant_node_open(&ba, "rly-far", rly_on_message, NULL, &bo);
      U = rant_node_open(&ua, "rly-uni", NULL, NULL, &uo);
      ST_CHECK(U!=NULL, "relay: unicast-only node opens with no multicast membership");
      ST_CHECK(B!=NULL, "relay: far node opens");
      if (U && R && B){
          bt = rant_node_create_topic(B, "rly/t", RANT_SUB_ONLY, NULL,
                   &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE } });
          ut = rant_node_create_topic(U, "rly/t", RANT_PUB_ONLY, NULL,
                   &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE, .keep_last=8 } });
          ST_CHECK(ut && bt, "relay: topics up");

          for (t=0;t<3000 && !(rly_sees(B,"rly-uni") && rly_sees(U,"rly-far"));t++)
              rly_pump(U,R,B,1);
          ST_CHECK(rly_sees(B,"rly-uni"),
                   "relay: far node discovers the unicast-only node it cannot hear");
          ST_CHECK(rly_sees(U,"rly-far"),
                   "relay: and the unicast-only node discovers it back");

          if (ut && bt){
              int r;
              for (t=0;t<2000 && rant_topic_match_count(ut)==0;t++) rly_pump(U,R,B,1);
              ST_CHECK(rant_topic_match_count(ut)==1, "relay: topic matches across the introduction (%d)",
                       rant_topic_match_count(ut));
              r = rant_topic_send(ut, rant_bytes("over-unicast", 12), NULL);
              for (t=0;t<2000 && rly_recv==0;t++) rly_pump(U,R,B,1);
              ST_CHECK(r==RANT_OK && rly_recv==1 && strcmp(rly_last,"over-unicast")==0,
                       "relay: data flows unicast end to end (r=%d recv=%lu '%s')", r, rly_recv, rly_last);
          }

          /* the relay only INTRODUCED: it is not in the data path and not needed to keep
             the pair alive, since each now unicasts its announces to the other directly */
          rant_node_close(R,1); R=NULL;
          rly_pump(U,NULL,B,400);                       /* ~0.8s: several announce intervals */
          ST_CHECK(rly_sees(B,"rly-uni") && rly_sees(U,"rly-far"),
                   "relay: the pair outlives the relay (B sees U=%d, U sees B=%d)",
                   rly_sees(B,"rly-uni"), rly_sees(U,"rly-far"));
          ST_CHECK(!rly_sees(B,"rly-relay") && !rly_sees(U,"rly-relay"),
                   "relay: ...and the departed relay itself is gone from both");
          if (ut && bt){
              int r = rant_topic_send(ut, rant_bytes("after-relay-died", 16), NULL);
              for (t=0;t<2000 && rly_recv<2;t++) rly_pump(U,NULL,B,1);
              ST_CHECK(r==RANT_OK && rly_recv==2 && strcmp(rly_last,"after-relay-died")==0,
                       "relay: data still flows with the relay dead (r=%d recv=%lu '%s')",
                       r, rly_recv, rly_last);
          }
      }
      if (R) rant_node_close(R,1);
      if (U) rant_node_close(U,1);
      if (B) rant_node_close(B,1);
      rant_allocator_reset(&ua); rant_allocator_reset(&ra); rant_allocator_reset(&ba);
    }

    /* (b) CONTROL: the same unicast only node with nobody to relay it is unreachable in
       both directions, so (a) measured the relay, not the loopback */
    { RantAllocator ua = rant_allocator_heap(0);
      RantAllocator ba = rant_allocator_heap(0);
      RantNodeOpts uo, bo; RantNode *U=NULL, *B=NULL;
      memset(&bo,0,sizeof bo); bo.domain=ST_DOMAIN+28; bo.disable_shm=1; bo.discovery.max_peers=4;
      bo.discovery.announce_interval_us=200000;
      bo.net.multicast_interface="127.0.0.1";
      uo=bo; uo.net.unicast_only=1;              /* no seeds: it can announce to nobody at all */
      U = rant_node_open(&ua, "rly-alone", NULL, NULL, &uo);
      B = rant_node_open(&ba, "rly-far2", NULL, NULL, &bo);
      ST_CHECK(U && B, "relay: control pair up");
      if (U && B){
          rly_pump(U,B,NULL,400);                 /* ~0.8s: several announce intervals each way */
          ST_CHECK(!rly_sees(B,"rly-alone") && !rly_sees(U,"rly-far2"),
                   "relay: unrelayed unicast-only node stays invisible (B sees U=%d, U sees B=%d)",
                   rly_sees(B,"rly-alone"), rly_sees(U,"rly-far2"));
      }
      if (U) rant_node_close(U,1);
      if (B) rant_node_close(B,1);
      rant_allocator_reset(&ua); rant_allocator_reset(&ba);
    }
}

/* The NAT phase: a unicast only node behind an outbound only NAT. U advertises a black
 * holed port, so only the observed sources can carry the mesh (spec/testing.md). */
static void nat_checks(void){
    const uint16_t R2_PORT = (uint16_t)(21000u + st_domain_base % 15000u);
    uint16_t DEAD_PORT = 0;
    RantDiscoveryAddr seed; int t;
    i_RantSock bh;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4; seed.port=R2_PORT;

    /* the black hole owns U's advertised port and never reads it, so every datagram sent to
       U's locator disappears like a NAT drop. OS assigned, so it never collides. */
    i_rant_plat_startup();            /* raw socket outside any node: needs the net stack up */
    bh = i_rant_plat_udp_open();
    if (bh != RANT_SOCK_BAD){
        if (i_rant_plat_bind(bh, 0, 0, 0)) DEAD_PORT = i_rant_plat_local_port(bh);
        if (!DEAD_PORT){ i_rant_plat_close(bh); bh = RANT_SOCK_BAD; }
    }
    ST_CHECK(DEAD_PORT != 0, "nat: black-hole port binds");

    { RantAllocator ua = rant_allocator_heap(0);
      RantAllocator ra = rant_allocator_heap(0);
      RantAllocator ba = rant_allocator_heap(0);
      RantNodeOpts uo, ro, bo; RantNode *U=NULL, *R=NULL, *B=NULL;
      RantTopic *ut=NULL, *bt=NULL;
      memset(&ro,0,sizeof ro); ro.domain=ST_DOMAIN+29; ro.disable_shm=1; ro.discovery.max_peers=4;
      ro.discovery.announce_interval_us=200000;
      ro.net.multicast_interface="127.0.0.1";
      ro.net.data_port=R2_PORT;                /* the seeded address */
      bo=ro; bo.net.data_port=0;
      uo=ro; uo.net.data_port=0;
      uo.net.unicast_only=1;
      uo.net.advertise_port=DEAD_PORT;         /* the locator lie: what a NAT'd node's blob
                                                  port amounts to on the outside */
      uo.net.seed_peers=&seed; uo.net.n_seed_peers=1;
      rly_recv=0; rly_last[0]='\0';

      R = rant_node_open(&ra, "nat-relay", NULL, NULL, &ro);
      B = rant_node_open(&ba, "nat-far", rly_on_message, NULL, &bo);
      U = rant_node_open(&ua, "nat-uni", NULL, NULL, &uo);
      ST_CHECK(R!=NULL, "nat: relay node opens (is port %d free?)", (int)R2_PORT);
      ST_CHECK(U!=NULL && B!=NULL, "nat: NAT'd + far nodes open");
      if (U && R && B){
          bt = rant_node_create_topic(B, "nat/t", RANT_SUB_ONLY, NULL,
                   &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE } });
          ut = rant_node_create_topic(U, "nat/t", RANT_PUB_ONLY, NULL,
                   &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE, .keep_last=8 } });
          ST_CHECK(ut && bt, "nat: topics up");

          /* B cannot dial U (locator black-holed) and U cannot hear multicast: the pair
             can only form through R's introduction followed by U speaking first */
          for (t=0;t<3000 && !(rly_sees(B,"nat-uni") && rly_sees(U,"nat-far"));t++)
              rly_pump(U,R,B,1);
          ST_CHECK(rly_sees(U,"nat-far"),
                   "nat: the NAT'd node is INTRODUCED to a peer it cannot hear");
          ST_CHECK(rly_sees(B,"nat-uni"),
                   "nat: and reaches it first, so the far node binds its observed source");

          if (ut && bt){
              int r;
              for (t=0;t<2000 && rant_topic_match_count(ut)==0;t++) rly_pump(U,R,B,1);
              ST_CHECK(rant_topic_match_count(ut)==1, "nat: topic matches through the mappings (%d)",
                       rant_topic_match_count(ut));
              r = rant_topic_send(ut, rant_bytes("through-the-nat", 15), NULL);
              for (t=0;t<2000 && rly_recv==0;t++) rly_pump(U,R,B,1);
              ST_CHECK(r==RANT_OK && rly_recv==1 && strcmp(rly_last,"through-the-nat")==0,
                       "nat: reliable data flows despite a dead locator (r=%d recv=%lu '%s')",
                       r, rly_recv, rly_last);
          }

          /* the introducer is bootstrap only: the pair sustains itself point to point */
          rant_node_close(R,1); R=NULL;
          rly_pump(U,NULL,B,400);
          ST_CHECK(rly_sees(B,"nat-uni") && rly_sees(U,"nat-far"),
                   "nat: the pair outlives the introducer (B sees U=%d, U sees B=%d)",
                   rly_sees(B,"nat-uni"), rly_sees(U,"nat-far"));
          if (ut && bt){
              int r = rant_topic_send(ut, rant_bytes("after-introducer-died", 21), NULL);
              for (t=0;t<2000 && rly_recv<2;t++) rly_pump(U,NULL,B,1);
              ST_CHECK(r==RANT_OK && rly_recv==2 && strcmp(rly_last,"after-introducer-died")==0,
                       "nat: data still flows with the introducer dead (r=%d recv=%lu '%s')",
                       r, rly_recv, rly_last);
          }
      }
      if (R) rant_node_close(R,1);
      if (U) rant_node_close(U,1);
      if (B) rant_node_close(B,1);
      rant_allocator_reset(&ua); rant_allocator_reset(&ra); rant_allocator_reset(&ba);
    }
    if (bh != RANT_SOCK_BAD) i_rant_plat_close(bh);
    i_rant_plat_cleanup();
}

/* The self ip phase: stating our own locator. On one host the advertised port carries
 * the proof, it is deliberately not the port A bound. */
static int sip_addr_of(RantNode *n, const char *name, RantDiscoveryAddr *out){
    uint16_t c = 0, i; size_t nl = strlen(name);
    const RantDiscoveryPeer *p = st_peers(n, &c);
    for (i=0;i<c;i++)
        if (p[i].liveness == RANT_PEER_ACTIVE && p[i].name.len == nl &&
            memcmp(p[i].name.data, name, nl) == 0){ *out = p[i].addr; return 1; }
    return 0;
}
static void selfip_checks(void){
    const uint16_t ADV_PORT = (uint16_t)(45000u + st_domain_base % 15000u);
    int t;
    /* (a) peers record what we STATE, not where our packets came from */
    { RantAllocator aa = rant_allocator_heap(0);
      RantAllocator ba = rant_allocator_heap(0);
      RantNodeOpts ao, bo; RantNode *A=NULL, *B=NULL; RantDiscoveryAddr got;
      memset(&bo,0,sizeof bo); bo.domain=ST_DOMAIN+29; bo.disable_shm=1; bo.discovery.max_peers=4;
      bo.discovery.announce_interval_us=200000; bo.net.multicast_interface="127.0.0.1";
      ao=bo; ao.net.self_ip="127.0.0.1"; ao.net.advertise_port=ADV_PORT;
      A = rant_node_open(&aa, "sip-a", NULL, NULL, &ao);
      B = rant_node_open(&ba, "sip-b", NULL, NULL, &bo);
      ST_CHECK(A && B, "selfip: pair up");
      if (A && B){
          memset(&got,0,sizeof got);
          for (t=0;t<3000 && !sip_addr_of(B,"sip-a",&got);t++){ rant_node_poll(A,2); rant_node_poll(B,2); }
          ST_CHECK(got.ip_len==4 && got.ip[0]==127 && got.ip[3]==1 && got.port==ADV_PORT,
                   "selfip: peers record the stated locator (%u.%u.%u.%u:%u, want 127.0.0.1:%u)",
                   got.ip[0], got.ip[1], got.ip[2], got.ip[3],
                   (unsigned)got.port, (unsigned)ADV_PORT);
      }
      if (A) rant_node_close(A,1);
      if (B) rant_node_close(B,1);
      rant_allocator_reset(&aa); rant_allocator_reset(&ba);
    }
    /* (b) an unparseable locator is a config error, never a silent fallback to the default:
       a node advertising an unreachable address would look healthy and receive nothing */
    { RantAllocator aa = rant_allocator_heap(0);
      RantNodeOpts o; RantNode *n; RantEvent err; char line[160];
      memset(&o,0,sizeof o); o.domain=ST_DOMAIN+29; o.net.multicast_interface="127.0.0.1";
      o.net.self_ip="not-an-ip";
      n = rant_node_open(&aa, "sip-bad", NULL, NULL, &o);
      ST_CHECK(n==NULL, "selfip: unparseable self ip refuses the open");
      err = rant_last_error(NULL);
      ST_CHECK(err.kind==RANT_ERROR && err.error==RANT_E_BAD_ADDRESS,
               "selfip: ...and says why (%s)", rant_event_str(&err, line, sizeof line));
      if (n) rant_node_close(n,1);
      rant_allocator_reset(&aa);
    }
}

/* The source timestamp phase: every message carries the writer's wall clock unless the
 * topic opts out. Delivery, opt out, replay, the queued path and the patterns are covered. */
#define TS_CH_PLAIN  0   /* reliable, catch_up 2: the stamp + the late-joiner replay */
#define TS_CH_OFF    1   /* publisher sets qos.no_timestamp: stamp 0, bytes untouched */
#define TS_CH_QUEUED 2   /* the subscriber drains it with rant_topic_take */
#define TS_CH_BIG    3   /* payload past the fragment size: SHM where available */
static uint64_t ts_sent[8], ts_recv_wall[8], ts_capture[8];
static unsigned long ts_msgs[8];
static size_t   ts_len[8]; static unsigned long ts_sum[8];
static void ts_on_message(const RantMsg *msg){
    uint16_t c = msg->topic_index < 8 ? msg->topic_index : 7;
    const unsigned char *p = (const unsigned char*)msg->data.data;
    size_t i; unsigned long s = 0;
    for (i=0;i<msg->data.len;i++) s += p[i];
    ts_sent[c] = msg->written_us; ts_recv_wall[c] = i_rant_plat_wall_us();
    ts_capture[c] = msg->capture_us;
    ts_len[c] = msg->data.len; ts_sum[c] = s;
    ts_msgs[c]++;
}
/* late joiner (its own node, one topic at index 0) */
static uint64_t ts_late_sent; static unsigned long ts_late_msgs;
static void ts_late_on_message(const RantMsg *msg){ ts_late_sent = msg->written_us; ts_late_msgs++; }
/* patterns capture: the handler's request stamp, the owner's write stamp */
static uint64_t ts_req_sent, ts_var_sent;
static volatile int ts_req_done;
static void ts_on_request(RantRequest *req, void *user){
    (void)user; ts_req_sent = req->written_us; ts_req_done = 1;
    rant_request_reply(req, rant_bytes(NULL,0));
}
static void ts_on_var_write(const RantVariableUpdate *u, void *user){
    (void)user; ts_var_sent = u->written_us;
}

static void ts_checks(void){
    static uint8_t mem_a[1], mem_b[1];
    static unsigned char big[64*1024];
    RantTopicDef ca[4], cb[4];
    RantNodeOpts ao, bo; RantDiscoveryAddr seed;
    RantNode *A, *B;
    uint8_t payload[64];
    uint64_t before, after;
    int i, t;

    memset(ca,0,sizeof ca); memset(payload,0xA5,sizeof payload);
    memset(ts_sent,0,sizeof ts_sent); memset(ts_msgs,0,sizeof ts_msgs);
    ca[TS_CH_PLAIN].name="ts/plain";
    ca[TS_CH_PLAIN].qos.reliability=RANT_RELIABLE; ca[TS_CH_PLAIN].qos.keep_last=8;
    ca[TS_CH_PLAIN].qos.catch_up=2; ca[TS_CH_PLAIN].qos.heartbeat_us=20000;
    ca[TS_CH_OFF].name="ts/off";
    ca[TS_CH_OFF].qos.reliability=RANT_RELIABLE; ca[TS_CH_OFF].qos.keep_last=8;
    ca[TS_CH_OFF].qos.heartbeat_us=20000; ca[TS_CH_OFF].qos.no_timestamp=1;
    ca[TS_CH_QUEUED].name="ts/queued";
    ca[TS_CH_QUEUED].qos.reliability=RANT_RELIABLE; ca[TS_CH_QUEUED].qos.keep_last=8;
    ca[TS_CH_QUEUED].qos.heartbeat_us=20000;
    ca[TS_CH_BIG].name="ts/big";
    ca[TS_CH_BIG].qos.reliability=RANT_RELIABLE; ca[TS_CH_BIG].qos.keep_last=4;
    ca[TS_CH_BIG].qos.heartbeat_us=20000; ca[TS_CH_BIG].qos.repair_delay_us=5000;
    memcpy(cb, ca, sizeof ca);
    for (i=0;i<4;i++){ ca[i].role=RANT_PUB_ONLY; cb[i].role=RANT_SUB_ONLY; }

    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&ao,0,sizeof ao); ao.domain=(uint16_t)(ST_DOMAIN+50); ao.discovery.max_peers=4;
    ao.net.multicast_interface="127.0.0.1"; ao.net.seed_peers=&seed; ao.net.n_seed_peers=1;
    bo = ao;
    A = test_node_open(mem_a, sizeof mem_a, "ts-pub", NULL, NULL, ao, ca, 4);
    B = test_node_open(mem_b, sizeof mem_b, "ts-sub", ts_on_message, NULL, bo, cb, 4);
    ST_CHECK(A && B, "writets: nodes open");
    if (!(A && B)){ if (A) rant_node_close(A,0); if (B) rant_node_close(B,0); return; }
    { uint64_t end = i_rant_plat_now_us()+5000000u;
      while (i_rant_plat_now_us()<end &&
             (rant_node_publisher_match_count(A,TS_CH_PLAIN)<1 ||
              rant_node_publisher_match_count(A,TS_CH_OFF)<1 ||
              rant_node_publisher_match_count(A,TS_CH_QUEUED)<1 ||
              rant_node_publisher_match_count(A,TS_CH_BIG)<1))
          st_pump(A,B,10); }
    ST_CHECK(rant_node_publisher_match_count(A,TS_CH_PLAIN)==1
          && rant_node_publisher_match_count(A,TS_CH_BIG)==1, "writets: matches formed");

    /* (a) a stamped message: written_us lies between the wall clock before the send and the
       wall clock read inside the callback */
    before = i_rant_plat_wall_us();
    rant_node_send(A, TS_CH_PLAIN, payload, sizeof payload);
    for (t=0;t<800 && ts_msgs[TS_CH_PLAIN]==0;t++) st_pump(A,B,2);
    after = ts_recv_wall[TS_CH_PLAIN];
    ST_CHECK(ts_msgs[TS_CH_PLAIN]==1, "writets: stamped message delivered (%lu)", ts_msgs[TS_CH_PLAIN]);
    ST_CHECK(ts_sent[TS_CH_PLAIN] >= before && ts_sent[TS_CH_PLAIN] <= after,
             "writets: written_us within [before, delivered] (%llu in [%llu, %llu])",
             (unsigned long long)ts_sent[TS_CH_PLAIN], (unsigned long long)before,
             (unsigned long long)after);
    ST_CHECK(ts_len[TS_CH_PLAIN]==sizeof payload,
             "writets: the stamp is stripped, payload intact (%lu B)", (unsigned long)ts_len[TS_CH_PLAIN]);

    /* (b) the opt-out: written_us 0, payload byte-identical */
    {   unsigned long want = 0; size_t k;
        for (k=0;k<sizeof payload;k++) want += payload[k];
        rant_node_send(A, TS_CH_OFF, payload, sizeof payload);
        for (t=0;t<800 && ts_msgs[TS_CH_OFF]==0;t++) st_pump(A,B,2);
        ST_CHECK(ts_msgs[TS_CH_OFF]==1 && ts_sent[TS_CH_OFF]==0,
                 "writets: no_timestamp topic delivers written_us 0 (n=%lu ts=%llu)",
                 ts_msgs[TS_CH_OFF], (unsigned long long)ts_sent[TS_CH_OFF]);
        ST_CHECK(ts_len[TS_CH_OFF]==sizeof payload && ts_sum[TS_CH_OFF]==want,
                 "writets: opted-out payload byte-identical (%lu B)", (unsigned long)ts_len[TS_CH_OFF]);
    }

    /* (c) catch_up replay: a LATE joiner gets the original stamp, not a fresh one */
    {   RantAllocator la = rant_allocator_heap(0);
        RantNodeOpts lo = ao; RantNode *L; RantTopic *lt;
        RantTopicOpts lopt; uint64_t join_wall;
        memset(&lopt,0,sizeof lopt); lopt.qos = ca[TS_CH_PLAIN].qos;
        st_pump(A,B,50);
        join_wall = i_rant_plat_wall_us();
        ts_late_msgs = 0; ts_late_sent = 0;
        lo.max_topics = 1;
        L = rant_node_open(&la, "ts-late", ts_late_on_message, NULL, &lo);
        lt = L ? rant_node_create_topic(L, "ts/plain", RANT_SUB_ONLY, NULL, &lopt) : NULL;
        ST_CHECK(L && lt, "writets: late joiner opens");
        if (L && lt){
            for (t=0;t<2000 && ts_late_msgs==0;t++){ st_pump(A,B,2); rant_node_poll(L,2); }
            ST_CHECK(ts_late_msgs>=1, "writets: replayed history reached the late joiner (%lu)",
                     ts_late_msgs);
            ST_CHECK(ts_late_sent==ts_sent[TS_CH_PLAIN],
                     "writets: replay keeps the ORIGINAL stamp (%llu == %llu)",
                     (unsigned long long)ts_late_sent, (unsigned long long)ts_sent[TS_CH_PLAIN]);
            ST_CHECK(ts_late_sent!=0 && ts_late_sent < join_wall,
                     "writets: the stamp predates the join (%llu < %llu)",
                     (unsigned long long)ts_late_sent, (unsigned long long)join_wall);
            rant_node_close(L,1);
        }
        rant_allocator_reset(&la);
    }

    /* (d) the queued path: take surfaces the same stamp an inline callback would */
    {   RantTopic *qt = rant_node_topic(B, TS_CH_QUEUED); RantMsg m;
        int r; uint64_t q_before;
        memset(&m,0,sizeof m);
        r = rant_topic_take(qt, &m, 0);            /* the first take makes the topic queued */
        ST_CHECK(r==0, "writets: queued topic starts empty (%d)", r);
        q_before = i_rant_plat_wall_us();
        rant_node_send(A, TS_CH_QUEUED, payload, sizeof payload);
        r = 0;
        for (t=0;t<800 && r!=1;t++){ rant_node_poll(A,0); r = rant_topic_take(qt, &m, 20); }
        ST_CHECK(r==1 && ts_msgs[TS_CH_QUEUED]==0,
                 "writets: queued message taken, no inline callback (r=%d cb=%lu)", r, ts_msgs[TS_CH_QUEUED]);
        ST_CHECK(r==1 && m.written_us >= q_before && m.written_us <= i_rant_plat_wall_us(),
                 "writets: taken message carries the stamp (%llu >= %llu)",
                 (unsigned long long)m.written_us, (unsigned long long)q_before);
        ST_CHECK(r==1 && m.data.len==sizeof payload,
                 "writets: taken payload intact (%lu B)", (unsigned long)m.data.len);
    }

    /* (e) a fragmenting payload (the SHM path where compiled in): the stamp rides the
       reassembled/shared bytes just the same */
    {   unsigned long want = 0; size_t k;
        for (k=0;k<sizeof big;k++){ big[k]=(unsigned char)((k*31u+7u)&0xFF); want += big[k]; }
        before = i_rant_plat_wall_us();
        rant_node_send(A, TS_CH_BIG, big, sizeof big);
        for (t=0;t<2000 && ts_msgs[TS_CH_BIG]==0;t++) st_pump(A,B,2);
        ST_CHECK(ts_msgs[TS_CH_BIG]==1 && ts_len[TS_CH_BIG]==sizeof big && ts_sum[TS_CH_BIG]==want,
                 "writets: %lu B message byte-exact (n=%lu len=%lu)", (unsigned long)sizeof big,
                 ts_msgs[TS_CH_BIG], (unsigned long)ts_len[TS_CH_BIG]);
        ST_CHECK(ts_sent[TS_CH_BIG] >= before && ts_sent[TS_CH_BIG] <= ts_recv_wall[TS_CH_BIG],
                 "writets: big message carries the stamp (%llu)", (unsigned long long)ts_sent[TS_CH_BIG]);
#ifdef RANT_SHM
        {   uint32_t shm_tx = 0;
            rant_node_shm_stats(A, &shm_tx, NULL);     /* same host: it went through shared memory */
            ST_CHECK(shm_tx>=1, "writets: the big message took the SHM path (tx=%u)", shm_tx); }
#endif
    }

    /* (f) the capture stamp: per message, costs nothing unset, and its marker bit never
       leaks into written_us. The plain, fragmenting, queued and opted out paths agree. */
    {   RantTopic *pt = rant_node_topic(A, TS_CH_PLAIN);
        RantTopic *ot = rant_node_topic(A, TS_CH_OFF);
        RantTopic *bt = rant_node_topic(A, TS_CH_BIG);
        RantTopic *qt = rant_node_topic(B, TS_CH_QUEUED);
        const uint64_t want = 1234567890123456ull;   /* a wall clock well inside 51 bits */
        uint64_t b0 = 0, b1 = 0, b2 = 0;
        RantMsg m; int r;

        /* unset: capture_us 0, and the sample is no larger than before */
        ts_msgs[TS_CH_PLAIN]=0; ts_capture[TS_CH_PLAIN]=1;
        rant_topic_counts(pt, NULL, &b0, NULL, NULL);
        rant_topic_send(pt, rant_bytes(payload, sizeof payload), NULL);
        for (t=0;t<800 && ts_msgs[TS_CH_PLAIN]==0;t++) st_pump(A,B,2);
        rant_topic_counts(pt, NULL, &b1, NULL, NULL);
        ST_CHECK(ts_msgs[TS_CH_PLAIN]==1 && ts_capture[TS_CH_PLAIN]==0,
                 "capture: an unset capture delivers 0 (%llu)",
                 (unsigned long long)ts_capture[TS_CH_PLAIN]);

        /* set: delivered exactly, written_us still a sane wall clock beside it */
        before = i_rant_plat_wall_us();
        ts_msgs[TS_CH_PLAIN]=0; ts_capture[TS_CH_PLAIN]=0;
        rant_topic_send(pt, rant_bytes(payload, sizeof payload),
                        &(RantSendOpts){ .capture_us = want });
        for (t=0;t<800 && ts_msgs[TS_CH_PLAIN]==0;t++) st_pump(A,B,2);
        rant_topic_counts(pt, NULL, &b2, NULL, NULL);
        ST_CHECK(ts_msgs[TS_CH_PLAIN]==1 && ts_capture[TS_CH_PLAIN]==want,
                 "capture: capture_us delivered exactly (%llu)",
                 (unsigned long long)ts_capture[TS_CH_PLAIN]);
        ST_CHECK(ts_sent[TS_CH_PLAIN] >= before
              && ts_sent[TS_CH_PLAIN] <= ts_recv_wall[TS_CH_PLAIN],
                 "capture: the marker never leaks into written_us (%llu in [%llu, %llu])",
                 (unsigned long long)ts_sent[TS_CH_PLAIN], (unsigned long long)before,
                 (unsigned long long)ts_recv_wall[TS_CH_PLAIN]);
        ST_CHECK(ts_len[TS_CH_PLAIN]==sizeof payload,
                 "capture: both stamps stripped, payload intact (%lu B)",
                 (unsigned long)ts_len[TS_CH_PLAIN]);
        ST_CHECK((b2 - b1) == (b1 - b0) + RANT_CAPTURE_BYTES,
                 "capture: it costs its 8 bytes only when set (%llu vs %llu)",
                 (unsigned long long)(b2 - b1), (unsigned long long)(b1 - b0));

        /* a no_timestamp topic frames neither slot, so a capture cannot ride it */
        ts_msgs[TS_CH_OFF]=0; ts_capture[TS_CH_OFF]=1;
        rant_topic_send(ot, rant_bytes(payload, sizeof payload),
                        &(RantSendOpts){ .capture_us = want });
        for (t=0;t<800 && ts_msgs[TS_CH_OFF]==0;t++) st_pump(A,B,2);
        ST_CHECK(ts_msgs[TS_CH_OFF]==1 && ts_capture[TS_CH_OFF]==0
              && ts_sent[TS_CH_OFF]==0 && ts_len[TS_CH_OFF]==sizeof payload,
                 "capture: an opted out topic carries neither stamp (cap=%llu len=%lu)",
                 (unsigned long long)ts_capture[TS_CH_OFF], (unsigned long)ts_len[TS_CH_OFF]);

        /* the fragmenting path, which is SHM where it is compiled in */
        ts_msgs[TS_CH_BIG]=0; ts_capture[TS_CH_BIG]=0;
        rant_topic_send(bt, rant_bytes(big, sizeof big),
                        &(RantSendOpts){ .capture_us = want });
        for (t=0;t<2000 && ts_msgs[TS_CH_BIG]==0;t++) st_pump(A,B,2);
        ST_CHECK(ts_msgs[TS_CH_BIG]==1 && ts_capture[TS_CH_BIG]==want
              && ts_len[TS_CH_BIG]==sizeof big,
                 "capture: a fragmenting message carries it (cap=%llu len=%lu)",
                 (unsigned long long)ts_capture[TS_CH_BIG], (unsigned long)ts_len[TS_CH_BIG]);

        /* the queued path: take surfaces it exactly as an inline callback would */
        memset(&m,0,sizeof m);
        rant_topic_send(rant_node_topic(A, TS_CH_QUEUED),
                        rant_bytes(payload, sizeof payload),
                        &(RantSendOpts){ .capture_us = want });
        r = 0;
        for (t=0;t<800 && r!=1;t++){ rant_node_poll(A,0); r = rant_topic_take(qt, &m, 20); }
        ST_CHECK(r==1 && m.capture_us==want,
                 "capture: a taken message carries it (r=%d cap=%llu)",
                 r, (unsigned long long)m.capture_us);
    }
    rant_node_close(B,1); rant_node_close(A,1);

    /* (g) the patterns layer: a request's stamp at the handler, a remote write's stamp at
       the variable owner */
    {   RantAllocator pa = rant_allocator_heap(0);
        RantAllocator qa = rant_allocator_heap(0);
        RantNodeOpts po = ao, co; RantNode *P, *C;
        RantFunction *fd, *fr; RantVariable *vd, *vr;
        uint8_t b[4];
        po.domain = (uint16_t)(ST_DOMAIN+51); co = po;
        P = rant_node_open(&pa, "ts-def", NULL, NULL, &po);
        C = rant_node_open(&qa, "ts-rem", NULL, NULL, &co);
        ST_CHECK(P && C, "writets: pattern nodes open");
        if (P && C){
            i_rant_le_w32(b, 1);
            fd = rant_node_create_function_definition(P, "ts/fn", NULL, NULL, ts_on_request, NULL, NULL);
            fr = rant_node_create_remote_function(C, "ts/fn", NULL, NULL, NULL);
            vd = rant_node_create_variable_definition(P, "ts/var", NULL,
                     &(RantVariableOpts){ .initial=rant_bytes(b,4) });
            vr = rant_node_create_remote_variable(C, "ts/var", NULL, NULL);
            ST_CHECK(fd && fr && vd && vr, "writets: pattern entities created");
            rant_variable_on_write(vd, ts_on_var_write, NULL);
            for (t=0;t<2000 && rant_function_match_count(fr)==0;t++) pf_pump(P,C,2);
            ts_req_done = 0; ts_req_sent = 0;
            rant_function_call_async(fr, rant_bytes(b,4), NULL, NULL, NULL);
            for (t=0;t<800 && !ts_req_done;t++) pf_pump(P,C,2);
            ST_CHECK(ts_req_done && ts_req_sent!=0,
                     "writets: RantRequest.written_us stamped (done=%d ts=%llu)",
                     ts_req_done, (unsigned long long)ts_req_sent);
            ts_var_sent = 0;
            i_rant_le_w32(b, 42);
            for (t=0;t<2000 && rant_variable_match_count(vr)==0;t++) pf_pump(P,C,2);
            rant_variable_set(vr, rant_bytes(b,4));
            for (t=0;t<800 && ts_var_sent==0;t++) pf_pump(P,C,2);
            ST_CHECK(ts_var_sent!=0, "writets: RantVariableUpdate.written_us stamped (%llu)",
                     (unsigned long long)ts_var_sent);
            rant_variable_on_write(vd, NULL, NULL);
        }
        if (C) rant_node_close(C,1);
        if (P) rant_node_close(P,1);
        rant_allocator_reset(&pa); rant_allocator_reset(&qa);
    }
}

/* (19b3) the interest paging codec and the external overlay flag: build both forms, page
   a blob by byte range, reject malformed pages. Sans IO, codec only. */
static void interest_codec_checks(void){
    static uint8_t tmem[1<<16];
    RantAllocator ma = rant_allocator_heap(0);
    RantConfig tc; RantTransportState *tr; RantTopicDef ch[3];
    uint8_t meta[256], req[64], page[128];
    uint16_t ml; uint32_t total, off; RantBytes chunk;

    memset(ch,0,sizeof ch);
    ch[0].name="ic/a";
    ch[1].name="ic/b"; ch[1].role=RANT_SUB_ONLY; ch[1].qos.max_rate_hz=50;    /* rate section too */
    ch[2].name="ic/c";
    memset(&tc,0,sizeof tc); tc.topics=ch; tc.n_topics=3; tc.max_peers=2;
    tc.allocator=rant_allocator_alloc; tc.user=&ma;
    tr = rant_transport_init(tmem, sizeof tmem, &tc);
    ST_CHECK(tr!=NULL, "icodec: transport init");
    if (!tr){ rant_allocator_reset(&ma); return; }

    /* inline overlay: flag clear, interest present, all three size views agree */
    ml = rant_transport_meta_build(tr, meta, sizeof meta, 1200, 0, NULL, 0);
    ST_CHECK(ml == rant_transport_meta_size(tr), "icodec: inline build == meta_size (%u)", ml);
    ST_CHECK(rant_meta_frag(rant_bytes(meta,ml))==1200, "icodec: frag survives");
    ST_CHECK(!rant_meta_interest_external(rant_bytes(meta,ml)), "icodec: inline -> external flag clear");
    {   RantBytes in = rant_meta_interest(rant_bytes(meta,ml));
        ST_CHECK(in.data && in.len == rant_transport_interest_size(tr),
                 "icodec: interest slice == interest_size (%u)", (unsigned)in.len); }

    /* bootstrap overlay: fixed size, flag set, interest NULL (never read as empty) */
    ml = rant_transport_meta_build(tr, meta, sizeof meta, 1200, 0, NULL, 1);
    ST_CHECK(ml == rant_transport_meta_bootstrap_size(), "icodec: bootstrap size (%u)", ml);
    ST_CHECK(rant_meta_interest_external(rant_bytes(meta,ml)), "icodec: bootstrap -> external flag set");
    ST_CHECK(rant_meta_interest(rant_bytes(meta,ml)).data == NULL, "icodec: bootstrap interest is NULL");
    ST_CHECK(rant_meta_frag(rant_bytes(meta,ml))==1200, "icodec: bootstrap still carries frag");

    /* REQ roundtrip + header gating */
    {   size_t rl = rant_interest_req_build(7, 42u, 123u, req, sizeof req);
        ST_CHECK(rl==18u, "icodec: req builds (%u bytes)", (unsigned)rl);
        ST_CHECK(rant_detail_kind(rant_bytes(req,rl))==RANT_INTEREST_REQ
              && rant_detail_domain(rant_bytes(req,rl))==7
              && rant_detail_meta_version(rant_bytes(req,rl))==42u, "icodec: req header decodes");
        ST_CHECK(rant_interest_req_offset(rant_bytes(req,rl), &off) && off==123u, "icodec: req offset rides");
        ST_CHECK(!rant_interest_req_offset(rant_bytes(req,rl-1u), &off), "icodec: short req rejected");
        ST_CHECK(rant_interest_req_build(7,42u,0u,req,17u)==0u, "icodec: tiny cap refused"); }

    /* RESP page roundtrip + bounds */
    {   uint8_t blob[40]; size_t hl; unsigned i;
        for (i=0;i<sizeof blob;i++) blob[i]=(uint8_t)i;
        hl = rant_interest_resp_head(7, 42u, 40u, 10u, 20u, page, sizeof page);
        ST_CHECK(hl==RANT_INTEREST_RESP_HEAD, "icodec: resp head builds");
        memcpy(page+hl, blob+10, 20);
        ST_CHECK(rant_interest_resp_parse(rant_bytes(page, hl+20u), &total, &off, &chunk)
              && total==40u && off==10u && chunk.len==20u && chunk.data[0]==10u,
                 "icodec: resp page parses (total=%u off=%u len=%u)", total, off, (unsigned)chunk.len);
        ST_CHECK(!rant_interest_resp_parse(rant_bytes(page, hl+19u), &total, &off, &chunk),
                 "icodec: truncated page rejected");
        rant_interest_resp_head(7, 42u, 40u, 30u, 20u, page, sizeof page);     /* 30+20 > 40 */
        memcpy(page+RANT_INTEREST_RESP_HEAD, blob, 20);
        ST_CHECK(!rant_interest_resp_parse(rant_bytes(page, RANT_INTEREST_RESP_HEAD+20u), &total, &off, &chunk),
                 "icodec: out-of-blob range rejected"); }
    rant_allocator_reset(&ma);
}

/* (19b4) external interest end to end: a 300 topic publisher ships a sub MTU bootstrap
   and the subscriber pulls the blob by byte range paging (spec/testing.md). */
#define IX_TOPICS 300
static int ix_recv;
static uint32_t ix_epoch_pid, ix_epoch;
static void ix_on_message(const RantMsg *msg){ (void)msg; ix_recv++; }
static void interest_external_checks(void){
    RantAllocator pa = rant_allocator_heap(0);
    RantAllocator sa = rant_allocator_heap(0);
    RantNodeOpts po, so; RantNode *P, *S; RantTopic *pub=NULL, *sub=NULL;
    RantTopicOpts co; RantDiscoveryAddr seed; char name[16];
    uint8_t payload[8]; int i, t;
    memset(&co,0,sizeof co); co.qos.reliability=RANT_RELIABLE; co.qos.keep_last=4; co.qos.catch_up=1;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=ST_DOMAIN+30; po.max_topics=IX_TOPICS+2;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po; so.max_topics=2;
#ifdef _WIN32
    g_tx_max_len=0; g_tx_interest_req=0;
#endif
    P = rant_node_open(&pa, "ix-pub", NULL, NULL, &po);
    S = rant_node_open(&sa, "ix-sub", ix_on_message, NULL, &so);
    ST_CHECK(P&&S, "interest: nodes open");
    if (!(P&&S)){ if(P)rant_node_close(P,0); if(S)rant_node_close(S,0); return; }
    for (i=0;i<IX_TOPICS;i++){
        snprintf(name,sizeof name,"ix/%03d",i);
        if (!rant_node_create_topic(P, name, RANT_PUB_ONLY, NULL, &co)) break;
    }
    ST_CHECK(i==IX_TOPICS, "interest: %d topics created (%d)", IX_TOPICS, i);
    pub = rant_node_topic(P, 250);
    sub = rant_node_create_topic(S, "ix/250", RANT_SUB_ONLY, NULL, &co);
    ST_CHECK(pub && sub, "interest: endpoints ready");
    ix_recv=0;
    for (t=0;t<2000 && (!pub || rant_topic_match_count(pub)==0);t++){ rant_node_poll(P,2); rant_node_poll(S,2); }
    ST_CHECK(pub && rant_topic_match_count(pub)>0, "interest: match formed through the external fetch");
    memset(payload,0x77,sizeof payload);
    if (pub) rant_topic_send(pub, rant_bytes(payload,sizeof payload), NULL);
    for (t=0;t<800 && ix_recv==0;t++){ rant_node_poll(P,1); rant_node_poll(S,2); }
    ST_CHECK(ix_recv>=1, "interest: delivery across the external match (%d)", ix_recv);
    {   /* reflection reads the assembled interest, not the announce: the entity fold must
           enumerate the external peer's whole set. The interest epoch is the observer cache key. */
        uint16_t cnt, k; int ents = 0; uint32_t pid = 0, epoch = 0;
        const RantDiscoveryPeer *ps = st_peers(S, &cnt);
        for (k=0;k<cnt;k++)
            if (ps && ps[k].name.len==6 && !memcmp(ps[k].name.data,"ix-pub",6)){
                pid = ps[k].id; epoch = i_rant_node_peer_interest_epoch(&ps[k]);
            }
        ST_CHECK(pid!=0, "interest: publisher visible in the peer view");
        ST_CHECK(epoch>0, "interest: interest epoch advanced on assembly (%u)", epoch);
        if (pid){
            RantIter eit; RantEntityInfo ei;
            memset(&eit,0,sizeof eit);
            while (rant_node_entities_next(S, pid, &eit, &ei))
                ents++;   /* the @rant/ builtins are hidden from the walk */
            ST_CHECK(ents==IX_TOPICS, "interest: reflection enumerates all %d external entities (%d)",
                     IX_TOPICS, ents);
        }
        ix_epoch_pid = pid; ix_epoch = epoch;   /* steady-state stability checked below */
    }
#ifdef _WIN32
    ST_CHECK(g_tx_max_len>0 && g_tx_max_len<=(int)RANT_DGRAM_MAX,
             "interest: no datagram exceeded RANT_DGRAM_MAX (max=%d)", g_tx_max_len);
    ST_CHECK(g_tx_interest_req>=1, "interest: the blob traveled by paged fetch (%llu reqs)",
             (unsigned long long)g_tx_interest_req);
    {   unsigned long long before = g_tx_interest_req;   /* steady state: the version dedup */
        for (t=0;t<250;t++){ rant_node_poll(P,5); rant_node_poll(S,5); }
        ST_CHECK(g_tx_interest_req==before,
                 "interest: steady-state announces trigger no re-fetch (+%llu)",
                 (unsigned long long)(g_tx_interest_req-before)); }
#endif
    {   /* the epoch is quiet in steady state too: an epoch-keyed observer cache
           (the explorer) re-walks only on real change, never per announce */
        uint16_t cnt, k; uint32_t epoch_now = 0;
        const RantDiscoveryPeer *ps = st_peers(S, &cnt);
        for (k=0;k<cnt;k++)
            if (ps && ps[k].id==ix_epoch_pid) epoch_now = i_rant_node_peer_interest_epoch(&ps[k]);
        ST_CHECK(ix_epoch_pid && epoch_now==ix_epoch,
                 "interest: epoch stable across steady state (%u -> %u)", ix_epoch, epoch_now);
    }
    rant_node_close(P,1); rant_node_close(S,1);
}

/* The metalog phase: A logs before anyone listens and triggers a mirrored error, B late
 * joins the error level and reads both, then calls A's @rant/meta directed at A. */
static void metalog_checks(void){
    RantAllocator aa = rant_allocator_heap(0);
    RantAllocator ba = rant_allocator_heap(0);
    RantNodeOpts ao, bo; RantNode *A=NULL, *B=NULL; RantDiscoveryAddr seed;
    int t, i;

    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&ao,0,sizeof ao); ao.domain=ST_DOMAIN+35; ao.discovery.max_peers=4;
    ao.net.multicast_interface="127.0.0.1"; ao.net.seed_peers=&seed; ao.net.n_seed_peers=1;
    bo=ao;
    ao.match_wait_ms = -1;    /* so the unmatched send below commits + fires immediately */
    A = rant_node_open(&aa, "meta-a", NULL, NULL, &ao);
    B = rant_node_open(&ba, "meta-b", NULL, NULL, &bo);
    ST_CHECK(A && B, "metalog: nodes open");
    if (!(A && B)){ if(A)rant_node_close(A,0); if(B)rant_node_close(B,0);
                    rant_allocator_reset(&ba); rant_allocator_reset(&aa); return; }

    ST_CHECK(rant_node_log_topic(A, RANT_LOG_ERROR) && rant_node_log_topic(A, RANT_LOG_WARN)
             && rant_node_log_topic(A, RANT_LOG_INFO), "metalog: log topics exist");
    ST_CHECK(rant_node_meta_function(A) != NULL && rant_node_meta_function(B) != NULL,
             "metalog: meta endpoint hosted");

    /* log BEFORE any subscriber exists: the lines land in KEEP_LAST history and a late
       joiner replays them (catch_up = keep_last) */
    for (i=0;i<3;i++){
        int lr = rant_node_log(A, RANT_LOG_ERROR, "boom %d", i);   /* hoisted out of ST_CHECK */
        ST_CHECK(lr == RANT_OK, "metalog: log %d accepted (%d)", i, lr);
    }
    { uint64_t txm=0;
      rant_topic_counts(rant_node_log_topic(A, RANT_LOG_ERROR), &txm, NULL, NULL, NULL);
      ST_CHECK(txm >= 3, "metalog: tx counter counts the lines (%u)", (unsigned)txm); }

    /* an internal error mirrors onto @rant/log/error: an unmatched send right after open
       (gather unsettled, wait disabled) fires RANT_E_UNMATCHED_SEND */
    { RantTopic *src = rant_node_create_topic(A, "mirror-src", RANT_PUB_ONLY, NULL, NULL);
      uint8_t payload[4] = {1,2,3,4};
      ST_CHECK(src != NULL, "metalog: mirror-src created");
      if (src) rant_topic_send(src, rant_bytes(payload, 4), NULL);
      rant_node_poll(A, 0);     /* flush the mirror ring into the log topic */
    }

    /* late subscriber: widen B's own handle of the error level to PUBSUB, take the replay */
    { RantTopic *eh = rant_node_log_topic(B, RANT_LOG_ERROR);
      RantMsg m; int got_boom0=0, got_mirror=0, n_got=0;
      int sub_ok = eh && rant_topic_set_role(eh, RANT_PUBSUB) == 0;
      ST_CHECK(sub_ok, "metalog: log subscribe");
      for (t=0;t<1500 && !(got_boom0 && got_mirror);t++){
          pf_pump(A,B,2);
          while (n_got<16 && rant_topic_take(eh, &m, 0) == 1){
              RantString txt = rant_get_string(m.data, m.schema, "text");
              uint64_t wall = rant_get_uint(m.data, m.schema, "wall_us");
              char tb[64]; size_t tl = txt.len < sizeof tb - 1 ? txt.len : sizeof tb - 1;
              memcpy(tb, txt.data, tl); tb[tl]='\0';
              n_got++;
              if (strcmp(tb, "boom 0") == 0 && wall) got_boom0 = 1;
              if (strstr(tb, "unmatched-send")) got_mirror = 1;
          }
      }
      ST_CHECK(n_got >= 4 && got_boom0 && got_mirror,
               "metalog: replay + mirrored error received (n=%d boom0=%d mirror=%d)",
               n_got, got_boom0, got_mirror); }

    /* @rant/meta: B calls A's endpoint directed at A's peer id. A answers on its service
       thread while B blocks in the call */
    { const RantDiscoveryPeer *ps; uint16_t cnt=0; uint32_t idA=0;
      RantResponse rep; int rc;
      ps = st_peers(B, &cnt);
      for (i=0;i<(int)cnt;i++)
          if (ps[i].name.len==6 && memcmp(ps[i].name.data,"meta-a",6)==0) idA = ps[i].id;
      ST_CHECK(idA != 0, "metalog: found A's peer id (%u)", idA);
      rant_node_start(A);
      rc = rant_function_call(rant_node_meta_function(B), rant_bytes(NULL,0), &rep, 3000,
                              &(RantCallOpts){ .provider = idA });
      rant_node_stop(A);
      ST_CHECK(rc==1 && rep.status==RANT_CALL_OK && rep.data.len>0 && rep.schema,
               "metalog: meta call answered (rc=%d st=%d len=%u)",
               rc, rep.status, (unsigned)rep.data.len);
      if (rc==1 && rep.status==RANT_CALL_OK && rep.schema){
          RantBytes info = rant_get_map(rep.data, rep.schema, "info");
          RantValue nodev, namev, topv, upt;
          int ok_node   = rant_map_get(info, "node", &nodev);
          int ok_name   = ok_node && rant_map_get(nodev.bytes, "name", &namev);
          int ok_topics = rant_map_get(info, "topics", &topv);
          ST_CHECK(ok_name && namev.bytes.len==6 && memcmp(namev.bytes.data,"meta-a",6)==0,
                   "metalog: snapshot node.name == meta-a (%d)", ok_name);
          { /* only APP topics ride the snapshot: mirror-src, never the @rant/ builtins */
            RantValue row, nm; uint16_t r; int hid = 0;
            for (r = 0; ok_topics && r < topv.count; r++)
                if (rant_map_array_at(topv.bytes, r, &row) && rant_map_get(row.bytes, "name", &nm)
                    && nm.bytes.len >= 8 && !memcmp(nm.bytes.data, "@rant/", 8)) hid++;
            ST_CHECK(ok_topics && topv.count == 1 && hid == 0,
                     "metalog: snapshot lists app topics only (%u rows, %d hidden)",
                     (unsigned)(ok_topics?topv.count:0), hid); }
          ST_CHECK(ok_node && rant_map_get(nodev.bytes, "uptime_us", &upt) && upt.v.u > 0,
                   "metalog: snapshot uptime present");
      }

      /* the builtins are hidden from reflection too: A's walks see only mirror-src,
         B hosts nothing visible at all */
      { RantIter eit; RantEntityInfo ei; int la=0, lb=0, pa=0;
        memset(&eit,0,sizeof eit);
        while (rant_node_entities_next(A, RANT_SELF, &eit, &ei)) la++;
        memset(&eit,0,sizeof eit);
        while (rant_node_entities_next(B, RANT_SELF, &eit, &ei)) lb++;
        memset(&eit,0,sizeof eit);
        while (rant_node_entities_next(B, idA, &eit, &ei)) pa++;
        ST_CHECK(la==1 && lb==0 && pa==1,
                 "metalog: entity walks hide the builtins (A=%d B=%d peerA=%d)", la, lb, pa); }

      /* the hidden namespace is reserved: a leading '@' is refused in every constructor */
      { RantVariable *ev = rant_node_create_variable_definition(A, "@rant/evil", NULL, NULL);
        RantTopic *tp = rant_node_create_topic(A, "a@b", RANT_PUB_ONLY, NULL, NULL);
        ST_CHECK(ev == NULL && tp == NULL, "metalog: reserved '@' names refused"); } }

    rant_node_close(B,1); rant_node_close(A,1);
    rant_allocator_reset(&ba); rant_allocator_reset(&aa);
}

static int selftest_main(void){
    static uint8_t mem_w[1<<20], mem_r[1<<20];
    uint8_t payload[32]; unsigned i;
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: keep output on a crash */
    /* per-process domain base (see ST_DOMAIN): phases span base..base+~40, strides of
       64 keep concurrent runs disjoint, offset past the small domains real nodes use */
    st_domain_base = (uint16_t)(1000u + (uint16_t)(i_rant_plat_now_us() % 900u) * 64u);
    printf("selftest domains: %u..\n", (unsigned)st_domain_base);
    memset(payload, 0x5A, sizeof payload);

    RantTopicDef ch[4]; memset(ch, 0, sizeof ch);
    ch[0].name = "st/gap";
    ch[0].qos.reliability = RANT_RELIABLE; ch[0].qos.keep_last = ST_DEPTH;
    ch[0].qos.max_message_bytes = 64; ch[0].qos.heartbeat_us = 50000;
    ch[1] = ch[0]; ch[1].name = "st/block"; ch[1].qos.backpressure_wait_us = ST_BLOCK_US;
    ch[2] = ch[0]; ch[2].name = "st/dyn";
    ch[2].qos.catch_up = ST_DEPTH;   /* phase 5 asserts ring replay on join */
    ch[3] = ch[0]; ch[3].name = "st/block2";
    ch[3].qos.backpressure_wait_us = ST_BLOCK_US; ch[3].qos.repair_delay_us = ST_NACK_US;

    /* disable_shm: phases 2 to 4b exercise the UDP reliability path. The same host SHM path
       has its own coverage and its chunk recycle semantics differ. */
    RantNodeOpts wo = { .domain = ST_DOMAIN, .disable_shm = 1 };
    { RantNodeOpts ro = wo; RantTopicDef chr[4]; RantNode *w, *r;
      memcpy(chr, ch, sizeof ch);
      ch[0].role = ch[1].role = ch[2].role = ch[3].role = RANT_PUB_ONLY;
      chr[0].role = chr[1].role = chr[3].role = RANT_SUB_ONLY;
      chr[2].role = RANT_INACTIVE;

      w = test_node_open(mem_w, sizeof mem_w, NULL, NULL, NULL, wo, ch, 4);
      r = test_node_open(mem_r, sizeof mem_r, NULL, st_on_message, st_on_event, ro, chr, 4);
      if (!w || !r){ fprintf(stderr, "node open failed\n"); return 1; }

      /* 1. JOIN: the writer streams while discovery completes. The reader must adopt the
         stream head silently, no gap for a late joiner */
      { uint64_t end = i_rant_plat_now_us() + 5000000u;
        while (st_samples[ST_CH_GAP]==0 && i_rant_plat_now_us() < end){
            rant_node_send(w, ST_CH_GAP, payload, sizeof payload);
            st_pump(w, r, 20);
        } }
      ST_CHECK(st_samples[ST_CH_GAP] > 0, "join: reader receives (got %lu)", st_samples[ST_CH_GAP]);
      ST_CHECK(st_gap_calls[ST_CH_GAP] == 0, "join: no on_gap for late join (calls=%lu)", st_gap_calls[ST_CH_GAP]);

      /* 2. GAP: stage 50 samples with no flush in between. Only the last ST_DEPTH survive,
         the rest must arrive as exactly one gap */
      st_pump(w, r, 200);                           /* settle acks */
      { unsigned long s0 = st_samples[ST_CH_GAP];
        for (i=0;i<50;i++) rant_node_send(w, ST_CH_GAP, payload, sizeof payload);
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
      for (i=0;i<ST_DEPTH;i++) rant_node_send(w, ST_CH_BLOCK, payload, sizeof payload);
      st_pump(w, NULL, 50);                         /* flush. The reader is silent, no acks */
      { uint64_t t0 = i_rant_plat_now_us(), dt;
        rant_node_send(w, ST_CH_BLOCK, payload, sizeof payload);     /* evicts un-acked */
        dt = i_rant_plat_now_us() - t0;
        ST_CHECK(dt >= ST_BLOCK_US-10000 && dt < 4*ST_BLOCK_US,
                 "blocked: send waited ~backpressure_wait_us (%.1f ms)", dt/1000.0);
      }

      /* 4. RELEASED: let the reader catch up and ack. Sends are instant */
      st_pump(w, r, 300);
      { uint64_t t0 = i_rant_plat_now_us(), dt;
        rant_node_send(w, ST_CH_BLOCK, payload, sizeof payload);
        dt = i_rant_plat_now_us() - t0;
        ST_CHECK(dt < 20000, "released: acked ring sends instantly (%.1f ms)", dt/1000.0);
        st_pump(w, r, 100);
        /* the blocked phase eviction dropped only history already in flight to the reader's
           socket, so every send must have been delivered and no gap reported */
        ST_CHECK(st_gap_calls[ST_CH_BLOCK] == 0 && st_samples[ST_CH_BLOCK] == ST_DEPTH+2,
                 "blocked: in-flight eviction loses nothing (gaps=%lu, samples=%lu/%u)",
                 st_gap_calls[ST_CH_BLOCK], st_samples[ST_CH_BLOCK], ST_DEPTH+2);
      }

      /* 4b. SWEEP-ACK: ST_CH_BLOCK2 has nack_delay > 0, so the reader's ACKNACK is timer armed
         and flushed only by the sweep once the writer goes quiet (spec/testing.md). */
      st_pump(w, r, 200);                              /* match + settle */
      { unsigned long s0 = st_samples[ST_CH_BLOCK2];
        for (i=0;i<ST_DEPTH;i++) rant_node_send(w, ST_CH_BLOCK2, payload, sizeof payload);
        st_pump(w, r, 200);   /* the reader drains the burst, the sweep must ack */
        ST_CHECK(st_samples[ST_CH_BLOCK2]-s0 == ST_DEPTH, "sweep-ack: ring delivered (%lu, want %u)",
                 st_samples[ST_CH_BLOCK2]-s0, ST_DEPTH);
        { uint64_t t0 = i_rant_plat_now_us(), dt;
          rant_node_send(w, ST_CH_BLOCK2, payload, sizeof payload);     /* would evict slot 0 */
          dt = i_rant_plat_now_us() - t0;
          ST_CHECK(dt < 20000,
                   "sweep-ack: sub-only reader's timer ack releases backpressure (%.1f ms)", dt/1000.0);
        }
      }

      /* 5. DYNAMIC: ST_CH_DYN is inactive on the reader. Subscribe replays the cached ring,
         unsubscribe goes silent at the writer, resubscribe replays again, all gap free. */
      st_pump(w, r, 200);
      for (i=0;i<3;i++) rant_node_send(w, ST_CH_DYN, payload, sizeof payload);
      st_pump(w, r, 300);
      ST_CHECK(st_samples[ST_CH_DYN] == 0, "dynamic: inactive receives nothing (%lu)",
               st_samples[ST_CH_DYN]);
      rant_node_set_role(r, ST_CH_DYN, RANT_SUB_ONLY);
      st_pump(w, r, 400);
      ST_CHECK(st_samples[ST_CH_DYN] == 3, "dynamic: subscribe replays cached history (%lu, want 3)",
               st_samples[ST_CH_DYN]);
      rant_node_send(w, ST_CH_DYN, payload, sizeof payload);
      st_pump(w, r, 300);
      ST_CHECK(st_samples[ST_CH_DYN] == 4, "dynamic: live sample delivered (%lu, want 4)",
               st_samples[ST_CH_DYN]);
      rant_node_set_role(r, ST_CH_DYN, RANT_INACTIVE);
      st_pump(w, r, 300);                  /* let the new list reach the writer */
      for (i=0;i<5;i++) rant_node_send(w, ST_CH_DYN, payload, sizeof payload);
      st_pump(w, r, 300);
      ST_CHECK(st_samples[ST_CH_DYN] == 4, "dynamic: unsubscribed receives nothing (%lu)",
               st_samples[ST_CH_DYN]);
      rant_node_set_role(r, ST_CH_DYN, RANT_SUB_ONLY);
      st_pump(w, r, 400);
      ST_CHECK(st_samples[ST_CH_DYN] == 4+ST_DEPTH, "dynamic: resubscribe replays ring (%lu, want %u)",
               st_samples[ST_CH_DYN], 4+ST_DEPTH);
      ST_CHECK(st_gap_calls[ST_CH_DYN] == 0, "dynamic: joins are silent (gaps=%lu)",
               st_gap_calls[ST_CH_DYN]);

      /* 5b. FLAP and EPOCH GUARD: a one sided flap where the reader rebuilt its state. The
         changed reader epoch in the first ACKNACK must make every writer lane re join. */
      st_pump(w, r, 200);
      { unsigned long s0 = st_samples[ST_CH_DYN];
        uint32_t wid = 0; uint16_t k; uint8_t ib[256]; size_t il;
        for (k=0;k<i_rant_node_core_max_peers(r->core);k++) if (i_rant_node_core_peer_at(r->core,k,&wid,NULL,NULL,NULL)) break;
        rant_transport_peer_remove(r->transport, wid);
        rant_transport_peer_add(r->transport, wid,RANT_FRAG_SIZE);
        il = rant_transport_build_interest(w->transport, ib, sizeof ib);
        rant_transport_apply_peer_interest(r->transport, wid, rant_bytes(ib, il));
        /* the apply only nominates, since peer_remove dropped the cached verdicts. Run the sans
           IO detail exchange by hand so the flapped reader re verifies and rematches */
        {   RantDetailWant wl[8]; uint8_t rq[256], rp[1024]; uint16_t nw2, verified; size_t rl2, pl2;
            nw2 = rant_transport_detail_wants(r->transport, NULL, wid, rant_bytes(ib, il), wl, 8);
            ST_CHECK(nw2 > 0, "flap: re-added peer nominates pending candidates (%u)", nw2);
            rl2 = rant_detail_req_build(ST_DOMAIN, 0, wl, nw2, rq, sizeof rq);
            pl2 = rant_transport_detail_respond(w->transport, NULL, 0, rant_bytes(rq, rl2), rp, sizeof rp);
            verified = rant_transport_apply_peer_details(r->transport, wid, rant_bytes(rp, pl2));
            ST_CHECK(verified == nw2, "flap: details verify every candidate (%u/%u)", verified, nw2);
            rant_transport_apply_peer_interest(r->transport, wid, rant_bytes(ib, il));
        }
        st_pump(w, r, 600);
        ST_CHECK(st_samples[ST_CH_DYN] == s0+ST_DEPTH,
                 "flap: writer re-joins new reader incarnation, replays ring (%lu, want %lu)",
                 st_samples[ST_CH_DYN], s0+ST_DEPTH);
        ST_CHECK(st_gap_calls[ST_CH_DYN] == 0, "flap: recovery is silent (gaps=%lu)",
                 st_gap_calls[ST_CH_DYN]);
      }

      /* 5c. RESUME: a discovery blip drops the peer on both sides without tearing down
         transport state. A same incarnation resume replays the withheld backlog with no gap. */
      st_pump(w, r, 200);
      { unsigned long s0 = st_samples[ST_CH_DYN], g0 = st_gap_calls[ST_CH_DYN];
        uint32_t wid = 0, rid = 0; uint16_t k;
        for (k=0;k<i_rant_node_core_max_peers(r->core);k++) if (i_rant_node_core_peer_at(r->core,k,&wid,NULL,NULL,NULL)) break;
        for (k=0;k<i_rant_node_core_max_peers(w->core);k++) if (i_rant_node_core_peer_at(w->core,k,&rid,NULL,NULL,NULL)) break;
        rant_transport_peer_dormant(w->transport, rid);     /* drops the reader from flow control */
        rant_transport_peer_dormant(r->transport, wid);     /* reader stops acking the writer */
        for (i=0;i<3;i++) rant_node_send(w, ST_CH_DYN, payload, sizeof payload);
        st_pump(w, r, 300);
        ST_CHECK(st_samples[ST_CH_DYN] == s0, "resume: dormant peer withholds sends (%lu, want %lu)",
                 st_samples[ST_CH_DYN], s0);
        rant_transport_peer_resume(w->transport, rid);
        rant_transport_peer_resume(r->transport, wid);
        st_pump(w, r, 400);
        ST_CHECK(st_samples[ST_CH_DYN] == s0+3,
                 "resume: backlog replays from preserved position (%lu, want %lu)",
                 st_samples[ST_CH_DYN], s0+3);
        ST_CHECK(st_gap_calls[ST_CH_DYN] == g0, "resume: lossless, no gap (gaps=%lu, want %lu)",
                 st_gap_calls[ST_CH_DYN], g0);
      }

      rant_node_close(r, 1);
      rant_node_close(w, 1);
    }

    /* 6. SCALE: 40 topics. The full interest list rides one announce blob, matched at peer_up */
    { static uint8_t mem_a[1<<20], mem_b[1<<20];
      static RantTopicDef cha[ST_NCH], chb[ST_NCH];
      static char snames[ST_NCH][12];     /* "scale/0".."scale/39" */
      RantNodeOpts ao, bo; RantNode *a, *b; uint16_t k;
      memset(cha, 0, sizeof cha);
      for (k=0;k<ST_NCH;k++){
          sprintf(snames[k], "scale/%u", k); cha[k].name = snames[k];
          cha[k].qos.reliability = RANT_RELIABLE;
          cha[k].qos.keep_last = 1;
          cha[k].qos.catch_up = 1;      /* sent before discovery completes */
          cha[k].qos.max_message_bytes = 32;
          cha[k].qos.heartbeat_us = 50000;
          cha[k].role = RANT_PUB_ONLY;
      }
      memcpy(chb, cha, sizeof cha);
      for (k=0;k<ST_NCH;k++) chb[k].role = RANT_SUB_ONLY;
      ao = (RantNodeOpts){ .domain = ST_DOMAIN+1 };
      bo = ao;
      a = test_node_open(mem_a, sizeof mem_a, NULL, NULL, NULL, ao, cha, ST_NCH);
      b = test_node_open(mem_b, sizeof mem_b, NULL, st_on_message, st_on_event, bo, chb, ST_NCH);
      ST_CHECK(a && b, "scale: %u-topic nodes open", ST_NCH);
      if (a && b){
          st_any = 0;
          for (k=0;k<ST_NCH;k++) rant_node_send(a, k, payload, 16);
          { uint64_t end = i_rant_plat_now_us() + 5000000u;
            while (st_any < ST_NCH && i_rant_plat_now_us() < end) st_pump(a, b, 20); }
          ST_CHECK(st_any == ST_NCH, "scale: all topics delivered (%lu/%u)", st_any, ST_NCH);
          rant_node_close(b, 1);
          rant_node_close(a, 1);
      }
    }

    /* 7. NAMED: the cross peer identity is the topic name hash, independent of each node's
       local handle. A distinct name never cross wires and clean names raise no collision. */
    { static uint8_t mem_nw[1<<20], mem_nr[1<<20];
      RantTopicDef nw[1], nr[2];
      RantNodeOpts wo2, ro2; RantNode *w2, *r2;
      memset(nw,0,sizeof nw); memset(nr,0,sizeof nr);
      nw[0].name="robot/lidar"; nw[0].role=RANT_PUB_ONLY;
      nw[0].qos.reliability=RANT_RELIABLE; nw[0].qos.keep_last=1;
      nw[0].qos.catch_up=1; nw[0].qos.max_message_bytes=32; nw[0].qos.heartbeat_us=50000;
      nr[0]=nw[0]; nr[0].role=RANT_SUB_ONLY;     /* same name (index 0), other node */
      nr[1]=nw[0]; nr[1].name="sensors/imu"; nr[1].role=RANT_SUB_ONLY;     /* index 1 */
      wo2 = (RantNodeOpts){ .domain=ST_DOMAIN+2, .discovery={ .max_peers=4 } };
      ro2=wo2;
      st_samples[0]=st_samples[1]=0; st_collisions=0; st_last_sender[0]='\0';
      w2=test_node_open(mem_nw,sizeof mem_nw,"lidar-node",NULL,NULL,wo2,nw,1);
      r2=test_node_open(mem_nr,sizeof mem_nr,"reader-node",st_on_message,st_on_event,ro2,nr,2);
      ST_CHECK(w2 && r2, "named: nodes open");
      if (w2 && r2){
          uint64_t end = i_rant_plat_now_us() + 5000000u;
          while (st_samples[0]==0 && i_rant_plat_now_us()<end){
              rant_node_send(w2, 0, payload, 16); st_pump(w2,r2,20);
          }
          ST_CHECK(st_samples[0] > 0, "named: same name matches across nodes (%lu)", st_samples[0]);
          /* the node name is synced via discovery and surfaces as RantMsg.publisher_name */
          ST_CHECK(strcmp(st_last_sender, "lidar-node")==0,
                   "named: publisher_name carries the publisher's node name (%s)", st_last_sender);
          st_pump(w2,r2,200);
          ST_CHECK(st_samples[1] == 0, "named: distinct name never cross-wires (%lu)", st_samples[1]);
          ST_CHECK(st_collisions == 0, "named: clean names raise no collision (%lu)", st_collisions);
          rant_node_close(r2,1); rant_node_close(w2,1);
      }
    }

    /* 8. COLLISION: two names with the same 64 bit identity, found by Pollard's rho against
       FNV-1a. Rant must fire on_collision and refuse the match, never cross wire. */
    { static uint8_t mem_cw[1<<20], mem_cr[1<<20];
      const char *A="iuZA9tcJzAG", *B="5wVGxhTCmOC";   /* both hash to 23f58aa8628b1cce */
      RantTopicDef cw, cr; RantNodeOpts wo3, ro3; RantNode *w3, *r3;
      ST_CHECK(rant_topic_id(A)==rant_topic_id(B) && strcmp(A,B)!=0,
               "collision: test pair still shares one identity (else regen via collide)");
      memset(&cw,0,sizeof cw);
      cw.name=A; cw.role=RANT_PUB_ONLY;
      cw.qos.reliability=RANT_RELIABLE; cw.qos.keep_last=1; cw.qos.catch_up=1;
      cw.qos.max_message_bytes=32; cw.qos.heartbeat_us=50000;
      cr=cw; cr.name=B; cr.role=RANT_SUB_ONLY;
      wo3 = (RantNodeOpts){ .domain=ST_DOMAIN+3, .discovery={ .max_peers=4 } };
      ro3=wo3;
      st_samples[0]=0; st_collisions=0;
      w3=test_node_open(mem_cw,sizeof mem_cw,NULL,NULL,NULL,wo3,&cw,1);
      r3=test_node_open(mem_cr,sizeof mem_cr,NULL,st_on_message,st_on_event,ro3,&cr,1);
      ST_CHECK(w3 && r3, "collision: nodes open");
      if (w3 && r3){
          for (i=0;i<60;i++){ rant_node_send(w3,0,payload,16); rant_node_poll(w3,0); rant_node_poll(r3,20); }
          ST_CHECK(st_collisions >= 1, "collision: detected (RANT_E_NAME_COLLISION fired %lu)", st_collisions);
          ST_CHECK(st_samples[0] == 0, "collision: match refused, no cross-wire (%lu)", st_samples[0]);
          rant_node_close(r3,1); rant_node_close(w3,1);
      }
    }
    disc_core_checks();   /* 8. discovery-core peer lifecycle (sans-IO) */
    node_core_checks();   /* 8b. node-core peer table + lifecycle (sans-IO, no sockets) */
#ifdef RANT_SHM
    shm_module_checks();  /* 9.  SHM mapping module + seqlock guards          */
    shm_loss_checks();    /* 10. SHM loss/repair/skip (transport core)        */
    shm_node_checks();    /* 11. SHM full-node: size classes + inline fallback */
#endif
    unit_checks();        /* 12. pure-helper unit checks: clamp, result codes, byte packing */
    open_fail_checks();   /* 13. rant_node_open staged-cleanup (goto fail) paths               */
    event_user_checks();  /* 14. transport-fired event reaches on_event with the app user_data */
    dynamic_grow_checks();        /* 16. dynamic grow: relocate mid stream, lose nothing */
    qos_match_checks();           /* 17. QoS: a reliable sub refuses a best effort pub */
    beff_flow_checks();           /* 17b. a best effort reader stays out of flow control */
    rate_checks();                /* 17d. best effort rate throttle, no false loss */
    lapped_checks();              /* 17e. NACK merge, a lapped reader rejoins at the head */
    rtt_checks();                 /* 17f. per peer RTT: adaptive backstop and tail HB */
    ahead_checks();               /* 17g. one sample held ahead of the head while it repairs */
    schema_dsl_checks();          /* 17c. schema DSL: text == builder wire, layout, rejects */
    schema_advert_checks();       /* 18. the topic schema rides the announce */
    schema_bind_checks();         /* 19. a subset reader binds to the writer's layout */
    schema_bigenum_checks();      /* 19a2. a 1024 option enum round trips to a peer */
    schema_root_checks();         /* 19a3. primitive rooted schemas: bare types, hashes */
    schema_v8_checks();           /* 19a4. the schema wire: named types, arrays, print */
    stdtypes_checks();            /* 19a5. the standard type library: golden bytes, e2e */
    detail_codec_checks();        /* 19b. pairwise detail codec: responder, wire inlining, paging */
    detail_paging_checks();       /* 19b2. detail paging fits one datagram, never wedges */
    interest_codec_checks();      /* 19b3. interest paging codec + the external-overlay flag */
    interest_external_checks();   /* 19b4. external interest: bootstrap plus paged fetch */
    detail_live_checks();         /* 19c. 'uDTL' on the data socket: stateless reply to source */
    queue_checks();               /* 19d. consumer queues: take, dispatch, overwrite, park */
    callback_queue_checks();      /* 19e. callback queues: explicit queue, order, refusals */
    callback_pattern_checks();    /* 19f. callback queues on functions, tasks and variables */
    patterns_checks();            /* 19e. functions: request, reply, defer, timeout, sync */
    metalog_checks();             /* 19e1. built-in @rant/log topics + the @rant/meta endpoint */
    dup_authority_checks();       /* 19e2. duplicate provider or owner, both rivals */
    retire_checks();              /* 19e3. pattern retire: a successor binds, no shadow */
    reflect_dropped_checks();     /* 19e4. entity walk refuses dropped peers unless opted in */
    varwait_checks();             /* 19e5. accessor first write rides the match wait */
    churn_checks();               /* 19e6. retire/reuse churn soak: slots reuse, nothing balloons */
    task_checks();                /* 19e7. tasks: progress, cancel, no_cancel, bare return */
    taskx_checks();               /* 19e8. task matrix: demux, providers, loss, retire, churn */
    loud_checks();                /* 19e9. refused creates say why, twins, typed sends, force gate */
    matchwait_checks();           /* 19f. send-path match wait + writer-authoritative repair */
    relay_checks();               /* 19f2. unicast-only node relayed into the mesh by a peer */
    nat_checks();                 /* 19f2b. a unicast only node behind an outbound only NAT */
    selfip_checks();              /* 19f3. stating our own locator (self_ip / advertise_port) */
    ts_checks();                  /* 19g. the source timestamp: stamp, opt out, replay, queue */
#ifdef RANT_THREADS
    threaded_checks();            /* 20 to 24. service thread, flow control, unsent guard, waker */
#endif

    printf(st_fail ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return st_fail;
}

/* The sweep: spawns N node children per rate with stdout to a temp file, waits for their
 * exit, and aggregates the SUMMARY lines into a table. Pure C, Windows and POSIX. */

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

/* wait for the children to self exit, pumping the control node so the control plane
 * stays live, and kill stragglers at the deadline. cmd is re published every 2 s. */
static void sw_wait_pump(sw_child *cs, int n, int timeout_ms, RantNode *ctl, const char *cmd){
    uint64_t deadline = now_ns() + (uint64_t)timeout_ms*1000000ull;
    uint64_t next_cmd = 0;
    int left = 0, i;
    for (i=0;i<n;i++) if (!cs[i].reaped) left++;
    while (left > 0 && now_ns() < deadline){
        if (ctl) rant_node_poll(ctl, 20); else sw_sleep_ms(5);
        if (ctl && cmd && now_ns() >= next_cmd){
            rant_node_send(ctl, CTL_CMD, cmd, strlen(cmd));
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

/* The control plane for two machine sweeps over Rant itself: serve workers spawn the
 * same node children and publish their SUMMARY lines back (spec/testing.md). */
static char g_ctl_cmd[512];
static int  g_ctl_cmd_new = 0;
static char g_ctl_res[SW_MAX_RESULTS][CTL_RES_MAX];
static int  g_ctl_res_n = 0, g_ctl_done = 0, g_ctl_res_drop = 0;
static int  g_ctl_dom_filter = -1;   /* accept RESULTs for this domain only */
static char g_ctl_workers[16][24];
static int  g_ctl_nworkers = 0;

static void ctl_on_message(const RantMsg *msg){
    uint16_t ch = msg->topic_index; const void *d = msg->data.data; size_t len = msg->data.len;
    if (ch==CTL_CMD && len < sizeof g_ctl_cmd){
        memcpy(g_ctl_cmd, d, len); g_ctl_cmd[len]=0; g_ctl_cmd_new=1;
    } else if (ch==CTL_RES){
        /* every control peer flap replays the worker's whole RES history by design. Old rate
           replays must not eat inbox slots, so filter by the rate's domain up front. */
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

static RantNode *ctl_open(uint16_t domain, int coordinator,
                         const char *if_ip, const char *peer_ip){
    static uint8_t mem[8<<20];
    static RantTopicDef ch[2];
    static RantDiscoveryAddr seed;
    RantNodeOpts opts;
    memset(ch, 0, sizeof ch);
    ch[0].name                  = "ctl/cmd";
    ch[0].qos.reliability       = RANT_RELIABLE;
    ch[0].qos.keep_last         = 8;
    ch[0].qos.max_message_bytes = sizeof g_ctl_cmd;
    ch[0].role = coordinator ? RANT_PUB_ONLY : RANT_SUB_ONLY;
    ch[1] = ch[0];
    ch[1].name                  = "ctl/res";
    ch[1].qos.keep_last         = SW_MAX_RESULTS;
    ch[1].qos.catch_up          = SW_MAX_RESULTS;
    ch[1].qos.max_message_bytes = CTL_RES_MAX;
    ch[1].role = coordinator ? RANT_SUB_ONLY : RANT_PUB_ONLY;
    opts = (RantNodeOpts){
        .domain     = domain,
        .net  = { .multicast_interface = if_ip },     /* pin on multihomed hosts */
        .discovery = { .announce_interval_us = 500000,   /* ride through data floods */
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

/* the IP of the first control peer, for seeding the test children. NULL if none yet */
static const char *ctl_peer_ip(RantNode *ctl, char out[20]){
    uint16_t i;
    for (i=0;i<i_rant_node_core_max_peers(ctl->core);i++){
        uint8_t pip[16], pil;
        if (i_rant_node_core_peer_at(ctl->core, i, NULL, pip, &pil, NULL) && pil==4){
            snprintf(out, 20, "%u.%u.%u.%u", pip[0], pip[1], pip[2], pip[3]);
            return out;
        }
    }
    return NULL;
}

/* log control-peer transitions: which side lost whom, and when, is the first
 * question in any two-machine debugging session */
static void ctl_watch(const char *who, RantNode *ctl){
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

/* consume new inbox entries: HELLOs grow the worker set, RESULTs matching (domain, run)
 * parse into sum and sum2 at *count. Returns the results consumed. */
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
    RantNode *ctl;
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
    rant_node_send(ctl, CTL_RES, hello, strlen(hello));
    printf("serve: worker %s on control domain %u, waiting for sweeps\n", prefix, dom);

    { unsigned long last_run = (unsigned long)-1;
    for (;;){
        sw_kv c; unsigned long run;
        int domain, dur, mcast, rel, blk, xch, spread, nodes; long rate;
        rant_node_poll(ctl, 100);
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
              snprintf(cs[i].out, sizeof cs[i].out, "%srantw_%lu_n%d.txt", tmpdir, mypid, i);
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
                rant_node_send(ctl, CTL_RES, res, strlen(res));
            remove(cs[i].out);
        }
        { uint64_t end = now_ns()+1500000000ull;     /* flush + repair window */
          while (now_ns() < end) rant_node_poll(ctl, 20); }
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
    int remote=0; uint16_t ctl_dom=CTL_DOMAIN; RantNode *ctl=NULL;
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
          while (now_ns() < end){ rant_node_poll(ctl, 50); ctl_drain(-1, 0, NULL, NULL, NULL); } }
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
            rant_node_send(ctl, CTL_CMD, cmd, strlen(cmd));
            for (i=0;i<10;i++) rant_node_poll(ctl, 1);     /* push it out now */
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
                rant_node_poll(ctl, 50);
                ctl_watch("sweep", ctl);
                if (now_ns() >= next_cmd){       /* keep recovered workers in sync */
                    rant_node_send(ctl, CTL_CMD, cmd, strlen(cmd));
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
/* memscale: one reliable publisher and N subscribers on loopback over a grid of payload
 * size, keep_last and subscriber count. Steady state must be alloc free (spec/testing.md). */
static int g_ms_rx;
static void ms_on_message(const RantMsg *m){ (void)m; g_ms_rx++; }

static unsigned ms_domain = 200;
/* sum heap (re)alloc counts across the publisher and every subscriber */
static uint64_t ms_allocs(RantNode *P, RantNode **S, int nsubs){
    uint64_t a, sum=0; int i; rant_node_mem_stats(P,NULL,NULL,&a); sum=a;
    for (i=0;i<nsubs;i++){ rant_node_mem_stats(S[i],NULL,NULL,&a); sum+=a; }
    return sum;
}
static void ms_run(size_t plen, uint16_t keep, int nsubs, int disable_shm){
    RantAllocator pa, sa[16]; RantNodeOpts po, so; RantNode *P=NULL, *S[16];
    RantTopic *pc=NULL; RantTopicOpts co; RantDiscoveryAddr seed;
    uint8_t *payload; int i, j, k, t, nwarm, nsteady; unsigned dom = ms_domain++;
    uint64_t a0=0, a1=0, a2=0; size_t ppeak=0, speak=0;
    double cold_sum=0, warm_sum=0, t0, t1, msg_s; int coldc=0;
    if (nsubs>16) nsubs=16;
    payload=(uint8_t*)malloc(plen?plen:1); if(!payload) return; memset(payload,0x5A,plen?plen:1);
    nsteady = plen<=1024 ? 300 : plen<=65536 ? 100 : 20;
    nwarm   = keep + 4;
    memset(&co,0,sizeof co); co.qos.reliability=RANT_RELIABLE; co.qos.keep_last=keep; co.qos.heartbeat_us=50000;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=(uint16_t)dom; po.max_topics=4;
    po.discovery.max_peers=(uint16_t)(nsubs+2); po.disable_shm=(uint8_t)disable_shm;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po;
    pa=rant_allocator_heap(0);
    P=rant_node_open(&pa,"ms-pub",NULL,NULL,&po);
    for (i=0;i<nsubs;i++){ sa[i]=rant_allocator_heap(0); S[i]=rant_node_open(&sa[i],"ms-sub",ms_on_message,NULL,&so); }
    if (!P){ free(payload); return; }
    pc=rant_node_create_topic(P,"ms/ch",RANT_PUB_ONLY,NULL,&co);
    for (i=0;i<nsubs;i++) rant_node_create_topic(S[i],"ms/ch",RANT_SUB_ONLY,NULL,&co);
    for (t=0;t<4000 && rant_topic_match_count(pc)<nsubs;t++){ rant_node_poll(P,1); for(j=0;j<nsubs;j++) rant_node_poll(S[j],1); }

    a0=ms_allocs(P,S,nsubs);                              /* total allocs before any traffic */
    g_ms_rx=0;                                            /* warmup: fill + size the buffers */
    for (i=0;i<nwarm;i++){
        double s0=(double)i_rant_plat_now_us(); rant_topic_send(pc,rant_bytes(payload,plen), NULL); double s1=(double)i_rant_plat_now_us();
        if (i<keep){ cold_sum += s1-s0; coldc++; }
        for (k=0;k<400000 && g_ms_rx < (i+1)*nsubs;k++){ rant_node_poll(P,0); for(j=0;j<nsubs;j++) rant_node_poll(S[j],0); }
    }
    a1=ms_allocs(P,S,nsubs);                              /* allocs after warmup */
    g_ms_rx=0; t0=(double)i_rant_plat_now_us();     /* steady: the buffers are sized, so no alloc */
    for (i=0;i<nsteady;i++){
        double s0=(double)i_rant_plat_now_us(); rant_topic_send(pc,rant_bytes(payload,plen), NULL); double s1=(double)i_rant_plat_now_us();
        warm_sum += s1-s0;
        for (k=0;k<400000 && g_ms_rx < (i+1)*nsubs;k++){ rant_node_poll(P,0); for(j=0;j<nsubs;j++) rant_node_poll(S[j],0); }
    }
    t1=(double)i_rant_plat_now_us();
    a2=ms_allocs(P,S,nsubs);
    rant_node_mem_stats(P,NULL,&ppeak,NULL); rant_node_mem_stats(S[0],NULL,&speak,NULL);
    msg_s = (t1>t0) ? nsteady / ((t1-t0)/1e6) : 0;
    printf("%-9lu %-5u %-5d %11.1f %11.1f %11llu %13llu %10.0f %9.2f %9.2f\n",
        (unsigned long)plen, keep, nsubs, ppeak/1024.0, speak/1024.0,
        (unsigned long long)(a1-a0), (unsigned long long)(a2-a1),
        msg_s, coldc?cold_sum/coldc:0.0, nsteady?warm_sum/nsteady:0.0);
    rant_node_close(P,0); for(i=0;i<nsubs;i++) rant_node_close(S[i],0);
    free(payload);
}
static void ms_grid(int disable_shm){
    printf("\n--- %s path ---\n", disable_shm ? "UDP (cross-host; writer/reader buffers on the heap)"
                                              : "SHM (same-host; payload in mmap'd segments)");
    printf("%-9s %-5s %-5s %11s %11s %11s %13s %10s %9s %9s\n",
        "payload","keep","subs","pub_peak_kB","sub_peak_kB","warm_alloc","steady_alloc","msg/s","cold_us","warm_us");
    ms_run(64,8,1,disable_shm); ms_run(1024,8,1,disable_shm);
    ms_run(65536,8,1,disable_shm); ms_run(1048576,8,1,disable_shm);              /* payload sweep */
    ms_run(1024,1,1,disable_shm); ms_run(1024,16,1,disable_shm); ms_run(1024,256,1,disable_shm);
    ms_run(1024,8,4,disable_shm); ms_run(1024,8,16,disable_shm);   /* subscriber sweep */
}
static int memscale_main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    printf("Rant dynamic-allocator scale test (in-process, reliable, loopback)\n");
    printf("peak = live message-buffer bytes; warm/steady_alloc = heap (re)allocs (pub+subs); cold/warm_us = send-call time\n");
    ms_grid(1);   /* the allocation-relevant path */
    ms_grid(0);   /* same-host fast path, for comparison */
    return 0;
}

/* threadbench: the threaded send path cost and the drain backpressure, one started pub
 * node, one started sub node and this sender thread per target rate (spec/testing.md). */
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
    co.qos.keep_last = 64;               /* best-effort: isolates the unsent drain guard */
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

    t0 = i_rant_plat_now_us();
    t_end = t0 + (uint64_t)(dur_s * 1e6);
    if (rate_hz) next = t0;
    while (i_rant_plat_now_us() < t_end){
        if (rate_hz){
            uint64_t now = i_rant_plat_now_us();
            if (now < next) continue;                /* busy-wait pacing */
            next += 1000000u / rate_hz;
            if (next < now) next = now;              /* fell behind: no burst catch-up */
        }
        rant_topic_send(ch, rant_bytes(payload, sizeof payload), NULL);
        sent++;
    }
    wall = (i_rant_plat_now_us() - t0) / 1e6;

    sw_sleep_ms(300);                                /* let deliveries settle */
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
    { uint64_t end = i_rant_plat_now_us() + 5000000u;
      while (rant_topic_match_count(cw) == 0 && i_rant_plat_now_us() < end) sw_sleep_ms(2); }
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

static int threadbench_main(void){
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Rant threaded send-path bench (32B best-effort, keep_last 64, loopback, one sender)\n"
           "waited = sends that slept in the drain/backpressure wait; wait%% = of sender wall time\n");
    if (tb_mode("threaded, UDP loopback (shm off)", 1)) return 1;
#ifdef RANT_SHM
    if (tb_mode("threaded, SHM same-host path", 0)) return 1;
#endif

    /* single-threaded flat-out baseline: the naive send + poll(0) + poll(0) loop */
    {
        RantNode *w = tb_open("tb-base-pub", 0, 1);
        RantNode *r = tb_open("tb-base-sub", 1, 1);
        RantTopic *cw = tb_channel(w, 0), *cr = tb_channel(r, 1);
        uint8_t payload[32]; unsigned long sent = 0, recv0;
        uint64_t t0, t_end; double wall;
        (void)cr;
        if (!w || !r || !cw){ fprintf(stderr, "threadbench: baseline open failed\n"); return 1; }
        memset(payload, 0x42, sizeof payload);
        { uint64_t end = i_rant_plat_now_us() + 5000000u;
          while (rant_topic_match_count(cw) == 0 && i_rant_plat_now_us() < end){
              rant_node_poll(w, 1); rant_node_poll(r, 0);
          } }
        recv0 = g_tb_recv;
        t0 = i_rant_plat_now_us(); t_end = t0 + 2000000u;
        while (i_rant_plat_now_us() < t_end){
            rant_topic_send(cw, rant_bytes(payload, sizeof payload), NULL);
            rant_node_poll(w, 0);
            rant_node_poll(r, 0);
            sent++;
        }
        wall = (i_rant_plat_now_us() - t0) / 1e6;
        { uint64_t settle = i_rant_plat_now_us() + 300000u;
          while (i_rant_plat_now_us() < settle){ rant_node_poll(w, 0); rant_node_poll(r, 0); } }
        printf("\n== single-threaded baseline: send+poll(0)+poll(0) flat-out, UDP ==\n");
        printf("  %10.0f send/s   delivered %.2f%%\n",
               sent / wall, sent ? 100.0 * (double)(g_tb_recv - recv0) / sent : 0.0);
        rant_node_close(r, 1);
        rant_node_close(w, 1);
    }
    return 0;
}

/* queuebench: the consumer queue cost against inline callbacks, one started pair on
 * loopback per payload size and mode, flat out reliable (spec/testing.md). */
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
    RantNode *w, *r; RantTopic *cw, *cr;
    i_RantThread th;
    uint64_t t0, t_end;
    unsigned long recv = 0;
    double wall, rate;
    uint32_t shm_rx0 = 0, shm_rx1 = 0;

    memset(&o, 0, sizeof o);
    o.domain = 52; o.disable_shm = (uint8_t)disable_shm;
    o.net.multicast_interface = "127.0.0.1";
    w = rant_node_open(&aw, "qb-pub", NULL, NULL, &o);
    r = rant_node_open(&ar, "qb-sub", queued ? NULL : qb_on_message, NULL, &o);
    if (!w || !r){ fprintf(stderr, "queuebench: open failed\n"); exit(1); }
    memset(&co, 0, sizeof co);
    co.qos.reliability = RANT_RELIABLE;
    co.qos.keep_last = (uint16_t)(size >= 262144u ? 8 : 64);   /* shallow history when huge */
    co.qos.backpressure_wait_us = 200000;   /* lossless: the sender paces to the consumer */
    co.qos.heartbeat_us = 20000;
    co.qos.repair_delay_us = 5000;
    co.qos.shm_max_bytes = size;            /* pin one SHM size class per run */
    cw = rant_node_create_topic(w, "qb/t", RANT_PUB_ONLY, NULL, &co);
    /* the queue must cover the writer's in flight burst, else the reader parks and the
       overrun heals through the paced repair path. Twice the burst, at least 4 MB. */
    {   uint32_t qb = 2u * co.qos.keep_last * size;
        if (qb < (4u << 20)) qb = 4u << 20;
        co.qos.queue_bytes = queued ? qb : 0;
    }
    cr = rant_node_create_topic(r, "qb/t", RANT_SUB_ONLY, NULL, &co);
    if (!cw || !cr){ fprintf(stderr, "queuebench: topic create failed\n"); exit(1); }
    rant_node_start(w); rant_node_start(r);
    {   uint64_t end = i_rant_plat_now_us() + 5000000u;
        while (rant_topic_match_count(cw) == 0 && i_rant_plat_now_us() < end) sw_sleep_ms(2); }
    if (rant_topic_match_count(cw) != 1){ fprintf(stderr, "queuebench: no match\n"); exit(1); }

#ifdef RANT_SHM
    rant_node_shm_stats(r, NULL, &shm_rx0);
#endif
    g_qb_len = size; g_qb_stop = 0; g_qb_recv = 0;
    if (!i_rant_plat_thread_start(&th, qb_sender, cw)){ fprintf(stderr, "queuebench: thread\n"); exit(1); }
    t0 = i_rant_plat_now_us(); t_end = t0 + 2000000u;
    if (queued){
        RantMsg m;
        while (i_rant_plat_now_us() < t_end)
            if (rant_topic_take(cr, &m, 5) == 1) recv++;
    } else {
        while (i_rant_plat_now_us() < t_end) sw_sleep_ms(5);
        recv = g_qb_recv;
    }
    wall = (double)(i_rant_plat_now_us() - t0) / 1e6;
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

static int queuebench_main(void){
    static const uint32_t sizes[] = { 1024, 16384, 65536, 262144, 1048576 };
    unsigned k;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Rant consumer-queue throughput: inline callback (service thread) vs queued take\n"
           "(flat-out reliable, keep_last 64, backpressure-paced = lossless goodput, loopback, 2s/run)\n");
#ifdef RANT_SHM
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
#endif /* RANT_THREADS */

int main(int argc, char **argv){
    if (argc >= 2 && strcmp(argv[1], "memscale") == 0)
        return memscale_main();
    if (argc >= 2 && strcmp(argv[1], "queuebench") == 0){
#ifdef RANT_THREADS
        return queuebench_main();
#else
        fprintf(stderr, "queuebench needs threads (RANT_THREADS off)\n");
        return 1;
#endif
    }
    if (argc >= 2 && strcmp(argv[1], "threadbench") == 0){
#ifdef RANT_THREADS
        return threadbench_main();
#else
        fprintf(stderr, "threadbench needs threads (RANT_THREADS off: undetected platform or RANT_NO_THREADS)\n");
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
        "usage: rant_test <command> [args]\n"
        "  node <name> [domain] [load_hz] [duration_s] [if_mode] [reliable] [block_ms]\n"
        "       [extra_ch] [spread] [if_ip] [peer_ip]\n"
        "        if_ip: pin the discovery interface (multihomed hosts);\n"
        "        peer_ip: seed discovery with this address (no multicast needed);\n"
        "        \"0\" = unset for either\n"
        "        latency/throughput node; SUMMARY+SUMMARY2 on timed exit. Data is unicast.\n"
        "        reliable=1: load topic RANT_RELIABLE; block_ms: writer\n"
        "        backpressure window (qos.backpressure_wait_us) for that topic\n"
        "        extra_ch: declare N more topics; spread=1 round-robins the\n"
        "        load across them (0 = they stay idle, 2 = they are PUB_ONLY\n"
        "        everywhere so those writes have no readers)\n"
        "        if_mode: discovery interface -- 1 = loopback (single host), 2 = real NIC\n"
        "        env: RANT_DIAG_RCVBUF/RANT_DIAG_SNDBUF (bytes), RANT_DIAG_TRACE\n"
        "  sweep [--nodes N] [--domain D] [--duration S] [--rates a,b,c]\n"
        "        [--mcast 0|1|2] [--reliable] [--block-ms N] [--extra-ch N] [--spread]\n"
        "        [--void] [--remote] [--ctl-domain D] [--if IP] [--peer IP] [--diag]\n"
        "        spawn N node children per rate; print an RTT-vs-throughput table.\n"
        "        --mcast selects the discovery interface (1 pins loopback to isolate the\n"
        "        run from the LAN, 2 uses every real interface; data is unicast).\n"
        "        --remote also commands every 'serve' worker on the LAN to spawn N\n"
        "        nodes per rate and folds their SUMMARYs into the same table.\n"
        "        --if pins discovery to ONE interface (rarely needed: the default\n"
        "        joins and announces on all of them);\n"
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
