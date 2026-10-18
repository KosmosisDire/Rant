/* Opening a node: the staged cleanup on every failure, events reaching the app's user
 * data, growth past the topic reserve, and the launcher's environment variables.
 * spec/testing.md says what each case pins. */
#include "support/test.h"

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

static const TestCase cases[] = {
    { "open_fail", open_fail_checks },
    { "event_user", event_user_checks },
    { "dynamic_grow", dynamic_grow_checks },
    { "launch_name", launchname_checks },
    { "launch_domain", launchdomain_checks },
    { "prefix", prefix_checks },
};

TEST_MAIN(cases)
