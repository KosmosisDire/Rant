/* tiny pub/sub command-line tool over a DART node.

POSIX  : cc  -std=c99 -Wall -Idist tools/pubsub.c -o pubsub -lpthread -lrt
Windows: gcc -std=c99 -Wall -Idist tools/pubsub.c -o pubsub.exe -lws2_32 -lbcrypt -lwinmm

Same-host pub/sub goes over shared memory automatically (no extra flags): SHM is on
by default; the dynamic mode (no --max) provides the allocator it needs. -lrt is for
shm_open on Linux (drop it on macOS/BSD, or build -DDART_NO_SHM).
 */

#define DART_IMPLEMENTATION
#include "dart.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* The public node API is handle-based (dart_node_create_channel -> DartChannel*,
 * dart_channel_send, ...). This tool creates its channels in index order, so these
 * shims keep the concise (node, channel-index) call form: dart_node_channel(n, i)
 * maps a creation index back to its handle. */
#define dart_node_send(n, idx, d, l)         dart_channel_send(dart_node_channel((n),(idx)), (d), (l))
#define dart_node_drain(n, idx, ms)          dart_channel_drain(dart_node_channel((n),(idx)), (ms))
#define dart_node_writer_match_count(n, idx) dart_channel_match_count(dart_node_channel((n),(idx)))
#define dart_node_repair_stats(n, idx, o)    dart_channel_repair_stats(dart_node_channel((n),(idx)), (o))
#define dart_node_reader_progress(n, idx, p, b, h, t) \
        dart_channel_reader_progress(dart_node_channel((n),(idx)), (p),(b),(h),(t))

/* Minimal thread + lock shim. A DART node only does discovery, liveness, and
 * reliable repair inside dart_node_poll, so the interactive publisher must keep
 * polling while it blocks on stdin: otherwise announces stop, peers time out
 * (default 3.5s), and slowly-typed lines flap the peer and drop. We poll on a
 * background thread and serialize every dart_node_* call with one lock. */
#ifdef _WIN32
  #include <windows.h>
  typedef CRITICAL_SECTION lock_t;
  static void lock_init(lock_t *mutex){ InitializeCriticalSection(mutex); }
  static void lock_get (lock_t *mutex){ EnterCriticalSection(mutex); }
  static void lock_put (lock_t *mutex){ LeaveCriticalSection(mutex); }
  static void sleep_ms (int ms){ Sleep((DWORD)ms); }
#else
  #include <pthread.h>
  #include <time.h>
  typedef pthread_mutex_t lock_t;
  static void lock_init(lock_t *mutex){ pthread_mutex_init(mutex, NULL); }
  static void lock_get (lock_t *mutex){ pthread_mutex_lock(mutex); }
  static void lock_put (lock_t *mutex){ pthread_mutex_unlock(mutex); }
  static void sleep_ms (int ms){ struct timespec ts; ts.tv_sec = ms/1000;
                                 ts.tv_nsec = (long)(ms%1000)*1000000L; nanosleep(&ts, NULL); }
#endif

#ifdef _WIN32
  #include <io.h>
  static int stdin_is_tty(void){ return _isatty(_fileno(stdin)); }
  static void file_truncate(FILE *file, long n){ _chsize(_fileno(file), n); }
#else
  #include <unistd.h>
  static int stdin_is_tty(void){ return isatty(fileno(stdin)); }
  static void file_truncate(FILE *file, long n){ if (ftruncate(fileno(file), n)){} }
#endif

/* milliseconds since process start, for discovery-timing logs */
static long g_start_ms;
static long now_ms(void){
#ifdef _WIN32
    return (long)GetTickCount();
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec*1000 + ts.tv_nsec/1000000);
#endif
}

static DartNode  *g_node;
static lock_t      g_lock;
static volatile int g_pumping = 1;
static FILE        *g_outfile = NULL; /* sub --file: received messages saved here */
static int         g_rate_mode = 0;   /* sub --rate: report measured rate, no per-msg lines */
static unsigned long      g_rx_msgs  = 0;  /* messages received since the last rate report */
static unsigned long long g_rx_bytes = 0;  /* bytes received since the last rate report */
static unsigned long long g_rx_total = 0;  /* cumulative messages delivered to on_message */
static unsigned long long g_lost     = 0;  /* cumulative messages the transport reported lost */
static uint32_t           g_last_peer = 0; /* most recent sender: the peer we snapshot HOL progress for */
#define MAX_TOPICS 64
static const char *g_topics[MAX_TOPICS];   /* channel handle -> topic name, for the receive print */
static int         g_n_topics = 0;

#ifdef _WIN32
static DWORD WINAPI pump_thread(LPVOID arg){
#else
static void *pump_thread(void *arg){
#endif
    (void)arg;
    while (g_pumping){
        lock_get(&g_lock); dart_node_poll(g_node, 0); lock_put(&g_lock);
        sleep_ms(5);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static int parse_ipv4(const char *s, uint8_t out[4]){
    int i;
    for (i = 0; i < 4; i++){
        char *end;
        long byte_val = strtol(s, &end, 10);
        if (end == s || byte_val < 0 || byte_val > 255) return -1;
        if (i < 3 && *end != '.') return -1;
        if (i == 3 && *end != '\0') return -1;
        out[i] = (uint8_t)byte_val;
        s = end + 1;
    }
    return 0;
}

/* in-pump probe: the per-second publisher loop is BLOCKED inside dart_node_send for
 * the whole backpressure wait, so it can't print within a stall. The node calls this
 * on a ~200ms timer DURING that wait, giving a within-block time series. resent/nacks
 * are normalised to /s by the sample interval. The key discriminator: resent bursty-
 * then-flat (with idle ~= polls) => the reader stopped asking [self-quench]; resent
 * steady while the reader receives nothing => resends are being dropped. To stderr so
 * it never interleaves with the stdout rate lines. */
static void pump_probe(void *u, const DartPumpSample *s){
    double secs = s->interval_us/1e6;
    (void)u;
    fprintf(stderr, "[pub]   in-pump @%.2fs: resent %.0f/s  nacks %.0f/s  idle %u/%u polls\n",
            s->wait_elapsed_us/1e6,
            secs>0 ? s->frags_resent/secs : 0.0,
            secs>0 ? s->nacks_recv/secs   : 0.0,
            s->polls_idle, s->polls);
}

/* "4M" / "512k" / "1048576" -> bytes (binary K/M/G suffix). 0 on garbage. */
static size_t parse_size(const char *s){
    char *end; double value = strtod(s, &end), mult = 1.0;
    if      (*end=='k'||*end=='K') mult = 1024.0;
    else if (*end=='m'||*end=='M') mult = 1024.0*1024.0;
    else if (*end=='g'||*end=='G') mult = 1024.0*1024.0*1024.0;
    return (value > 0) ? (size_t)(value*mult) : 0;
}

static void on_message(const DartMsg *msg){
    const char *topic = msg->channel_name ? msg->channel_name : "?";
    uint32_t from = msg->sender_id; const void *data = msg->data; size_t len = msg->len;
    g_rx_msgs++; g_rx_bytes += len; g_rx_total++;   /* accounting for sub --rate (the loop prints it) */
    g_last_peer = from;                              /* the loop snapshots this writer's HOL progress */
    if (g_outfile){                       /* --file: each message OVERWRITES the file */
        rewind(g_outfile);                /* back to the start, not appending */
        fwrite(data, 1, len, g_outfile);
        fflush(g_outfile);                /* flush before truncating a longer prior message */
        file_truncate(g_outfile, (long)len);
        if (!g_rate_mode)
            printf("[%s <- peer %u] wrote %lu bytes\n", topic, from, (unsigned long)len);
    } else if (!g_rate_mode){             /* --rate: no per-msg line, the loop shows the rate */
        printf("[%s <- peer %u] %.*s\n", topic, from, (int)len, (const char*)data);
    }
}

static void on_event(const DartEvent *ev)
{
    char line[160];
    printf("  <event> %s\n", dart_event_str(ev, line, sizeof line));
}

static void usage(void){
    fprintf(stderr,
        "usage:\n"
        "  pubsub sub <topic> [<topic>...] [opts]   (subscribe to one or more topics)\n"
        "  pubsub pub <topic> [text...] [opts]      (no text = read lines from stdin)\n"
        "opts: --domain N  --mcast  --if <ip>  --peer <ip>  --best-effort  --wait MS  --file <name>  --max <size>\n"
        "      --rate HZ   (pub: repeat the text/--file payload at HZ; sub: bare --rate prints the measured receive rate)\n"
        "      --frag N    (UDP fragment payload bytes this node sends; advertised to peers. Build with -DDART_FRAG_PAYLOAD_MAX>=N)\n");
}

/* Allocator for DART's dynamic message buffers (used when no --max is set).
 * realloc semantics: size 0 frees. */
static void *pubsub_realloc(void *u, void *ptr, size_t size){
    (void)u;
    if (size == 0){ free(ptr); return NULL; }
    return realloc(ptr, size);
}

/* Wait for subscribers to match this channel, pumping the node throughout, so a
 * one-shot publisher never fires into the void. Returns once >=1 subscriber has
 * matched AND the count has stopped growing for a short quiet window -- so peers
 * discovered around the same time are all captured, not just the first -- or 0 on
 * timeout with none. (A late joiner that appears after the window still misses a
 * one-shot send: the publisher can't know how many to expect; use --subscribers
 * if you need a specific count, or a long-lived publisher for late joiners.) */
static int wait_for_sub(DartNode *n, uint16_t channel, int timeout_ms){
    int t, count = 0, stable = 0, printed_wait = 0;
    /* Short window: just longer than the startup-solicit reply jitter (~20ms),
       so a burst of already-up subscribers is captured, but a lone subscriber --
       the common case -- isn't made to wait for a second that will never come. */
    const int QUIET_WINDOW_MS = 60;
    for (t = 0; t < timeout_ms; t += 20){
        int match_count = dart_node_writer_match_count(n, channel);
        if (match_count > count){ count = match_count; stable = 0; }              /* a new sub: keep waiting */
        else if (count > 0 && (stable += 20) >= QUIET_WINDOW_MS) return 1;
        if (!printed_wait && count == 0 && t >= 400){ printf("[pub] waiting for a subscriber...\n"); printed_wait = 1; }
        dart_node_poll(n, 20);
    }
    return dart_node_writer_match_count(n, channel) > 0;
}

/* Read all of file into a freshly malloc'd buffer (caller frees), growing up to
 * cap bytes. Returns NULL and sets *too_big=1 if the input would exceed cap (we
 * refuse rather than split), or *too_big=0 on out-of-memory. *out_len gets the
 * byte count on success. */
static char *read_all(FILE *file, size_t cap, size_t *out_len, int *too_big){
    size_t buf_cap = 65536, len = 0, n_read;
    char *buf, *new_buf;
    *too_big = 0;
    if (buf_cap > cap) buf_cap = cap;
    buf = (char*)malloc(buf_cap);
    if (!buf) return NULL;
    for (;;){
        if (len == buf_cap){
            if (buf_cap >= cap){                       /* at the limit: more left? */
                if (fgetc(file) != EOF){ *too_big = 1; free(buf); return NULL; }
                break;                                /* exactly cap, at EOF */
            }
            { size_t want = buf_cap*2; if (want > cap) want = cap;
              new_buf = (char*)realloc(buf, want);
              if (!new_buf){ free(buf); return NULL; }
              buf = new_buf; buf_cap = want; }
        }
        n_read = fread(buf + len, 1, buf_cap - len, file);
        if (n_read == 0) break;                            /* EOF */
        len += n_read;
    }
    *out_len = len;
    return buf;
}

/* Publish the whole byte stream from file as ONE message (the transport fragments
 * + reassembles it). Refuses input larger than cap rather than splitting.
 * Settles first so a freshly discovered subscriber is matched, then drains
 * delivery before returning. */
static int publish_stream(DartNode *n, uint16_t channel, FILE *file, int wait_ms, size_t cap){
    size_t len = 0; int too_big, t;
    char *buf;
    if (!wait_for_sub(n, channel, wait_ms)){
        fprintf(stderr, "[pub] no subscriber matched in %dms; nothing sent\n", wait_ms);
        return -1;
    }
    buf = read_all(file, cap, &len, &too_big);
    if (!buf){
        if (too_big) fprintf(stderr, "[pub] input exceeds the %lu-byte message limit; raise --max\n",
                          (unsigned long)cap);
        else      fprintf(stderr, "out of memory\n");
        return -1;
    }
    if (dart_node_send(n, channel, buf, len) < 0){ fprintf(stderr, "send failed\n"); free(buf); return -1; }
    printf("[pub] sent %lu bytes\n", (unsigned long)len);
    /* Wait until the receiver has acked every byte before returning (the caller
       then sends BYE). A fixed grace would cut off a large or slow transfer:
       with the BYE the reader drops the peer and abandons in-flight data. */
    if (!dart_node_drain(n, channel, 30000))
        fprintf(stderr, "[pub] warning: delivery incomplete (reader slow or gone)\n");
    for (t = 0; t < 200; t += 20) dart_node_poll(n, 20);   /* brief flush (best-effort) */
    free(buf);
    return 0;
}

/* --rate: repeat the same payload at hz until interrupted (Ctrl-C). A long-
 * lived publisher, so we settle for the first subscriber but publish even if
 * none showed: late joiners are caught by discovery + KEEP_LAST history.
 *
 * Paced on dart_plat_now_us. We fire at most ONE message per loop and re-read the
 * clock right after the send, because dart_node_send blocks here while reliable
 * backpressure waits on a slow reader -- a blocking send can consume seconds. The
 * old loop sampled the clock once and fired "every due tick" in an inner burst, so
 * a backpressure-blocked stretch came back owing dozens of ticks and (a) bursted
 * them into the 4-deep history and (b) made the once-a-second print fire every few
 * REAL seconds while labelling the accumulated count as "/s" -- which is what made
 * a true ~6 msg/s look like "64/s". One-send-per-iteration + bounded catch-up
 * (recover sub-RESYNC_LAG_US jitter so the target rate holds, but resync past a long
 * block so we never burst a backlog) + a wall-clock-normalised rate readout fix both.
 * The unbounded "drop every missed tick" rebase this replaced lost ~(lag*hz) messages
 * for EVERY jitter event -- the once-a-second status print alone capped 2000 Hz at
 * ~1998 -- because a tick was only ever dropped, never made up. When the bottleneck
 * is bandwidth there is nothing to "catch up" anyway: the wire is already full and a
 * long block resyncs, so extra sends would only be backpressured or evicted.
 * Never returns. */
static void publish_rate(DartNode *n, uint16_t channel, const void *data, size_t len,
                         double hz, int wait_ms){
    uint64_t period_us = (uint64_t)(1000000.0/hz + 0.5);
    uint64_t next, now, last_print;
    unsigned long sent = 0, last_sent = 0;
    uint64_t backpressure_us = 0; uint32_t backpressure_waits = 0, last_backpressure_waits = 0;   /* backpressure engagement */
    DartRepairStats repair_stats, prev_repair_stats;                     /* reliable-repair throughput (writer side) */
    memset(&prev_repair_stats, 0, sizeof prev_repair_stats);
    if (period_us == 0) period_us = 1;                 /* clamp absurd rates to ~1 MHz */
    /* Lag below this is jitter (the status print, a scheduler preempt): we leave the
       grid in the past and fire the missed ticks back-to-back over the next few spin
       iterations, so the rate holds. Lag above it is a real reliable-backpressure
       block (seconds): resync instead, so a multi-second stall never bursts a backlog
       into the 4-deep history. Sits well above expected jitter (~1-10ms) and well
       below a backpressure block. */
    const uint64_t RESYNC_LAG_US = 50000;              /* 50 ms */
    if (!wait_for_sub(n, channel, wait_ms))
        fprintf(stderr, "[pub] no subscriber yet; publishing anyway (late joiners catch up)\n");
    printf("[pub] publishing %lu bytes at %g Hz (Ctrl-C to stop)...\n", (unsigned long)len, hz);
    now = dart_plat_now_us(); next = now; last_print = now;
    for (;;){
        now = dart_plat_now_us();
        if (now >= next){
            if (dart_node_send(n, channel, data, len) >= 0) sent++;  /* may block in backpressure */
            next += period_us;
            now = dart_plat_now_us();                  /* a blocking send moved the clock */
            if (next + RESYNC_LAG_US < now) next = now; /* far behind = real block: resync, no burst.
                                                           a small lag is left in the past so the next
                                                           spin iterations catch it up one send each */
        }
        /* sleep only when there's >=1ms of real slack before the next tick; otherwise
           spin with a non-blocking poll so high rates aren't capped at ~1 kHz */
        dart_node_poll(n, (next > now && next - now >= 1000) ? 1 : 0);
        now = dart_plat_now_us();
        if (now - last_print >= 1000000u){             /* >=1 REAL second: true rate + flow control */
            double secs = (now - last_print)/1e6;
            dart_node_backpressure_stats(n, &backpressure_us, &backpressure_waits);
            printf("[pub] %.1f msg/s (sent %lu)  matched=%d  bp_waits=+%u  bp_total=%.2fs\n",
                   (sent - last_sent)/secs, sent,
                   dart_node_writer_match_count(n, channel),
                   backpressure_waits - last_backpressure_waits, backpressure_us/1e6);
            dart_node_repair_stats(n, channel, &repair_stats);   /* only meaningful for a reliable channel */
            if (repair_stats.nacks_recv != prev_repair_stats.nacks_recv || repair_stats.frags_resent != prev_repair_stats.frags_resent){
                uint64_t delta_sent = repair_stats.frags_sent - prev_repair_stats.frags_sent;
                uint64_t delta_resent  = repair_stats.frags_resent - prev_repair_stats.frags_resent;
                /* normalise by REAL elapsed: a backpressure-blocked second can span many
                   wall seconds, so a raw delta labelled "/s" overstates the true rate. */
                printf("[pub]   repair: nacks %.1f/s  resent %.1f/s  (%.1f%% of TX)\n",
                       (repair_stats.nacks_recv - prev_repair_stats.nacks_recv)/secs,
                       delta_resent/secs,
                       delta_sent ? 100.0*(double)delta_resent/(double)delta_sent : 0.0);
            }
            last_sent = sent; last_print = now; last_backpressure_waits = backpressure_waits; prev_repair_stats = repair_stats;
        }
    }
}

int main(int argc, char **argv){
    const char *mode = NULL, *if_ip = NULL, *peer_ip = NULL, *file_name = NULL;
    const char *pos[64]; int npos = 0;     /* positional args: [mode, topic(s)/message...] */
    uint16_t domain = 7;
    int mcast = 0, reliable = 1, wait_ms = 5000, rate_set = 0, frag = 0;
    double rate_hz = 0;                    /* --rate: pub repeats at N Hz; sub measures rate */
    size_t cap = 4u<<20; int max_set = 0; /* --max: fixed message cap (else dynamic) */
    char msg[65536]; size_t msg_len = 0;  /* one-shot publish text, if any */
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    g_start_ms = now_ms();

    /* Parse: recognized --flags anywhere; the rest are positionals
     * [mode, channel, message words...]. Message words join with spaces. */
    for (i = 1; i < argc; i++){
        const char *arg = argv[i];
        if      (!strcmp(arg, "--domain") && i+1 < argc) domain  = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(arg, "--if")     && i+1 < argc) if_ip   = argv[++i];
        else if (!strcmp(arg, "--peer")   && i+1 < argc) peer_ip = argv[++i];
        else if (!strcmp(arg, "--wait")   && i+1 < argc) wait_ms = atoi(argv[++i]);
        else if (!strcmp(arg, "--file")   && i+1 < argc) file_name = argv[++i];
        else if (!strcmp(arg, "--max")    && i+1 < argc){ cap = parse_size(argv[++i]); max_set = 1; }
        else if (!strcmp(arg, "--frag")   && i+1 < argc) frag = atoi(argv[++i]);
        else if (!strcmp(arg, "--rate")){              /* HZ optional: bare --rate on a sub */
            rate_set = 1;
            if (i+1 < argc){                         /* consume the next token only if numeric, */
                char *end; double value = strtod(argv[i+1], &end);   /* so --rate --file isn't eaten */
                if (end != argv[i+1] && *end == '\0'){ rate_hz = value; i++; }
            }
        }
        else if (!strcmp(arg, "--mcast"))                mcast   = 1;
        else if (!strcmp(arg, "--best-effort"))          reliable = 0;
        else if (!strcmp(arg, "--help") || !strcmp(arg, "-h")){ usage(); return 0; }
        else if (arg[0] == '-' && arg[1] == '-'){ fprintf(stderr, "unknown option %s\n", arg); usage(); return 2; }
        else if (npos < 64) pos[npos++] = arg;
    }
    if (max_set && cap < 64) cap = 64;   /* --max 0/garbage: keep a sane floor */

    mode = npos > 0 ? pos[0] : NULL;
    if (!mode){ usage(); return 2; }
    int is_pub = !strcmp(mode, "pub");
    int is_sub = !strcmp(mode, "sub");
    if (!is_pub && !is_sub){ fprintf(stderr, "mode must be 'pub' or 'sub'\n"); usage(); return 2; }

    /* sub takes every positional after the mode as a topic (a list); pub takes the
       first as its topic and joins the rest as the message text. */
    if (is_sub){
        for (i = 1; i < npos && g_n_topics < MAX_TOPICS; i++) g_topics[g_n_topics++] = pos[i];
    } else {
        if (npos > 1) g_topics[g_n_topics++] = pos[1];
        for (i = 2; i < npos; i++){
            size_t word_len = strlen(pos[i]);
            if (msg_len && msg_len < sizeof msg - 1) msg[msg_len++] = ' ';
            if (word_len > sizeof msg - 1 - msg_len) word_len = sizeof msg - 1 - msg_len;
            memcpy(msg + msg_len, pos[i], word_len); msg_len += word_len;
        }
    }
    msg[msg_len] = '\0';
    if (g_n_topics == 0){ usage(); return 2; }

    int dynamic = !max_set;                 /* no --max => grow buffers via malloc */
    size_t send_limit = dynamic ? DART_MESSAGE_MAX : cap;

    /* A topic name's 64-bit hash is its cross-peer identity; the local handle is the
       channel's creation index. The publisher sends on the first channel (index 0). */
    const uint16_t channel = 0;

    /* A deliberate big-message profile (not the defaults), shared by every topic:
       shallow keep_last because messages can be megabytes, fast 5ms repair, and a long
       flow-control window so a multi-chunk file drains before KEEP_LAST evicts un-acked
       history. A dead reader still releases at the peer timeout. */
    DartQos qos = {
        .reliability         = reliable ? DART_RELIABLE : DART_BEST_EFFORT,
        .keep_last           = 4,
        .catch_up            = 2,
        .max_message_bytes   = dynamic ? 0u : (uint32_t)cap,
        .heartbeat_us        = 200000,
        .repair_delay_us     = 2000,
        .backpressure_wait_us= 5000000,
    };
    const uint16_t max_peers = 8;   /* a few peers; bounds per-peer reassembly */

    /* announce/timeout left at defaults (1s / 3.5s): the startup solicit makes
       discovery near-instant, so the periodic announce is just the slow backstop.
       data_port defaults to 0 = an OS-assigned ephemeral port. */
    DartNodeOpts opts; memset(&opts, 0, sizeof opts);
    opts.domain       = domain;
    opts.max_channels = (uint16_t)g_n_topics;
    opts.on_event     = on_event;
    opts.allocator    = dynamic ? pubsub_realloc : NULL;   /* dynamic message sizing */
    opts.discovery.max_peers = max_peers;
    /* A single big message has no within-message flow control, so the receive
       socket must buffer it whole or fragments drop and 32-wide NACK repair
       crawls. Size the socket buffers to hold one message (clamped 8..64 MB).
       On Linux raise net.core.rmem_max to match (Windows honors it as-is). */
    { size_t sb = dynamic ? DART_MESSAGE_MAX : cap;
      if (sb < (8u<<20))  sb = 8u<<20;
      if (sb > (64u<<20)) sb = 64u<<20;
      opts.net.recv_buffer_bytes = (uint32_t)sb;
      opts.net.send_buffer_bytes = (uint32_t)(sb > (16u<<20) ? (16u<<20) : sb); }
    if (frag) opts.net.fragment_size = (uint16_t)frag;   /* needs -DDART_FRAG_PAYLOAD_MAX>=frag */
    if (if_ip)      opts.net.multicast_interface = if_ip;          /* multihomed: pin it */
    else if (mcast) opts.net.multicast_interface = "127.0.0.1";    /* same-host: stay local */

    DartDiscoveryAddr seed;
    if (peer_ip){
        memset(&seed, 0, sizeof seed);
        if (parse_ipv4(peer_ip, seed.ip) < 0){ fprintf(stderr, "bad --peer ip %s\n", peer_ip); return 2; }
        seed.ip_len = 4;           /* port 0 = use the discovery port */
        opts.net.seed_peers = &seed; opts.net.n_seed_peers = 1;
    }

    /* Size the arena. Dynamic mode keeps message buffers out of the arena (the
       allocator mallocs them on demand), so a small fixed arena suffices; fixed
       mode (--max) carves (keep_last + max_peers) x cap of history+reassembly per
       channel from it. The node mallocs this and owns it for the whole run. */
    size_t mem_size = 8u<<20;
    if (!dynamic)
        mem_size += (size_t)g_n_topics * ((size_t)qos.keep_last + max_peers) * cap;
    DartNode *n = dart_node_open(mem_size, NULL, on_message, &opts);
    if (!n){ fprintf(stderr, "dart_node_open failed (arena %lu bytes)\n", (unsigned long)mem_size); return 1; }

    /* Create channels in index order, so channel index i is g_topics[i] and the
       shims above resolve an index straight to its handle. */
    for (i = 0; i < g_n_topics; i++){
        DartChannelOpts co; memset(&co, 0, sizeof co);
        co.qos = qos; co.multicast = (uint8_t)mcast;
        if (!dart_node_create_channel(n, g_topics[i], is_pub ? DART_PUB_ONLY : DART_SUB_ONLY, &co)){
            fprintf(stderr, "create channel '%s' failed\n", g_topics[i]);
            dart_node_close(n, 0); return 1;
        }
    }

    if (is_sub){
        if (file_name){
            g_outfile = fopen(file_name, "wb");   /* truncate; on_sample appends + flushes */
            if (!g_outfile){ fprintf(stderr, "cannot open %s for writing\n", file_name);
                             dart_node_close(n, 1); return 1; }
        }
        g_rate_mode = rate_set;   /* --rate on a sub: report measured throughput, not each msg */
        printf("[sub] %d topic(s):", g_n_topics);
        for (i = 0; i < g_n_topics; i++) printf(" %s", g_topics[i]);
        printf(" | domain %u, %s%s%s. Listening (Ctrl-C to quit)...\n",
               domain, reliable ? "reliable" : "best-effort",
               file_name ? ", saving to " : "", file_name ? file_name : "");
        /* short tick either way: let the timer sweep flush ACKNACKs promptly
           (< nack_delay) so a reliable publisher's backpressure window keeps
           draining instead of stalling. */
        if (g_rate_mode){
            /* Once a second, print the measured receive rate (msg/s and KB/s)
               over the real elapsed interval; stay quiet until the first message
               so an idle wait isn't a stream of 0/s lines. */
            uint64_t last = dart_plat_now_us(), repair_last = last; int seen = 0;
            DartRepairStats repair_stats, prev_repair_stats; memset(&prev_repair_stats, 0, sizeof prev_repair_stats);
            for (;;){
                uint64_t now, rate_elapsed, repair_elapsed;
                dart_node_poll(n, 2);
                now = dart_plat_now_us();
                if (g_rx_msgs) seen = 1;
                /* fine-grained (~250ms) reader repair series: finer than the 1s rate line
                   so a within-stall plateau is visible. Deltas normalised to /s, plus the
                   arm attribution -- during the dead period the prediction is arms_hb ticks
                   at the heartbeat rate while arms_data is flat (reader only re-asks on a
                   packet, never on its own timer). Gated on activity so idle stays quiet. */
                repair_elapsed = now - repair_last;
                if (repair_elapsed >= 250000u){
                    double repair_secs = repair_elapsed/1e6;
                    dart_node_repair_stats(n, channel, &repair_stats);
                    if (repair_stats.nacks_sent != prev_repair_stats.nacks_sent || repair_stats.frags_recv != prev_repair_stats.frags_recv
                        || repair_stats.frags_old != prev_repair_stats.frags_old || repair_stats.frags_ahead != prev_repair_stats.frags_ahead){
                        uint64_t base; uint32_t have, total;
                        int mid_reassembly = dart_node_reader_progress(n, channel, g_last_peer, &base, &have, &total);
                        /* recv = ACCEPTED (base==deliver_upto). old/ahead = arrived-but-rejected,
                           so "recv 0" with old/ahead high means the flood is landing on the wrong
                           seqno position, not failing to arrive. */
                        printf("[sub]   repair: nacks %.0f/s  recv %.0f/s  dup %.0f/s  old %.0f/s  ahead %.0f/s  arms(d/hb) %.0f/%.0f",
                               (repair_stats.nacks_sent - prev_repair_stats.nacks_sent)/repair_secs,
                               (repair_stats.frags_recv - prev_repair_stats.frags_recv)/repair_secs,
                               (repair_stats.frags_dup  - prev_repair_stats.frags_dup)/repair_secs,
                               (repair_stats.frags_old  - prev_repair_stats.frags_old)/repair_secs,
                               (repair_stats.frags_ahead- prev_repair_stats.frags_ahead)/repair_secs,
                               (repair_stats.arms_data  - prev_repair_stats.arms_data)/repair_secs,
                               (repair_stats.arms_hb    - prev_repair_stats.arms_hb)/repair_secs);
                        if (mid_reassembly) printf("  | HOL base=%llu have=%u/%u", (unsigned long long)base, have, total);
                        printf("  skipped=%llu\n", (unsigned long long)repair_stats.msgs_skipped);
                    }
                    prev_repair_stats = repair_stats; repair_last = now;
                }
                rate_elapsed = now - last;
                if (rate_elapsed >= 1000000u){                       /* 1s: measured receive rate */
                    double secs = rate_elapsed/1e6;
                    if (seen) printf("[sub] %.0f msg/s, %.1f KB/s (total %llu, lost %llu)\n",
                                     g_rx_msgs/secs, (g_rx_bytes/1024.0)/secs, g_rx_total, g_lost);
                    g_rx_msgs = 0; g_rx_bytes = 0; last = now;
                }
            }
            /* not reached */
        }
        for (;;) dart_node_poll(n, 2);
        /* not reached */
    }

    /* publisher */
    printf("[pub] topic %s, domain %u, %s.\n",
           g_topics[0], domain, reliable ? "reliable" : "best-effort");
    /* in-pump diagnostic: fires only while a send is blocked in backpressure (the stall
       condition), so it is silent in healthy operation. Surfaces the within-block repair
       series the once-a-second loop can't (it's blocked inside the send). */
    dart_node_set_pump_probe(n, pump_probe, 200000u, NULL);

    /* pub --rate must carry a positive HZ (on a sub the value is ignored). */
    if (rate_set && rate_hz <= 0){
        fprintf(stderr, "pub --rate needs a positive HZ, e.g. --rate 100\n");
        dart_node_close(n, 1); return 2;
    }
    /* --rate needs a fixed payload to repeat: CLI text or --file, not stdin. */
    if (rate_hz > 0 && !msg_len && !file_name){
        fprintf(stderr, "--rate needs data to repeat: give a message or --file\n");
        dart_node_close(n, 1); return 2;
    }

    /* --file <name>: publish that file's contents (whole, same framing as piped
     * stdin), regardless of terminal. Takes precedence over CLI text and stdin. */
    if (file_name){
        FILE *file = fopen(file_name, "rb");
        if (!file){ fprintf(stderr, "cannot open %s\n", file_name);
                 dart_node_close(n, 1); return 1; }
        if (rate_hz > 0){                    /* load once, then repeat at the rate */
            size_t len; int too_big;
            char *buf = read_all(file, send_limit, &len, &too_big);
            fclose(file);
            if (!buf){
                if (too_big) fprintf(stderr, "[pub] %s exceeds the %lu-byte message limit; raise --max\n",
                                  file_name, (unsigned long)send_limit);
                else      fprintf(stderr, "out of memory\n");
                dart_node_close(n, 1); return 1;
            }
            publish_rate(n, channel, buf, len, rate_hz, wait_ms);   /* never returns */
        }
        int rc = publish_stream(n, channel, file, wait_ms, send_limit);
        fclose(file);
        dart_node_close(n, 1);
        return rc < 0 ? 1 : 0;
    }

    if (msg_len){
        int t;
        if (rate_hz > 0)
            publish_rate(n, channel, msg, msg_len, rate_hz, wait_ms);   /* never returns */
        /* one-shot: wait for a matched subscriber, publish the CLI text, drain
         * delivery, then exit. */
        if (!wait_for_sub(n, channel, wait_ms)){
            fprintf(stderr, "[pub] no subscriber matched in %dms; nothing sent\n", wait_ms);
            dart_node_close(n, 1); return 1;
        }
        if (dart_node_send(n, channel, msg, msg_len) < 0)
            fprintf(stderr, "send failed\n");
        else
            printf("[pub] sent %lu bytes\n", (unsigned long)msg_len);
        dart_node_drain(n, channel, 30000);      /* wait for delivery, not a fixed grace */
        for (t = 0; t < 200; t += 20) dart_node_poll(n, 20);   /* brief flush */
        dart_node_close(n, 1);   /* BYE: peers drop us now, not after timeout */
        return 0;
    }

    /* prewritten input piped or redirected on stdin (not a terminal): send the
     * whole stream as messages with newlines intact, splitting only at the 64KB
     * sample cap, never one-per-line. */
    if (!stdin_is_tty()){
        int rc = publish_stream(n, channel, stdin, wait_ms, send_limit);
        dart_node_close(n, 1);
        return rc < 0 ? 1 : 0;
    }

    /* interactive (terminal): one message per typed line until EOF. A background
     * thread keeps the node polled the whole time we block on input, so discovery
     * and reliable repair never stall no matter how long between lines. */
    {   static char line[65536];   /* static: keep this 64KB off the stack */
        g_node = n; lock_init(&g_lock);
#ifdef _WIN32
        HANDLE th = CreateThread(NULL, 0, pump_thread, NULL, 0, NULL);
#else
        pthread_t th; pthread_create(&th, NULL, pump_thread, NULL);
#endif
        printf("[pub] type a message per line, Ctrl-D / Ctrl-Z+Enter to end:\n");
        while (fgets(line, sizeof line, stdin)){
            size_t line_len = strlen(line);
            while (line_len && (line[line_len-1] == '\n' || line[line_len-1] == '\r')) line[--line_len] = '\0';
            if (!line_len) continue;
            lock_get(&g_lock);
            if (dart_node_send(n, channel, line, line_len) < 0) fprintf(stderr, "send failed\n");
            lock_put(&g_lock);
        }
        g_pumping = 0;              /* stop the pump first: only we touch the node now */
#ifdef _WIN32
        WaitForSingleObject(th, INFINITE); CloseHandle(th);
#else
        pthread_join(th, NULL);
#endif
        dart_node_drain(n, channel, 30000);   /* deliver the last typed messages before BYE */
    }

    dart_node_close(n, 1);   /* BYE: peers drop us now instead of after timeout */
    return 0;
}
