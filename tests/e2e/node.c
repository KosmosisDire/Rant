/* Opening a node: the staged cleanup on every failure, events reaching the app's user
 * data, growth past the topic reserve, and the launcher's environment variables.
 * spec/testing.md says what each case pins. */
#include "support/test.h"
#ifndef _WIN32
  #include <signal.h>
  #include <sys/time.h>
#endif

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
        n = rant_node_open(&a, NULL, NULL, NULL,
            &(RantNodeOpts){ .domain=TEST_DOMAIN, .net={ .multicast_interface="127.0.0.1" } });
        CHECK(n != NULL, "open-fail: node opens for create-fail check");
        c = n ? rant_node_create_topic(n, longname, RANT_PUBSUB, NULL, NULL) : NULL;
        CHECK(c == NULL, "open-fail: over-long topic name -> create_channel NULL");
        if (n) rant_node_close(n, 0);
    }

    /* a failed multicast join unwinds the node through fail_sock, and rant_last_error(NULL)
       names MCAST_JOIN with no handle to query */
    {   RantAllocator a = rant_allocator_static(mem, sizeof mem);
        RantEvent err; char line[160];
        wire_tap_write(&wire_tap.fail_join, 1);
        n = rant_node_open(&a, NULL, NULL, NULL, &(RantNodeOpts){ .domain=TEST_DOMAIN,
            .net={ .multicast_interface="127.0.0.1" } });
        wire_tap_write(&wire_tap.fail_join, 0);
        CHECK(n == NULL, "open-fail: a failed multicast join -> NULL");
        err = rant_last_error(NULL);
        CHECK(err.kind == RANT_ERROR && err.error == RANT_E_MCAST_JOIN,
                 "open-fail: last_error is RANT_E_MCAST_JOIN (%s)", rant_event_str(&err, line, sizeof line));
        if (n) rant_node_close(n, 0);
    }

    /* fail_sock: occupy an ephemeral port, then aim the node's data socket at it. The bind
       takes no reuse, so it collides and last_error names RANT_E_BIND with port and errno. */
    {   i_RantSock occupy;
        i_rant_plat_startup();
        occupy = i_rant_plat_udp_open();
        if (occupy != RANT_SOCK_BAD && i_rant_plat_bind(occupy, i_rant_plat_ipv4(127,0,0,1), 0, 0)){
            uint16_t port = i_rant_plat_local_port(occupy);
            RantAllocator a = rant_allocator_static(mem, sizeof mem);
            RantEvent err; char line[160];
            n = rant_node_open(&a, NULL, NULL, NULL, &(RantNodeOpts){ .domain=TEST_DOMAIN,
                .net={ .data_port=port, .multicast_interface="127.0.0.1" } });
            CHECK(n == NULL, "open-fail: data-port collision -> NULL (fail_sock)");
            err = rant_last_error(NULL);
            CHECK(err.kind == RANT_ERROR && err.error == RANT_E_BIND && err.port == port,
                     "open-fail: last_error is RANT_E_BIND port=%u (%s)", port, rant_event_str(&err, line, sizeof line));
            if (n) rant_node_close(n, 0);
        }
        if (occupy != RANT_SOCK_BAD) i_rant_plat_close(occupy);
        i_rant_plat_cleanup();
    }

    /* an on_event handler also receives the open failure directly (no handle needed): the
       node fires it on the passed-in callback before returning NULL. */
    {   RantAllocator a = rant_allocator_static(mem, sizeof mem);
        wire_tap_write(&wire_tap.fail_join, 1);
        n = rant_node_open(&a, NULL, NULL, of_on_event, &(RantNodeOpts){ .domain=TEST_DOMAIN,
            .net={ .multicast_interface="127.0.0.1" } });
        wire_tap_write(&wire_tap.fail_join, 0);
        CHECK(n == NULL, "open-fail: open still returns NULL with on_event set");
        CHECK(of_seen_error == RANT_E_MCAST_JOIN,
                 "open-fail: on_event received the failure (error=%d)", of_seen_error);
        if (n) rant_node_close(n, 0);
    }

    /* every open refused for memory names the bytes its pool refused, whichever take that was:
       a dynamic main bounded below what the open takes, then a static bulk too small */
    {   static uint8_t bulk[1u << 15]; size_t cap; int pass, refused = 0, unnamed = 0;
        RantNodeOpts o; RantAllocator a, b;
        memset(&o, 0, sizeof o); o.domain = TEST_DOMAIN; o.net.multicast_interface = "127.0.0.1";
        for (pass = 0; pass < 2; pass++)
            for (cap = 1024, n = NULL; !n && cap <= sizeof bulk + (pass ? 0u : (1u << 20)); cap += 256){
                RantEvent err;
                if (pass){ a = rant_allocator_static(mem, sizeof mem); b = rant_allocator_static(bulk, cap); o.bulk = &b; }
                else { a = rant_allocator_heap(0); a.max_bytes = cap; }
                n = rant_node_open(&a, NULL, NULL, NULL, &o);
                if (n){ rant_node_close(n, 0); break; }
                err = rant_last_error(NULL);
                refused++;
                unnamed += err.error != RANT_E_OOM || err.too_big_bytes == 0;
            }
        CHECK(refused > 0 && unnamed == 0,
              "open-fail: each of %d opens refused for memory is OOM with the bytes asked (%d without)", refused, unnamed);
    }

    /* after the failed opens a normal open must still succeed (cleanup balanced) */
    {   RantAllocator a = rant_allocator_static(mem, sizeof mem);
        n = rant_node_open(&a, NULL, NULL, NULL,
            &(RantNodeOpts){ .domain=TEST_DOMAIN, .net={ .multicast_interface="127.0.0.1" } });
        CHECK(n != NULL, "open-fail: normal open still works after failures");
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
 static int sentinel;
    const char *A="iuZA9tcJzAG", *B="5wVGxhTCmOC";   /* both to one identity, see collide.c */
    i_RantTopicDef cw, cr; RantNodeOpts wo, ro; RantNode *w, *r;
    uint8_t payload[16]; int i; memset(payload,0x5A,sizeof payload);
    memset(&cw,0,sizeof cw);
    cw.name=A; cw.role=RANT_PUB_ONLY;
    cw.qos.reliability=RANT_RELIABLE; cw.qos.keep_last=1; cw.qos.catch_up=1;
    cw.qos.max_message_bytes=32; cw.qos.heartbeat_us=50000;
    cr=cw; cr.name=B; cr.role=RANT_SUB_ONLY;
    wo = (RantNodeOpts){ .domain=TEST_DOMAIN+5, .discovery={ .max_peers=4 }, .net={ .multicast_interface="127.0.0.1" } };
    ro = wo;
    ro.user_data=&sentinel;   /* dynamic mode, so SHM capable */
    evu_user=NULL; evu_collisions=0;
    w=test_node_open(NULL,NULL,NULL,wo,&cw,1);
    r=test_node_open(NULL,NULL,evu_on_event,ro,&cr,1);
    CHECK(w && r, "event-user: nodes open");
    if (w && r){
        for (i=0;i<120 && evu_collisions==0;i++){ test_send(w,0,payload,16); rant_node_poll(w,0); rant_node_poll(r,20); }
        CHECK(evu_collisions >= 1, "event-user: collision event fired (%d)", evu_collisions);
        CHECK(evu_user == (void*)&sentinel,
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
    RantTopicOpts co; RantAddr seed; uint8_t payload[8]; int i, t;
    memset(&co,0,sizeof co); co.qos.reliability=RANT_RELIABLE; co.qos.keep_last=32;
    co.qos.catch_up=32; co.qos.heartbeat_us=50000;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=TEST_DOMAIN+7; po.max_topics=2; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po;
    for (i=0;i<24;i++) dg_recv[i]=0;
    dg_seq_ok=1; dg_next0=0;
    P = rant_node_open(&pa, "dg-pub", NULL, NULL, &po);
    S = rant_node_open(&sa, "dg-sub", dg_on_message, NULL, &so);
    CHECK(P&&S, "dyn-grow: nodes open (max_topics=2)");
    if (!(P&&S)){ if(P)rant_node_close(P,0); if(S)rant_node_close(S,0); return; }
    pc0 = rant_node_create_topic(P, names[0], RANT_PUB_ONLY, NULL, &co);
    rant_node_create_topic(S, names[0], RANT_SUB_ONLY, NULL, &co);
    CHECK(pc0 != NULL, "dyn-grow: topic 0 created");
    PUMP_UNTIL(P, S, 6400, rant_topic_match_count(pc0)!=0);
    CHECK(rant_topic_match_count(pc0)>0, "dyn-grow: topic 0 matched");
    for (i=0;i<5;i++){ payload[0]=(uint8_t)i; rant_topic_send(pc0,rant_bytes(payload,1), NULL); rant_node_poll(P,1); rant_node_poll(S,2); }
    /* create topics 1 to 11 on both: several grows, max_topics 2, 4, 8, 16 */
    for (i=1;i<12;i++){ rant_node_create_topic(P, names[i], RANT_PUB_ONLY, NULL, &co);
                        rant_node_create_topic(S, names[i], RANT_SUB_ONLY, NULL, &co);
                        rant_node_poll(P,1); rant_node_poll(S,1); }
    CHECK(rant_topic_match_count(pc0)>0, "dyn-grow: topic 0 still matched after grows");
    for (i=5;i<10;i++){ payload[0]=(uint8_t)i; rant_topic_send(pc0,rant_bytes(payload,1), NULL); rant_node_poll(P,1); rant_node_poll(S,2); }
    PUMP_UNTIL(P, S, 2400, dg_recv[0] >= 10);
    CHECK(dg_recv[0]==10, "dyn-grow: all 10 on topic 0 delivered across grows (got %d)", dg_recv[0]);
    CHECK(dg_seq_ok, "dyn-grow: topic 0 in-order, no loss/dup across grows");
    pcN = rant_node_topic(P, 11);                    /* a topic created AFTER a grow */
    for (i=0;i<3;i++){ payload[0]=0xAA; if(pcN) rant_topic_send(pcN,rant_bytes(payload,1), NULL); rant_node_poll(P,1); rant_node_poll(S,2); }
    PUMP_UNTIL(P, S, 2000, dg_recv[11] >= 3);
    CHECK(dg_recv[11]==3, "dyn-grow: post-grow topic delivers (got %d)", dg_recv[11]);
    rant_node_close(P,1); rant_node_close(S,1);
}

static void launchname_checks(void){
    RantAllocator a = rant_allocator_heap(0);
    RantNodeOpts o; RantNode *n;
    memset(&o,0,sizeof o); o.domain=TEST_DOMAIN+38; o.net.multicast_interface="127.0.0.1";
    test_setenv("RANT_NODE_NAME", "launched");
    n = rant_node_open(&a, "from-code", NULL, NULL, &o);
    CHECK(n && strcmp(n->name, "launched") == 0, "launchname: RANT_NODE_NAME names the node (%s)",
             n ? n->name : "no node");
    if (n) rant_node_close(n, 0);
    rant_allocator_reset(&a);
    test_setenv("RANT_NODE_NAME", "");
    n = rant_node_open(&a, "from-code", NULL, NULL, &o);
    CHECK(n && strcmp(n->name, "from-code") == 0, "launchname: unset, the code's name stands (%s)",
             n ? n->name : "no node");
    if (n) rant_node_close(n, 0);
    rant_allocator_reset(&a);
}

/* The launch domain phase: RANT_DOMAIN fills a domain the code left at 0, never one it set,
 * and a value that is not a u16 refuses the open. */
static void launchdomain_checks(void){
    static const char *bad[] = { "70000", "12x", "-1", "0000000000000000000000000000000000001" };
    RantAllocator a = rant_allocator_heap(0);
    RantNodeOpts o; RantNode *n; char env[16]; size_t i;
    memset(&o,0,sizeof o); o.net.multicast_interface="127.0.0.1";
    snprintf(env, sizeof env, "%u", (unsigned)(TEST_DOMAIN+39));
    test_setenv("RANT_DOMAIN", env);
    n = rant_node_open(&a, "ld", NULL, NULL, &o);
    CHECK(n && n->domain == TEST_DOMAIN+39, "launchdomain: RANT_DOMAIN fills the default domain (%u)",
             n ? (unsigned)n->domain : 0u);
    if (n) rant_node_close(n, 0);
    rant_allocator_reset(&a);
    o.domain = TEST_DOMAIN+40;
    n = rant_node_open(&a, "ld", NULL, NULL, &o);
    CHECK(n && n->domain == TEST_DOMAIN+40, "launchdomain: a domain set in code stands (%u)",
             n ? (unsigned)n->domain : 0u);
    if (n) rant_node_close(n, 0);
    rant_allocator_reset(&a);
    o.domain = 0;
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++){
        RantEvent err;
        test_setenv("RANT_DOMAIN", bad[i]);
        n = rant_node_open(&a, "ld", NULL, NULL, &o);
        err = rant_last_error(NULL);
        CHECK(n == NULL && err.error == RANT_E_BAD_DOMAIN, "launchdomain: '%s' refuses the open", bad[i]);
        if (n) rant_node_close(n, 0);
        rant_allocator_reset(&a);
    }
    test_setenv("RANT_DOMAIN", "");
}

/* The launch log level phase: RANT_LOG_LEVEL in any case fills a level the code left at 0,
 * never one it set, Info is the default, and a word or number that is no level refuses the open. */
static void launchloglevel_checks(void){
    static const char *bad[] = { "verbose", "3", "debugx", "warning" };
    RantAllocator a = rant_allocator_heap(0);
    RantNodeOpts o; RantNode *n; RantEvent err; size_t i;
    memset(&o,0,sizeof o); o.domain=TEST_DOMAIN+45; o.net.multicast_interface="127.0.0.1";
    n = rant_node_open(&a, "ll", NULL, NULL, &o);
    CHECK(n && n->log_level == RANT_LEVEL_INFO, "launchloglevel: Info by default (%d)", n ? n->log_level : -1);
    if (n) rant_node_close(n, 0);
    rant_allocator_reset(&a);
    test_setenv("RANT_LOG_LEVEL", "DeBuG");
    n = rant_node_open(&a, "ll", NULL, NULL, &o);
    CHECK(n && n->log_level == RANT_LEVEL_DEBUG, "launchloglevel: RANT_LOG_LEVEL fills the default (%d)",
             n ? n->log_level : -1);
    if (n) rant_node_close(n, 0);
    rant_allocator_reset(&a);
    o.log_level = RANT_LEVEL_ERROR;
    n = rant_node_open(&a, "ll", NULL, NULL, &o);
    CHECK(n && n->log_level == RANT_LEVEL_ERROR, "launchloglevel: a level set in code stands (%d)",
             n ? n->log_level : -1);
    if (n) rant_node_close(n, 0);
    rant_allocator_reset(&a);
    o.log_level = 9;
    n = rant_node_open(&a, "ll", NULL, NULL, &o);
    err = rant_last_error(NULL);
    CHECK(n == NULL && err.error == RANT_E_BAD_LOG_LEVEL, "launchloglevel: opts.log_level 9 refuses the open");
    if (n) rant_node_close(n, 0);
    rant_allocator_reset(&a);
    o.log_level = 0;
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++){
        test_setenv("RANT_LOG_LEVEL", bad[i]);
        n = rant_node_open(&a, "ll", NULL, NULL, &o);
        err = rant_last_error(NULL);
        CHECK(n == NULL && err.error == RANT_E_BAD_LOG_LEVEL, "launchloglevel: '%s' refuses the open", bad[i]);
        if (n) rant_node_close(n, 0);
        rant_allocator_reset(&a);
    }
    test_setenv("RANT_LOG_LEVEL", "");
}

static int st_name_is(RantTopic *t, const char *want){
    RantString s = i_rant_topic_name(t);
    return t && s.len == strlen(want) && memcmp(s.data, want, s.len) == 0;
}

/* The name prefix phase: RANT_PREFIX stacks outside opts.prefix on every create, a leading
 * '/' skips both, a bad prefix refuses the open and RANT_NODE_NAME_PREFIX names the node. */
static void prefix_checks(void){
    static const char *bad_code[] = { "/x", "x/", "a@b", "012345678901234567890123456789012345678901234567890123456789012" };
    RantAllocator a = rant_allocator_heap(0);
    RantNodeOpts o; RantNode *n; RantTopic *t; RantEntityInfo info; RantEvent err; size_t i;
    memset(&o,0,sizeof o); o.domain=TEST_DOMAIN+41; o.net.multicast_interface="127.0.0.1";
    o.prefix = "robot1";
    test_setenv("RANT_PREFIX", "cellA");
    test_setenv("RANT_NODE_NAME_PREFIX", "lab");
    n = rant_node_open(&a, "arm", NULL, NULL, &o);
    CHECK(n != NULL, "prefix: open with RANT_PREFIX and opts.prefix");
    if (n){
        CHECK(strcmp(n->name, "lab/arm") == 0, "prefix: RANT_NODE_NAME_PREFIX names the node (%s)", n->name);
        t = rant_node_create_topic(n, "pose", RANT_PUB_ONLY, NULL, NULL);
        CHECK(st_name_is(t, "cellA/robot1/pose"), "prefix: a topic goes under env then code");
        t = rant_node_create_topic(n, "/clock", RANT_SUB_ONLY, NULL, NULL);
        CHECK(st_name_is(t, "clock"), "prefix: a leading '/' skips the prefix");
        CHECK(rant_node_create_function_definition(n, "add", NULL, NULL, NULL, NULL, NULL) != NULL
                 && rant_node_mesh_find(n, RANT_ENTITY_FUNCTION, "cellA/robot1/add", &info) == 1,
                 "prefix: a function goes under the prefix");
        CHECK(rant_node_create_variable_definition(n, "speed", NULL, NULL) != NULL
                 && rant_node_mesh_find(n, RANT_ENTITY_VARIABLE, "cellA/robot1/speed", &info) == 1,
                 "prefix: a variable goes under the prefix");
        CHECK(rant_node_create_topic(n, "a@b", RANT_PUB_ONLY, NULL, NULL) == NULL
                 && rant_last_error(n).error == RANT_E_BAD_NAME, "prefix: '@' in a name is refused");
        /* 13 prefix bytes leave 51, and a function needs 4 more for its suffix */
        CHECK(rant_node_create_remote_function(n, "012345678901234567890123456789012345678901234567",
                                                  NULL, NULL, NULL) == NULL
                 && rant_last_error(n).error == RANT_E_BAD_NAME, "prefix: a name too long under the prefix is refused");
        rant_node_close(n, 0);
    }
    rant_allocator_reset(&a);
    test_setenv("RANT_PREFIX", ""); test_setenv("RANT_NODE_NAME_PREFIX", "");
    for (i = 0; i < sizeof bad_code / sizeof bad_code[0]; i++){
        o.prefix = bad_code[i];
        n = rant_node_open(&a, "arm", NULL, NULL, &o);
        err = rant_last_error(NULL);
        CHECK(n == NULL && err.error == RANT_E_BAD_PREFIX, "prefix: '%s' refuses the open", bad_code[i]);
        if (n) rant_node_close(n, 0);
        rant_allocator_reset(&a);
    }
    o.prefix = NULL;
    test_setenv("RANT_PREFIX", "/cellA");
    n = rant_node_open(&a, "arm", NULL, NULL, &o);
    err = rant_last_error(NULL);
    CHECK(n == NULL && err.error == RANT_E_BAD_PREFIX, "prefix: a bad RANT_PREFIX refuses the open");
    if (n) rant_node_close(n, 0);
    rant_allocator_reset(&a);
    test_setenv("RANT_PREFIX", "");
}

/* Two domains on one host: each discovery socket receives its own domain's group only. The
 * other domain's nodes send nothing unicast, so its announces could only come by the group.
 * Other programs' seeds reach port 6730 by unicast, so their domains are not counted. */
static long long dg_sock[2];
static uint16_t  dg_dom[2];
static int       dg_own[2], dg_foreign[2];
static void dg_on_recv(long long sock, const unsigned char *buf, long len, const struct sockaddr *from){
    int i;
    uint16_t dom;
    (void)from;
    if (len < 8 || memcmp(buf, "uDSC", 4) != 0) return;
    dom = (uint16_t)(buf[6] | (buf[7] << 8));
    for (i = 0; i < 2; i++){
        if (sock != dg_sock[i]) continue;
        if (dom == dg_dom[i]) dg_own[i]++;
        else if (dom == dg_dom[1 - i]) dg_foreign[i]++;
    }
}
static void domaingroup_checks(void){
    RantAllocator aa = rant_allocator_heap(0), ab = rant_allocator_heap(0), ba = rant_allocator_heap(0);
    RantNodeOpts o;
    RantNode *A = NULL, *A2 = NULL, *B = NULL;
    memset(&o, 0, sizeof o);
    o.disable_shm = 1; o.discovery.max_peers = 4; o.discovery.announce_interval_us = 200000;
    o.net.multicast_interface = "127.0.0.1";
    dg_dom[0] = (uint16_t)(TEST_DOMAIN + 40); dg_dom[1] = (uint16_t)(TEST_DOMAIN + 41);
    memset(dg_own, 0, sizeof dg_own); memset(dg_foreign, 0, sizeof dg_foreign);
    dg_sock[0] = dg_sock[1] = -1;
    wire_tap_on_recv = dg_on_recv;
    o.domain = dg_dom[0];
    A  = rant_node_open(&aa, "dg-a", NULL, NULL, &o);
    A2 = rant_node_open(&ab, "dg-a2", NULL, NULL, &o);
    o.domain = dg_dom[1];
    B  = rant_node_open(&ba, "dg-b", NULL, NULL, &o);
    CHECK(A && A2 && B, "domaingroup: three nodes on two domains open");
    if (A && A2 && B){
        unsigned c = 0; RantIter it; RantPeerInfo p;
        dg_sock[0] = (long long)i_rant_discovery_fd(A->discovery);
        dg_sock[1] = (long long)i_rant_discovery_fd(B->discovery);
        test_pump3(A, A2, B, 1500);                  /* several announce intervals each */
        memset(&it, 0, sizeof it);
        while (rant_node_peers_next(A, &it, &p)) c++;
        CHECK(c == 1 && dg_own[0] > 0, "domaingroup: a domain still discovers itself (peers=%u own=%d)",
              (unsigned)c, dg_own[0]);
        CHECK(dg_foreign[0] == 0 && dg_foreign[1] == 0,
              "domaingroup: no announce of the other domain reaches a discovery socket (%d, %d)",
              dg_foreign[0], dg_foreign[1]);
    }
    if (A) rant_node_close(A, 1);
    if (A2) rant_node_close(A2, 1);
    if (B) rant_node_close(B, 1);
    wire_tap_on_recv = NULL;
    rant_allocator_reset(&aa); rant_allocator_reset(&ab); rant_allocator_reset(&ba);
}

static long long bm_sock_group, bm_sock_data, bm_group, bm_group_marked, bm_copies, bm_copies_one;
static uint8_t bm_uuid[16]; static uint16_t bm_dom;
static void bm_on_recv(long long sock, const unsigned char *buf, long len, const struct sockaddr *from){
    (void)from;
    if (len < 24 || memcmp(buf, "uDSC", 4) != 0 || !(buf[5] & RANT_DISCOVERY_FLAG_BYE)) return;
    if ((uint16_t)(buf[6] | (buf[7] << 8)) != bm_dom || memcmp(buf + 8, bm_uuid, 16) != 0) return;
    if (sock == bm_sock_group){
        test_atomic_add(&bm_group, 1);
        if (buf[5] != RANT_DISCOVERY_FLAG_BYE) test_atomic_add(&bm_group_marked, 1);
    } else if (sock == bm_sock_data){
        test_atomic_add(&bm_copies, 1);
        if (buf[5] & RANT_DISCOVERY_FLAG_ONE) test_atomic_add(&bm_copies_one, 1);
    }
}
/* A closing node sends its BYE several times from one buffer. Each to the group carries BYE
   alone, since a copy's mark is per copy, and each copy to the slotted peer carries ONE. */
static void bye_marks_checks(void){
    RantAllocator aa = rant_allocator_heap(0), ba = rant_allocator_heap(0);
    RantNodeOpts o; RantNode *A, *B; RantTopic *pub = NULL, *sub = NULL;
    memset(&o, 0, sizeof o);
    o.domain = bm_dom = (uint16_t)(TEST_DOMAIN + 53); o.disable_shm = 1; o.net.multicast_interface = "127.0.0.1";
    A = rant_node_open(&aa, "bm-a", NULL, NULL, &o);
    B = rant_node_open(&ba, "bm-b", NULL, NULL, &o);
    CHECK(A && B, "bye marks: both open");
    if (!A || !B) goto done;
    pub = rant_node_create_topic(A, "bm/t", RANT_PUB_ONLY, NULL, NULL);
    sub = rant_node_create_topic(B, "bm/t", RANT_SUB_ONLY, NULL, NULL);
    rant_node_start(A); rant_node_start(B);
    WAIT_UNTIL(5000, pub && rant_topic_match_count(pub) == 1);
    memcpy(bm_uuid, i_rant_discovery_uuid(i_rant_discovery_state(A->discovery)), 16);
    bm_sock_group = (long long)B->discovery->fd; bm_sock_data = (long long)B->fd;
    bm_group = bm_group_marked = bm_copies = bm_copies_one = 0;
    wire_tap_on_recv = bm_on_recv;
    rant_node_close(A, 1); A = NULL;
    WAIT_UNTIL(1000, test_atomic_load(&bm_group) >= 3 && test_atomic_load(&bm_copies) >= 3);
    wire_tap_on_recv = NULL;
    CHECK(sub && test_atomic_load(&bm_group) >= 3 && test_atomic_load(&bm_group_marked) == 0
          && test_atomic_load(&bm_copies) >= 3 && test_atomic_load(&bm_copies_one) == test_atomic_load(&bm_copies),
          "bye marks: each BYE to the group is unmarked, each copy is ONE (%lld group, %lld marked, %lld copies, %lld ONE)",
          test_atomic_load(&bm_group), test_atomic_load(&bm_group_marked), test_atomic_load(&bm_copies),
          test_atomic_load(&bm_copies_one));
done:
    wire_tap_on_recv = NULL;
    if (A) rant_node_close(A, 1);
    if (B) rant_node_close(B, 1);
}

static uint32_t bk_peer_id(RantNode *n, const char *name){
    RantIter it; RantPeerInfo p; size_t l = strlen(name);
    memset(&it, 0, sizeof it);
    while (rant_node_peers_next(n, &it, &p))
        if (p.name.len == l && !memcmp(p.name.data, name, l)) return p.id;
    return 0;
}

static volatile int bk_got_up, bk_got_down, bk_got_one;
static void bk_on_msg(const RantMsg *m){
    if (m->data.len == 200000u) (*(volatile int*)m->user)++;
}
static void bk_on_one(const RantMsg *m){ if (m->data.len == 64u) bk_got_one++; }

/* A bulk pool holds the message sized buffers: both ends of two 200 kB streams, a queued
   copy, a big variable's value and the meta answer, while each main pool stays flat. Every
   page returns to the pool that took it, and the main allocator passed as bulk is one pool. */
static void bulk_pools_checks(void){
    static uint8_t big[200000], value[150000], one[1u << 20], ref[1u << 20];
    uint16_t dom = (uint16_t)(TEST_DOMAIN + 50);
    RantAllocator pm, pb, sm, sb, oa = rant_allocator_static(one, sizeof one);
    RantNodeOpts o; RantNode *P, *S = NULL, *O, *H; RantQueue *q = NULL;
    RantTopic *down_pub = NULL, *down_sub = NULL, *up_pub = NULL, *up_sub = NULL;
    RantVariable *def = NULL, *rem = NULL; RantBytes got;
    size_t p0 = 0, s0 = 0, p1 = 0, s1 = 0; long long bulk0; uint8_t mask[4] = { 5, 0, 0, 0 };   /* NODE and TOPICS */
    RantResponse out; int r = 0, k;
    test_page_clear(); bk_got_up = bk_got_down = bk_got_one = 0;
    pm = rant_allocator_dynamic(test_page_source(0), 0); pb = rant_allocator_dynamic(test_page_source(1), 0);
    sm = rant_allocator_dynamic(test_page_source(2), 0); sb = rant_allocator_dynamic(test_page_source(3), 0);
    memset(big, 0x5A, sizeof big); memset(value, 0xA5, sizeof value);
    memset(&o, 0, sizeof o);
    o.domain = dom; o.disable_shm = 1; o.net.multicast_interface = "127.0.0.1";
    {   static uint8_t tiny[256]; RantAllocator ta = rant_allocator_static(tiny, sizeof tiny), ma = rant_allocator_static(ref, sizeof ref);
        o.bulk = &ta;
        O = rant_node_open(&ma, "bk-tiny", NULL, NULL, &o);
        CHECK(!O && rant_last_error(NULL).error == RANT_E_OOM && rant_last_error(NULL).too_big_bytes > sizeof tiny,
              "bulk pools: a static node whose bulk cannot hold its records refuses the open with OOM and the bytes asked (error %d, %llu bytes)",
              (int)rant_last_error(NULL).error, (unsigned long long)rant_last_error(NULL).too_big_bytes);
        if (O) rant_node_close(O, 0);
        o.bulk = NULL;
    }
    {   RantTopic *one_pub = NULL; RantAllocator ha = rant_allocator_heap(0), ra = rant_allocator_static(ref, sizeof ref);
        uint8_t m64[64]; size_t ref_use = 0, one_use = 0;
        O = rant_node_open(&ra, "bk-one", bk_on_one, NULL, &o);
        if (O){ rant_node_mem_stats(O, &ref_use, NULL, NULL); rant_node_close(O, 0); }
        o.bulk = &oa;
        O = rant_node_open(&oa, "bk-one", bk_on_one, NULL, &o);
        if (O) rant_node_mem_stats(O, &one_use, NULL, NULL);
        o.bulk = NULL;
        H = rant_node_open(&ha, "bk-one-pub", NULL, NULL, &o);
        if (O && H){
            rant_node_create_topic(O, "bk/one", RANT_SUB_ONLY, NULL, &(RantTopicOpts){ .qos = { .reliability = RANT_RELIABLE } });
            one_pub = rant_node_create_topic(H, "bk/one", RANT_PUB_ONLY, NULL, &(RantTopicOpts){ .qos = { .reliability = RANT_RELIABLE } });
            rant_node_start(O); rant_node_start(H);
            WAIT_UNTIL(5000, one_pub && rant_topic_match_count(one_pub) == 1);
            memset(m64, 1, sizeof m64);
            for (k = 0; k < 3 && one_pub; k++) rant_topic_send(one_pub, rant_bytes(m64, sizeof m64), NULL);
            WAIT_UNTIL(3000, bk_got_one == 3);
        }
        CHECK(O && H && bk_got_one == 3 && ref_use && one_use == ref_use,
              "bulk pools: the main allocator passed as bulk is one pool, counted whole, which matches and receives (%u and %u bytes, %d of 3)",
              (unsigned)one_use, (unsigned)ref_use, bk_got_one);
        if (H) rant_node_close(H, 1);
        if (O) rant_node_close(O, 1);
    }
    o.user_data = (void*)&bk_got_up; o.bulk = &pb;
    P = rant_node_open(&pm, "bk-p", bk_on_msg, NULL, &o);
    o.user_data = (void*)&bk_got_down; o.bulk = &sb;
    S = rant_node_open(&sm, "bk-s", bk_on_msg, NULL, &o);
    CHECK(P && S, "bulk pools: both nodes open on a main and a bulk pool");
    if (!P || !S) goto done;
    q = rant_node_create_queue(S);
    down_pub = rant_node_create_topic(P, "bk/down", RANT_PUB_ONLY, NULL, &(RantTopicOpts){ .qos = { .reliability = RANT_RELIABLE } });
    down_sub = rant_node_create_topic(S, "bk/down", RANT_SUB_ONLY, NULL,
                                      &(RantTopicOpts){ .qos = { .reliability = RANT_RELIABLE }, .queue = q });
    up_pub = rant_node_create_topic(S, "bk/up", RANT_PUB_ONLY, NULL, &(RantTopicOpts){ .qos = { .reliability = RANT_RELIABLE } });
    up_sub = rant_node_create_topic(P, "bk/up", RANT_SUB_ONLY, NULL, &(RantTopicOpts){ .qos = { .reliability = RANT_RELIABLE } });
    def = rant_node_create_variable_definition(P, "bk/var", NULL, &(RantVariableOpts){ .initial = rant_bytes(value, 1000) });
    rem = rant_node_create_remote_variable(S, "bk/var", NULL, NULL);
    (void)rant_node_meta_function(S);
    rant_node_start(P); rant_node_start(S);
    WAIT_UNTIL(5000, down_pub && up_pub && rant_topic_match_count(down_pub) == 1 && rant_topic_match_count(up_pub) == 1
                     && rem && rant_variable_match_count(rem) == 1);
    CHECK(q && down_sub && up_sub && def && rem && rant_variable_wait(rem, 3000) == 1
          && rant_variable_get(rem, &got) == 1 && got.len == 1000,
          "bulk pools: the streams match and the variable's first value arrives (%u bytes)", (unsigned)got.len);
    test_sleep_ms(300);
    rant_node_mem_stats(P, &p0, NULL, NULL); rant_node_mem_stats(S, &s0, NULL, NULL);
    bulk0 = test_page_live[1] + test_page_live[3];
    rant_topic_send(down_pub, rant_bytes(big, sizeof big), NULL);
    rant_topic_send(up_pub, rant_bytes(big, sizeof big), NULL);
    rant_variable_set(def, rant_bytes(value, sizeof value));
    for (k = 0; k < 500 && !(bk_got_down && bk_got_up && rant_variable_get(rem, &got) == 1 && got.len == sizeof value); k++)
        rant_queue_dispatch(q, 0, 10);
    r = rant_function_call(rant_node_meta_function(S), rant_bytes(mask, 4), &out, 3000,
                           &(RantCallOpts){ .provider = bk_peer_id(S, "bk-p") });
    rant_node_mem_stats(P, &p1, NULL, NULL); rant_node_mem_stats(S, &s1, NULL, NULL);
    CHECK(bk_got_down == 1 && bk_got_up == 1 && got.len == sizeof value && p1 <= p0 + 512u && s1 <= s0 + 1024u
          && test_page_live[1] + test_page_live[3] - bulk0 >= 5ll * 200000 + 4ll * 150000,
          "bulk pools: two 200 kB streams, a queued copy, a 150 kB value and a meta answer grow the bulk by %lld kB and the mains by %ld and %ld bytes",
          (test_page_live[1] + test_page_live[3] - bulk0) / 1024, (long)p1 - (long)p0, (long)s1 - (long)s0);
    if (r == 1 && out.status == RANT_CALL_OK && out.schema){
        RantBytes info = rant_get_map(out.data, out.schema, "info");
        RantValue node, mem, bulk;
        int ok = rant_map_get(info, "node", &node) && rant_map_get(node.bytes, "mem_in_use", &mem)
              && rant_map_get(node.bytes, "bulk_in_use", &bulk);
        CHECK(ok && bulk.v.u >= 600000u && mem.v.u > 0 && mem.v.u < bulk.v.u,
              "bulk pools: the meta answer reports each pool (main %llu, bulk %llu)",
              ok ? (unsigned long long)mem.v.u : 0ull, ok ? (unsigned long long)bulk.v.u : 0ull);
    } else CHECK(0, "bulk pools: the meta call is answered (ret %d)", r);
    rant_topic_retire(down_pub); rant_topic_retire(up_sub); rant_variable_retire(def);
    rant_node_close(S, 1); S = NULL;
    CHECK(test_page_live[2] == 0 && test_page_live[3] == 0 && test_page_foreign == 0,
          "bulk pools: a closed node gives every page back to its own source (main %lld, bulk %lld, foreign %lld)",
          test_page_live[2], test_page_live[3], test_page_foreign);
    test_sleep_ms(200);
done:
    if (S) rant_node_close(S, 1);
    if (P) rant_node_close(P, 1);
    CHECK(test_page_live[0] == 0 && test_page_live[1] == 0 && test_page_foreign == 0,
          "bulk pools: so does the other after its peer left (main %lld, bulk %lld, foreign %lld)",
          test_page_live[0], test_page_live[1], test_page_foreign);
}

static volatile int bq_ok, bq_cancelled, bq_no_provider;
static void bq_on_rsp(const RantResponse *r){
    if (r->status == RANT_CALL_OK && r->data.len == 4) bq_ok++;
    else if (r->status == RANT_CALL_CANCELLED) bq_cancelled++;
    else if (r->status == RANT_CALL_NO_PROVIDER) bq_no_provider++;
}
static void bq_on_req(RantRequest *req, void *user){
    uint8_t len[4];
    (void)user;
    len[0] = (uint8_t)req->data.len; len[1] = (uint8_t)(req->data.len >> 8);
    len[2] = (uint8_t)(req->data.len >> 16); len[3] = (uint8_t)(req->data.len >> 24);
    rant_request_reply(req, rant_bytes(len, 4));
}

/* A call made before its provider matched waits in the caller's bulk. It is sent once the
   provider matches, or ends cancelled or with no provider, and every page goes home. */
static void bulk_queued_checks(void){
    static uint8_t req[120000];
    RantAllocator cm = rant_allocator_dynamic(test_page_source(0), 0), cb = rant_allocator_dynamic(test_page_source(1), 0);
    RantAllocator pa = rant_allocator_heap(0);
    RantNodeOpts o; RantNode *C, *P = NULL; RantFunction *fn = NULL, *none = NULL, *def = NULL;
    size_t m0 = 0, m1 = 0; long long b0; uint32_t cid = 0; int r1, r2, r3, rc;
    test_page_clear(); bq_ok = bq_cancelled = bq_no_provider = 0;
    memset(req, 0x3C, sizeof req);
    memset(&o, 0, sizeof o);
    o.domain = (uint16_t)(TEST_DOMAIN + 52); o.disable_shm = 1; o.net.multicast_interface = "127.0.0.1";
    o.bulk = &cb;
    C = rant_node_open(&cm, "bq-caller", NULL, NULL, &o);
    CHECK(C != NULL, "bulk queued: the caller opens on a main and a bulk pool");
    if (!C) return;
    fn = rant_node_create_remote_function(C, "bq/fn", NULL, NULL, NULL);
    none = rant_node_create_remote_function(C, "bq/none", NULL, NULL, &(RantFunctionOpts){ .timeout_us = 300000 });
    rant_node_start(C);
    test_sleep_ms(200);
    rant_node_mem_stats(C, &m0, NULL, NULL);
    b0 = test_page_live[1];
    r1 = rant_function_call_async(fn, rant_bytes(req, sizeof req), bq_on_rsp, NULL, NULL);
    r2 = rant_function_call_async(fn, rant_bytes(req, sizeof req), bq_on_rsp, NULL, &(RantCallOpts){ .id_out = &cid });
    rc = rant_function_cancel(fn, cid);
    r3 = rant_function_call_async(none, rant_bytes(req, sizeof req), bq_on_rsp, NULL, NULL);
    rant_node_mem_stats(C, &m1, NULL, NULL);
    CHECK(fn && none && r1 == RANT_OK && r2 == RANT_OK && r3 == RANT_OK && rc == RANT_OK
          && test_page_live[1] - b0 >= 2ll * (long long)sizeof req && m1 <= m0 + 1024u,
          "bulk queued: three calls with no provider wait in the bulk (it grew %lld kB, the main %ld bytes)",
          (test_page_live[1] - b0) / 1024, (long)m1 - (long)m0);
    WAIT_UNTIL(3000, bq_no_provider == 1);
    o.bulk = NULL;
    P = rant_node_open(&pa, "bq-provider", NULL, NULL, &o);
    def = P ? rant_node_create_function_definition(P, "bq/fn", NULL, NULL, bq_on_req, NULL, NULL) : NULL;
    if (P) rant_node_start(P);
    WAIT_UNTIL(5000, bq_ok == 1);
    CHECK(def && bq_ok == 1 && bq_cancelled == 1 && bq_no_provider == 1,
          "bulk queued: the waiting call is answered once its provider matches, the others end cancelled and with no provider (%d, %d, %d)",
          bq_ok, bq_cancelled, bq_no_provider);
    rant_node_close(C, 1);
    CHECK(test_page_live[0] == 0 && test_page_live[1] == 0 && test_page_foreign == 0,
          "bulk queued: the closed caller gives every page back to its own source (main %lld, bulk %lld, foreign %lld)",
          test_page_live[0], test_page_live[1], test_page_foreign);
    if (P) rant_node_close(P, 1);
}

/* On the same host the one copy out of shared memory lands in the bulk, the main flat. */
static void bulk_shm_checks(void){
    static uint8_t big[200000];
    RantAllocator pm, pb, sm, sb; RantNodeOpts o; RantNode *P, *S; RantTopic *pub = NULL, *sub = NULL;
    size_t s0 = 0, s1 = 0; long long b0;
    test_page_clear(); bk_got_down = 0;
    pm = rant_allocator_dynamic(test_page_source(0), 0); pb = rant_allocator_dynamic(test_page_source(1), 0);
    sm = rant_allocator_dynamic(test_page_source(2), 0); sb = rant_allocator_dynamic(test_page_source(3), 0);
    memset(&o, 0, sizeof o);
    o.domain = (uint16_t)(TEST_DOMAIN + 51); o.net.multicast_interface = "127.0.0.1";
    o.bulk = &pb;
    P = rant_node_open(&pm, "bk-shm-p", NULL, NULL, &o);
    o.user_data = (void*)&bk_got_down; o.bulk = &sb;
    S = rant_node_open(&sm, "bk-shm-s", bk_on_msg, NULL, &o);
    CHECK(P && S, "bulk shm: both nodes open");
    if (!P || !S) goto done;
    pub = rant_node_create_topic(P, "bk/shm", RANT_PUB_ONLY, NULL, &(RantTopicOpts){ .qos = { .reliability = RANT_RELIABLE } });
    sub = rant_node_create_topic(S, "bk/shm", RANT_SUB_ONLY, NULL, &(RantTopicOpts){ .qos = { .reliability = RANT_RELIABLE } });
    rant_node_start(P); rant_node_start(S);
    WAIT_UNTIL(5000, pub && sub && rant_topic_match_count(pub) == 1);
    test_sleep_ms(300);
    rant_node_mem_stats(S, &s0, NULL, NULL); b0 = test_page_live[3];
    rant_topic_send(pub, rant_bytes(big, sizeof big), NULL);
    WAIT_UNTIL(3000, bk_got_down == 1);
    rant_node_mem_stats(S, &s1, NULL, NULL);
    CHECK(bk_got_down == 1 && s1 <= s0 + 4096u && test_page_live[3] - b0 >= 200000,
          "bulk shm: the copy out of shared memory grows the bulk by %lld kB and the main by %ld bytes",
          (test_page_live[3] - b0) / 1024, (long)s1 - (long)s0);
done:
    if (S) rant_node_close(S, 1);
    if (P) rant_node_close(P, 1);
    CHECK(test_page_live[0] + test_page_live[1] + test_page_live[2] + test_page_live[3] == 0 && test_page_foreign == 0,
          "bulk shm: every page goes back to its own source (foreign %lld)", test_page_foreign);
}

#ifndef _WIN32
static volatile sig_atomic_t ps_signals;
static int ps_poll_errors;
static void ps_on_signal(int sig){ (void)sig; ps_signals++; }
static void ps_on_event(const RantEvent *ev){ if (ev->kind == RANT_ERROR && ev->error == RANT_E_POLL) ps_poll_errors++; }
#endif

/* The poll signal phase: a signal in the wait ends it early and is no fault. */
static void poll_signal_checks(void){
#ifdef _WIN32
    test_skip("no POSIX signals");
#else
    struct sigaction sa;
    struct itimerval once = { {0, 0}, {0, 20000} }, every = { {0, 2000}, {0, 2000} }, off = { {0, 0}, {0, 0} };
    RantAllocator a = rant_allocator_heap(0);
    RantNodeOpts o; RantNode *n; i_RantPollfd p; uint64_t t0, end; int r, seen;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = ps_on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);   /* stays set: a shot still in flight must not end the program */

    p.fd = i_rant_plat_udp_open(); p.events = RANT_POLLIN; p.revents = 0;
    CHECK(p.fd != RANT_SOCK_BAD, "poll_signal: a socket to wait on");
    setitimer(ITIMER_REAL, &once, NULL);
    t0 = i_rant_plat_now_us();
    r = i_rant_plat_poll(&p, 1, 5000);
    CHECK(r == 0 && p.revents == 0, "poll_signal: a signal reads as a timeout (%d)", r);
    CHECK(ps_signals == 1 && i_rant_plat_now_us() - t0 < 2500000u,
          "poll_signal: the wait ends at the signal, not at its timeout (%d signals)", (int)ps_signals);
    i_rant_plat_close(p.fd);

    memset(&o, 0, sizeof o); o.domain = TEST_DOMAIN+46; o.net.multicast_interface = "127.0.0.1";
    n = rant_node_open(&a, "ps", NULL, ps_on_event, &o);
    CHECK(n != NULL, "poll_signal: the node opens");
    if (!n) return;
    ps_signals = 0;
    setitimer(ITIMER_REAL, &every, NULL);
    for (end = i_rant_plat_now_us() + 200000u; i_rant_plat_now_us() < end; ) rant_node_poll(n, 50);
    setitimer(ITIMER_REAL, &off, NULL);
    seen = ps_signals;
    CHECK(seen > 1 && ps_poll_errors == 0, "poll_signal: no RANT_E_POLL from %d signals (%d)", seen, ps_poll_errors);
    rant_node_close(n, 0);
#endif
}

static const TestCase cases[] = {
    { "open_fail", open_fail_checks },
    { "bye_marks", bye_marks_checks },
    { "bulk_pools", bulk_pools_checks },
    { "bulk_queued", bulk_queued_checks },
    { "bulk_shm", bulk_shm_checks },
    { "event_user", event_user_checks },
    { "dynamic_grow", dynamic_grow_checks },
    { "launch_name", launchname_checks },
    { "launch_domain", launchdomain_checks },
    { "launch_log_level", launchloglevel_checks },
    { "prefix", prefix_checks },
    { "domain_groups", domaingroup_checks },
    { "poll_signal", poll_signal_checks },
};

TEST_MAIN(cases)
