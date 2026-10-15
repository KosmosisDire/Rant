/* The harness behind harness.h. Compiled into every test program next to its cases, so it
 * shares the program's build flags. It uses the declarations only, never the internals. */
#include "harness.h"
#include "wire_tap.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <time.h>
#endif

uint16_t test_domain_base;

static int  g_case_failed, g_case_skipped;
static long g_checks, g_failures;

void test_check(int ok, const char *file, int line, const char *fmt, ...){
    va_list ap;
    printf(ok ? "  ok   " : "  FAIL ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (ok) printf("\n");
    else printf("  (%s:%d)\n", file, line);
    g_checks++;
    if (!ok){ g_failures++; g_case_failed = 1; }
}

void test_skip(const char *why){
    printf("  SKIP %s\n", why);
    g_case_skipped = 1;
}

static void pick_domain_base(void){
    const char *env = getenv("RANT_TEST_DOMAIN");
    long v = env ? strtol(env, NULL, 10) : 0;
    if (v > 0 && v <= 65535 - TEST_DOMAINS) test_domain_base = (uint16_t)v;
    /* by hand: a block from the clock, past the small domains real nodes use */
    else test_domain_base = (uint16_t)(1000u + (unsigned)(i_rant_plat_now_us() % 900u) * TEST_DOMAINS);
}

/* A test stays on loopback: a socket bound to the network makes the Windows firewall ask
   and leaves a rule behind. POSIX discovery binds any by design, so only Windows checks */
static void check_binds(void){
#ifdef _WIN32
    static long long seen;
    long long now = wire_tap_read(&wire_tap.bind_off_loopback);
    if (now != seen){
        test_check(0, __FILE__, __LINE__, "%lld sockets bound outside loopback, the last on port %lld",
                   now - seen, wire_tap_read(&wire_tap.bind_off_loopback_port));
        seen = now;
    }
#endif
}

int test_main(int argc, char **argv, const TestCase *cases, int n){
    int i, k, ran = 0, skipped = 0;
    char failed[1024] = "";
    setvbuf(stdout, NULL, _IONBF, 0);   /* keep every line on a crash */
    if (argc == 2 && !strcmp(argv[1], "--list")){
        for (i = 0; i < n; i++) printf("%s\n", cases[i].name);
        return 0;
    }
    for (k = 1; k < argc; k++){
        for (i = 0; i < n && strcmp(argv[k], cases[i].name); i++) {}
        if (i == n){
            fprintf(stderr, "no case named %s, --list lists them\n", argv[k]);
            return 2;
        }
    }
    pick_domain_base();
    printf("domains %u to %u\n", (unsigned)TEST_DOMAIN, (unsigned)(TEST_DOMAIN + TEST_DOMAINS - 1));
    for (i = 0; i < n; i++){
        if (argc > 1){
            for (k = 1; k < argc && strcmp(argv[k], cases[i].name); k++) {}
            if (k == argc) continue;
        }
        printf("-- %s\n", cases[i].name);
        g_case_failed = g_case_skipped = 0;
        cases[i].run();
        check_binds();
        ran++;
        if (g_case_skipped && !g_case_failed) skipped++;
        if (g_case_failed && strlen(failed) + strlen(cases[i].name) + 2 < sizeof failed){
            strcat(failed, " ");
            strcat(failed, cases[i].name);
        }
    }
    if (g_failures){
        printf("RESULT: FAIL in%s (%ld of %ld checks)\n", failed, g_failures, g_checks);
        return 1;
    }
    if (ran && skipped == ran){
        printf("RESULT: SKIP\n");
        return 77;
    }
    printf("RESULT: PASS (%d cases, %ld checks)\n", ran, g_checks);
    return 0;
}

void test_sleep_ms(int ms){
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

void test_pump3(RantNode *a, RantNode *b, RantNode *c, int ms){
    uint64_t end = i_rant_plat_now_us() + (uint64_t)ms * 1000u;
    do {
        rant_node_poll(a, 1);
        if (b) rant_node_poll(b, 0);
        if (c) rant_node_poll(c, 0);
    } while (i_rant_plat_now_us() < end);
}

RantNode *test_node_open(const char *name, RantMsgFn on_msg, RantEventFn on_event,
                         RantNodeOpts opts, const i_RantTopicDef *defs, uint16_t n){
    RantAllocator alloc = rant_allocator_heap(0);
    RantNode *node;
    uint16_t i;
    if (!opts.max_topics) opts.max_topics = n ? n : 1;
    node = rant_node_open(&alloc, name, on_msg, on_event, &opts);
    if (!node) return NULL;
    for (i = 0; i < n; i++){
        RantTopicOpts co;
        memset(&co, 0, sizeof co);
        co.qos = defs[i].qos;
        if (!rant_node_create_topic(node, defs[i].name, (RantRole)defs[i].role, NULL, &co)){
            rant_node_close(node, 0);
            return NULL;
        }
    }
    return node;
}

unsigned long test_samples[8], test_gap_calls[8], test_gap_lost[8], test_messages;
unsigned long test_collisions;
char test_last_sender[64];

void test_on_message(const RantMsg *msg){
    if (msg->topic_index < 8) test_samples[msg->topic_index]++;
    test_last_sender[0] = '\0';
    if (msg->publisher_name.data){
        size_t n = msg->publisher_name.len < sizeof test_last_sender - 1
                 ? msg->publisher_name.len : sizeof test_last_sender - 1;
        memcpy(test_last_sender, msg->publisher_name.data, n);
        test_last_sender[n] = '\0';
    }
    test_messages++;
}

void test_on_event(const RantEvent *ev){
    if (ev->kind == RANT_MSG_LOST){
        if (ev->topic < 8){ test_gap_calls[ev->topic]++; test_gap_lost[ev->topic] += (unsigned long)ev->lost_count; }
    } else if (ev->kind == RANT_ERROR && ev->error == RANT_E_NAME_COLLISION){
        test_collisions++;
    }
}

int test_builtin(const RantEntityInfo *e){
    static const char *const primary[] = { "@rant/log/error", "@rant/log/warn", "@rant/log/info",
                                           "@rant/meta@req" };
    size_t i;
    if (e->name.len) return e->name.len >= 6 && memcmp(e->name.data, "@rant/", 6) == 0;
    for (i = 0; i < sizeof primary / sizeof primary[0]; i++)
        if (e->hash == (uint32_t)i_rant_topic_id(primary[i])) return 1;
    return 0;
}

uint16_t test_free_port(uint16_t port){
    uint16_t k;
    for (k = 0; k < 100; k++){
        i_RantSock s = i_rant_plat_udp_open();
        int ok = s != RANT_SOCK_BAD && i_rant_plat_bind(s, i_rant_plat_ipv4(127,0,0,1), (uint16_t)(port + k), 0);
        if (s != RANT_SOCK_BAD) i_rant_plat_close(s);
        if (ok) return (uint16_t)(port + k);
    }
    return port;
}

void test_setenv(const char *name, const char *value){
#ifdef _WIN32
    _putenv_s(name, value);     /* an empty value removes it */
#else
    if (*value) setenv(name, value, 1); else unsetenv(name);
#endif
}

static i_RantTransportState *pair_side(i_RantTopicDef *def, i_RantTransportConfig *cfg,
                                       RantAllocator *alloc, void **mem, const char *topic,
                                       RantQos qos, RantRole role){
    size_t need;
    memset(def, 0, sizeof *def);
    def->name = topic; def->qos = qos; def->role = role;
    cfg->topics = def; cfg->n_topics = 1; cfg->max_peers = 2;
    *alloc = rant_allocator_heap(0);
    cfg->allocator = rant_allocator_alloc; cfg->user = alloc;
    need = i_rant_transport_required_memory(cfg);
    *mem = malloc(need);
    return *mem ? i_rant_transport_init(*mem, need, cfg) : NULL;
}

int test_pair_open(TestPair *p, const char *topic, RantQos wq, RantQos rq,
                   const i_RantTransportConfig *callbacks){
    memset(p, 0, sizeof *p);
    if (callbacks) p->r_cfg = *callbacks;
    p->w = pair_side(&p->w_def, &p->w_cfg, &p->w_alloc, &p->w_mem, topic, wq, RANT_PUB_ONLY);
    p->r = pair_side(&p->r_def, &p->r_cfg, &p->r_alloc, &p->r_mem, topic, rq, RANT_SUB_ONLY);
    if (!p->w || !p->r){ test_pair_close(p); return 0; }
    i_rant_transport_peer_add(p->w, 2u, RANT_FRAG_SIZE);
    i_rant_transport_peer_add(p->r, 1u, RANT_FRAG_SIZE);
    return 1;
}

void test_pair_close(TestPair *p){
    if (p->w) i_rant_transport_destroy(p->w);
    if (p->r) i_rant_transport_destroy(p->r);
    free(p->w_mem); free(p->r_mem);
    rant_allocator_reset(&p->w_alloc); rant_allocator_reset(&p->r_alloc);
    memset(p, 0, sizeof *p);
}

int test_golden(int index, TestGolden *out){
    char line[1024];
    int at = 0, found = 0;
    FILE *f = fopen(RANT_TEST_GOLDEN, "r");
    if (!f) return 0;
    while (!found && fgets(line, sizeof line, f)){
        char hex[600] = "";
        unsigned long long h;
        size_t i;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (at++ != index) continue;
        memset(out, 0, sizeof *out);
        if (sscanf(line, "%63s %llx %599s", out->text, &h, hex) < 2) break;
        out->hash = (uint64_t)h;
        for (i = 0; hex[2*i] && hex[2*i+1] && i < sizeof out->wire; i++){
            unsigned b;
            if (sscanf(hex + 2*i, "%2x", &b) != 1) break;
            out->wire[i] = (uint8_t)b;
        }
        out->wire_len = i;
        found = 1;
    }
    fclose(f);
    return found;
}

uint64_t test_golden_hash(const char *text){
    TestGolden g;
    int i;
    for (i = 0; test_golden(i, &g); i++)
        if (!strcmp(g.text, text)) return g.hash;
    return 0;
}

void test_apply_verified(i_RantTransportState *dst, uint32_t src_id, i_RantTransportState *src){
    uint8_t ib[512], rq[512], rp[2048]; size_t il, rl, pl; uint16_t nw;
    i_RantDetailWant wl[16];
    il = i_rant_transport_build_interest(src, ib, sizeof ib);
    i_rant_transport_apply_peer_interest(dst, src_id, rant_bytes(ib, il));
    nw = i_rant_transport_detail_wants(dst, NULL, src_id, rant_bytes(ib, il), wl, 16);
    if (!nw) return;                       /* nothing shared, or already verified */
    rl = i_rant_detail_req_build(0, 0, wl, nw, rq, sizeof rq);
    pl = i_rant_transport_detail_respond(src, NULL, 0, rant_bytes(rq, rl), rp, sizeof rp);
    i_rant_transport_apply_peer_details(dst, src_id, rant_bytes(rp, pl));
    i_rant_transport_apply_peer_interest(dst, src_id, rant_bytes(ib, il));
}
