/* Delivery off the poll: queue rings, pull topics, explicit callback queues and callback
 * queues on functions, tasks and variables. spec/testing.md says what each case pins. */
#include "support/test.h"

/* B's three topics share one callback queue: per topic counts, the last value and order */
static unsigned long qc_n[3], qc_bad[3];
static uint32_t      qc_last[3];
static uint64_t      qc_recv_us;
static char          qc_topic[16], qc_pub[16];
static void qc_on_message(const RantMsg *msg){
    uint16_t c = msg->topic_index;
    uint32_t v = msg->data.len >= 4 ? i_rant_le_r32(msg->data.data) : 0;
    if (c >= 3) return;
    if (v != (qc_n[c] ? qc_last[c] + 1u : 0u)) qc_bad[c]++;
    if (c == 0 && !qc_n[0]){
        size_t tl = msg->topic_name.len < sizeof qc_topic - 1 ? msg->topic_name.len : sizeof qc_topic - 1;
        size_t pl = msg->publisher_name.len < sizeof qc_pub - 1 ? msg->publisher_name.len : sizeof qc_pub - 1;
        memcpy(qc_topic, msg->topic_name.data, tl); qc_topic[tl] = '\0';
        memcpy(qc_pub, msg->publisher_name.data, pl); qc_pub[pl] = '\0';
        qc_recv_us = msg->recv_us;
    }
    qc_last[c] = v; qc_n[c]++;
}

/* The ring phase: what a handle's ring on a callback queue does at its cap, under the
 * reliability QoS. A timed dispatch drives the loop itself. */
static void queue_checks(void){

    uint8_t payload[512];
    i_RantTopicDef ca[3];
    RantNodeOpts ao, bo;
    RantAllocator ba = rant_allocator_heap(0);
    RantNode *a, *b;
    RantQueue *q = NULL;
    RantTopic *lazy = NULL, *be = NULL, *rel = NULL;
    int i;
    memset(ca, 0, sizeof ca); memset(payload, 0, sizeof payload);
    memset(qc_n, 0, sizeof qc_n); memset(qc_bad, 0, sizeof qc_bad);
    ca[0].name="q/lazy"; ca[0].role=RANT_PUB_ONLY;
    ca[0].qos.reliability=RANT_RELIABLE; ca[0].qos.keep_last=8; ca[0].qos.heartbeat_us=20000;
    ca[1].name="q/be";   ca[1].role=RANT_PUB_ONLY;                    /* best-effort */
    ca[1].qos.keep_last=8;
    ca[2].name="q/rel";  ca[2].role=RANT_PUB_ONLY;
    ca[2].qos.reliability=RANT_RELIABLE; ca[2].qos.keep_last=16;      /* holds the whole burst */
    ca[2].qos.heartbeat_us=20000; ca[2].qos.repair_delay_us=5000;
    ao = (RantNodeOpts){ .domain=TEST_DOMAIN+4, .discovery={ .max_peers=4 }, .net={ .multicast_interface="127.0.0.1" } };
    bo = ao; bo.max_topics = 3;
    test_gap_calls[0]=test_gap_calls[1]=test_gap_calls[2]=test_gap_calls[3]=0;
    a = test_node_open("qa-node", NULL, NULL, ao, ca, 3);
    b = rant_node_open(&ba, "qb-node", qc_on_message, test_on_event, &bo);
    if (b) q = rant_node_create_queue(b);
    if (q){
        RantTopicOpts co; memset(&co, 0, sizeof co);
        co.queue = q;
        co.qos = ca[0].qos; lazy = rant_node_create_topic(b, "q/lazy", RANT_SUB_ONLY, NULL, &co);
        co.qos = ca[1].qos; co.qos.queue_bytes = 4096;   /* a hard cap: forces overwrite oldest */
        be = rant_node_create_topic(b, "q/be", RANT_SUB_ONLY, NULL, &co);
        co.qos = ca[2].qos; co.qos.queue_bytes = 2048;   /* holds about 4 of the 400 B messages: parks */
        rel = rant_node_create_topic(b, "q/rel", RANT_SUB_ONLY, NULL, &co);
    }
    CHECK(a && b && lazy && be && rel, "queue: nodes and queued topics open");
    if (!(a && b && lazy && be && rel)){
        if (a) rant_node_close(a,0);
        if (b) rant_node_close(b,0);
        rant_allocator_reset(&ba); return;
    }

    { uint64_t end = i_rant_plat_now_us()+5000000u;      /* all three matches first */
      while (i_rant_plat_now_us()<end &&
             (test_match_count(a,0)<1 || test_match_count(a,1)<1 ||
              test_match_count(a,2)<1))
          test_pump(a,b,10); }
    CHECK(test_match_count(a,0)==1 && test_match_count(a,2)==1,
             "queue: matches formed");

    /* Q1: polling only parks, and a timed dispatch drives B's own loop */
    for (i=0;i<5;i++){ i_rant_le_w32(payload,(uint32_t)i); test_send(a, 0, payload, 64); }
    { uint64_t end = i_rant_plat_now_us()+5000000u; uint32_t msgs = 0;
      while (msgs<5 && i_rant_plat_now_us()<end){ test_pump(a,b,5); rant_topic_queue_stats(lazy,&msgs,NULL,NULL,NULL); }
      CHECK(msgs==5 && qc_n[0]==0, "queue: polling parks 5, no inline callback (%u, cb=%lu)", msgs, qc_n[0]); }
    { uint64_t end = i_rant_plat_now_us()+5000000u;
      i_rant_le_w32(payload, 5u); test_send(a, 0, payload, 64);
      while (qc_n[0]<6 && i_rant_plat_now_us()<end){
          rant_node_poll(a, 0);
          (void)rant_queue_dispatch(q, 0, 50);          /* waits by pumping b's own loop */
      } }
    CHECK(qc_n[0]==6 && qc_bad[0]==0, "queue: a timed dispatch drains 6 in order (%lu, bad=%lu)",
             qc_n[0], qc_bad[0]);
    CHECK(qc_recv_us != 0, "queue: a dispatched msg carries the poll side arrival stamp");
    CHECK(strcmp(qc_topic, "q/lazy")==0 && strcmp(qc_pub, "qa-node")==0,
             "queue: a dispatched msg carries the topic and sender names (%s, %s)", qc_topic, qc_pub);

    /* Q2: best-effort at a hard cap overwrites oldest, fires RANT_MSG_LOST, keeps newest */
    { uint32_t msgs=0, bytes=0, cap=0, dropped=0;
      for (i=0;i<60;i++){ i_rant_le_w32(payload,(uint32_t)i); test_send(a, 1, payload, 256); test_pump(a,b,1); }
      test_pump(a,b,50);
      rant_topic_queue_stats(be, &msgs, &bytes, &cap, &dropped);
      CHECK(cap==4096 && dropped>0 && msgs>0,
               "queue: BE cap held, oldest dropped (cap=%u msgs=%u dropped=%u)", cap, msgs, dropped);
      (void)rant_queue_dispatch(q, 0, 0);
      CHECK(qc_n[1]>0 && qc_last[1]==59u, "queue: newest survive a BE overflow (got=%lu last=%u)",
               qc_n[1], qc_last[1]);
      CHECK(test_gap_calls[1]>0, "queue: BE queue loss fired RANT_MSG_LOST (%lu)", test_gap_calls[1]);
    }

    /* Q3: reliable plus a tiny ring parks instead of losing. A slow dispatch loop still
       receives everything in order via unpark and the normal repair machinery */
    for (i=0;i<12;i++){ i_rant_le_w32(payload,(uint32_t)i); test_send(a, 2, payload, 400); test_pump(a,b,2); }
    test_pump(a,b,30);
    CHECK(test_drain(a, 2, 0)==0, "queue: parked reader withholds acks (writer not drained)");
    { uint64_t end=i_rant_plat_now_us()+8000000u;
      while (qc_n[2]<12 && i_rant_plat_now_us()<end){
          (void)rant_queue_dispatch(q, 1, 20);
          rant_node_poll(a, 0);
      } }
    CHECK(qc_n[2]==12 && qc_bad[2]==0, "queue: reliable park loses nothing, in order (%lu/12)", qc_n[2]);
    { RantRepairStats rs; test_repair_stats(b, 2, &rs);
      CHECK(rs.msgs_skipped==0 && test_gap_calls[2]==0,
               "queue: no skip during park (skipped=%llu lost_events=%lu)",
               (unsigned long long)rs.msgs_skipped, test_gap_calls[2]); }
    test_pump(a,b,50);
    CHECK(test_drain(a, 2, 2000)==1, "queue: writer fully acked once drained");

    rant_node_close(b, 1);
    rant_node_close(a, 1);
    rant_allocator_reset(&ba);
}

static unsigned long pl_inline;
static void pl_on_message(const RantMsg *msg){ (void)msg; pl_inline++; }

static void pull_checks(void){
    RantAllocator aa = rant_allocator_heap(0), ba = rant_allocator_heap(0);
    RantNodeOpts o; RantTopicOpts co;
    RantNode *a, *b; RantTopic *pub = NULL, *pl = NULL, *plain = NULL;
    RantMsg m; uint8_t payload[64]; int i, r, t;
    uint32_t waiting = 0;
    memset(&o, 0, sizeof o); memset(payload, 0, sizeof payload); memset(&m, 0, sizeof m);
    o.domain = TEST_DOMAIN+52; o.discovery.max_peers = 4; o.max_topics = 4;
    o.net.multicast_interface = "127.0.0.1";
    a = rant_node_open(&aa, "pl-a", NULL, NULL, &o);
    b = rant_node_open(&ba, "pl-b", pl_on_message, NULL, &o);
    memset(&co, 0, sizeof co);
    co.qos.reliability = RANT_RELIABLE; co.qos.keep_last = 16; co.qos.heartbeat_us = 20000;
    if (a) pub = rant_node_create_topic(a, "pl/x", RANT_PUB_ONLY, NULL, &co);
    co.pull = 1;
    if (b) pl = rant_node_create_topic(b, "pl/x", RANT_SUB_ONLY, NULL, &co);
    co.pull = 0;
    if (b) plain = rant_node_create_topic(b, "pl/plain", RANT_SUB_ONLY, NULL, &co);
    CHECK(a && b && pub && pl && plain, "pull: nodes and topics open");
    if (!(a && b && pub && pl && plain)) goto done;
    {   RantQueue *q = rant_node_create_queue(b);
        co.pull = 1; co.queue = q;
        CHECK(q && !rant_node_create_topic(b, "pl/both", RANT_SUB_ONLY, NULL, &co)
                 && rant_last_error(b).error == RANT_E_STATE, "pull: pull with a queue is refused");
        co.pull = 0; co.queue = NULL; }
    r = rant_topic_take(plain, &m, 0);
    CHECK(r == RANT_ERR_STATE, "pull: take on a topic created without pull is refused (%d)", r);
    r = rant_topic_take(pl, &m, 0);
    CHECK(r == 0, "pull: an empty take returns 0 (%d)", r);
    PUMP_UNTIL(a, b, 10000, rant_topic_match_count(pub) >= 1);

    for (i=0;i<5;i++){ i_rant_le_w32(payload,(uint32_t)i); rant_topic_send(pub, rant_bytes(payload, 64), NULL); }
    for (t=0;t<2500 && waiting<5;t++){ test_pump(a,b,2); rant_topic_queue_stats(pl,&waiting,NULL,NULL,NULL); }
    CHECK(waiting==5 && pl_inline==0, "pull: polling holds 5, no callback (%u, cb=%lu)", waiting, pl_inline);
    r = rant_topic_take(pl, &m, 0);
    CHECK(r==1 && i_rant_le_r32(m.data.data)==0 && m.data.len==64 && m.recv_us && m.written_us,
             "pull: take returns the oldest with its stamps (r=%d v=%u)", r, r==1 ? i_rant_le_r32(m.data.data) : 0);
    CHECK(m.topic_name.len==4 && !memcmp(m.topic_name.data,"pl/x",4)
             && m.publisher_name.len==4 && !memcmp(m.publisher_name.data,"pl-a",4),
             "pull: the taken msg carries the topic and sender names");
    r = rant_topic_take_latest(pl, &m, 0);
    rant_topic_queue_stats(pl,&waiting,NULL,NULL,NULL);
    CHECK(r==1 && i_rant_le_r32(m.data.data)==4, "pull: take_latest returns the newest (v=%u)",
             r==1 ? i_rant_le_r32(m.data.data) : 0);
    r = rant_topic_take(pl, &m, 0);
    CHECK(r==0, "pull: take_latest dropped the older ones (%d)", r);

    /* a timed take drives b's own loop */
    i_rant_le_w32(payload, 9u); rant_topic_send(pub, rant_bytes(payload, 64), NULL);
    r = 0;
    for (t=0;t<200 && r!=1;t++){ rant_node_poll(a, 0); r = rant_topic_take(pl, &m, 20); }
    CHECK(r==1 && i_rant_le_r32(m.data.data)==9 && pl_inline==0, "pull: a timed take pumps its own loop (r=%d)", r);

    /* a burst read with take_latest while it arrives ends on the newest */
    for (i=0;i<12;i++){ i_rant_le_w32(payload,100u+(uint32_t)i); rant_topic_send(pub, rant_bytes(payload, 64), NULL); test_pump(a,b,1); }
    {   uint32_t last = 0; uint64_t end = i_rant_plat_now_us()+5000000u;
        while (last != 111u && i_rant_plat_now_us() < end){
            test_pump(a,b,2);
            if (rant_topic_take_latest(pl, &m, 0) == 1) last = i_rant_le_r32(m.data.data);
        }
        CHECK(last==111u, "pull: take_latest reaches the newest of a burst (%u)", last); }
done:
    if (a) rant_node_close(a, 1);
    if (b) rant_node_close(b, 1);
    rant_allocator_reset(&aa); rant_allocator_reset(&ba);
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
    test_on_event(ev);
    cq_ev_n++;
    if (ev->kind == RANT_ERROR){
        cq_ev_error = ev->error;
        cq_ev_topic[0] = 0;
        if (ev->topic_name){ strncpy(cq_ev_topic, ev->topic_name, sizeof cq_ev_topic - 1); cq_ev_topic[sizeof cq_ev_topic - 1] = 0; }
    }
}
static void cq_on_message(const RantMsg *m){
    uint32_t v = i_rant_le_r32(m->data.data);
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

    uint8_t payload[64];
    i_RantTopicDef ca[3];
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
    ao = (RantNodeOpts){ .domain=TEST_DOMAIN+5, .discovery={ .max_peers=4 }, .net={ .multicast_interface="127.0.0.1" } };
    bo = ao; bo.max_topics = 3;
    a = test_node_open("cqa-node", NULL, NULL, ao, ca, 3);
    b = test_node_open("cqb-node", cq_on_message, cq_on_event, bo, NULL, 0);
    CHECK(a && b, "cqueue: nodes open");
    if (!a || !b){ if (a) rant_node_close(a,0); if (b) rant_node_close(b,0); return; }
    cq_b = b;
    cq_q = rant_node_create_queue(b);
    CHECK(cq_q != NULL, "cqueue: queue created");
    memset(&co, 0, sizeof co); co.qos = ca[0].qos; co.queue = cq_q;
    cq_x = rant_node_create_topic(b, "cq/x", RANT_SUB_ONLY, NULL, &co);
    cq_y = rant_node_create_topic(b, "cq/y", RANT_SUB_ONLY, NULL, &co);
    co.queue = NULL;
    in = rant_node_create_topic(b, "cq/in", RANT_SUB_ONLY, NULL, &co);
    CHECK(cq_x && cq_y && in, "cqueue: topics created (x=%d y=%d in=%d)", !!cq_x, !!cq_y, !!in);
    if (!cq_x || !cq_y || !in){ rant_node_close(b,0); rant_node_close(a,0); return; }

    /* a queue of another node is refused loudly */
    co.queue = cq_q;
    CHECK(rant_node_create_topic(a, "cq/foreign", RANT_SUB_ONLY, NULL, &co) == NULL
             && rant_last_error(a).error == RANT_E_STATE,
             "cqueue: another node's queue refused with STATE (%d)", (int)rant_last_error(a).error);
    /* the queue budget */
    for (i = 1; i < RANT_QUEUES_MAX; i++) if (!rant_node_create_queue(b)) break;
    CHECK(i == RANT_QUEUES_MAX && rant_node_create_queue(b) == NULL
             && rant_last_error(b).error == RANT_E_STATE,
             "cqueue: the %dth queue is refused with STATE (made %d)", RANT_QUEUES_MAX + 1, i);

    { uint64_t end = i_rant_plat_now_us()+5000000u;
      while (i_rant_plat_now_us()<end &&
             (test_match_count(a,0)<1 || test_match_count(a,1)<1 ||
              test_match_count(a,2)<1))
          test_pump(a,b,10); }
    CHECK(test_match_count(a,0)==1 && test_match_count(a,1)==1
             && test_match_count(a,2)==1, "cqueue: matches formed");

    /* CQ1: six sends in a known arrival order, x0 y1 x2 in3 y4 x5. The inline one fires
       during the poll, the queued ones wait */
    memset(cq_seen, 0, sizeof cq_seen); cq_n_order = 0; cq_inline_rc = 1;
    {   static const uint16_t on[6] = { 0, 1, 0, 2, 1, 0 };
        for (i=0;i<6;i++){ i_rant_le_w32(payload,(uint32_t)i); test_send(a, on[i], payload, 64); test_pump(a,b,5); }
    }
    test_pump(a,b,30);
    rant_queue_stats(cq_q, &waiting, &dropped);
    CHECK(cq_seen[2]==1 && cq_seen[0]==0 && cq_seen[1]==0 && waiting==5 && dropped==0,
             "cqueue: inline fired in the poll, 5 parked (in=%lu x=%lu y=%lu waiting=%u)",
             cq_seen[2], cq_seen[0], cq_seen[1], waiting);
#ifdef RANT_THREADS
    CHECK(cq_inline_rc == RANT_ERR_STATE, "cqueue: dispatch from an inline callback is refused (%d)", cq_inline_rc);
#endif

    /* CQ2: a capped dispatch runs the two oldest, across topics, in arrival order */
    cq_n_order = 0;
    r = rant_queue_dispatch(cq_q, 2, 0);
    CHECK(r==2 && cq_n_order==2 && cq_order[0]==0 && cq_order[1]==1,
             "cqueue: max 2 ran x0 then y1 (r=%d got %u %u)", r, cq_order[0], cq_order[1]);
    CHECK(cq_nested_rc == RANT_ERR_STATE, "cqueue: nested dispatch refused (%d)", cq_nested_rc);
    CHECK(cq_retire_self_rc == RANT_ERR_STATE, "cqueue: retire from its own callback refused (%d)", cq_retire_self_rc);
    CHECK(cq_close_rc == RANT_ERR_STATE, "cqueue: close from a dispatched callback refused (%d)", cq_close_rc);
    r = rant_queue_dispatch(cq_q, 0, 0);
    CHECK(r==3 && cq_n_order==5 && cq_order[2]==2 && cq_order[3]==4 && cq_order[4]==5,
             "cqueue: the rest ran in arrival order (r=%d got %u %u %u)", r, cq_order[2], cq_order[3], cq_order[4]);
    CHECK(cq_retire_other_rc == RANT_OK, "cqueue: retire of an idle sibling from a callback (%d)", cq_retire_other_rc);
    r = rant_queue_dispatch(cq_q, 0, 0);
    CHECK(r==0, "cqueue: nothing left (%d)", r);
    { uint64_t t0 = i_rant_plat_now_us();
      r = rant_queue_dispatch(cq_q, 0, 30);
      CHECK(r==0 && i_rant_plat_now_us()-t0 >= 25000u, "cqueue: a timed wait pumps and returns 0 (%d)", r); }

    /* CQ3: y comes back on the queue in its retired slot, and a retire from outside drops
       what it had not run yet */
    co.queue = cq_q;
    cq_y = rant_node_create_topic(b, "cq/y", RANT_SUB_ONLY, NULL, &co);
    CHECK(cq_y != NULL, "cqueue: y recreated on the queue");
    /* a must process the hole and the recreate before the first send: the lane re forms
       future only, so a message committed to the old reader is a late subscriber's loss */
    test_pump(a,b,300);
    for (i=0;i<3;i++){ i_rant_le_w32(payload,100u+(uint32_t)i); test_send(a, 1, payload, 64); test_pump(a,b,5); }
    { uint64_t end = i_rant_plat_now_us()+3000000u;         /* the reformed lane may still repair */
      do { test_pump(a,b,10); rant_queue_stats(cq_q, &waiting, NULL); } while (waiting<3 && i_rant_plat_now_us()<end); }
    r = cq_y ? rant_topic_retire(cq_y) : -1;
    CHECK(waiting==3 && r==RANT_OK, "cqueue: 3 parked, retire drops them (waiting=%u rc=%d)", waiting, r);
    rant_queue_stats(cq_q, &waiting, NULL);
    CHECK(waiting==0, "cqueue: nothing waiting after the retire (%u)", waiting);

#ifdef RANT_THREADS
    /* CQ4: under service threads a timed dispatch wakes on arrival and runs the callback on
       the caller's thread. The starts have side effects, so they run outside CHECK. */
    r = (rant_node_start(a)==RANT_OK && rant_node_start(b)==RANT_OK);
    CHECK(r, "cqueue: services start");
    cq_n_order = 0; cq_thread = 0;
    for (i=0;i<20;i++){
        i_rant_le_w32(payload,200u+(uint32_t)i);
        test_send(a, 0, payload, 64);
        if (rant_queue_dispatch(cq_q, 0, 2000) < 1 || cq_order[cq_n_order-1] != 200u+(uint32_t)i) break;
    }
    CHECK(i==20 && cq_thread == i_rant_plat_thread_id(),
             "cqueue: threaded dispatch delivers 20 in order on this thread (%d)", i);
    rant_node_stop(b); rant_node_stop(a);
#endif
    /* CQ5: events park on the event queue, last_error stays current, the strings are
       copied, and NULL drops what is parked */
    r = rant_node_set_event_queue(b, cq_q);
    CHECK(r==RANT_OK, "cqueue: event queue set (%d)", r);
    cq_ev_n = 0; cq_ev_error = RANT_E_NONE;
    CHECK(rant_node_create_topic(b, "@bad", RANT_SUB_ONLY, NULL, NULL)==NULL
             && rant_last_error(b).error==RANT_E_BAD_NAME && cq_ev_n==0,
             "cqueue: error event parked, last_error current (n=%d err=%d)", cq_ev_n, (int)rant_last_error(b).error);
    rant_queue_stats(cq_q, &waiting, NULL);
    r = rant_queue_dispatch(cq_q, 0, 0);
    CHECK(waiting==1 && r==1 && cq_ev_n==1 && cq_ev_error==RANT_E_BAD_NAME && strcmp(cq_ev_topic, "@bad")==0,
             "cqueue: event dispatched with its copied name (r=%d n=%d name=%s)", r, cq_ev_n, cq_ev_topic);
    (void)rant_node_create_topic(b, "@bad2", RANT_SUB_ONLY, NULL, NULL);
    rant_queue_stats(cq_q, &waiting, NULL);
    r = rant_node_set_event_queue(b, NULL);
    CHECK(waiting==1 && r==RANT_OK && cq_ev_n==1, "cqueue: NULL drops the parked event (rc=%d n=%d)", r, cq_ev_n);
    rant_queue_stats(cq_q, &waiting, NULL);
    (void)rant_node_create_topic(b, "@bad3", RANT_SUB_ONLY, NULL, NULL);
    CHECK(waiting==0 && cq_ev_n==2, "cqueue: events inline again (waiting=%u n=%d)", waiting, cq_ev_n);

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
    for (t = 0; t < 500; t++){ test_pump(P, C, 2); rant_queue_stats(q, &waiting, NULL); if (waiting) break; }
    return waiting;
}

static void callback_pattern_checks(void){
    RantAllocator pa = rant_allocator_heap(0);
    RantAllocator ca = rant_allocator_heap(0);
    RantNodeOpts po, co; RantNode *P=NULL, *C=NULL; RantAddr seed;
    RantFunction *cadd, *ctask, *cnone, *pinl, *cinl;
    RantVariable *pvar, *cvar;
    uint32_t waiting, id = 0; int t, r, r2;
    uint8_t req[8], val[4];
    uint16_t dom = TEST_DOMAIN+40;

    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=dom; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    co=po;
    P = rant_node_open(&pa, "cq-prov", NULL, NULL, &po);
    C = rant_node_open(&ca, "cq-call", NULL, NULL, &co);
    CHECK(P && C, "cpat: nodes open");
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
    CHECK(cp_pq && cp_cq && cp_def && cadd && cp_tdef && ctask && cnone && pinl && cinl && pvar && cvar,
             "cpat: queues and handles created");
    if (!(cp_pq && cp_cq && cp_def && cadd && cp_tdef && ctask && cnone && pinl && cinl && pvar && cvar)){
        rant_node_close(C,0); rant_node_close(P,0); return;
    }
    rant_function_on_cancel(cp_tdef, cp_on_cancel, NULL);
    rant_variable_on_write(pvar, cp_on_write, NULL);
    for (t=0;t<2000 && (rant_function_match_count(cadd)==0 || rant_function_match_count(ctask)==0
                        || rant_function_match_count(cinl)==0 || !rant_variable_get(cvar, NULL));t++)
        test_pump(P,C,2);
    CHECK(rant_function_match_count(cadd)==1 && rant_variable_get(cvar, NULL),
             "cpat: providers matched, the initial value cached (%d)", rant_function_match_count(cadd));

    /* CP1: the request parks at the provider, the reply parks at the caller */
    i_rant_le_w32(req,2); i_rant_le_w32(req+4,3);
    cp_req_runs=0; cp_rsp_done=0; cp_req_retire_rc=1;
    r = rant_function_call_async(cadd, rant_bytes(req,8), cp_on_reply, NULL, NULL);
    waiting = cp_wait_parked(P, C, cp_pq);
    CHECK(r==RANT_OK && waiting==1 && cp_req_runs==0,
             "cpat: request parked at the provider (waiting=%u runs=%d)", waiting, cp_req_runs);
    r = rant_queue_dispatch(cp_pq, 0, 0);
    CHECK(r==1 && cp_req_runs==1 && cp_req_retire_rc==RANT_ERR_STATE,
             "cpat: dispatch ran the handler, its own retire refused (r=%d rc=%d)", r, cp_req_retire_rc);
#ifdef RANT_THREADS
    CHECK(cp_req_thread == i_rant_plat_thread_id(), "cpat: handler ran on the dispatching thread");
#endif
    waiting = cp_wait_parked(P, C, cp_cq);
    CHECK(waiting==1 && cp_rsp_done==0, "cpat: reply parked at the caller (waiting=%u done=%d)", waiting, cp_rsp_done);
    r = rant_queue_dispatch(cp_cq, 0, 0);
    CHECK(r==1 && cp_rsp_done==1 && cp_rsp_status==RANT_CALL_OK && cp_rsp_val==5,
             "cpat: dispatched reply add(2,3)=5 (r=%d st=%d val=%u)", r, cp_rsp_status, cp_rsp_val);

#ifdef RANT_THREADS
    /* CP2: the blocking call completes inline through a queued rsp channel, nothing parks */
    {   RantResponse out;
        rant_node_start(P);      /* the inline provider answers on its own thread */
        i_rant_le_w32(req,4); i_rant_le_w32(req+4,5);
        r = rant_function_call(cinl, rant_bytes(req,8), &out, 2000, NULL);
        rant_node_stop(P);
        rant_queue_stats(cp_cq, &waiting, NULL);
        CHECK(r==1 && out.status==RANT_CALL_OK && out.data.len==4 && i_rant_le_r32(out.data.data)==9 && waiting==0,
                 "cpat: blocking call completes inline on a queued handle (r=%d st=%d waiting=%u)", r, out.status, waiting);
    }
#endif

    /* CP3: a synthesized outcome parks like a delivered one */
    cp_rsp_done=0;
    r = rant_function_call_async(cnone, rant_bytes(NULL,0), cp_on_reply, NULL, NULL);
    waiting = cp_wait_parked(P, C, cp_cq);
    CHECK(r==RANT_OK && waiting==1 && cp_rsp_done==0, "cpat: NO_PROVIDER outcome parked (waiting=%u)", waiting);
    r = rant_queue_dispatch(cp_cq, 0, 0);
    CHECK(r==1 && cp_rsp_done==1 && cp_rsp_status==RANT_CALL_NO_PROVIDER,
             "cpat: NO_PROVIDER dispatched (r=%d st=%d)", r, cp_rsp_status);

    /* CP4: a task: RUNNING, progress, the cancel and the terminal outcome each park */
    cp_token=0; cp_prog_n=0; cp_prog_first_empty=0; cp_cancel_calls=0; cp_rsp_done=0; id=0;
    r = rant_function_call_async(ctask, rant_bytes(NULL,0), cp_on_reply, NULL,
                                 &(RantCallOpts){ .on_progress = cp_on_progress, .id_out = &id });
    waiting = cp_wait_parked(P, C, cp_pq);
    r2 = rant_queue_dispatch(cp_pq, 0, 0);          /* the handler defers, RUNNING goes out */
    CHECK(r==RANT_OK && waiting==1 && r2==1 && cp_token!=0, "cpat: task request dispatched and deferred (r=%d)", r2);
    waiting = cp_wait_parked(P, C, cp_cq);
    CHECK(waiting==1 && cp_prog_n==0, "cpat: RUNNING parked (waiting=%u)", waiting);
    r = rant_queue_dispatch(cp_cq, 0, 0);
    CHECK(r==1 && cp_prog_n==1 && cp_prog_first_empty, "cpat: RUNNING dispatched as empty progress (n=%d)", cp_prog_n);
    i_rant_le_w32(val, 42);
    r = rant_function_progress(cp_tdef, cp_token, rant_bytes(val,4));
    waiting = cp_wait_parked(P, C, cp_cq);
    CHECK(r==RANT_OK && waiting==1 && cp_prog_n==1, "cpat: progress parked (waiting=%u)", waiting);
    r = rant_queue_dispatch(cp_cq, 0, 0);
    CHECK(r==1 && cp_prog_n==2 && cp_prog_last==42, "cpat: progress dispatched (last=%u)", cp_prog_last);
    r = rant_function_cancel(ctask, id);
    waiting = cp_wait_parked(P, C, cp_pq);
    CHECK(r==RANT_OK && waiting==1 && cp_cancel_calls==0 && rant_function_cancelled(cp_tdef, cp_token)==1,
             "cpat: cancel flag set at receipt, on_cancel parked (rc=%d waiting=%u)", r, waiting);
    r = rant_queue_dispatch(cp_pq, 0, 0);
    CHECK(r==1 && cp_cancel_calls==1, "cpat: on_cancel dispatched (%d)", cp_cancel_calls);
    r = rant_function_complete(cp_tdef, cp_token, RANT_CALL_CANCELLED, NULL, rant_bytes(NULL,0));
    waiting = cp_wait_parked(P, C, cp_cq);
    r2 = rant_queue_dispatch(cp_cq, 0, 0);
    CHECK(r==RANT_OK && waiting==1 && r2==1 && cp_rsp_done==1 && cp_rsp_status==RANT_CALL_CANCELLED,
             "cpat: terminal CANCELLED dispatched (rc=%d st=%d)", r, cp_rsp_status);

    /* CP5: variables: the replay parks, the owner applies a remote write at receipt and
       notifies at dispatch, the remote caches the new value before its own dispatch */
    cp_change_n=0;
    rant_variable_on_change(cvar, cp_on_change, NULL);
    rant_queue_stats(cp_cq, &waiting, NULL);
    CHECK(waiting==1 && cp_change_n==0, "cpat: on_change replay parked (waiting=%u)", waiting);
    r = rant_queue_dispatch(cp_cq, 0, 0);
    CHECK(r==1 && cp_change_n==1 && cp_change_val==7, "cpat: replay dispatched with the value (val=%u)", cp_change_val);
    i_rant_le_w32(val, 8); cp_write_n=0;
    r = rant_variable_set(cvar, rant_bytes(val,4));
    waiting = cp_wait_parked(P, C, cp_pq);
    {   RantBytes cur; int has = rant_variable_get(pvar, &cur);
        CHECK(r==RANT_OK && waiting==1 && cp_write_n==0 && has && cur.len==4 && i_rant_le_r32(cur.data)==8,
                 "cpat: owner applied the write at receipt, on_write parked (waiting=%u)", waiting); }
    r = rant_queue_dispatch(cp_pq, 0, 0);
    CHECK(r==1 && cp_write_n==1 && cp_write_val==8 && cp_write_src!=0,
             "cpat: on_write dispatched with the write's value and source (val=%u src=%u)", cp_write_val, cp_write_src);
    waiting = cp_wait_parked(P, C, cp_cq);
    {   RantBytes cur; rant_variable_get(cvar, &cur);
        CHECK(waiting==1 && cp_change_n==1 && cur.len==4 && i_rant_le_r32(cur.data)==8,
                 "cpat: remote cached 8 at receipt, on_change parked (waiting=%u)", waiting); }
    r = rant_queue_dispatch(cp_cq, 0, 0);
    CHECK(r==1 && cp_change_n==2 && cp_change_val==8, "cpat: on_change dispatched (val=%u)", cp_change_val);

    /* CP6: a retire with a parked reply answers it CANCELLED inline and drops its records */
    i_rant_le_w32(req,1); i_rant_le_w32(req+4,1);
    cp_rsp_done=0;
    rant_function_call_async(cadd, rant_bytes(req,8), cp_on_reply, NULL, NULL);
    (void)cp_wait_parked(P, C, cp_pq);
    (void)rant_queue_dispatch(cp_pq, 0, 0);
    waiting = cp_wait_parked(P, C, cp_cq);
    r = rant_function_retire(cadd);
    CHECK(waiting==1 && r==RANT_OK && cp_rsp_done==1 && cp_rsp_status==RANT_CALL_CANCELLED,
             "cpat: retire settles the parked reply CANCELLED (rc=%d done=%d st=%d)", r, cp_rsp_done, cp_rsp_status);
    rant_queue_stats(cp_cq, &waiting, NULL);
    CHECK(waiting==0, "cpat: the retired handle's records are gone (%u)", waiting);

    rant_node_close(C, 1);
    rant_node_close(P, 1);
}

static const TestCase cases[] = {
    { "queue", queue_checks },
    { "pull", pull_checks },
    { "callback_queue", callback_queue_checks },
    { "callback_pattern", callback_pattern_checks },
};

TEST_MAIN(cases)
