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
 *   --wait MS      publisher settle + delivery grace per message (default 1000)
 *   --file <name>  pub: publish the named file's contents (whole, like piped
 *                  stdin); sub: save every received message to the named file
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
#else
  #include <unistd.h>
  static int stdin_is_tty(void){ return isatty(fileno(stdin)); }
#endif

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

/* subscriber callback: keep it cheap, never call back into dart_*. Printing the
 * payload as text is fine. */
static void on_sample(void *u, uint16_t ch, uint32_t from, const void *data, size_t len){
    (void)u;
    if (g_outfile){                       /* --file: save raw bytes, keep the console quiet */
        fwrite(data, 1, len, g_outfile);
        fflush(g_outfile);                /* flush each: a Ctrl-C must not lose messages */
        printf("[ch %u <- peer %u] saved %lu bytes\n", ch, from, (unsigned long)len);
    } else {
        printf("[ch %u <- peer %u] %.*s\n", ch, from, (int)len, (const char*)data);
    }
}

static void usage(void){
    fprintf(stderr,
        "usage:\n"
        "  pubsub sub <channel> [opts]\n"
        "  pubsub pub <channel> [text...] [opts]   (no text = read lines from stdin)\n"
        "opts: --domain N  --mcast  --if <ip>  --peer <ip>  --best-effort  --wait MS  --file <name>\n");
}

/* Publish a whole byte stream from f as one or more messages: newlines are kept,
 * and it splits only when a read fills the 64KB buffer, never per line. Settles
 * before the first send so a freshly discovered subscriber is already matched,
 * and grants delivery grace after the last. */
static void publish_stream(dart_node *n, uint16_t cid, FILE *f, int wait_ms,
                           char *buf, size_t cap){
    int t; size_t r;
    for (t = 0; t < wait_ms; t += 20) dart_node_poll(n, 20);
    while ((r = fread(buf, 1, cap, f)) > 0){
        if (dart_node_send(n, cid, buf, r) < 0) fprintf(stderr, "send failed\n");
        else printf("[pub] sent %lu bytes\n", (unsigned long)r);
        dart_node_poll(n, 0);
    }
    for (t = 0; t < wait_ms; t += 20) dart_node_poll(n, 20);
}

int main(int argc, char **argv){
    const char *mode = NULL, *chan = NULL, *if_ip = NULL, *peer_ip = NULL, *file_name = NULL;
    uint16_t domain = 7;
    int mcast = 0, reliable = 1, wait_ms = 1000;
    char msg[65536]; size_t msg_len = 0;  /* one-shot publish text, if any */
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);

    /* Parse: recognized --flags anywhere; the rest are positionals
     * [mode, channel, message words...]. Message words join with spaces. */
    for (i = 1; i < argc; i++){
        const char *a = argv[i];
        if      (!strcmp(a, "--domain") && i+1 < argc) domain  = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(a, "--if")     && i+1 < argc) if_ip   = argv[++i];
        else if (!strcmp(a, "--peer")   && i+1 < argc) peer_ip = argv[++i];
        else if (!strcmp(a, "--wait")   && i+1 < argc) wait_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--file")   && i+1 < argc) file_name = argv[++i];
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

    if (!mode || !chan){ usage(); return 2; }
    int is_pub = !strcmp(mode, "pub");
    int is_sub = !strcmp(mode, "sub");
    if (!is_pub && !is_sub){ fprintf(stderr, "mode must be 'pub' or 'sub'\n"); usage(); return 2; }

    uint16_t cid; const char *cname;
    chan_from_token(chan, &cid, &cname);

    dart_channel_def ch; memset(&ch, 0, sizeof ch);
    ch.channel_id           = cid;
    ch.name                 = cname;
    ch.qos.reliability      = reliable ? DART_RELIABLE : DART_BEST_EFFORT;
    ch.qos.history_depth    = 16;
    ch.qos.join_replay      = 8;        /* late subscribers see recent history */
    ch.qos.max_sample_bytes = sizeof msg;
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
    cfg.so_rcvbuf  = 4<<20;        /* 4 MB: absorb bursts of fragments (a 64KB    */
    cfg.so_sndbuf  = 4<<20;        /* sample is 64 TUs) so large transfers don't drop */
    if (if_ip)      cfg.mcast_if = if_ip;            /* multihomed: pin it */
    else if (mcast) cfg.mcast_if = "127.0.0.1";      /* same-host: stay local */

    dart_discovery_addr seed;
    if (peer_ip){
        memset(&seed, 0, sizeof seed);
        if (parse_ipv4(peer_ip, seed.ip) < 0){ fprintf(stderr, "bad --peer ip %s\n", peer_ip); return 2; }
        seed.ip_len = 4;           /* port 0 = use the discovery port */
        cfg.seeds = &seed; cfg.n_seeds = 1;
    }

    static uint8_t mem[8<<20];   /* 8 MB: 64KB samples x history depth x peers + meta */
    dart_node *n = dart_node_open(mem, sizeof mem, &cfg);
    if (!n){ fprintf(stderr, "dart_node_open failed (need %lu bytes)\n",
                     (unsigned long)dart_node_required_memory(&cfg)); return 1; }

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
        publish_stream(n, cid, f, wait_ms, msg, sizeof msg - 1);
        fclose(f);
        dart_node_close(n, 1);
        return 0;
    }

    if (msg_len){
        /* one-shot: settle so a waiting subscriber is already a matched reader,
         * publish the CLI text, pump for delivery + acks, then exit. */
        int t;
        for (t = 0; t < wait_ms; t += 20) dart_node_poll(n, 20);
        if (dart_node_send(n, cid, msg, msg_len) < 0)
            fprintf(stderr, "send failed\n");
        else
            printf("[pub] sent %lu bytes\n", (unsigned long)msg_len);
        for (t = 0; t < wait_ms; t += 20) dart_node_poll(n, 20);
        dart_node_close(n, 1);   /* BYE: peers drop us now, not after timeout */
        return 0;
    }

    /* prewritten input piped or redirected on stdin (not a terminal): send the
     * whole stream as messages with newlines intact, splitting only at the 64KB
     * sample cap, never one-per-line. */
    if (!stdin_is_tty()){
        publish_stream(n, cid, stdin, wait_ms, msg, sizeof msg - 1);
        dart_node_close(n, 1);
        return 0;
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
        sleep_ms(wait_ms);          /* let the last message reach its readers */
        g_pumping = 0;              /* stop and join before we touch the node */
#ifdef _WIN32
        WaitForSingleObject(th, INFINITE); CloseHandle(th);
#else
        pthread_join(th, NULL);
#endif
    }

    dart_node_close(n, 1);   /* BYE: peers drop us now instead of after timeout */
    return 0;
}
