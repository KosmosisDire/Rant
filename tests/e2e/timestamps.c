/* The source timestamp: stamped at the writer, the opt out, replay keeps it, and the
 * queued path and the patterns carry it. spec/testing.md says what each case pins. */
#include "support/test.h"

/* The source timestamp phase: every message carries the writer's wall clock unless the
 * topic opts out. Delivery, opt out, replay, the queued path and the patterns are covered. */
#define TS_CH_PLAIN  0   /* reliable, catch_up 2: the stamp + the late-joiner replay */
#define TS_CH_OFF    1   /* publisher sets qos.no_timestamp: stamp 0, bytes untouched */
#define TS_CH_QUEUED 2   /* the subscriber parks it on a callback queue */
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

    static unsigned char big[64*1024];
    i_RantTopicDef ca[4], cb[4];
    RantNodeOpts ao, bo; RantAddr seed;
    RantNode *A, *B;
    RantQueue *ts_q = NULL;
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
    memset(&ao,0,sizeof ao); ao.domain=(uint16_t)(TEST_DOMAIN+50); ao.discovery.max_peers=4;
    ao.net.multicast_interface="127.0.0.1"; ao.net.seed_peers=&seed; ao.net.n_seed_peers=1;
    bo = ao;
    A = test_node_open("ts-pub", NULL, NULL, ao, ca, 4);
    bo.max_topics = 4;
    B = test_node_open("ts-sub", ts_on_message, NULL, bo, cb, 0);
    if (B) ts_q = rant_node_create_queue(B);
    for (i=0;i<4 && ts_q;i++){
        RantTopicOpts co; memset(&co, 0, sizeof co);
        co.qos = cb[i].qos;
        co.queue = i == TS_CH_QUEUED ? ts_q : NULL;
        if (!rant_node_create_topic(B, cb[i].name, RANT_SUB_ONLY, NULL, &co)) ts_q = NULL;
    }
    CHECK(A && B && ts_q, "writets: nodes open");
    if (!ts_q && B){ rant_node_close(B,0); B = NULL; }
    if (!(A && B)){ if (A) rant_node_close(A,0); if (B) rant_node_close(B,0); return; }
    { uint64_t end = i_rant_plat_now_us()+5000000u;
      while (i_rant_plat_now_us()<end &&
             (test_match_count(A,TS_CH_PLAIN)<1 ||
              test_match_count(A,TS_CH_OFF)<1 ||
              test_match_count(A,TS_CH_QUEUED)<1 ||
              test_match_count(A,TS_CH_BIG)<1))
          test_pump(A,B,10); }
    CHECK(test_match_count(A,TS_CH_PLAIN)==1
          && test_match_count(A,TS_CH_BIG)==1, "writets: matches formed");

    /* (a) a stamped message: written_us lies between the wall clock before the send and the
       wall clock read inside the callback */
    before = i_rant_plat_wall_us();
    test_send(A, TS_CH_PLAIN, payload, sizeof payload);
    PUMP_UNTIL(A, B, 3200, ts_msgs[TS_CH_PLAIN]!=0);
    after = ts_recv_wall[TS_CH_PLAIN];
    CHECK(ts_msgs[TS_CH_PLAIN]==1, "writets: stamped message delivered (%lu)", ts_msgs[TS_CH_PLAIN]);
    CHECK(ts_sent[TS_CH_PLAIN] >= before && ts_sent[TS_CH_PLAIN] <= after,
             "writets: written_us within [before, delivered] (%llu in [%llu, %llu])",
             (unsigned long long)ts_sent[TS_CH_PLAIN], (unsigned long long)before,
             (unsigned long long)after);
    CHECK(ts_len[TS_CH_PLAIN]==sizeof payload,
             "writets: the stamp is stripped, payload intact (%lu B)", (unsigned long)ts_len[TS_CH_PLAIN]);

    /* (b) the opt-out: written_us 0, payload byte-identical */
    {   unsigned long want = 0; size_t k;
        for (k=0;k<sizeof payload;k++) want += payload[k];
        test_send(A, TS_CH_OFF, payload, sizeof payload);
        PUMP_UNTIL(A, B, 3200, ts_msgs[TS_CH_OFF]!=0);
        CHECK(ts_msgs[TS_CH_OFF]==1 && ts_sent[TS_CH_OFF]==0,
                 "writets: no_timestamp topic delivers written_us 0 (n=%lu ts=%llu)",
                 ts_msgs[TS_CH_OFF], (unsigned long long)ts_sent[TS_CH_OFF]);
        CHECK(ts_len[TS_CH_OFF]==sizeof payload && ts_sum[TS_CH_OFF]==want,
                 "writets: opted-out payload byte-identical (%lu B)", (unsigned long)ts_len[TS_CH_OFF]);
    }

    /* (c) catch_up replay: a LATE joiner gets the original stamp, not a fresh one */
    {   RantAllocator la = rant_allocator_heap(0);
        RantNodeOpts lo = ao; RantNode *L; RantTopic *lt;
        RantTopicOpts lopt; uint64_t join_wall;
        memset(&lopt,0,sizeof lopt); lopt.qos = ca[TS_CH_PLAIN].qos;
        test_pump(A,B,50);
        join_wall = i_rant_plat_wall_us();
        ts_late_msgs = 0; ts_late_sent = 0;
        lo.max_topics = 1;
        L = rant_node_open(&la, "ts-late", ts_late_on_message, NULL, &lo);
        lt = L ? rant_node_create_topic(L, "ts/plain", RANT_SUB_ONLY, NULL, &lopt) : NULL;
        CHECK(L && lt, "writets: late joiner opens");
        if (L && lt){
            for (t=0;t<2000 && ts_late_msgs==0;t++){ test_pump(A,B,2); rant_node_poll(L,2); }
            CHECK(ts_late_msgs>=1, "writets: replayed history reached the late joiner (%lu)",
                     ts_late_msgs);
            CHECK(ts_late_sent==ts_sent[TS_CH_PLAIN],
                     "writets: replay keeps the ORIGINAL stamp (%llu == %llu)",
                     (unsigned long long)ts_late_sent, (unsigned long long)ts_sent[TS_CH_PLAIN]);
            CHECK(ts_late_sent!=0 && ts_late_sent < join_wall,
                     "writets: the stamp predates the join (%llu < %llu)",
                     (unsigned long long)ts_late_sent, (unsigned long long)join_wall);
            rant_node_close(L,1);
        }
        rant_allocator_reset(&la);
    }

    /* (d) the queued path: a dispatch surfaces the same stamp an inline callback would */
    {   RantTopic *qt = rant_node_topic(B, TS_CH_QUEUED);
        uint32_t waiting = 0; uint64_t q_before;
        q_before = i_rant_plat_wall_us();
        test_send(A, TS_CH_QUEUED, payload, sizeof payload);
        for (t=0;t<800 && waiting==0;t++){ test_pump(A,B,2); rant_topic_queue_stats(qt,&waiting,NULL,NULL,NULL); }
        CHECK(waiting==1 && ts_msgs[TS_CH_QUEUED]==0,
                 "writets: queued message parked, no inline callback (%u cb=%lu)", waiting, ts_msgs[TS_CH_QUEUED]);
        (void)rant_queue_dispatch(ts_q, 0, 0);
        CHECK(ts_msgs[TS_CH_QUEUED]==1 && ts_sent[TS_CH_QUEUED] >= q_before
                 && ts_sent[TS_CH_QUEUED] <= ts_recv_wall[TS_CH_QUEUED],
                 "writets: a dispatched message carries the stamp (%llu >= %llu)",
                 (unsigned long long)ts_sent[TS_CH_QUEUED], (unsigned long long)q_before);
        CHECK(ts_len[TS_CH_QUEUED]==sizeof payload,
                 "writets: dispatched payload intact (%lu B)", (unsigned long)ts_len[TS_CH_QUEUED]);
    }

    /* (e) a fragmenting payload (the SHM path where compiled in): the stamp rides the
       reassembled/shared bytes just the same */
    {   unsigned long want = 0; size_t k;
        for (k=0;k<sizeof big;k++){ big[k]=(unsigned char)((k*31u+7u)&0xFF); want += big[k]; }
        before = i_rant_plat_wall_us();
        test_send(A, TS_CH_BIG, big, sizeof big);
        PUMP_UNTIL(A, B, 8000, ts_msgs[TS_CH_BIG]!=0);
        CHECK(ts_msgs[TS_CH_BIG]==1 && ts_len[TS_CH_BIG]==sizeof big && ts_sum[TS_CH_BIG]==want,
                 "writets: %lu B message byte-exact (n=%lu len=%lu)", (unsigned long)sizeof big,
                 ts_msgs[TS_CH_BIG], (unsigned long)ts_len[TS_CH_BIG]);
        CHECK(ts_sent[TS_CH_BIG] >= before && ts_sent[TS_CH_BIG] <= ts_recv_wall[TS_CH_BIG],
                 "writets: big message carries the stamp (%llu)", (unsigned long long)ts_sent[TS_CH_BIG]);
#ifdef RANT_SHM
        {   uint32_t shm_tx = 0;
            rant_node_shm_stats(A, &shm_tx, NULL);     /* same host: it went through shared memory */
            CHECK(shm_tx>=1, "writets: the big message took the SHM path (tx=%u)", shm_tx); }
#endif
    }

    /* (f) the capture stamp: per message, costs nothing unset, and its marker bit never
       leaks into written_us. The plain, fragmenting, queued and opted out paths agree. */
    {   RantTopic *pt = rant_node_topic(A, TS_CH_PLAIN);
        RantTopic *ot = rant_node_topic(A, TS_CH_OFF);
        RantTopic *bt = rant_node_topic(A, TS_CH_BIG);
        const uint64_t want = 1234567890123456ull;   /* a wall clock well inside 51 bits */
        uint64_t b0 = 0, b1 = 0, b2 = 0;

        /* unset: capture_us 0, and the sample is no larger than before */
        ts_msgs[TS_CH_PLAIN]=0; ts_capture[TS_CH_PLAIN]=1;
        rant_topic_counts(pt, NULL, &b0, NULL, NULL);
        rant_topic_send(pt, rant_bytes(payload, sizeof payload), NULL);
        PUMP_UNTIL(A, B, 3200, ts_msgs[TS_CH_PLAIN]!=0);
        rant_topic_counts(pt, NULL, &b1, NULL, NULL);
        CHECK(ts_msgs[TS_CH_PLAIN]==1 && ts_capture[TS_CH_PLAIN]==0,
                 "capture: an unset capture delivers 0 (%llu)",
                 (unsigned long long)ts_capture[TS_CH_PLAIN]);

        /* set: delivered exactly, written_us still a sane wall clock beside it */
        before = i_rant_plat_wall_us();
        ts_msgs[TS_CH_PLAIN]=0; ts_capture[TS_CH_PLAIN]=0;
        rant_topic_send(pt, rant_bytes(payload, sizeof payload),
                        &(RantSendOpts){ .capture_us = want });
        PUMP_UNTIL(A, B, 3200, ts_msgs[TS_CH_PLAIN]!=0);
        rant_topic_counts(pt, NULL, &b2, NULL, NULL);
        CHECK(ts_msgs[TS_CH_PLAIN]==1 && ts_capture[TS_CH_PLAIN]==want,
                 "capture: capture_us delivered exactly (%llu)",
                 (unsigned long long)ts_capture[TS_CH_PLAIN]);
        CHECK(ts_sent[TS_CH_PLAIN] >= before
              && ts_sent[TS_CH_PLAIN] <= ts_recv_wall[TS_CH_PLAIN],
                 "capture: the marker never leaks into written_us (%llu in [%llu, %llu])",
                 (unsigned long long)ts_sent[TS_CH_PLAIN], (unsigned long long)before,
                 (unsigned long long)ts_recv_wall[TS_CH_PLAIN]);
        CHECK(ts_len[TS_CH_PLAIN]==sizeof payload,
                 "capture: both stamps stripped, payload intact (%lu B)",
                 (unsigned long)ts_len[TS_CH_PLAIN]);
        CHECK((b2 - b1) == (b1 - b0) + RANT_CAPTURE_BYTES,
                 "capture: it costs its 8 bytes only when set (%llu vs %llu)",
                 (unsigned long long)(b2 - b1), (unsigned long long)(b1 - b0));

        /* a no_timestamp topic frames neither slot, so a capture cannot ride it */
        ts_msgs[TS_CH_OFF]=0; ts_capture[TS_CH_OFF]=1;
        rant_topic_send(ot, rant_bytes(payload, sizeof payload),
                        &(RantSendOpts){ .capture_us = want });
        PUMP_UNTIL(A, B, 3200, ts_msgs[TS_CH_OFF]!=0);
        CHECK(ts_msgs[TS_CH_OFF]==1 && ts_capture[TS_CH_OFF]==0
              && ts_sent[TS_CH_OFF]==0 && ts_len[TS_CH_OFF]==sizeof payload,
                 "capture: an opted out topic carries neither stamp (cap=%llu len=%lu)",
                 (unsigned long long)ts_capture[TS_CH_OFF], (unsigned long)ts_len[TS_CH_OFF]);

        /* the fragmenting path, which is SHM where it is compiled in */
        ts_msgs[TS_CH_BIG]=0; ts_capture[TS_CH_BIG]=0;
        rant_topic_send(bt, rant_bytes(big, sizeof big),
                        &(RantSendOpts){ .capture_us = want });
        PUMP_UNTIL(A, B, 8000, ts_msgs[TS_CH_BIG]!=0);
        CHECK(ts_msgs[TS_CH_BIG]==1 && ts_capture[TS_CH_BIG]==want
              && ts_len[TS_CH_BIG]==sizeof big,
                 "capture: a fragmenting message carries it (cap=%llu len=%lu)",
                 (unsigned long long)ts_capture[TS_CH_BIG], (unsigned long)ts_len[TS_CH_BIG]);

        /* the queued path: a dispatch surfaces it exactly as an inline callback would */
        ts_msgs[TS_CH_QUEUED]=0; ts_capture[TS_CH_QUEUED]=0;
        rant_topic_send(rant_node_topic(A, TS_CH_QUEUED),
                        rant_bytes(payload, sizeof payload),
                        &(RantSendOpts){ .capture_us = want });
        for (t=0;t<800 && ts_msgs[TS_CH_QUEUED]==0;t++){
            rant_node_poll(A,0);
            (void)rant_queue_dispatch(ts_q, 0, 20);
        }
        CHECK(ts_msgs[TS_CH_QUEUED]==1 && ts_capture[TS_CH_QUEUED]==want,
                 "capture: a dispatched message carries it (n=%lu cap=%llu)",
                 ts_msgs[TS_CH_QUEUED], (unsigned long long)ts_capture[TS_CH_QUEUED]);
    }
    rant_node_close(B,1); rant_node_close(A,1);

    /* (g) the patterns layer: a request's stamp at the handler, a remote write's stamp at
       the variable owner */
    {   RantAllocator pa = rant_allocator_heap(0);
        RantAllocator qa = rant_allocator_heap(0);
        RantNodeOpts po = ao, co; RantNode *P, *C;
        RantFunction *fd, *fr; RantVariable *vd, *vr;
        uint8_t b[4];
        po.domain = (uint16_t)(TEST_DOMAIN+51); co = po;
        P = rant_node_open(&pa, "ts-def", NULL, NULL, &po);
        C = rant_node_open(&qa, "ts-rem", NULL, NULL, &co);
        CHECK(P && C, "writets: pattern nodes open");
        if (P && C){
            i_rant_le_w32(b, 1);
            fd = rant_node_create_function_definition(P, "ts/fn", NULL, NULL, ts_on_request, NULL, NULL);
            fr = rant_node_create_remote_function(C, "ts/fn", NULL, NULL, NULL);
            vd = rant_node_create_variable_definition(P, "ts/var", NULL,
                     &(RantVariableOpts){ .initial=rant_bytes(b,4) });
            vr = rant_node_create_remote_variable(C, "ts/var", NULL, NULL);
            CHECK(fd && fr && vd && vr, "writets: pattern entities created");
            rant_variable_on_write(vd, ts_on_var_write, NULL);
            PUMP_UNTIL(P, C, 8000, rant_function_match_count(fr)!=0);
            ts_req_done = 0; ts_req_sent = 0;
            rant_function_call_async(fr, rant_bytes(b,4), NULL, NULL, NULL);
            PUMP_UNTIL(P, C, 3200, ts_req_done);
            CHECK(ts_req_done && ts_req_sent!=0,
                     "writets: RantRequest.written_us stamped (done=%d ts=%llu)",
                     ts_req_done, (unsigned long long)ts_req_sent);
            ts_var_sent = 0;
            i_rant_le_w32(b, 42);
            PUMP_UNTIL(P, C, 8000, rant_variable_match_count(vr)!=0);
            rant_variable_set(vr, rant_bytes(b,4));
            PUMP_UNTIL(P, C, 3200, ts_var_sent!=0);
            CHECK(ts_var_sent!=0, "writets: RantVariableUpdate.written_us stamped (%llu)",
                     (unsigned long long)ts_var_sent);
            rant_variable_on_write(vd, NULL, NULL);
        }
        if (C) rant_node_close(C,1);
        if (P) rant_node_close(P,1);
        rant_allocator_reset(&pa); rant_allocator_reset(&qa);
    }
}

static const TestCase cases[] = {
    { "timestamps", ts_checks },
};

TEST_MAIN(cases)
