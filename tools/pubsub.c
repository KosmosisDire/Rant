/* tiny pub/sub command-line tool over a DART node.
 *
 *   pubsub sub <channel> [opts]            subscribe: print every message
 *   pubsub pub <channel> <text...> [opts]  publish one text message, then exit
 *   pubsub pub <channel> [opts]            publish from stdin: a terminal sends
 *                                          one message per typed line; a piped
 *                                          or redirected file is sent whole
 *                                          (newlines preserved)
 *
 * <channel> is a number (0..65534 = a local channel id, used directly as the
 * cross-peer identity) or a topic name (its 64-bit hash is the identity; the
 * name rides discovery so a hash clash is detected, not silently cross-wired).
 * Both sides must use the same channel, domain, and transport (unicast or
 * --mcast). Messages are reliable KEEP_LAST, so a subscriber already up gets
 * them in order, and one that joins late sees the recent history a live
 * publisher still holds.
 *
 *   POSIX  : cc  -std=c99 -Wall -Idist tools/pubsub.c -o pubsub -lpthread
 *   Windows: gcc -std=c99 -Wall -Idist tools/pubsub.c -o pubsub.exe -lws2_32 -lbcrypt
 *
 * Try it: in one terminal `pubsub sub chat`, in another `pubsub pub chat hello`.
 *
 * Options:
 *   --domain N     discovery domain (default 7)
 *   --mcast        let high-fanout data ride a per-channel multicast group
 *   --if <ip>      multicast interface IP (pin this on multihomed hosts)
 *   --peer <ip>    seed a peer by IP so discovery works without multicast
 *   --best-effort  drop reliability (fire and forget, no repair)
 *   --wait MS      how long a one-shot/file publisher waits for a subscriber to
 *                  match before sending (default 5000); delivery is then awaited
 *                  automatically, so a too-short wait no longer drops a message
 *   --file <name>  pub: publish the named file's contents (whole, like piped
 *                  stdin); sub: write each received message to the named file,
 *                  OVERWRITING it (the file mirrors the latest message, so pair
 *                  with --max big enough that a whole file is one message)
 *   --max <size>   FIXED message cap (e.g. 512k, 16M): bounds memory, refuses a
 *                  bigger message, and a receiver drops + reports one over its cap.
 *                  OMIT for DYNAMIC sizing (default): buffers grow via malloc to
 *                  fit any message up to DART's ~64MB single-message limit, so no
 *                  size need be set on capable machines. A file is always sent as
 *                  ONE message (never split); too big to fit is an error.
 */
#define DART_IMPLEMENTATION
#include "dart.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Minimal thread + lock shim. A DART node only does discovery, liveness, and
 * reliable repair inside dart_node_poll, so the interactive publisher must keep
 * polling while it blocks on stdin: otherwise announces stop, peers time out
 * (default 3.5s), and slowly-typed lines flap the peer and drop. We poll on a
 * background thread and serialize every dart_node_* call with one lock. */
#ifdef _WIN32
  #include <windows.h>
  typedef CRITICAL_SECTION lock_t;
  static void lock_init(lock_t *m){ InitializeCriticalSection(m); }
  static void lock_get (lock_t *m){ EnterCriticalSection(m); }
  static void lock_put (lock_t *m){ LeaveCriticalSection(m); }
  static void sleep_ms (int ms){ Sleep((DWORD)ms); }
#else
  #include <pthread.h>
  #include <time.h>
  typedef pthread_mutex_t lock_t;
  static void lock_init(lock_t *m){ pthread_mutex_init(m, NULL); }
  static void lock_get (lock_t *m){ pthread_mutex_lock(m); }
  static void lock_put (lock_t *m){ pthread_mutex_unlock(m); }
  static void sleep_ms (int ms){ struct timespec ts; ts.tv_sec = ms/1000;
                                 ts.tv_nsec = (long)(ms%1000)*1000000L; nanosleep(&ts, NULL); }
#endif

/* Is stdin a terminal? Interactive typing publishes one message per line; a
 * piped or redirected file is published whole, newlines and all. */
#ifdef _WIN32
  #include <io.h>
  static int stdin_is_tty(void){ return _isatty(_fileno(stdin)); }
  static void file_truncate(FILE *f, long n){ _chsize(_fileno(f), n); }
#else
  #include <unistd.h>
  static int stdin_is_tty(void){ return isatty(fileno(stdin)); }
  static void file_truncate(FILE *f, long n){ if (ftruncate(fileno(f), n)){} }
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

static dart_node  *g_node;
static lock_t      g_lock;
static volatile int g_pumping = 1;
static FILE        *g_outfile = NULL;   /* sub --file: received messages saved here */

/* Service the socket ~200x/s. The brief lock is dropped during the sleep, so a
 * sender on the main thread never waits long to publish. */
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

/* Resolve a channel token. A plain number is a local channel id (no name, so the
 * id itself is the cross-peer identity, 0..65534). Anything else is a topic NAME:
 * the transport hashes it to a 64-bit identity, and we give it a fixed local
 * handle (only this process ever sees it). */
static void chan_from_token(const char *s, uint16_t *id_out, const char **name_out){
    char *end;
    long v = strtol(s, &end, 10);
    if (*s && !*end && v >= 0 && v < (long)DART_CHAN_META){ *id_out=(uint16_t)v; *name_out=NULL; }
    else { *id_out = 1; *name_out = s; }
}

/* Two distinct topic names hashed to the same identity: the transport refuses
 * the match (never cross-wires); we just warn. Astronomically rare at 64 bits. */
static void on_collision(void *u, uint64_t id, const char *ours,
                         const char *peer, size_t plen){
    (void)u;
    fprintf(stderr, "[warn] topic hash collision %016llx: ours=\"%s\" peer=\"%.*s\" (ignored)\n",
            (unsigned long long)id, ours, (int)plen, peer);
}

/* "a.b.c.d" into 4 network-order octets. Returns 0 ok, <0 on malformed input.
 * Kept local so the tool needs no socket headers of its own. */
static int parse_ipv4(const char *s, uint8_t out[4]){
    int i;
    for (i = 0; i < 4; i++){
        char *end;
        long b = strtol(s, &end, 10);
        if (end == s || b < 0 || b > 255) return -1;
        if (i < 3 && *end != '.') return -1;
        if (i == 3 && *end != '\0') return -1;
        out[i] = (uint8_t)b;
        s = end + 1;
    }
    return 0;
}

/* "4M" / "512k" / "1048576" -> bytes (binary K/M/G suffix). 0 on garbage. */
static size_t parse_size(const char *s){
    char *end; double v = strtod(s, &end), m = 1.0;
    if      (*end=='k'||*end=='K') m = 1024.0;
    else if (*end=='m'||*end=='M') m = 1024.0*1024.0;
    else if (*end=='g'||*end=='G') m = 1024.0*1024.0*1024.0;
    return (v > 0) ? (size_t)(v*m) : 0;
}

/* subscriber callback: keep it cheap, never call back into dart_*. Printing the
 * payload as text is fine. */
static void on_sample(void *u, uint16_t ch, uint32_t from, const void *data, size_t len){
    (void)u;
    if (g_outfile){                       /* --file: each message OVERWRITES the file */
        rewind(g_outfile);                /* back to the start, not appending */
        fwrite(data, 1, len, g_outfile);
        fflush(g_outfile);                /* flush before truncating a longer prior message */
        file_truncate(g_outfile, (long)len);
        printf("[ch %u <- peer %u] wrote %lu bytes\n", ch, from, (unsigned long)len);
    } else {
        printf("[ch %u <- peer %u] %.*s\n", ch, from, (int)len, (const char*)data);
    }
}

/* A peer sent a message bigger than our --max, so the transport can't hold it
 * (it skips past it rather than wedging). Report it; both ends must share a
 * --max >= the largest message. */
static void on_oversize(void *u, uint16_t ch, uint32_t from, uint32_t bytes){
    (void)u;
    fprintf(stderr, "[sub] dropped a %lu-byte message on ch %u from peer %u: exceeds --max; raise --max\n",
            (unsigned long)bytes, ch, from);
}

/* discovery visibility: log when a peer is found / lost, with elapsed time so
 * you can see how long discovery actually took. */
static void on_peer_up(void *u, uint32_t id, const dart_discovery_addr *a){
    (void)u;
    if (a->ip_len == 4)
        fprintf(stderr, "[disc +%ldms] peer %u discovered at %u.%u.%u.%u:%u\n",
                now_ms()-g_start_ms, id, a->ip[0],a->ip[1],a->ip[2],a->ip[3], a->port);
    else
        fprintf(stderr, "[disc +%ldms] peer %u discovered\n", now_ms()-g_start_ms, id);
}
static void on_peer_down(void *u, uint32_t id){
    (void)u;
    fprintf(stderr, "[disc +%ldms] peer %u lost\n", now_ms()-g_start_ms, id);
}

static void usage(void){
    fprintf(stderr,
        "usage:\n"
        "  pubsub sub <channel> [opts]\n"
        "  pubsub pub <channel> [text...] [opts]   (no text = read lines from stdin)\n"
        "opts: --domain N  --mcast  --if <ip>  --peer <ip>  --best-effort  --wait MS  --file <name>  --max <size>\n");
}

/* Allocator for DART's dynamic message buffers (used when no --max is set).
 * realloc semantics: size 0 frees. */
static void *pubsub_realloc(void *u, void *ptr, size_t size){
    (void)u;
    if (size == 0){ free(ptr); return NULL; }
    return realloc(ptr, size);
}

/* Wait up to timeout_ms for >=1 subscriber to match this channel (discovery +
 * interest exchange), pumping the node throughout, so a one-shot publisher never
 * fires into the void: with no matched reader a send still "succeeds" and drains
 * vacuously, yet nobody receives it. Returns 1 if matched. A brief settle after
 * the first match lets the reader's own side finish wiring up. */
static int wait_for_sub(dart_node *n, uint16_t cid, int timeout_ms){
    int t, said = 0;
    for (t = 0; t < timeout_ms; t += 20){
        if (dart_node_writer_match_count(n, cid) > 0){
            int s; for (s = 0; s < 200; s += 20) dart_node_poll(n, 20);
            return 1;
        }
        if (!said && t >= 400){ printf("[pub] waiting for a subscriber...\n"); said = 1; }
        dart_node_poll(n, 20);
    }
    return dart_node_writer_match_count(n, cid) > 0;
}

/* Publish the whole byte stream from f as ONE message (the transport fragments
 * + reassembles it). Reads into a buffer that grows up to cap, and refuses if
 * the input would exceed cap rather than splitting. Settles first so a freshly
 * discovered subscriber is matched, then drains delivery before returning. */
static int publish_stream(dart_node *n, uint16_t cid, FILE *f, int wait_ms, size_t cap){
    size_t bufcap = 65536, len = 0, r;
    char *buf, *nb;
    int t;
    if (bufcap > cap) bufcap = cap;
    buf = (char*)malloc(bufcap);
    if (!buf){ fprintf(stderr, "out of memory\n"); return -1; }
    if (!wait_for_sub(n, cid, wait_ms)){
        fprintf(stderr, "[pub] no subscriber matched in %dms; nothing sent\n", wait_ms);
        free(buf); return -1;
    }
    for (;;){
        if (len == bufcap){
            if (bufcap >= cap){                       /* at the limit: more left? */
                if (fgetc(f) != EOF){
                    fprintf(stderr, "[pub] input exceeds the %lu-byte message limit; raise --max\n",
                            (unsigned long)cap);
                    free(buf); return -1;
                }
                break;                                /* exactly cap, at EOF */
            }
            { size_t want = bufcap*2; if (want > cap) want = cap;
              nb = (char*)realloc(buf, want);
              if (!nb){ free(buf); fprintf(stderr, "out of memory\n"); return -1; }
              buf = nb; bufcap = want; }
        }
        r = fread(buf + len, 1, bufcap - len, f);
        if (r == 0) break;                            /* EOF */
        len += r;
    }
    if (dart_node_send(n, cid, buf, len) < 0){ fprintf(stderr, "send failed\n"); free(buf); return -1; }
    printf("[pub] sent %lu bytes\n", (unsigned long)len);
    /* Wait until the receiver has acked every byte before returning (the caller
       then sends BYE). A fixed grace would cut off a large or slow transfer:
       with the BYE the reader drops the peer and abandons in-flight data. */
    if (!dart_node_drain(n, cid, 30000))
        fprintf(stderr, "[pub] warning: delivery incomplete (reader slow or gone)\n");
    for (t = 0; t < 200; t += 20) dart_node_poll(n, 20);   /* brief flush (best-effort) */
    free(buf);
    return 0;
}

int main(int argc, char **argv){
    const char *mode = NULL, *chan = NULL, *if_ip = NULL, *peer_ip = NULL, *file_name = NULL;
    uint16_t domain = 7;
    int mcast = 0, reliable = 1, wait_ms = 5000;
    size_t cap = 4u<<20; int max_set = 0; /* --max: fixed message cap (else dynamic) */
    char msg[65536]; size_t msg_len = 0;  /* one-shot publish text, if any */
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    g_start_ms = now_ms();

    /* Parse: recognized --flags anywhere; the rest are positionals
     * [mode, channel, message words...]. Message words join with spaces. */
    for (i = 1; i < argc; i++){
        const char *a = argv[i];
        if      (!strcmp(a, "--domain") && i+1 < argc) domain  = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(a, "--if")     && i+1 < argc) if_ip   = argv[++i];
        else if (!strcmp(a, "--peer")   && i+1 < argc) peer_ip = argv[++i];
        else if (!strcmp(a, "--wait")   && i+1 < argc) wait_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--file")   && i+1 < argc) file_name = argv[++i];
        else if (!strcmp(a, "--max")    && i+1 < argc){ cap = parse_size(argv[++i]); max_set = 1; }
        else if (!strcmp(a, "--mcast"))                mcast   = 1;
        else if (!strcmp(a, "--best-effort"))          reliable = 0;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")){ usage(); return 0; }
        else if (a[0] == '-' && a[1] == '-'){ fprintf(stderr, "unknown option %s\n", a); usage(); return 2; }
        else if (!mode) mode = a;
        else if (!chan) chan = a;
        else {   /* message word: append with a separating space */
            size_t k = strlen(a);
            if (msg_len && msg_len < sizeof msg - 1) msg[msg_len++] = ' ';
            if (k > sizeof msg - 1 - msg_len) k = sizeof msg - 1 - msg_len;
            memcpy(msg + msg_len, a, k); msg_len += k;
        }
    }
    msg[msg_len] = '\0';
    if (max_set && cap < 64) cap = 64;   /* --max 0/garbage: keep a sane floor */

    if (!mode || !chan){ usage(); return 2; }
    int is_pub = !strcmp(mode, "pub");
    int is_sub = !strcmp(mode, "sub");
    if (!is_pub && !is_sub){ fprintf(stderr, "mode must be 'pub' or 'sub'\n"); usage(); return 2; }

    int dynamic = !max_set;                 /* no --max => grow buffers via malloc */
    size_t send_limit = dynamic ? DART_SAMPLE_MAX : cap;

    uint16_t cid; const char *cname;
    chan_from_token(chan, &cid, &cname);

    dart_channel_def ch; memset(&ch, 0, sizeof ch);
    ch.channel_id           = cid;
    ch.name                 = cname;
    ch.qos.reliability      = reliable ? DART_RELIABLE : DART_BEST_EFFORT;
    ch.qos.history_depth    = 4;        /* shallow: samples can be megabytes */
    ch.qos.join_replay      = 2;        /* late subscribers see recent history */
    ch.qos.max_sample_bytes = dynamic ? 0u : (uint32_t)cap;
    ch.qos.heartbeat_us     = 200000;   /* 200 ms */
    ch.qos.nack_delay_us    = 5000;     /* 5 ms  */
    ch.qos.max_block_us     = 5000000;  /* 5s flow control: pace the publisher to
                                           the reader so a multi-chunk file is
                                           delivered before KEEP_LAST(16) evicts
                                           un-acked history. A dead reader still
                                           releases at the peer timeout. */
    ch.dir   = is_pub ? DART_PUB_ONLY : DART_SUB_ONLY;
    ch.mcast = (uint8_t)mcast;

    dart_node_config cfg; memset(&cfg, 0, sizeof cfg);
    cfg.domain_id  = domain;
    cfg.data_port  = 0;            /* OS-assigned ephemeral port */
    cfg.channels   = &ch;
    cfg.n_channels = 1;
    cfg.on_sample  = on_sample;
    cfg.on_collision = on_collision;
    cfg.on_oversize = on_oversize;
    cfg.realloc_fn  = dynamic ? pubsub_realloc : NULL;   /* dynamic message sizing */
    cfg.on_peer_up   = on_peer_up;
    cfg.on_peer_down = on_peer_down;
    /* announce/timeout left at defaults (1s / 3.5s): the startup solicit makes
       discovery near-instant, so the periodic announce is just the slow backstop. */
    cfg.max_peers  = 8;            /* a few peers; bounds the per-peer reassembly */
    /* A single big sample has no within-sample flow control, so the receive
       socket must buffer it whole or fragments drop and 32-wide NACK repair
       crawls. Size the socket buffers to hold one sample (clamped 8..64 MB).
       On Linux raise net.core.rmem_max to match (Windows honors it as-is). */
    { size_t sb = dynamic ? DART_SAMPLE_MAX : cap;
      if (sb < (8u<<20))  sb = 8u<<20;
      if (sb > (64u<<20)) sb = 64u<<20;
      cfg.so_rcvbuf = (uint32_t)sb;
      cfg.so_sndbuf = (uint32_t)(sb > (16u<<20) ? (16u<<20) : sb); }
    if (if_ip)      cfg.mcast_if = if_ip;            /* multihomed: pin it */
    else if (mcast) cfg.mcast_if = "127.0.0.1";      /* same-host: stay local */

    dart_discovery_addr seed;
    if (peer_ip){
        memset(&seed, 0, sizeof seed);
        if (parse_ipv4(peer_ip, seed.ip) < 0){ fprintf(stderr, "bad --peer ip %s\n", peer_ip); return 2; }
        seed.ip_len = 4;           /* port 0 = use the discovery port */
        cfg.seeds = &seed; cfg.n_seeds = 1;
    }

    /* Size the arena to the cap: ~ (history_depth + max_peers) x max_sample_bytes
       plus the meta channel. malloc'd (can be tens of MB) and intentionally not
       freed: the process owns it for its whole life. */
    size_t need = dart_node_required_memory(&cfg);
    uint8_t *mem = (uint8_t*)malloc(need);
    if (!mem){ fprintf(stderr, "out of memory (need %lu bytes)\n", (unsigned long)need); return 1; }
    dart_node *n = dart_node_open(mem, need, &cfg);
    if (!n){ fprintf(stderr, "dart_node_open failed\n"); free(mem); return 1; }

    if (is_sub){
        if (file_name){
            g_outfile = fopen(file_name, "wb");   /* truncate; on_sample appends + flushes */
            if (!g_outfile){ fprintf(stderr, "cannot open %s for writing\n", file_name);
                             dart_node_close(n, 1); return 1; }
        }
        printf("[sub] channel %s (id %u), domain %u, %s%s%s. Listening (Ctrl-C to quit)...\n",
               chan, cid, domain, reliable ? "reliable" : "best-effort",
               file_name ? ", saving to " : "", file_name ? file_name : "");
        for (;;) dart_node_poll(n, 2);     /* short tick: let the timer sweep flush
                                              ACKNACKs promptly (< nack_delay) so a
                                              reliable publisher's backpressure window
                                              keeps draining instead of stalling */
        /* not reached */
    }

    /* publisher */
    printf("[pub] channel %s (id %u), domain %u, %s.\n",
           chan, cid, domain, reliable ? "reliable" : "best-effort");

    /* --file <name>: publish that file's contents (whole, same framing as piped
     * stdin), regardless of terminal. Takes precedence over CLI text and stdin. */
    if (file_name){
        FILE *f = fopen(file_name, "rb");
        if (!f){ fprintf(stderr, "cannot open %s\n", file_name);
                 dart_node_close(n, 1); return 1; }
        int rc = publish_stream(n, cid, f, wait_ms, send_limit);
        fclose(f);
        dart_node_close(n, 1);
        return rc < 0 ? 1 : 0;
    }

    if (msg_len){
        /* one-shot: wait for a matched subscriber, publish the CLI text, drain
         * delivery, then exit. */
        int t;
        if (!wait_for_sub(n, cid, wait_ms)){
            fprintf(stderr, "[pub] no subscriber matched in %dms; nothing sent\n", wait_ms);
            dart_node_close(n, 1); return 1;
        }
        if (dart_node_send(n, cid, msg, msg_len) < 0)
            fprintf(stderr, "send failed\n");
        else
            printf("[pub] sent %lu bytes\n", (unsigned long)msg_len);
        dart_node_drain(n, cid, 30000);      /* wait for delivery, not a fixed grace */
        for (t = 0; t < 200; t += 20) dart_node_poll(n, 20);   /* brief flush */
        dart_node_close(n, 1);   /* BYE: peers drop us now, not after timeout */
        return 0;
    }

    /* prewritten input piped or redirected on stdin (not a terminal): send the
     * whole stream as messages with newlines intact, splitting only at the 64KB
     * sample cap, never one-per-line. */
    if (!stdin_is_tty()){
        int rc = publish_stream(n, cid, stdin, wait_ms, send_limit);
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
            size_t ln = strlen(line);
            while (ln && (line[ln-1] == '\n' || line[ln-1] == '\r')) line[--ln] = '\0';
            if (!ln) continue;
            lock_get(&g_lock);
            if (dart_node_send(n, cid, line, ln) < 0) fprintf(stderr, "send failed\n");
            lock_put(&g_lock);
        }
        g_pumping = 0;              /* stop the pump first: only we touch the node now */
#ifdef _WIN32
        WaitForSingleObject(th, INFINITE); CloseHandle(th);
#else
        pthread_join(th, NULL);
#endif
        dart_node_drain(n, cid, 30000);   /* deliver the last typed messages before BYE */
    }

    dart_node_close(n, 1);   /* BYE: peers drop us now instead of after timeout */
    return 0;
}
