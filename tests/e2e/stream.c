/* One writer and one reader on loopback: join, gap, backpressure, the sweep ack, dynamic
 * interest, flap and resume, then forty topics, name identity and a forced hash collision.
 * spec/testing.md says what each case pins. */
#include "support/test.h"

/* the stream case's topics, as test_node_open indices */
#define ST_CH_GAP    0   /* reliable, depth 4, no backpressure */
#define ST_CH_BLOCK  1   /* reliable, depth 4, a 100 ms backpressure wait */
#define ST_CH_DYN    2   /* reliable, depth 4, the reader starts inactive */
#define ST_CH_BLOCK2 3   /* like BLOCK with a repair delay, so the reader's ack is timer armed */
#define ST_DEPTH     4
#define ST_BLOCK_US  100000u
#define ST_NACK_US   5000u
#define ST_NCH       40

static uint8_t payload[32];

/* the domain base and the loopback interface every case uses */
static RantNodeOpts st_opts(uint16_t offset){
    RantNodeOpts o;
    memset(&o, 0, sizeof o);
    o.domain = (uint16_t)(TEST_DOMAIN + offset);
    o.net.multicast_interface = "127.0.0.1";
    o.discovery.max_peers = 4;
    return o;
}

/* The writer's peer id at r, the reader's at w */
static uint32_t st_peer_id(RantNode *n){
    uint32_t id = 0; uint16_t k;
    for (k = 0; k < i_rant_node_core_max_peers(n->core); k++)
        if (i_rant_node_core_peer_at(n->core, k, &id, NULL, NULL, NULL)) break;
    return id;
}

static void stream_checks(void){
    i_RantTopicDef ch[4], chr[4];
    RantNodeOpts o;
    RantNode *w, *r;
    unsigned i;
    memset(payload, 0x5A, sizeof payload);
    memset(ch, 0, sizeof ch);
    ch[0].name = "st/gap";
    ch[0].qos.reliability = RANT_RELIABLE; ch[0].qos.keep_last = ST_DEPTH;
    ch[0].qos.max_message_bytes = 64; ch[0].qos.heartbeat_us = 50000;
    ch[1] = ch[0]; ch[1].name = "st/block"; ch[1].qos.backpressure_wait_us = ST_BLOCK_US;
    ch[2] = ch[0]; ch[2].name = "st/dyn";
    ch[2].qos.catch_up = ST_DEPTH;   /* dynamic asserts ring replay on join */
    ch[3] = ch[0]; ch[3].name = "st/block2";
    ch[3].qos.backpressure_wait_us = ST_BLOCK_US; ch[3].qos.repair_delay_us = ST_NACK_US;
    memcpy(chr, ch, sizeof ch);
    ch[0].role = ch[1].role = ch[2].role = ch[3].role = RANT_PUB_ONLY;
    chr[0].role = chr[1].role = chr[3].role = RANT_SUB_ONLY;
    chr[2].role = RANT_INACTIVE;

    /* disable_shm: these phases exercise the UDP reliability path. Shared memory has its own
       coverage and its chunk recycle semantics differ */
    o = st_opts(0);
    o.disable_shm = 1;
    w = test_node_open(NULL, NULL, NULL, o, ch, 4);
    r = test_node_open(NULL, test_on_message, test_on_event, o, chr, 4);
    CHECK(w && r, "stream: nodes open");
    if (!w || !r){ if (w) rant_node_close(w, 0); if (r) rant_node_close(r, 0); return; }

    /* JOIN: the writer streams while discovery completes. The reader adopts the head
       silently, no gap for a late joiner */
    {   uint64_t end = i_rant_plat_now_us() + 5000000u;
        while (test_samples[ST_CH_GAP]==0 && i_rant_plat_now_us() < end){
            test_send(w, ST_CH_GAP, payload, sizeof payload);
            test_pump(w, r, 20);
        } }
    CHECK(test_samples[ST_CH_GAP] > 0, "join: reader receives (got %lu)", test_samples[ST_CH_GAP]);
    CHECK(test_gap_calls[ST_CH_GAP] == 0, "join: no on_gap for late join (calls=%lu)", test_gap_calls[ST_CH_GAP]);

    /* GAP: 50 samples with no flush in between. Only the last ST_DEPTH survive, the rest
       arrive as exactly one gap */
    test_pump(w, r, 200);                           /* settle acks */
    {   unsigned long s0 = test_samples[ST_CH_GAP];
        for (i=0;i<50;i++) test_send(w, ST_CH_GAP, payload, sizeof payload);
        PUMP_UNTIL(w, r, 3000, test_samples[ST_CH_GAP]-s0 >= ST_DEPTH && test_gap_calls[ST_CH_GAP] >= 1);
        CHECK(test_gap_calls[ST_CH_GAP] == 1, "gap: one on_gap call (calls=%lu)", test_gap_calls[ST_CH_GAP]);
        CHECK(test_gap_lost[ST_CH_GAP] == 50-ST_DEPTH, "gap: count == evicted span (%lu, want %u)",
              test_gap_lost[ST_CH_GAP], 50-ST_DEPTH);
        CHECK(test_samples[ST_CH_GAP]-s0 == ST_DEPTH, "gap: surviving tail delivered (%lu, want %u)",
              test_samples[ST_CH_GAP]-s0, ST_DEPTH);
    }

    /* BLOCKED: fill ST_CH_BLOCK's ring and send again while the reader never runs. A send
       that evicts unacked history waits about ST_BLOCK_US */
    test_pump(w, r, 200);
    for (i=0;i<ST_DEPTH;i++) test_send(w, ST_CH_BLOCK, payload, sizeof payload);
    test_pump(w, NULL, 50);                         /* flush. The reader is silent, no acks */
    {   uint64_t t0 = i_rant_plat_now_us(), dt;
        test_send(w, ST_CH_BLOCK, payload, sizeof payload);
        dt = i_rant_plat_now_us() - t0;
        CHECK(dt >= ST_BLOCK_US-10000 && dt < 4*ST_BLOCK_US,
              "blocked: send waited ~backpressure_wait_us (%.1f ms)", dt/1000.0);
    }

    /* RELEASED: once the reader caught up and acked, sends are instant */
    PUMP_UNTIL(w, r, 3000, test_samples[ST_CH_BLOCK] == ST_DEPTH+1);
    test_pump(w, r, 50);                            /* the acks travel back */
    {   uint64_t t0 = i_rant_plat_now_us(), dt;
        test_send(w, ST_CH_BLOCK, payload, sizeof payload);
        dt = i_rant_plat_now_us() - t0;
        CHECK(dt < 20000, "released: acked ring sends instantly (%.1f ms)", dt/1000.0);
        PUMP_UNTIL(w, r, 3000, test_samples[ST_CH_BLOCK] == ST_DEPTH+2);
        /* the blocked send evicted only history already in flight to the reader's socket,
           so every send arrives and no gap is reported */
        CHECK(test_gap_calls[ST_CH_BLOCK] == 0 && test_samples[ST_CH_BLOCK] == ST_DEPTH+2,
              "blocked: in-flight eviction loses nothing (gaps=%lu, samples=%lu/%u)",
              test_gap_calls[ST_CH_BLOCK], test_samples[ST_CH_BLOCK], ST_DEPTH+2);
    }

    /* SWEEP-ACK: ST_CH_BLOCK2 has a repair delay, so the reader's ACKNACK is timer armed and
       only the sweep flushes it once the writer goes quiet */
    test_pump(w, r, 200);
    {   unsigned long s0 = test_samples[ST_CH_BLOCK2];
        for (i=0;i<ST_DEPTH;i++) test_send(w, ST_CH_BLOCK2, payload, sizeof payload);
        PUMP_UNTIL(w, r, 3000, test_samples[ST_CH_BLOCK2]-s0 == ST_DEPTH);
        test_pump(w, r, 100);                       /* a few 25 ms sweeps, which must ack */
        CHECK(test_samples[ST_CH_BLOCK2]-s0 == ST_DEPTH, "sweep-ack: ring delivered (%lu, want %u)",
              test_samples[ST_CH_BLOCK2]-s0, ST_DEPTH);
        {   uint64_t t0 = i_rant_plat_now_us(), dt;
            test_send(w, ST_CH_BLOCK2, payload, sizeof payload);     /* would evict slot 0 */
            dt = i_rant_plat_now_us() - t0;
            CHECK(dt < 20000, "sweep-ack: sub-only reader's timer ack releases backpressure (%.1f ms)",
                  dt/1000.0);
        }
    }

    /* DYNAMIC: ST_CH_DYN is inactive on the reader. Subscribe replays the cached ring,
       unsubscribe goes silent at the writer, resubscribe replays again, all gap free */
    test_pump(w, r, 200);
    for (i=0;i<3;i++) test_send(w, ST_CH_DYN, payload, sizeof payload);
    test_pump(w, r, 300);                           /* nothing may arrive in this window */
    CHECK(test_samples[ST_CH_DYN] == 0, "dynamic: inactive receives nothing (%lu)", test_samples[ST_CH_DYN]);
    test_set_role(r, ST_CH_DYN, RANT_SUB_ONLY);
    PUMP_UNTIL(w, r, 3000, test_samples[ST_CH_DYN] >= 3);
    CHECK(test_samples[ST_CH_DYN] == 3, "dynamic: subscribe replays cached history (%lu, want 3)",
          test_samples[ST_CH_DYN]);
    test_send(w, ST_CH_DYN, payload, sizeof payload);
    PUMP_UNTIL(w, r, 3000, test_samples[ST_CH_DYN] >= 4);
    CHECK(test_samples[ST_CH_DYN] == 4, "dynamic: live sample delivered (%lu, want 4)", test_samples[ST_CH_DYN]);
    test_set_role(r, ST_CH_DYN, RANT_INACTIVE);
    test_pump(w, r, 300);                           /* the new interest list reaches the writer */
    for (i=0;i<5;i++) test_send(w, ST_CH_DYN, payload, sizeof payload);
    test_pump(w, r, 300);                           /* nothing may arrive in this window */
    CHECK(test_samples[ST_CH_DYN] == 4, "dynamic: unsubscribed receives nothing (%lu)", test_samples[ST_CH_DYN]);
    test_set_role(r, ST_CH_DYN, RANT_SUB_ONLY);
    PUMP_UNTIL(w, r, 3000, test_samples[ST_CH_DYN] >= 4+ST_DEPTH);
    CHECK(test_samples[ST_CH_DYN] == 4+ST_DEPTH, "dynamic: resubscribe replays ring (%lu, want %u)",
          test_samples[ST_CH_DYN], 4+ST_DEPTH);
    CHECK(test_gap_calls[ST_CH_DYN] == 0, "dynamic: joins are silent (gaps=%lu)", test_gap_calls[ST_CH_DYN]);

    /* FLAP: a one sided flap where the reader rebuilt its state. The changed reader epoch in
       the first ACKNACK makes every writer lane join again */
    test_pump(w, r, 200);
    {   unsigned long s0 = test_samples[ST_CH_DYN];
        uint32_t wid = st_peer_id(r); uint8_t ib[256]; size_t il;
        i_rant_transport_peer_remove(r->transport, wid);
        i_rant_transport_peer_add(r->transport, wid, RANT_FRAG_SIZE);
        il = i_rant_transport_build_interest(w->transport, ib, sizeof ib);
        i_rant_transport_apply_peer_interest(r->transport, wid, rant_bytes(ib, il));
        /* the apply only nominates, since peer_remove dropped the verdicts: run the detail
           exchange by hand so the flapped reader verifies again and rematches */
        {   i_RantDetailWant wl[8]; uint8_t rq[256], rp[1024]; uint16_t nw2, verified; size_t rl2, pl2;
            nw2 = i_rant_transport_detail_wants(r->transport, NULL, wid, rant_bytes(ib, il), wl, 8);
            CHECK(nw2 > 0, "flap: re-added peer nominates pending candidates (%u)", nw2);
            rl2 = i_rant_detail_req_build(TEST_DOMAIN, 0, wl, nw2, rq, sizeof rq);
            pl2 = i_rant_transport_detail_respond(w->transport, NULL, 0, rant_bytes(rq, rl2), rp, sizeof rp);
            verified = i_rant_transport_apply_peer_details(r->transport, wid, rant_bytes(rp, pl2));
            CHECK(verified == nw2, "flap: details verify every candidate (%u/%u)", verified, nw2);
            i_rant_transport_apply_peer_interest(r->transport, wid, rant_bytes(ib, il));
        }
        PUMP_UNTIL(w, r, 3000, test_samples[ST_CH_DYN] >= s0+ST_DEPTH);
        CHECK(test_samples[ST_CH_DYN] == s0+ST_DEPTH,
              "flap: writer re-joins new reader incarnation, replays ring (%lu, want %lu)",
              test_samples[ST_CH_DYN], s0+ST_DEPTH);
        CHECK(test_gap_calls[ST_CH_DYN] == 0, "flap: recovery is silent (gaps=%lu)", test_gap_calls[ST_CH_DYN]);
    }

    /* RESUME: a discovery blip drops the peer on both sides without tearing down transport
       state. A same incarnation resume replays the withheld backlog with no gap */
    test_pump(w, r, 200);
    {   unsigned long s0 = test_samples[ST_CH_DYN], g0 = test_gap_calls[ST_CH_DYN];
        uint32_t wid = st_peer_id(r), rid = st_peer_id(w);
        i_rant_transport_peer_dormant(w->transport, rid);     /* drops the reader from flow control */
        i_rant_transport_peer_dormant(r->transport, wid);     /* the reader stops acking */
        for (i=0;i<3;i++) test_send(w, ST_CH_DYN, payload, sizeof payload);
        test_pump(w, r, 300);                       /* nothing may arrive in this window */
        CHECK(test_samples[ST_CH_DYN] == s0, "resume: dormant peer withholds sends (%lu, want %lu)",
              test_samples[ST_CH_DYN], s0);
        i_rant_transport_peer_resume(w->transport, rid);
        i_rant_transport_peer_resume(r->transport, wid);
        PUMP_UNTIL(w, r, 3000, test_samples[ST_CH_DYN] >= s0+3);
        CHECK(test_samples[ST_CH_DYN] == s0+3, "resume: backlog replays from preserved position (%lu, want %lu)",
              test_samples[ST_CH_DYN], s0+3);
        CHECK(test_gap_calls[ST_CH_DYN] == g0, "resume: lossless, no gap (gaps=%lu, want %lu)",
              test_gap_calls[ST_CH_DYN], g0);
    }

    rant_node_close(r, 1);
    rant_node_close(w, 1);
}

/* SCALE: 40 topics. The whole interest list rides one announce, matched at peer up */
static void scale_checks(void){
    static i_RantTopicDef cha[ST_NCH], chb[ST_NCH];
    static char snames[ST_NCH][12];     /* "scale/0" to "scale/39" */
    RantNode *a, *b; uint16_t k;
    memset(payload, 0x5A, sizeof payload);
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
    a = test_node_open(NULL, NULL, NULL, st_opts(1), cha, ST_NCH);
    b = test_node_open(NULL, test_on_message, test_on_event, st_opts(1), chb, ST_NCH);
    CHECK(a && b, "scale: %u-topic nodes open", ST_NCH);
    if (a && b){
        test_messages = 0;
        for (k=0;k<ST_NCH;k++) test_send(a, k, payload, 16);
        PUMP_UNTIL(a, b, 5000, test_messages >= ST_NCH);
        CHECK(test_messages == ST_NCH, "scale: all topics delivered (%lu/%u)", test_messages, ST_NCH);
    }
    if (b) rant_node_close(b, 1);
    if (a) rant_node_close(a, 1);
}

/* NAMED: the cross peer identity is the topic name hash, independent of each node's local
   handle. A distinct name never cross wires and clean names raise no collision */
static void named_checks(void){
    i_RantTopicDef nw[1], nr[2];
    RantNode *w2, *r2;
    memset(payload, 0x5A, sizeof payload);
    memset(nw,0,sizeof nw); memset(nr,0,sizeof nr);
    nw[0].name="robot/lidar"; nw[0].role=RANT_PUB_ONLY;
    nw[0].qos.reliability=RANT_RELIABLE; nw[0].qos.keep_last=1;
    nw[0].qos.catch_up=1; nw[0].qos.max_message_bytes=32; nw[0].qos.heartbeat_us=50000;
    nr[0]=nw[0]; nr[0].role=RANT_SUB_ONLY;                                /* same name, index 0 */
    nr[1]=nw[0]; nr[1].name="sensors/imu"; nr[1].role=RANT_SUB_ONLY;     /* index 1 */
    test_samples[0]=test_samples[1]=0; test_collisions=0; test_last_sender[0]='\0';
    w2=test_node_open("lidar-node",NULL,NULL,st_opts(2),nw,1);
    r2=test_node_open("reader-node",test_on_message,test_on_event,st_opts(2),nr,2);
    CHECK(w2 && r2, "named: nodes open");
    if (w2 && r2){
        uint64_t end = i_rant_plat_now_us() + 5000000u;
        while (test_samples[0]==0 && i_rant_plat_now_us()<end){
            test_send(w2, 0, payload, 16); test_pump(w2,r2,20);
        }
        CHECK(test_samples[0] > 0, "named: same name matches across nodes (%lu)", test_samples[0]);
        /* the node name travels with discovery and surfaces as RantMsg.publisher_name */
        CHECK(strcmp(test_last_sender, "lidar-node")==0,
              "named: publisher_name carries the publisher's node name (%s)", test_last_sender);
        test_pump(w2,r2,200);                       /* nothing may cross in this window */
        CHECK(test_samples[1] == 0, "named: distinct name never cross-wires (%lu)", test_samples[1]);
        CHECK(test_collisions == 0, "named: clean names raise no collision (%lu)", test_collisions);
    }
    if (r2) rant_node_close(r2,1);
    if (w2) rant_node_close(w2,1);
}

/* COLLISION: two names with the same 64 bit identity, found by Pollard's rho against
   FNV-1a. The collision event fires and the match is refused, never cross wired */
static void collision_checks(void){
    const char *A="iuZA9tcJzAG", *B="5wVGxhTCmOC";   /* both hash to 23f58aa8628b1cce */
    i_RantTopicDef cw, cr; RantNode *w3, *r3;
    uint64_t end;
    memset(payload, 0x5A, sizeof payload);
    CHECK(i_rant_topic_id(A)==i_rant_topic_id(B) && strcmp(A,B)!=0,
          "collision: test pair still shares one identity (else regen via collide)");
    memset(&cw,0,sizeof cw);
    cw.name=A; cw.role=RANT_PUB_ONLY;
    cw.qos.reliability=RANT_RELIABLE; cw.qos.keep_last=1; cw.qos.catch_up=1;
    cw.qos.max_message_bytes=32; cw.qos.heartbeat_us=50000;
    cr=cw; cr.name=B; cr.role=RANT_SUB_ONLY;
    test_samples[0]=0; test_collisions=0;
    w3=test_node_open(NULL,NULL,NULL,st_opts(3),&cw,1);
    r3=test_node_open(NULL,test_on_message,test_on_event,st_opts(3),&cr,1);
    CHECK(w3 && r3, "collision: nodes open");
    if (w3 && r3){
        end = i_rant_plat_now_us() + 3000000u;
        while (test_collisions == 0 && i_rant_plat_now_us() < end){
            test_send(w3,0,payload,16); test_pump(w3, r3, 20);
        }
        test_send(w3,0,payload,16); test_pump(w3, r3, 100);   /* a refused match stays refused */
        CHECK(test_collisions >= 1, "collision: detected (RANT_E_NAME_COLLISION fired %lu)", test_collisions);
        CHECK(test_samples[0] == 0, "collision: match refused, no cross-wire (%lu)", test_samples[0]);
    }
    if (r3) rant_node_close(r3,1);
    if (w3) rant_node_close(w3,1);
}

static const TestCase cases[] = {
    { "stream", stream_checks },
    { "scale", scale_checks },
    { "named", named_checks },
    { "collision", collision_checks },
};

TEST_MAIN(cases)
