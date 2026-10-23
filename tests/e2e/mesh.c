/* What a node must still do on a big mesh or one split in segments: hear a seeded peer it
 * shares nothing with, answer and ask meta from anywhere, keep a static arena bounded, give a
 * slot only to a peer that matches and give it back, and let an observer see the mesh fast.
 * Public API only, so the same cases run against any Rant. spec/discovery.md, the directory. */
#include "support/test.h"

#define MS_CROWD 72

typedef struct { long long up, down, refused, matched; } MsEvents;
static void ms_on_event(const RantEvent *ev){
    MsEvents *e = (MsEvents*)ev->user;
    if (!e) return;
    if (ev->kind == RANT_PEER_UP) e->up++;
    if (ev->kind == RANT_PEER_DOWN) e->down++;
    if (ev->kind == RANT_ERROR && ev->error == RANT_E_PEER_REFUSED) e->refused++;
    if (ev->kind == RANT_PEER_INTEREST && (ev->publish_topics || ev->receive_topics)) e->matched++;
}

static RantNodeOpts ms_opts(uint16_t domain, uint16_t max_peers, uint16_t max_topics, uint32_t interval_us){
    static RantAddr seed;
    RantNodeOpts o;
    memset(&seed, 0, sizeof seed); seed.ip[0] = 127; seed.ip[3] = 1; seed.ip_len = 4;
    memset(&o, 0, sizeof o);
    o.domain = domain; o.disable_shm = 1; o.max_topics = max_topics;
    o.discovery.max_peers = max_peers;
    o.discovery.announce_interval_us = interval_us;
    o.net.multicast_interface = "127.0.0.1"; o.net.seed_peers = &seed; o.net.n_seed_peers = 1;
    return o;
}

static RantNode *ms_heap(const char *name, RantNodeOpts *o){
    RantAllocator a = rant_allocator_heap(0);
    return rant_node_open(&a, name, NULL, ms_on_event, o);
}

static int ms_active(RantNode *n){
    RantIter it; RantPeerInfo p; int c = 0;
    memset(&it, 0, sizeof it);
    while (rant_node_peers_next(n, &it, &p)) if (p.liveness == RANT_PEER_ACTIVE) c++;
    return c;
}
static uint32_t ms_peer_id(RantNode *n, const char *name){
    RantIter it; RantPeerInfo p; size_t l = strlen(name);
    memset(&it, 0, sizeof it);
    while (rant_node_peers_next(n, &it, &p))
        if (p.liveness == RANT_PEER_ACTIVE && p.name.len == l && !memcmp(p.name.data, name, l)) return p.id;
    return 0;
}
static int ms_liveness(RantNode *n, const char *name){
    RantIter it; RantPeerInfo p; size_t l = strlen(name);
    memset(&it, 0, sizeof it);
    while (rant_node_peers_next(n, &it, &p))
        if (p.name.len == l && !memcmp(p.name.data, name, l)) return (int)p.liveness;
    return -1;
}
static void ms_wait(int ms){
    uint64_t end = i_rant_plat_now_us() + (uint64_t)ms * 1000u;
    while (i_rant_plat_now_us() < end) test_sleep_ms(5);
}
#define MS_WAIT_UNTIL(ms, cond) do { \
        uint64_t ms_end_ = i_rant_plat_now_us() + (uint64_t)(ms) * 1000u; \
        while (!(cond) && i_rant_plat_now_us() < ms_end_) test_sleep_ms(2); \
    } while (0)

/* started peers on service threads, each with its own topics that no case node wants */
static RantNode *ms_crowd[MS_CROWD]; static int ms_ncrowd;
static void ms_crowd_open(uint16_t domain, int n, int topics, uint32_t interval_us){
    int k, t; char name[32], tp[40];
    for (k = 0; k < n && ms_ncrowd < MS_CROWD; k++){
        RantNodeOpts o = ms_opts(domain, 0, (uint16_t)(topics + 2), interval_us);
        RantNode *c;
        sprintf(name, "ms-crowd%d", ms_ncrowd);
        c = ms_heap(name, &o);
        if (!c) continue;
        for (t = 0; t < topics; t++){
            sprintf(tp, "ms/crowd%d/t%d", ms_ncrowd, t);
            rant_node_create_topic(c, tp, RANT_PUB_ONLY, NULL, NULL);
        }
        rant_node_start(c);
        ms_crowd[ms_ncrowd++] = c;
    }
}
static void ms_crowd_close(void){
    int k;
    for (k = 0; k < ms_ncrowd; k++) rant_node_close(ms_crowd[k], 1);
    ms_ncrowd = 0;
}
static int ms_crowd_all_see(const char *name){
    int k;
    for (k = 0; k < ms_ncrowd; k++) if (!ms_peer_id(ms_crowd[k], name)) return 0;
    return 1;
}

/* Two segments that multicast does not cross, joined by A's seed. B shares nothing with A,
   yet each keeps hearing the other: unicast is answered with unicast. */
static void seeded_segments_checks(void){
    MsEvents ea, eb; RantAddr sb; RantNode *A, *B; int t, lost_b = 0, lost_a = 0;
    uint16_t dom = (uint16_t)(TEST_DOMAIN + 1);
    uint16_t port = (uint16_t)(20000u + ((unsigned)(TEST_DOMAIN - 1000u) / TEST_DOMAINS) * 2u);
    RantNodeOpts oa = ms_opts(dom, 0, 4, 200000), ob = ms_opts(dom, 0, 4, 200000);
    memset(&ea, 0, sizeof ea); memset(&eb, 0, sizeof eb);
    memset(&sb, 0, sizeof sb); sb.ip[0] = 127; sb.ip[3] = 1; sb.ip_len = 4; sb.port = (uint16_t)(port + 1);
    oa.net.discovery_port = port; oa.net.seed_peers = &sb; oa.net.n_seed_peers = 1;
    ob.net.discovery_port = (uint16_t)(port + 1); ob.net.seed_peers = NULL; ob.net.n_seed_peers = 0;
    oa.discovery.peer_timeout_us = ob.discovery.peer_timeout_us = 800000;
    oa.user_data = &ea; ob.user_data = &eb;
    A = ms_heap("ms-seg-a", &oa); B = ms_heap("ms-seg-b", &ob);
    CHECK(A && B, "seeded segments: both open");
    if (!(A && B)) goto done;
    PUMP_UNTIL(A, B, 4000, ms_peer_id(A, "ms-seg-b") && ms_peer_id(B, "ms-seg-a"));
    CHECK(ms_peer_id(A, "ms-seg-b") && ms_peer_id(B, "ms-seg-a"), "seeded segments: they find each other");
    for (t = 0; t < 30; t++){             /* 3 s, nearly four peer timeouts */
        test_pump(A, B, 100);
        if (ms_liveness(A, "ms-seg-b") != RANT_PEER_ACTIVE) lost_b++;
        if (ms_liveness(B, "ms-seg-a") != RANT_PEER_ACTIVE) lost_a++;
    }
    CHECK(lost_b == 0 && ea.down == 0, "seeded segments: A keeps hearing B (lost %d of 30, down %lld)",
          lost_b, ea.down);
    CHECK(lost_a == 0 && eb.down == 0, "seeded segments: B keeps hearing A (lost %d of 30, down %lld)",
          lost_a, eb.down);
done:
    if (A) rant_node_close(A, 1);
    if (B) rant_node_close(B, 1);
}

/* A node that asks meta only from its own callbacks still gets answers. */
static RantNode *mcb_node; static uint32_t mcb_target; static int mcb_asks, mcb_ok, mcb_failed;
static void mcb_ask(void);
static void mcb_on_response(const RantResponse *r){
    if (r->status == RANT_CALL_OK) mcb_ok++; else mcb_failed++;
    if (!mcb_ok) mcb_ask();
}
static void mcb_ask(void){
    uint8_t mask[4] = { 1, 0, 0, 0 };     /* RANT_META_NODE */
    RantFunction *fn = rant_node_meta_function(mcb_node);
    if (!fn || mcb_asks >= 4) return;
    mcb_asks++;
    if (rant_function_call_async(fn, rant_bytes(mask, 4), mcb_on_response, NULL,
                                 &(RantCallOpts){ .provider = mcb_target }) != RANT_OK) mcb_failed++;
}
static void mcb_on_event(const RantEvent *ev){
    if (ev->kind == RANT_PEER_UP && !mcb_target){ mcb_target = ev->peer; mcb_ask(); }
}
static void meta_from_callbacks_checks(void){
    RantAllocator h = rant_allocator_heap(0);
    uint16_t dom = (uint16_t)(TEST_DOMAIN + 2);
    RantNodeOpts oa = ms_opts(dom, 0, 4, 500000), ob = ms_opts(dom, 0, 4, 500000);
    RantNode *A;
    mcb_target = 0; mcb_asks = mcb_ok = mcb_failed = 0;
    A = ms_heap("ms-mcb-a", &oa);
    mcb_node = rant_node_open(&h, "ms-mcb-b", NULL, mcb_on_event, &ob);
    CHECK(A && mcb_node, "meta from callbacks: both open");
    if (A && mcb_node) PUMP_UNTIL(A, mcb_node, 12000, mcb_ok > 0);
    CHECK(mcb_ok > 0, "meta from callbacks: answered (asks %d, ok %d, failed %d)", mcb_asks, mcb_ok, mcb_failed);
    if (A) rant_node_close(A, 1);
    if (mcb_node) rant_node_close(mcb_node, 1);
}

/* A call aimed at one provider is held until that provider matches, never sent the moment
   another one did. Once for a function, once for the first meta call among other nodes. */
static RantFunction *dfc_remote; static uint32_t dfc_first; static int dfc_called, dfc_status = -1, dfc_rc;
static void dfc_on_response(const RantResponse *r){ dfc_status = (int)r->status; }
static void dfc_on_event(const RantEvent *ev){
    if (ev->kind != RANT_PEER_UP || !dfc_remote || !dfc_first || ev->peer == dfc_first || dfc_called) return;
    dfc_called = 1;     /* the second provider just came up: its function match is still forming */
    dfc_rc = rant_function_call_async(dfc_remote, rant_bytes(NULL, 0), dfc_on_response, NULL,
                                      &(RantCallOpts){ .provider = ev->peer });
}
static void dfc_handler(RantRequest *req, void *user){ (void)req; (void)user; }
static void directed_first_call_checks(void){
    RantAllocator h = rant_allocator_heap(0);
    uint16_t dom = (uint16_t)(TEST_DOMAIN + 3);
    RantNodeOpts oc = ms_opts(dom, 0, 8, 500000), o1 = ms_opts(dom, 0, 8, 500000), o2 = ms_opts(dom, 0, 8, 500000);
    RantNode *C, *P1, *P2 = NULL;
    dfc_remote = NULL; dfc_first = 0; dfc_called = 0; dfc_status = -1; dfc_rc = 0;
    C = rant_node_open(&h, "ms-dfc-c", NULL, dfc_on_event, &oc);
    P1 = ms_heap("ms-dfc-p1", &o1);
    CHECK(C && P1, "directed first call: caller and first provider open");
    if (!(C && P1)) goto meta;
    rant_node_create_function_definition(P1, "ms/f", NULL, NULL, dfc_handler, NULL, NULL);
    dfc_remote = rant_node_create_remote_function(C, "ms/f", NULL, NULL, NULL);
    PUMP_UNTIL(C, P1, 6000, rant_function_match_count(dfc_remote) == 1);
    dfc_first = ms_peer_id(C, "ms-dfc-p1");
    CHECK(rant_function_match_count(dfc_remote) == 1 && dfc_first, "directed first call: one provider matched");
    P2 = ms_heap("ms-dfc-p2", &o2);
    if (P2) rant_node_create_function_definition(P2, "ms/f", NULL, NULL, dfc_handler, NULL, NULL);
    PUMP3_UNTIL(C, P1, P2, 6000, dfc_status >= 0);
    CHECK(dfc_called && dfc_rc == RANT_OK && dfc_status == RANT_CALL_OK,
          "directed first call: a call aimed at a provider still matching is answered (rc %d, status %d)",
          dfc_rc, dfc_status);
meta:
    if (P2) rant_node_close(P2, 1);
    if (P1) rant_node_close(P1, 1);
    if (C) rant_node_close(C, 1);
    {   RantNodeOpts ot = ms_opts(dom, 0, 4, 500000), oo = ms_opts(dom, 0, 4, 500000);
        RantNode *T, *O; RantResponse out; uint32_t id; int r = 0; uint8_t mask[4] = { 1, 0, 0, 0 };
        ms_crowd_open(dom, 6, 1, 500000);
        T = ms_heap("ms-dfc-t", &ot); O = ms_heap("ms-dfc-o", &oo);
        if (T) rant_node_start(T);
        if (O) rant_node_start(O);
        MS_WAIT_UNTIL(6000, O && ms_active(O) == 7);
        id = O ? ms_peer_id(O, "ms-dfc-t") : 0;
        CHECK(id != 0, "directed first call: the observer knows the target among six others");
        if (id) r = rant_function_call(rant_node_meta_function(O), rant_bytes(mask, 4), &out, 3000,
                                       &(RantCallOpts){ .provider = id });
        CHECK(r == 1 && out.status == RANT_CALL_OK,
              "directed first call: the first meta call is answered (ret %d, status %d)", r, r == 1 ? (int)out.status : -1);
        if (O) rant_node_close(O, 1);
        if (T) rant_node_close(T, 1);
        ms_crowd_close();
    }
}

/* A static node's memory for peers that share nothing with it stays bounded, however many
   there are. Four times the peers here cost it next to nothing. */
static void static_bounded_checks(void){
    static uint8_t abuf[1u << 20];
    RantAllocator a = rant_allocator_static(abuf, sizeof abuf);
    uint16_t dom = (uint16_t)(TEST_DOMAIN + 4);
    RantNodeOpts o = ms_opts(dom, 2, 4, 500000);
    RantNode *T; size_t m10 = 0, m40 = 0;
    T = rant_node_open(&a, "ms-static", NULL, NULL, &o);
    CHECK(T != NULL, "static bounded: opens");
    if (!T) return;
    rant_node_start(T);
    ms_crowd_open(dom, 10, 20, 500000);
    MS_WAIT_UNTIL(8000, ms_crowd_all_see("ms-static"));
    ms_wait(1500);
    rant_node_mem_stats(T, &m10, NULL, NULL);
    ms_crowd_open(dom, 30, 20, 500000);
    MS_WAIT_UNTIL(8000, ms_crowd_all_see("ms-static"));
    ms_wait(1500);
    rant_node_mem_stats(T, &m40, NULL, NULL);
    CHECK(m40 <= m10 + 2048u, "static bounded: 30 more peers cost %ld bytes", (long)m40 - (long)m10);
    rant_node_close(T, 1);
    ms_crowd_close();
}

/* An observer asking a static node for its peers among many gets an answer whose size and
   cost do not grow with the mesh, listing and counting only the peer it matched. */
static void static_meta_peers_checks(void){
    static uint8_t abuf[1u << 20];
    RantAllocator a = rant_allocator_static(abuf, sizeof abuf);
    uint16_t dom = (uint16_t)(TEST_DOMAIN + 5);
    RantNodeOpts ot = ms_opts(dom, 4, 4, 500000), oo = ms_opts(dom, 0, 4, 500000);
    RantNode *T, *O; RantResponse out; uint32_t id; int r = 0;
    size_t before = 0, after = 0; uint8_t mask[4] = { 9, 0, 0, 0 };     /* RANT_META_NODE and PEERS */
    T = rant_node_open(&a, "ms-robot", NULL, NULL, &ot);
    O = ms_heap("ms-observer", &oo);
    CHECK(T && O, "static meta peers: open");
    if (!(T && O)) goto done;
    (void)rant_node_meta_function(O);     /* the asking side open from the start */
    rant_node_start(T); rant_node_start(O);
    ms_crowd_open(dom, 30, 1, 500000);
    MS_WAIT_UNTIL(8000, ms_crowd_all_see("ms-robot") && ms_peer_id(O, "ms-robot"));
    ms_wait(1500);
    id = ms_peer_id(O, "ms-robot");
    rant_node_mem_stats(T, &before, NULL, NULL);
    if (id) r = rant_function_call(rant_node_meta_function(O), rant_bytes(mask, 4), &out, 3000,
                                   &(RantCallOpts){ .provider = id });
    rant_node_mem_stats(T, &after, NULL, NULL);
    CHECK(r == 1 && out.status == RANT_CALL_OK, "static meta peers: answered (ret %d, status %d)",
          r, r == 1 ? (int)out.status : -1);
    CHECK(r == 1 && out.data.len <= 2048u, "static meta peers: the answer stays small among 30 peers (%u bytes)",
          r == 1 ? (unsigned)out.data.len : 0u);
    CHECK(after <= before + 8192u, "static meta peers: the ask costs the node %ld bytes", (long)after - (long)before);
    if (r == 1 && out.status == RANT_CALL_OK && out.schema){
        RantBytes info = rant_get_map(out.data, out.schema, "info");
        RantValue node, peers, matched, live;
        int ok = rant_map_get(info, "node", &node) && rant_map_get(info, "peers", &peers)
              && rant_map_get(node.bytes, "matched_peers", &matched) && rant_map_get(node.bytes, "peers", &live);
        CHECK(ok && matched.v.u == 1 && peers.count == 1 && live.v.u > 1,
              "static meta peers: only the observer is matched and listed (matched %u, listed %u, heard %u)",
              ok ? (unsigned)matched.v.u : 0u, ok ? (unsigned)peers.count : 0u, ok ? (unsigned)live.v.u : 0u);
    }
done:
    if (O) rant_node_close(O, 1);
    if (T) rant_node_close(T, 1);
    ms_crowd_close();
}

/* An observer, the way the CLI completes a name, sees every peer within 100 ms of opening. */
static void observer_fast_checks(void){
    uint16_t dom = (uint16_t)(TEST_DOMAIN + 6);
    RantNodeOpts o = ms_opts(dom, 256, 16, 500000);
    RantNode *O; int k, seen;
    ms_crowd_open(dom, 70, 1, 500000);
    for (k = 0; k < 100; k++){            /* the crowd knows itself before the observer comes */
        int i, all = 1;
        for (i = 0; i < ms_ncrowd && all; i++) if (ms_active(ms_crowd[i]) < ms_ncrowd - 1) all = 0;
        if (all) break;
        ms_wait(100);
    }
    ms_wait(1000);
    o.fetch_details = 1; o.disable_logs = 1;
    O = ms_heap("@rant/ms-observer", &o);
    CHECK(O != NULL, "observer fast: opens");
    if (!O){ ms_crowd_close(); return; }
    (void)rant_node_settle(O, 100);
    seen = ms_active(O);
    CHECK(seen == ms_ncrowd, "observer fast: every peer seen within 100 ms (%d of %d)", seen, ms_ncrowd);
    rant_node_close(O, 1);
    ms_crowd_close();
}

/* Polls n alone until cond or ms passed. */
#define MS_POLL_UNTIL(n, ms, cond) do { \
        uint64_t ms_end_ = i_rant_plat_now_us() + (uint64_t)(ms) * 1000u; \
        while (!(cond) && i_rant_plat_now_us() < ms_end_) rant_node_poll((n), 2); \
    } while (0)

/* A topic created on a node that already knows its subscribers matches every one at once,
   even past the table it started with. The node polls in bursts, so every answer lands in
   one tick and the table grows to fit them all. */
static void grow_at_once_checks(void){
    uint16_t dom = (uint16_t)(TEST_DOMAIN + 7);
    RantNodeOpts o = ms_opts(dom, 2, 4, 3000000);
    RantNode *C, *subs[12]; RantTopic *pub; int k, n = 0; uint64_t t0; double ms = -1;
    C = ms_heap("ms-grow", &o);
    CHECK(C != NULL, "grow at once: opens");
    if (!C) return;
    for (k = 0; k < 12; k++){
        RantNodeOpts so = ms_opts(dom, 0, 2, 3000000); char name[32];
        sprintf(name, "ms-grow-sub%d", k);
        subs[n] = ms_heap(name, &so);
        if (!subs[n]) continue;
        rant_node_create_topic(subs[n], "ms/grow", RANT_SUB_ONLY, NULL, NULL);
        rant_node_start(subs[n]); n++;
    }
    MS_POLL_UNTIL(C, 15000, ms_active(C) == n);
    CHECK(ms_active(C) == n, "grow at once: all %d subscribers known (%d)", n, ms_active(C));
    MS_POLL_UNTIL(C, 500, 0);
    pub = rant_node_create_topic(C, "ms/grow", RANT_PUB_ONLY, NULL, NULL);
    rant_node_poll(C, 0);                 /* the announce and the fetches leave */
    ms_wait(300);                         /* every answer waits in the socket */
    t0 = i_rant_plat_now_us();
    MS_POLL_UNTIL(C, 5000, pub && rant_topic_match_count(pub) == n);
    if (pub && rant_topic_match_count(pub) == n) ms = (double)(i_rant_plat_now_us() - t0) / 1000.0;
    CHECK(ms >= 0 && ms < 200.0, "grow at once: all %d matched within 200 ms of the answers (%.0f ms, %d matched)",
          n, ms, pub ? rant_topic_match_count(pub) : 0);
    rant_node_close(C, 1);
    for (k = 0; k < n; k++) rant_node_close(subs[k], 1);
}

/* A peer too big for one announce that shares nothing with a one slot node never takes that
   slot from the subscriber that needs it. */
static void big_peer_checks(void){
    static uint8_t abuf[1u << 20];
    RantAllocator a = rant_allocator_static(abuf, sizeof abuf);
    MsEvents et;
    uint16_t dom = (uint16_t)(TEST_DOMAIN + 8);
    RantNodeOpts ot = ms_opts(dom, 1, 4, 500000), ob = ms_opts(dom, 0, 320, 500000), os = ms_opts(dom, 0, 2, 500000);
    RantNode *T, *B = NULL, *S = NULL; RantTopic *pub = NULL; int k; char tp[32];
    memset(&et, 0, sizeof et); ot.user_data = &et;
    T = rant_node_open(&a, "ms-one-slot", NULL, ms_on_event, &ot);
    CHECK(T != NULL, "big peer: opens");
    if (!T) return;
    pub = rant_node_create_topic(T, "ms/needed", RANT_PUB_ONLY, NULL, NULL);
    rant_node_start(T);
    B = ms_heap("ms-big", &ob);
    if (B){
        for (k = 0; k < 300; k++){ sprintf(tp, "ms/big/t%d", k); rant_node_create_topic(B, tp, RANT_PUB_ONLY, NULL, NULL); }
        rant_node_start(B);
    }
    MS_WAIT_UNTIL(6000, B && ms_peer_id(B, "ms-one-slot"));
    ms_wait(1500);
    S = ms_heap("ms-needs", &os);
    if (S){ rant_node_create_topic(S, "ms/needed", RANT_SUB_ONLY, NULL, NULL); rant_node_start(S); }
    MS_WAIT_UNTIL(6000, pub && rant_topic_match_count(pub) == 1);
    CHECK(pub && rant_topic_match_count(pub) == 1 && et.refused == 0,
          "big peer: the subscriber matched, nobody refused (matched %d, refused %lld)",
          pub ? rant_topic_match_count(pub) : 0, et.refused);
    if (S) rant_node_close(S, 1);
    if (B) rant_node_close(B, 1);
    rant_node_close(T, 1);
}

/* A peer that stops sharing a topic gives its slot back, so the next one that needs it gets it. */
static void slot_back_checks(void){
    static uint8_t abuf[1u << 20];
    RantAllocator a = rant_allocator_static(abuf, sizeof abuf);
    uint16_t dom = (uint16_t)(TEST_DOMAIN + 9);
    RantNodeOpts ot = ms_opts(dom, 1, 4, 500000), ox = ms_opts(dom, 0, 2, 500000), oy = ms_opts(dom, 0, 2, 500000);
    RantNode *T, *X = NULL, *Y = NULL; RantTopic *pub = NULL, *xs = NULL;
    T = rant_node_open(&a, "ms-slot", NULL, NULL, &ot);
    CHECK(T != NULL, "slot back: opens");
    if (!T) return;
    pub = rant_node_create_topic(T, "ms/slot", RANT_PUB_ONLY, NULL, NULL);
    rant_node_start(T);
    X = ms_heap("ms-slot-x", &ox);
    if (X){ xs = rant_node_create_topic(X, "ms/slot", RANT_SUB_ONLY, NULL, NULL); rant_node_start(X); }
    MS_WAIT_UNTIL(6000, pub && rant_topic_match_count(pub) == 1);
    CHECK(pub && rant_topic_match_count(pub) == 1, "slot back: the first subscriber matched");
    if (xs) rant_topic_retire(xs);
    MS_WAIT_UNTIL(6000, pub && rant_topic_match_count(pub) == 0);
    Y = ms_heap("ms-slot-y", &oy);
    if (Y){ rant_node_create_topic(Y, "ms/slot", RANT_SUB_ONLY, NULL, NULL); rant_node_start(Y); }
    MS_WAIT_UNTIL(6000, pub && rant_topic_match_count(pub) == 1);
    CHECK(pub && rant_topic_match_count(pub) == 1 && X && ms_liveness(T, "ms-slot-x") == RANT_PEER_ACTIVE,
          "slot back: the next subscriber gets the slot while the first lives (matched %d)",
          pub ? rant_topic_match_count(pub) : 0);
    if (Y) rant_node_close(Y, 1);
    if (X) rant_node_close(X, 1);
    rant_node_close(T, 1);
}

/* A static node whose records are all taken by peers it shares nothing with still matches a
   subscriber that opens last, in the place of one of them, and turns nobody away. */
static void crowd_late_match_checks(void){
    static uint8_t abuf[1u << 20];
    RantAllocator a = rant_allocator_static(abuf, sizeof abuf);
    uint16_t dom = (uint16_t)(TEST_DOMAIN + 12);
    RantNodeOpts ot = ms_opts(dom, 2, 4, 500000), ol = ms_opts(dom, 0, 2, 500000);
    MsEvents et; RantNode *T, *L = NULL; RantTopic *pub; int known;
    memset(&et, 0, sizeof et);
    ot.user_data = &et;
    T = rant_node_open(&a, "ms-late", NULL, ms_on_event, &ot);
    CHECK(T != NULL, "crowd late match: opens");
    if (!T) return;
    pub = rant_node_create_topic(T, "ms/late", RANT_PUB_ONLY, NULL, NULL);
    rant_node_start(T);
    ms_crowd_open(dom, 30, 1, 500000);
    MS_WAIT_UNTIL(8000, ms_crowd_all_see("ms-late"));
    ms_wait(1000);
    known = ms_active(T);
    L = ms_heap("ms-late-sub", &ol);
    if (L){ rant_node_create_topic(L, "ms/late", RANT_SUB_ONLY, NULL, NULL); rant_node_start(L); }
    MS_WAIT_UNTIL(6000, pub && rant_topic_match_count(pub) == 1);
    CHECK(known == 8 && pub && rant_topic_match_count(pub) == 1 && et.refused == 0,
          "crowd late match: the late subscriber matched, nobody turned away (known %d, matched %d, refused %lld)",
          known, pub ? rant_topic_match_count(pub) : 0, et.refused);
    if (L) rant_node_close(L, 1);
    rant_node_close(T, 1);
    ms_crowd_close();
}

/* A static node with a static bulk keeps its records and peer blobs out of the main buffer:
   it opens smaller than the same node without one, a crowd leaves the main flat, a late
   subscriber still matches, and the meta answer reports the bulk. */
static void static_bulk_checks(void){
    static uint8_t abuf[1u << 20], bbuf[1u << 20], cbuf[1u << 20];
    RantAllocator a = rant_allocator_static(abuf, sizeof abuf), b = rant_allocator_static(bbuf, sizeof bbuf);
    RantAllocator c = rant_allocator_static(cbuf, sizeof cbuf);
    uint16_t dom = (uint16_t)(TEST_DOMAIN + 13);
    RantNodeOpts ot = ms_opts(dom, 2, 4, 500000), ol = ms_opts(dom, 0, 2, 500000);
    MsEvents et; RantNode *T, *U, *L = NULL; RantTopic *pub; RantResponse out;
    size_t whole = 0, open = 0, m10 = 0, m30 = 0; int known, r = 0; uint32_t id = 0;
    uint8_t mask[4] = { 1, 0, 0, 0 };     /* RANT_META_NODE */
    U = rant_node_open(&c, "ms-bulk-ref", NULL, NULL, &ot);
    if (U){ rant_node_mem_stats(U, &whole, NULL, NULL); rant_node_close(U, 0); }
    memset(&et, 0, sizeof et);
    ot.user_data = &et; ot.bulk = &b;
    T = rant_node_open(&a, "ms-bulk", NULL, ms_on_event, &ot);
    if (T) rant_node_mem_stats(T, &open, NULL, NULL);
    CHECK(T && U && open + 1024u <= whole, "static bulk: opens %ld bytes smaller than without a bulk",
          (long)whole - (long)open);
    if (!T) return;
    pub = rant_node_create_topic(T, "ms/bulk", RANT_PUB_ONLY, NULL, NULL);
    rant_node_start(T);
    ms_crowd_open(dom, 10, 20, 500000);
    MS_WAIT_UNTIL(8000, ms_crowd_all_see("ms-bulk"));
    ms_wait(1000);
    rant_node_mem_stats(T, &m10, NULL, NULL);
    ms_crowd_open(dom, 20, 20, 500000);
    MS_WAIT_UNTIL(8000, ms_crowd_all_see("ms-bulk"));
    ms_wait(1000);
    rant_node_mem_stats(T, &m30, NULL, NULL);
    known = ms_active(T);
    CHECK(m30 <= m10 + 2048u && known == 8, "static bulk: 20 more peers cost the main %ld bytes (known %d)",
          (long)m30 - (long)m10, known);
    L = ms_heap("ms-bulk-sub", &ol);
    if (L){ rant_node_create_topic(L, "ms/bulk", RANT_SUB_ONLY, NULL, NULL); (void)rant_node_meta_function(L); rant_node_start(L); }
    MS_WAIT_UNTIL(6000, pub && rant_topic_match_count(pub) == 1 && L && (id = ms_peer_id(L, "ms-bulk")) != 0);
    CHECK(pub && rant_topic_match_count(pub) == 1 && et.refused == 0,
          "static bulk: the late subscriber matched, nobody turned away (refused %lld)", et.refused);
    if (id) r = rant_function_call(rant_node_meta_function(L), rant_bytes(mask, 4), &out, 3000,
                                   &(RantCallOpts){ .provider = id });
    if (r == 1 && out.status == RANT_CALL_OK && out.schema){
        RantBytes info = rant_get_map(out.data, out.schema, "info");
        RantValue node, bulk;
        int ok = rant_map_get(info, "node", &node) && rant_map_get(node.bytes, "bulk_in_use", &bulk);
        CHECK(ok && bulk.v.u > 0 && bulk.v.u <= sizeof bbuf, "static bulk: the meta answer reports the bulk (%llu bytes)",
              ok ? (unsigned long long)bulk.v.u : 0ull);
    } else CHECK(0, "static bulk: the meta call is answered (ret %d)", r);
    if (L) rant_node_close(L, 1);
    rant_node_close(T, 1);
    ms_crowd_close();
}

/* A topic made at runtime among peers that hold no blob here: one nobody wants sends at once,
   and one three peers want reaches all three with its first message. */
static void ms_rt_msg(const RantMsg *m){ if (m->user) *(volatile int*)m->user = 1; }
static void runtime_topic_checks(void){
    uint16_t dom = (uint16_t)(TEST_DOMAIN + 10);
    RantNodeOpts ot = ms_opts(dom, 0, 8, 3000000);
    RantNode *T, *S[3] = { NULL, NULL, NULL }; RantTopic *lonely, *wanted;
    static volatile int got[3];
    uint64_t t0, blocked_ms; int k, reached = 0;
    ms_crowd_open(dom, 30, 1, 3000000);
    for (k = 0; k < 3; k++){
        RantNodeOpts os = ms_opts(dom, 0, 2, 3000000); RantAllocator a = rant_allocator_heap(0); char nm[24];
        got[k] = 0;
        os.user_data = (void*)&got[k];
        sprintf(nm, "ms-rt-sub%d", k);
        S[k] = rant_node_open(&a, nm, ms_rt_msg, NULL, &os);
        if (!S[k]) continue;
        rant_node_create_topic(S[k], "ms/rt/wanted", RANT_SUB_ONLY, NULL,
                               &(RantTopicOpts){ .qos = { .reliability = RANT_RELIABLE } });
        rant_node_start(S[k]);
    }
    ms_wait(1500);
    T = ms_heap("ms-rt", &ot);
    CHECK(T != NULL, "runtime topic: opens");
    if (T){
        rant_node_start(T);
        MS_WAIT_UNTIL(6000, ms_active(T) >= ms_ncrowd + 3);
        ms_wait(1500);
        lonely = rant_node_create_topic(T, "ms/rt/lonely", RANT_PUB_ONLY, NULL, NULL);
        t0 = i_rant_plat_now_us();
        rant_topic_send(lonely, rant_bytes("x", 1), NULL);
        blocked_ms = (i_rant_plat_now_us() - t0) / 1000u;
        CHECK(blocked_ms < 150, "runtime topic: one nobody wants sends at once (%llu ms)",
              (unsigned long long)blocked_ms);
        wanted = rant_node_create_topic(T, "ms/rt/wanted", RANT_PUB_ONLY, NULL,
                                        &(RantTopicOpts){ .qos = { .reliability = RANT_RELIABLE } });
        rant_topic_send(wanted, rant_bytes("first", 5), NULL);
        MS_WAIT_UNTIL(2000, got[0] && got[1] && got[2]);
        for (k = 0; k < 3; k++) reached += got[k] != 0;
        CHECK(reached == 3, "runtime topic: the first send reaches every peer that wants it (%d of 3)", reached);
        rant_node_close(T, 1);
    }
    for (k = 0; k < 3; k++) if (S[k]) rant_node_close(S[k], 1);
    ms_crowd_close();
}

/* Two nodes that share no topic match nothing, the builtins included, until one asks meta:
   then it matches the other's answering side. */
static void builtins_quiet_checks(void){
    MsEvents ea, eb; RantNode *A, *B; RantResponse out; uint32_t id; int r = 0;
    uint8_t mask[4] = { 1, 0, 0, 0 };     /* RANT_META_NODE */
    uint16_t dom = (uint16_t)(TEST_DOMAIN + 11);
    RantNodeOpts oa = ms_opts(dom, 0, 4, 500000), ob = ms_opts(dom, 0, 4, 500000);
    memset(&ea, 0, sizeof ea); memset(&eb, 0, sizeof eb);
    oa.user_data = &ea; ob.user_data = &eb;
    A = ms_heap("ms-quiet-a", &oa); B = ms_heap("ms-quiet-b", &ob);
    CHECK(A && B, "builtins quiet: both open");
    if (!(A && B)) goto done;
    rant_node_start(A); rant_node_start(B);
    MS_WAIT_UNTIL(4000, ms_peer_id(A, "ms-quiet-b") && ms_peer_id(B, "ms-quiet-a"));
    ms_wait(1000);                        /* two announce intervals for any match to form */
    CHECK(ea.matched == 0 && eb.matched == 0,
          "builtins quiet: plain nodes match nothing (A %lld, B %lld)", ea.matched, eb.matched);
    id = ms_peer_id(A, "ms-quiet-b");
    if (id) r = rant_function_call(rant_node_meta_function(A), rant_bytes(mask, 4), &out, 3000,
                                   &(RantCallOpts){ .provider = id });
    CHECK(r == 1 && out.status == RANT_CALL_OK && ea.matched > 0,
          "builtins quiet: an ask matches the answering side (ret %d, matched %lld)", r, ea.matched);
done:
    if (A) rant_node_close(A, 1);
    if (B) rant_node_close(B, 1);
}

static const TestCase cases[] = {
    { "builtins_quiet",      builtins_quiet_checks },
    { "seeded_segments",     seeded_segments_checks },
    { "meta_from_callbacks", meta_from_callbacks_checks },
    { "directed_first_call", directed_first_call_checks },
    { "static_bounded",      static_bounded_checks },
    { "static_meta_peers",   static_meta_peers_checks },
    { "observer_fast",       observer_fast_checks },
    { "grow_at_once",        grow_at_once_checks },
    { "big_peer",            big_peer_checks },
    { "slot_back",           slot_back_checks },
    { "crowd_late_match",    crowd_late_match_checks },
    { "static_bulk",         static_bulk_checks },
    { "runtime_topic",       runtime_topic_checks },
};

TEST_MAIN(cases)
