/* The transport core, sans IO with a controlled clock: send results, rate throttle, NACK
 * merge and lapped readers, per peer RTT, the held sample, QoS matching and best effort flow
 * control. spec/testing.md says what each case pins. */
#include "support/test.h"

/* Best effort rate throttle on the transport core with a controlled clock. The writer
   paces its lane to the subscriber's max_rate_hz (spec/testing.md). The pump can drop. */
static int rate_recv, rate_lost, rate_drop;
static uint64_t rate_clk;
static i_RantTransportState *rate_W, *rate_R;
static int rate_on_msg(void *u, uint16_t ch, uint32_t from, RantBytes d){
    (void)u;(void)ch;(void)from;(void)d; rate_recv++; return 0;
}
static void rate_on_event(const RantEvent *ev){
    if (ev->kind==RANT_MSG_LOST) rate_lost += (int)ev->lost_count;
}
static void rate_send(void){
    static unsigned char p[16];
    i_rant_transport_send(rate_W, 0, rant_bytes(p, sizeof p), rate_clk);
}
static void rate_pump(uint64_t dt){   /* flush W to R, then advance the clock */
    uint8_t buf[RANT_DGRAM_MAX]; uint32_t to; size_t ol;
    while (i_rant_transport_poll_send(rate_W,&to,buf,sizeof buf,&ol,rate_clk)){
        if (rate_drop>0 && (buf[0]&0x07u)==1u){ rate_drop--; continue; }   /* drop this DATA */
        i_rant_transport_on_datagram(rate_R, 1u, rant_bytes(buf, ol), rate_clk);
    }
    rate_clk += dt;
}
static void rate_checks(void){
    TestPair pair; i_RantTransportConfig cb; int i;
    RantQos q, rq; memset(&q,0,sizeof q); q.reliability=RANT_BEST_EFFORT; q.keep_last=4;
    rq = q; rq.max_rate_hz = 100;
    memset(&cb,0,sizeof cb); cb.on_message=rate_on_msg; cb.on_event=rate_on_event;
    if (!test_pair_open(&pair, "ratech", q, rq, &cb)){ CHECK(0, "rate: transport pair opens"); return; }
    rate_W = pair.w; rate_R = pair.r;
    rate_clk=1000000;
    test_apply_verified(rate_R, 1u, rate_W);   /* reader learns the writer */
    test_apply_verified(rate_W, 2u, rate_R);   /* writer learns the reader + its advertised rate */
    CHECK(i_rant_transport_publisher_match_count(rate_W,0)>0, "rate: writer matched the throttled reader");

    /* decimation: publish 200 samples across ~100 ms of virtual time. At 100 Hz the reader
       gets ~10-12, not 200, and NO false loss (the paced skips are absorbed by wire_skip). */
    rate_recv=0; rate_lost=0; rate_drop=0;
    for (i=0;i<200;i++){ rate_send(); rate_pump(500u); }
    for (i=0;i<3;i++) rate_pump(20000u);        /* drain the final held tick via the sweep */
    CHECK(rate_recv>=5 && rate_recv<=20, "rate: decimated to ~rate*time (%d of 200 delivered)", rate_recv);
    CHECK(rate_lost==0, "rate: paced skips are not loss (lost=%d)", rate_lost);

    /* a dropped sent sample must still be reported: send and tick, drop that DATA, then
       let later ticks deliver, so the reader's per peer seqno gap surfaces as one MSG_LOST */
    rate_recv=0; rate_lost=0;
    rate_send(); rate_clk += 20000u; rate_drop=1;   /* the next tick's DATA is dropped */
    rate_pump(20000u);
    CHECK(rate_recv==0, "rate: the tick's only sample was dropped on the wire (recv=%d)", rate_recv);
    for (i=0;i<5;i++){ rate_send(); rate_pump(20000u); }
    CHECK(rate_recv>=1 && rate_lost>=1,
             "rate: a dropped SENT sample IS reported as loss (recv=%d lost=%d)", rate_recv, rate_lost);
    test_pair_close(&pair);
}

/* Lapped reader and NACK merge on the transport core with a controlled clock: a six
   fragment reliable stream through a pump that can drop DATA (spec/testing.md). */
static int lap_recv, lap_lost, lap_drop_all; static unsigned lap_drop_mask;
static uint64_t lap_clk;
static i_RantTransportState *lap_W, *lap_R;
static uint8_t lap_held[8][RANT_DGRAM_MAX]; static size_t lap_hl[8]; static int lap_nh;
static int lap_on_msg(void *u, uint16_t ch, uint32_t from, RantBytes d){
    (void)u;(void)ch;(void)from;(void)d; lap_recv++; return 0;
}
static void lap_on_event(const RantEvent *ev){
    if (ev->kind==RANT_MSG_LOST) lap_lost += (int)ev->lost_count;
}
static void lap_send(void){                       /* one 6-fragment message */
    static unsigned char p[6*RANT_FRAG_SIZE - 100];
    i_rant_transport_send(lap_W, 0, rant_bytes(p, sizeof p), lap_clk);
}
/* R to W: collect now, feed later, so ACKNACKs armed at different points land in one pass */
static void lap_collect_r(void){
    uint8_t buf[RANT_DGRAM_MAX]; uint32_t to; size_t ol;
    while (lap_nh<8 && i_rant_transport_poll_send(lap_R,&to,buf,sizeof buf,&ol,lap_clk)){
        memcpy(lap_held[lap_nh], buf, ol); lap_hl[lap_nh++] = ol; }
}
static void lap_feed_w(void){
    int i;
    for (i=0;i<lap_nh;i++) i_rant_transport_on_datagram(lap_W, 2u, rant_bytes(lap_held[i], lap_hl[i]), lap_clk);
    lap_nh = 0;
}
/* W to R, dropping DATA per the lap_drop switches, one message. After split_after DATA
   fragments R's pending ACKNACK is collected mid stream. A batched HB still passes. */
static void lap_flush_w(int split_after){
    uint8_t buf[RANT_DGRAM_MAX]; uint32_t to; size_t ol; int fed=0;
    while (i_rant_transport_poll_send(lap_W,&to,buf,sizeof buf,&ol,lap_clk)){
        size_t off=0;
        if ((buf[0]&0x07u)==1u && !(buf[0]&RANT_F_SINGLE)){
            unsigned frag = (unsigned)buf[RANT_OFFSET_FRAG] | ((unsigned)buf[RANT_OFFSET_FRAG+1]<<8);
            size_t sub = RANT_HEADER_DATA_MULTI + ((size_t)buf[RANT_OFFSET_PAYLOAD_LEN] | ((size_t)buf[RANT_OFFSET_PAYLOAD_LEN+1]<<8));
            int drop = lap_drop_all || (lap_drop_mask & (1u<<frag));
            if (frag==5) lap_drop_mask = 0;
            if (drop) off = sub; else fed++;
        }
        if (off < ol) i_rant_transport_on_datagram(lap_R, 1u, rant_bytes(buf+off, ol-off), lap_clk);
        if (split_after && fed==split_after){ lap_collect_r(); split_after=0; }
    }
}
static void lap_pump(uint64_t dt){ lap_flush_w(0); lap_collect_r(); lap_feed_w(); lap_clk += dt; }
static void lap_run(int ms){ while (ms-- > 0) lap_pump(1000); }   /* 1 ms steps */
static void lapped_checks(void){
    TestPair pair; i_RantTransportConfig cb; int i;
    RantRepairStats rs;
    RantQos q; memset(&q,0,sizeof q); q.reliability=RANT_RELIABLE; q.keep_last=4;
    q.heartbeat_us=30000; q.repair_delay_us=50000;
    memset(&cb,0,sizeof cb); cb.on_message=lap_on_msg; cb.on_event=lap_on_event;
    if (!test_pair_open(&pair, "lapped", q, q, &cb)){ CHECK(0, "lapped: transport pair opens"); return; }
    lap_W = pair.w; lap_R = pair.r;
    lap_clk=1000000; lap_nh=0; lap_drop_all=0; lap_drop_mask=0;
    test_apply_verified(lap_R, 1u, lap_W);
    test_apply_verified(lap_W, 2u, lap_R);
    CHECK(i_rant_transport_publisher_match_count(lap_W,0)>0, "lapped: writer matched reader");

    /* [a] baseline */
    lap_recv=0; lap_lost=0; lap_send(); lap_pump(1000);
    CHECK(lap_recv==1 && lap_lost==0, "lapped: [a] a 6-fragment message delivers (recv=%d)", lap_recv);

    /* [b] NACK merge: frags 1 and 4 of one message are lost, so two requests reach the
       writer in one pass. Both holes must be resent at once, an overwrite would resend only 4. */
    lap_recv=0; lap_lost=0; lap_drop_mask=(1u<<1)|(1u<<4);
    lap_send(); lap_flush_w(2); lap_collect_r();
    CHECK(lap_nh==2, "lapped: [b] two ACKNACKs collected in one pass (%d)", lap_nh);
    lap_feed_w(); lap_pump(1000);
    i_rant_transport_repair_stats(lap_W, 0, &rs);
    CHECK(lap_recv==1 && rs.frags_resent==2,
             "lapped: [b] merged request: both holes resent at once, message complete (recv=%d resent=%llu)",
             lap_recv, (unsigned long long)rs.frags_resent);

    /* [c] the writer's floor passes a reader that heard nothing: it restarts at the oldest
       cached sample and repairs the cached window (6 sent, keep_last 4: 2 lost, 4 recovered). */
    lap_recv=0; lap_lost=0; lap_drop_all=1;
    for (i=0;i<6;i++){ lap_send(); lap_pump(1000); }
    lap_drop_all=0;
    lap_run(120);     /* the tail HB skips, the backstop NACK resends */
    CHECK(lap_recv==4 && lap_lost==12,
             "lapped: [c] one floor skip restarts at the oldest cached sample (recv=%d lost=%d)", lap_recv, lap_lost);

    /* [d] lapped: a second floor skip with nothing delivered in between. The reader rejoins
       at the writer's head, the cached window is reported lost, the next message is in order. */
    lap_recv=0; lap_lost=0; lap_drop_all=1;
    for (i=0;i<6;i++){ lap_send(); lap_pump(1000); }
    lap_run(30);                                       /* skip #1 (its resends stay dropped) */
    CHECK(lap_recv==0 && lap_lost==12, "lapped: [d] first skip (recv=%d lost=%d)", lap_recv, lap_lost);
    lap_send(); lap_pump(1000); lap_send(); lap_pump(1000);
    lap_run(30);                                       /* skip #2 while still fetching: lapped */
    CHECK(lap_recv==0 && lap_lost==48, "lapped: [d] second skip gives up the cached window (lost=%d)", lap_lost);
    lap_drop_all=0;
    i_rant_transport_repair_stats(lap_W, 0, &rs);
    { unsigned long long resent0 = rs.frags_resent;
      lap_send(); lap_run(5);
      i_rant_transport_repair_stats(lap_W, 0, &rs);
      CHECK(lap_recv==1 && lap_lost==48 && rs.frags_resent==resent0,
               "lapped: [d] rejoined at the head: next message in order, no repair (recv=%d lost=%d resent=%llu)",
               lap_recv, lap_lost, (unsigned long long)(rs.frags_resent-resent0)); }
    /* [e] a delivery resets the streak: the stream runs on, and one later skip restarts at
       the floor again rather than the head */
    for (i=0;i<3;i++){ lap_send(); lap_pump(1000); }
    CHECK(lap_recv==4, "lapped: [e] stream continues after the rejoin (recv=%d)", lap_recv);
    lap_recv=0; lap_lost=0; lap_drop_all=1;
    for (i=0;i<6;i++){ lap_send(); lap_pump(1000); }
    lap_drop_all=0;
    lap_run(120);
    CHECK(lap_recv==4 && lap_lost==12, "lapped: [e] streak reset: a later skip repairs the window again (recv=%d lost=%d)", lap_recv, lap_lost);
    test_pair_close(&pair);
}

/* Per peer RTT estimation on the transport core: W to R through a pump with a symmetric
   one way delay L, so every round trip is exactly 2L (spec/testing.md). */
static int rt_recv, rt_drop_all, rt_drop_ack; static unsigned rt_drop_mask, rt_drop_resend_of;
static uint64_t rt_clk, rt_lat;
static i_RantTransportState *rt_W, *rt_R;
static int rt_hb_seen;
static int rt_on_msg(void *u, uint16_t ch, uint32_t from, RantBytes d){
    (void)u;(void)ch;(void)from;(void)d; rt_recv++; return 0;
}
static void rt_send(void){                       /* one 6-fragment message */
    static unsigned char p[6*RANT_FRAG_SIZE - 100];
    i_rant_transport_send(rt_W, 0, rant_bytes(p, sizeof p), rt_clk);
}
/* one tick: W builds at clk, R hears it at clk plus L and answers, W hears that at clk
   plus 2L. DATA fragments drop per the rt_drop switches, rt_drop_ack drops R's ACKNACKs. */
static void rt_pump(void){
    uint8_t buf[RANT_DGRAM_MAX]; uint32_t to; size_t ol;
    while (i_rant_transport_poll_send(rt_W,&to,buf,sizeof buf,&ol,rt_clk)){
        size_t off=0;
        if ((buf[0]&0x07u)==1u && !(buf[0]&RANT_F_SINGLE)){
            unsigned frag = (unsigned)buf[RANT_OFFSET_FRAG] | ((unsigned)buf[RANT_OFFSET_FRAG+1]<<8);
            size_t sub = RANT_HEADER_DATA_MULTI + ((size_t)buf[RANT_OFFSET_PAYLOAD_LEN] | ((size_t)buf[RANT_OFFSET_PAYLOAD_LEN+1]<<8));
            int drop = rt_drop_all || (rt_drop_mask & (1u<<frag));
            if (!drop && rt_drop_resend_of == frag + 1u){ drop = 1; rt_drop_resend_of = 0; }
            if (frag==5) rt_drop_mask = 0;
            if (drop) off = sub;
        } else if ((buf[0]&0x07u)==2u) rt_hb_seen++;
        if (off < ol) i_rant_transport_on_datagram(rt_R, 1u, rant_bytes(buf+off, ol-off), rt_clk + rt_lat);
    }
    while (i_rant_transport_poll_send(rt_R,&to,buf,sizeof buf,&ol,rt_clk + rt_lat)){
        if (rt_drop_ack && (buf[0]&0x07u)==3u) continue;
        i_rant_transport_on_datagram(rt_W, 2u, rant_bytes(buf, ol), rt_clk + 2u*rt_lat);
    }
    rt_clk += 2u*rt_lat;
}
static void rtt_checks(void){
    TestPair pair; i_RantTransportConfig cb; int i, n;
    i_RantPeerRtt e;
    RantQos q; memset(&q,0,sizeof q); q.reliability=RANT_RELIABLE; q.keep_last=8;
    q.heartbeat_us=200000;   /* idle HB far off: only the tail HB matters here */
    memset(&cb,0,sizeof cb); cb.on_message=rt_on_msg;
    if (!test_pair_open(&pair, "rtt", q, q, &cb)){ CHECK(0, "rtt: transport pair opens"); return; }
    rt_W = pair.w; rt_R = pair.r;
    rt_clk=1000000; rt_lat=2500; rt_drop_all=0; rt_drop_mask=0; rt_drop_resend_of=0; rt_drop_ack=0;
    test_apply_verified(rt_R, 1u, rt_W);
    test_apply_verified(rt_W, 2u, rt_R);
    CHECK(i_rant_transport_publisher_match_count(rt_W,0)>0, "rtt: writer matched reader");
    CHECK(i_rant_transport_peer_rtt(rt_W, 2u, &e) && e.samples==0, "rtt: a fresh peer has no estimate");
    CHECK(!i_rant_transport_peer_rtt(rt_W, 9u, &e), "rtt: an unknown peer answers 0");

    /* [a] writer side: every clean sample is push-to-ack = one round trip = 2L */
    rt_recv=0;
    for (i=0;i<10;i++){ rt_send(); rt_pump(); rt_pump(); }
    i_rant_transport_peer_rtt(rt_W, 2u, &e);
    CHECK(rt_recv==10 && e.samples==10 && e.rtt_us==5000 && e.rtt_min_us==5000 && e.rtt_last_us==5000,
             "rtt: [a] writer samples push-to-ack (samples=%u rtt=%u min=%u last=%u)",
             e.samples, e.rtt_us, e.rtt_min_us, e.rtt_last_us);
    CHECK(e.rtt_jitter_us < 500, "rtt: [a] jitter decays on a steady path (%u)", e.rtt_jitter_us);
    i_rant_transport_peer_rtt(rt_R, 1u, &e);
    CHECK(e.samples==0, "rtt: [a] a reader that never repaired has no estimate (%u)", e.samples);

    /* [b] reader side, first loss: frag 2 and its resend are lost. No estimate yet, so the
       re ask waits the 50 ms default. The re asked seqno is ambiguous, so it is no sample. */
    rt_recv=0; rt_drop_mask=(1u<<2); rt_drop_resend_of=2u+1u;
    rt_send();
    for (n=0; n<40 && rt_recv==0; n++) rt_pump();
    CHECK(rt_recv==1 && n>=10 && n<=13, "rtt: [b] unmeasured reader re-asks at the 50 ms default (%d ticks)", n);
    i_rant_transport_peer_rtt(rt_R, 1u, &e);
    CHECK(e.samples==0, "rtt: [b] a seqno asked twice yields no sample, Karn (samples=%u)", e.samples);

    /* [c] a few clean repairs (one lost frag each) build the reader's estimate ... */
    for (i=0;i<6;i++){ rt_recv=0; rt_drop_mask=(1u<<3); rt_send(); for (n=0;n<8 && rt_recv==0;n++) rt_pump(); }
    i_rant_transport_peer_rtt(rt_R, 1u, &e);
    CHECK(e.samples==6 && e.rtt_us==5000 && e.rtt_jitter_us < 1000,
             "rtt: [c] reader samples request-to-resend (samples=%u rtt=%u jitter=%u)", e.samples, e.rtt_us, e.rtt_jitter_us);
    /* ... and the same double loss now re-asks at the RTT bound (5 ms + max(1 ms, 4 x jitter),
       about 7 ms): delivered in 3 to 5 ticks of 5 ms instead of 12 */
    rt_recv=0; rt_drop_mask=(1u<<2); rt_drop_resend_of=2u+1u;
    rt_send();
    for (n=0; n<40 && rt_recv==0; n++) rt_pump();
    CHECK(rt_recv==1 && n>=3 && n<=5, "rtt: [c] measured reader re-asks at the RTT bound (%d ticks)", n);

    /* [d] the writer's tail heartbeat follows the RTT bound: drop the final ack, the HB that
       chases it comes at ~6 ms (2 ticks), not the 20 ms RANT_HB_TAIL_US (4 ticks) */
    rt_recv=0; rt_drop_ack=1; rt_hb_seen=0;
    rt_send(); rt_pump();
    CHECK(rt_recv==1 && rt_hb_seen==0, "rtt: [d] delivered, ack dropped, no HB yet (recv=%d hb=%d)", rt_recv, rt_hb_seen);
    for (n=0; n<10 && rt_hb_seen==0; n++) rt_pump();
    CHECK(rt_hb_seen>=1 && n<=2, "rtt: [d] tail HB at the RTT bound, not the 20 ms default (%d ticks)", n);
    rt_drop_ack=0; rt_pump(); rt_pump();
    test_pair_close(&pair);
}

/* One sample held ahead of the head on the transport core with a controlled clock
   (spec/testing.md). Messages are 6 fragments, tagged with their number in byte 0. */
static int ah_recv, ah_lost, ah_refuse, ah_order_bad; static unsigned ah_drop_mask, ah_tag, ah_last;
static uint64_t ah_drop_lo, ah_drop_hi;   /* drop DATA seqnos in [lo, hi), a message plus resends */
static uint64_t ah_clk, ah_first_base; static int ah_have_first;
static i_RantTransportState *ah_W, *ah_R;
static uint8_t ah_held[8][RANT_DGRAM_MAX]; static size_t ah_hl[8]; static int ah_nh;
static int ah_on_msg(void *u, uint16_t ch, uint32_t from, RantBytes d){
    (void)u;(void)ch;(void)from;
    if (ah_refuse) return 1;
    {   unsigned tag = ((const unsigned char*)d.data)[RANT_TIMESTAMP_BYTES];
        if (tag <= ah_last) ah_order_bad++;   /* strictly increasing, no dup or step back */
        ah_last = tag; }
    ah_recv++; return 0;
}
static void ah_on_event(const RantEvent *ev){
    if (ev->kind==RANT_MSG_LOST) ah_lost += (int)ev->lost_count;
}
static void ah_send(void){                       /* one 6-fragment message, tagged */
    static unsigned char p[6*RANT_FRAG_SIZE - 100];
    p[0] = (unsigned char)++ah_tag;
    i_rant_transport_send(ah_W, 0, rant_bytes(p, sizeof p), ah_clk);
}
static uint64_t ah_base_of(unsigned tag){ return ah_first_base + 6u * (uint64_t)(tag - 1u); }
static void ah_collect_r(void){
    uint8_t buf[RANT_DGRAM_MAX]; uint32_t to; size_t ol;
    while (ah_nh<8 && i_rant_transport_poll_send(ah_R,&to,buf,sizeof buf,&ol,ah_clk)){
        memcpy(ah_held[ah_nh], buf, ol); ah_hl[ah_nh++] = ol; }
}
static void ah_feed_w(void){
    int i;
    for (i=0;i<ah_nh;i++) i_rant_transport_on_datagram(ah_W, 2u, rant_bytes(ah_held[i], ah_hl[i]), ah_clk);
    ah_nh = 0;
}
static void ah_flush_w(void){
    uint8_t buf[RANT_DGRAM_MAX]; uint32_t to; size_t ol;
    while (i_rant_transport_poll_send(ah_W,&to,buf,sizeof buf,&ol,ah_clk)){
        size_t off=0;
        if ((buf[0]&0x07u)==1u && !(buf[0]&RANT_F_SINGLE)){
            uint64_t seqno = i_rant_le_r64(buf+RANT_OFFSET_SEQNO);
            unsigned frag = (unsigned)buf[RANT_OFFSET_FRAG] | ((unsigned)buf[RANT_OFFSET_FRAG+1]<<8);
            size_t sub = RANT_HEADER_DATA_MULTI + ((size_t)buf[RANT_OFFSET_PAYLOAD_LEN] | ((size_t)buf[RANT_OFFSET_PAYLOAD_LEN+1]<<8));
            int drop;
            if (!ah_have_first){ ah_first_base = seqno - frag; ah_have_first = 1; }
            drop = (ah_drop_mask & (1u<<frag)) || (seqno >= ah_drop_lo && seqno < ah_drop_hi);
            if (frag==5) ah_drop_mask = 0;
            if (drop) off = sub;
        }
        if (off < ol) i_rant_transport_on_datagram(ah_R, 1u, rant_bytes(buf+off, ol-off), ah_clk);
    }
}
static void ah_pump(uint64_t dt){ ah_flush_w(); ah_collect_r(); ah_feed_w(); ah_clk += dt; }
static void ah_run(int ms){ while (ms-- > 0) ah_pump(1000); }
static void ahead_checks(void){
    TestPair pair; i_RantTransportConfig cb;
    RantRepairStats ws, rs; uint64_t resent0, ahead0;
    RantQos q; memset(&q,0,sizeof q); q.reliability=RANT_RELIABLE; q.keep_last=4;
    q.heartbeat_us=30000; q.repair_delay_us=50000;
    memset(&cb,0,sizeof cb); cb.on_message=ah_on_msg; cb.on_event=ah_on_event;
    if (!test_pair_open(&pair, "ahead", q, q, &cb)){ CHECK(0, "ahead: transport pair opens"); return; }
    ah_W = pair.w; ah_R = pair.r;
    ah_clk=1000000; ah_nh=0; ah_drop_mask=0; ah_drop_lo=ah_drop_hi=0; ah_refuse=0;
    ah_tag=0; ah_last=0; ah_order_bad=0; ah_have_first=0;
    test_apply_verified(ah_R, 1u, ah_W);
    test_apply_verified(ah_W, 2u, ah_R);
    CHECK(i_rant_transport_publisher_match_count(ah_W,0)>0, "ahead: writer matched reader");

    /* [a] baseline */
    ah_recv=0; ah_lost=0; ah_send(); ah_pump(1000);
    CHECK(ah_recv==1 && ah_lost==0, "ahead: [a] a 6-fragment message delivers (recv=%d)", ah_recv);

    /* [b] frag 2 of message 2 is lost and message 3 arrives whole before the resend: it is
       HELD, not dropped, and delivers right behind the repaired head. One resend, in order. */
    i_rant_transport_repair_stats(ah_W,0,&ws); resent0=ws.frags_resent;
    i_rant_transport_repair_stats(ah_R,0,&rs); ahead0=rs.frags_ahead;
    ah_recv=0; ah_drop_mask=(1u<<2);
    ah_send(); ah_send(); ah_pump(1000);
    i_rant_transport_repair_stats(ah_R,0,&rs);
    CHECK(ah_recv==0 && rs.frags_ahead==ahead0, "ahead: [b] the next message is held while the head repairs (recv=%d ahead=%llu)",
             ah_recv, (unsigned long long)(rs.frags_ahead-ahead0));
    ah_pump(1000);
    i_rant_transport_repair_stats(ah_W,0,&ws);
    CHECK(ah_recv==2 && ah_order_bad==0 && ws.frags_resent-resent0==1,
             "ahead: [b] one resend completes the head, the held message follows at once (recv=%d resent=%llu)",
             ah_recv, (unsigned long long)(ws.frags_resent-resent0));

    /* [c] two messages ahead: the first is held, the second is still dropped and comes back
       in order through repair (6 resends), everything in order */
    i_rant_transport_repair_stats(ah_W,0,&ws); resent0=ws.frags_resent;
    i_rant_transport_repair_stats(ah_R,0,&rs); ahead0=rs.frags_ahead;
    ah_recv=0; ah_drop_mask=(1u<<2);
    ah_send(); ah_send(); ah_send(); ah_pump(1000);
    i_rant_transport_repair_stats(ah_R,0,&rs);
    CHECK(ah_recv==0 && rs.frags_ahead-ahead0==6, "ahead: [c] the second future message is dropped (ahead=%llu)",
             (unsigned long long)(rs.frags_ahead-ahead0));
    ah_run(5);
    i_rant_transport_repair_stats(ah_W,0,&ws);
    CHECK(ah_recv==3 && ah_order_bad==0 && ws.frags_resent-resent0==7,
             "ahead: [c] head repaired, held one delivered, dropped one refetched (recv=%d resent=%llu)",
             ah_recv, (unsigned long long)(ws.frags_resent-resent0));

    /* [d] the consumer refuses the head (parked): the next message still fills the hold, and
       an accepted retry delivers both with no resend */
    i_rant_transport_repair_stats(ah_W,0,&ws); resent0=ws.frags_resent;
    i_rant_transport_repair_stats(ah_R,0,&rs); ahead0=rs.frags_ahead;
    ah_recv=0; ah_refuse=1;
    ah_send(); ah_pump(1000);
    ah_send(); ah_pump(1000);
    i_rant_transport_repair_stats(ah_R,0,&rs);
    CHECK(ah_recv==0 && rs.frags_ahead==ahead0 && i_rant_transport_deliver_parked(ah_R,0,ah_clk)==1,
             "ahead: [d] parked head, the next message held meanwhile (ahead=%llu)", (unsigned long long)(rs.frags_ahead-ahead0));
    ah_refuse=0;
    CHECK(i_rant_transport_deliver_parked(ah_R,0,ah_clk)==0 && ah_recv==2 && ah_order_bad==0,
             "ahead: [d] unpark delivers the head and the held message (recv=%d)", ah_recv);
    ah_pump(1000);
    i_rant_transport_repair_stats(ah_W,0,&ws);
    CHECK(ws.frags_resent==resent0, "ahead: [d] no resend was needed (resent=%llu)", (unsigned long long)(ws.frags_resent-resent0));

    /* [e] a message is lost whole and the next one is held. The writer evicts the lost one
       and its floor lands on the held message: delivered from the hold, one MSG_LOST */
    ah_recv=0; ah_lost=0;
    ah_drop_lo=ah_base_of(ah_tag+1u); ah_drop_hi=ah_drop_lo+6u;
    ah_send(); ah_send(); ah_pump(1000);          /* 7 lost whole, 8 held */
    ah_send(); ah_pump(1000); ah_send(); ah_pump(1000); ah_send(); ah_pump(1000);
    ah_run(120);
    CHECK(ah_recv==4 && ah_lost==6 && ah_order_bad==0,
             "ahead: [e] the floor landed on the held message: delivered, one lost, rest refetched in order (recv=%d lost=%d)",
             ah_recv, ah_lost);
    ah_drop_lo=ah_drop_hi=0;

    /* [f] a hold across a wholly lost message: 12 has a hole, 13 is lost whole once, 14 is
       held. 12 repairs, 13 is fetched in order, 14 delivers from the hold */
    i_rant_transport_repair_stats(ah_W,0,&ws); resent0=ws.frags_resent;
    i_rant_transport_repair_stats(ah_R,0,&rs); ahead0=rs.frags_ahead;
    ah_recv=0; ah_lost=0; ah_drop_mask=(1u<<2);
    ah_drop_lo=ah_base_of(ah_tag+2u); ah_drop_hi=ah_drop_lo+6u;
    ah_send(); ah_send(); ah_send(); ah_pump(1000);
    ah_drop_lo=ah_drop_hi=0;                     /* 13's resends get through */
    ah_run(5);
    i_rant_transport_repair_stats(ah_W,0,&ws); i_rant_transport_repair_stats(ah_R,0,&rs);
    CHECK(ah_recv==3 && ah_lost==0 && ah_order_bad==0 && ws.frags_resent-resent0==7 && rs.frags_ahead==ahead0,
             "ahead: [f] hold across a lost message: repaired, fetched, delivered in order (recv=%d resent=%llu ahead=%llu)",
             ah_recv, (unsigned long long)(ws.frags_resent-resent0), (unsigned long long)(rs.frags_ahead-ahead0));
    test_pair_close(&pair);
}

/* The small pure helpers: the fragment clamp, little endian packing, topic identity and the
   send result codes of a bare transport core. */
static void basics_checks(void){
    /* i_rant_clamp_frag: 0 = default, else clamp into [MIN, MAX] */
    CHECK(i_rant_clamp_frag(0) == RANT_FRAG_SIZE,
             "clamp: 0 -> default frag (%u)", (unsigned)i_rant_clamp_frag(0));
    CHECK(i_rant_clamp_frag(65535) == RANT_FRAG_SIZE_MAX,
             "clamp: above-max -> MAX (%u)", (unsigned)i_rant_clamp_frag(65535));
    CHECK(i_rant_clamp_frag(1) >= RANT_FRAG_SIZE_MIN,
             "clamp: tiny -> >= MIN (%u)", (unsigned)i_rant_clamp_frag(1));

    /* shared little-endian helpers: byte order + round-trip */
    {   uint8_t b[8];
        i_rant_le_w16(b, 0xBEEFu);
        CHECK(b[0]==0xEF && b[1]==0xBE && i_rant_le_r16(b)==0xBEEFu,
                 "bytes: w16/r16 little-endian round-trip");
        i_rant_le_w32(b, 0x01020304u);
        CHECK(b[0]==0x04 && b[3]==0x01 && i_rant_le_r32(b)==0x01020304u,
                 "bytes: w32/r32 little-endian round-trip");
        i_rant_le_w64(b, 0x0102030405060708ull);
        CHECK(b[0]==0x08 && b[7]==0x01 && i_rant_le_r64(b)==0x0102030405060708ull,
                 "bytes: w64/r64 little-endian round-trip");
    }

    /* topic identity: deterministic and name-distinct */
    CHECK(i_rant_topic_id("alpha") == i_rant_topic_id("alpha")
             && i_rant_topic_id("alpha") != i_rant_topic_id("beta"),
             "topic-id: deterministic and name-distinct");

    /* send result codes on the transport core: the size check precedes the role check, and
       an init with no allocator must refuse (spec/testing.md) */
    {   static uint8_t tmem[1<<16], amem[1<<12];
        i_RantTopicDef uch[2]; i_RantTransportConfig tc; i_RantTransportState *ts; uint8_t buf[128];
        static RantAllocator A; A = rant_allocator_static(amem, sizeof amem);
        memset(uch, 0, sizeof uch);
        uch[0].name = "u/pub"; uch[0].role = RANT_PUBSUB;
        uch[1].name = "u/sub"; uch[1].role = RANT_SUB_ONLY;
        memset(&tc, 0, sizeof tc);
        tc.topics = uch; tc.n_topics = 2; tc.max_peers = 2;
        CHECK(i_rant_transport_init(tmem, sizeof tmem, &tc) == NULL, "result: init without an allocator refused");
        tc.allocator = rant_allocator_alloc; tc.user = &A;
        ts = i_rant_transport_init(tmem, sizeof tmem, &tc);
        CHECK(ts != NULL, "result: transport init");
        if (ts){
            size_t wire_cap = 65535u * (size_t)i_rant_transport_frag(ts);     /* checked before copy */
            memset(buf, 0, sizeof buf);
            CHECK(i_rant_transport_send(ts, 5, rant_bytes(buf, 16),      0) == RANT_ERR_NO_TOPIC, "result: out-of-range topic -> NO_CHANNEL");
            CHECK(i_rant_transport_send(ts, 1, rant_bytes(buf, 16),      0) == RANT_ERR_ROLE,         "result: sub-only topic -> ROLE");
            CHECK(i_rant_transport_send(ts, 0, rant_bytes(buf, wire_cap + 1u), 0) == RANT_ERR_TOO_BIG, "result: past the wire cap -> TOO_BIG");
            CHECK(i_rant_transport_send(ts, 0, rant_bytes(buf, 16),      0) == RANT_OK,               "result: valid publish -> OK");
        }
    }
}

/* A reliable subscriber must refuse a best effort publisher, every other direction matches.
   Build the interest, apply it, read the match. */
static unsigned long qos_incompat_n;
static void qos_on_event(const RantEvent *ev){ if (ev->error==RANT_E_QOS_INCOMPATIBLE) qos_incompat_n++; }
static void qos_pair(int wrel, int rrel, uint16_t *recv_out, unsigned long *evt_out){
    TestPair pair; i_RantTransportConfig cb; uint16_t pub=0, recv=0;
    RantQos wq, rq;
    memset(&wq,0,sizeof wq); wq.reliability=wrel?RANT_RELIABLE:RANT_BEST_EFFORT; wq.keep_last=4;
    memset(&rq,0,sizeof rq); rq.reliability=rrel?RANT_RELIABLE:RANT_BEST_EFFORT; rq.keep_last=4;
    memset(&cb,0,sizeof cb); cb.on_event=qos_on_event;
    *recv_out = 0; *evt_out = 0;
    if (!test_pair_open(&pair, "qostopic", wq, rq, &cb)) return;
    qos_incompat_n=0;
    test_apply_verified(pair.r, 1u, pair.w);
    i_rant_transport_peer_match_counts(pair.r,1u,&pub,&recv);
    *recv_out=recv;
    *evt_out=qos_incompat_n;
    test_pair_close(&pair);
}
static void qos_match_checks(void){
    uint16_t recv; unsigned long evt;
    qos_pair(0,1,&recv,&evt);   /* best-effort pub, reliable sub: REFUSED */
    CHECK(recv==0, "qos: reliable sub refuses best-effort pub (receive_from=%u)", recv);
    CHECK(evt>=1,  "qos: refusal raised RANT_QOS_INCOMPATIBLE (n=%lu)", evt);
    qos_pair(1,1,&recv,&evt);   /* reliable pub, reliable sub */
    CHECK(recv==1 && evt==0, "qos: reliable sub matches reliable pub (recv=%u evt=%lu)", recv, evt);
    qos_pair(1,0,&recv,&evt);   /* reliable pub, best-effort sub: allowed downgrade */
    CHECK(recv==1 && evt==0, "qos: best-effort sub matches reliable pub (recv=%u evt=%lu)", recv, evt);
    qos_pair(0,0,&recv,&evt);   /* both best-effort */
    CHECK(recv==1 && evt==0, "qos: best-effort sub matches best-effort pub (recv=%u evt=%lu)", recv, evt);
}

/* A best effort reader matched to a reliable writer never counts toward backpressure. Fill
   the history ring past keep_last with no acks. -1 when the pair does not open */
static int beff_would_evict(int rrel){
    TestPair pair; uint8_t payload[8]; int i, evict;
    RantQos wq, rq;
    memset(&wq,0,sizeof wq); wq.reliability=RANT_RELIABLE; wq.keep_last=2;
    memset(&rq,0,sizeof rq); rq.reliability=rrel?RANT_RELIABLE:RANT_BEST_EFFORT; rq.keep_last=2;
    if (!test_pair_open(&pair, "beff", wq, rq, NULL)) return -1;
    test_apply_verified(pair.w, 2u, pair.r);   /* the writer learns and verifies the subscriber */
    memset(payload,0x5A,sizeof payload);
    for (i=0;i<5;i++) i_rant_transport_send(pair.w,0,rant_bytes(payload,sizeof payload),1000u+(uint64_t)i);
    evict = i_rant_transport_send_would_evict(pair.w,0);
    test_pair_close(&pair);
    return evict;
}
static void beff_flow_checks(void){
    CHECK(beff_would_evict(0)==0, "flow: best-effort reader never stalls a reliable writer");
    CHECK(beff_would_evict(1)==1, "flow: reliable reader does apply backpressure");
}

/* The wire tap mirrors the submessage framing it counts. A wire change must update both. */
static void tap_framing_checks(void){
    CHECK(WIRE_TAP_DATA == RANT_DATA && WIRE_TAP_HB == RANT_HB && WIRE_TAP_NACK == RANT_NACK
          && WIRE_TAP_MSG_MASK == RANT_MSG_MASK, "tap: submessage types");
    CHECK(WIRE_TAP_F_SINGLE == RANT_F_SINGLE && WIRE_TAP_OFFSET_INDEX == RANT_OFFSET_INDEX
          && WIRE_TAP_OFFSET_LEN_SINGLE == RANT_OFFSET_PAYLOAD_LEN_SINGLE
          && WIRE_TAP_OFFSET_LEN_MULTI == RANT_OFFSET_PAYLOAD_LEN, "tap: flags and offsets");
    CHECK(WIRE_TAP_HEADER_DATA_SINGLE == RANT_HEADER_DATA_SINGLE
          && WIRE_TAP_HEADER_DATA_MULTI == RANT_HEADER_DATA_MULTI
          && WIRE_TAP_HEADER_HB == RANT_HEADER_HB && WIRE_TAP_HEADER_NACK == RANT_HEADER_NACK,
          "tap: header sizes");
#ifdef RANT_SHM
    CHECK(WIRE_TAP_F_SHM == RANT_F_SHM && WIRE_TAP_SHM_DATA_BYTES == RANT_SHM_DATA_BYTES,
          "tap: shared memory descriptor");
#endif
}

/* The want set: which interest lists meet our topics, by the opposite side or a rival
   authority of the same kind, and when the set grew. The peer lists come from a second core. */
static size_t ws_interest(uint8_t *out, size_t cap, const char *name, uint8_t role, uint8_t kind){
    static uint8_t pmem[1<<16], amem[1<<12];
    static RantAllocator A;
    i_RantTopicDef d; i_RantTransportConfig tc; i_RantTransportState *p;
    A = rant_allocator_static(amem, sizeof amem);
    memset(&d, 0, sizeof d); d.name = name; d.role = role; d.kind = kind;
    memset(&tc, 0, sizeof tc); tc.topics = &d; tc.n_topics = 1; tc.max_peers = 1;
    tc.allocator = rant_allocator_alloc; tc.user = &A;
    p = i_rant_transport_init(pmem, sizeof pmem, &tc);
    return p ? i_rant_transport_build_interest(p, out, cap) : 0;
}
static int ws_meets(i_RantTransportState *st, const char *name, uint8_t role, uint8_t kind){
    uint8_t buf[256]; size_t n = ws_interest(buf, sizeof buf, name, role, kind);
    return n && i_rant_transport_interest_meets(st, rant_bytes(buf, n));
}
static void want_set_checks(void){
    static uint8_t tmem[1<<17], amem[1<<14];
    static RantAllocator A;
    i_RantTransportConfig tc; i_RantTransportState *st; i_RantTopicDef d;
    uint8_t buf[256]; size_t n;
    A = rant_allocator_static(amem, sizeof amem);
    memset(&tc, 0, sizeof tc); tc.n_topics = 8; tc.max_peers = 2;
    tc.allocator = rant_allocator_alloc; tc.user = &A;
    st = i_rant_transport_init(tmem, sizeof tmem, &tc);
    CHECK(st != NULL, "want set: transport init");
    if (!st) return;
    CHECK(!ws_meets(st, "ws/t", RANT_SUB_ONLY, RANT_KIND_TOPIC), "want set: no topic of ours meets nothing");
    i_rant_transport_want_new_clear(st);

    memset(&d, 0, sizeof d); d.name = "ws/t"; d.role = RANT_PUB_ONLY;
    CHECK(i_rant_transport_topic_define(st, 0, &d) == 0 && i_rant_transport_want_new_any(st),
          "want set: a new topic grows it");
    CHECK(ws_meets(st, "ws/t", RANT_SUB_ONLY, RANT_KIND_TOPIC) && ws_meets(st, "ws/t", RANT_PUBSUB, RANT_KIND_TOPIC)
          && !ws_meets(st, "ws/t", RANT_PUB_ONLY, RANT_KIND_TOPIC) && !ws_meets(st, "ws/other", RANT_SUB_ONLY, RANT_KIND_TOPIC)
          && !ws_meets(st, "ws/t", RANT_INACTIVE, RANT_KIND_TOPIC),
          "want set: our publish meets a subscriber, never a publisher, another name or an inactive entry");
    CHECK(ws_meets(st, "ws/t", RANT_SUB_ONLY, RANT_KIND_VARIABLE),
          "want set: the opposite side meets whatever its kind, so the kind mismatch is reported");

    memset(&d, 0, sizeof d); d.name = "ws/v"; d.role = RANT_PUB_ONLY; d.kind = RANT_KIND_VARIABLE;
    (void)i_rant_transport_topic_define(st, 1, &d);
    CHECK(ws_meets(st, "ws/v", RANT_PUB_ONLY, RANT_KIND_VARIABLE) && !ws_meets(st, "ws/v", RANT_PUB_ONLY, RANT_KIND_TOPIC),
          "want set: a variable we own meets a rival owner of the same kind only");
    memset(&d, 0, sizeof d); d.name = "ws/f@req"; d.role = RANT_SUB_ONLY; d.kind = RANT_KIND_FUNC_REQ;
    (void)i_rant_transport_topic_define(st, 2, &d);
    memset(&d, 0, sizeof d); d.name = "ws/m@req"; d.role = RANT_SUB_ONLY; d.kind = RANT_KIND_FUNC_REQ;
    d.attrs = RANT_ATTR_MULTI;
    (void)i_rant_transport_topic_define(st, 3, &d);
    CHECK(ws_meets(st, "ws/f@req", RANT_SUB_ONLY, RANT_KIND_FUNC_REQ) && !ws_meets(st, "ws/m@req", RANT_SUB_ONLY, RANT_KIND_FUNC_REQ)
          && ws_meets(st, "ws/m@req", RANT_PUB_ONLY, RANT_KIND_FUNC_REQ),
          "want set: a function we serve meets a rival provider unless it is MULTI, a caller always");

    i_rant_transport_want_new_clear(st);
    CHECK(i_rant_transport_set_role(st, 0, RANT_INACTIVE) == 0 && !i_rant_transport_want_new_any(st)
          && !ws_meets(st, "ws/t", RANT_SUB_ONLY, RANT_KIND_TOPIC),
          "want set: a parked topic meets nothing and does not grow it");
    CHECK(i_rant_transport_set_role(st, 0, RANT_PUB_ONLY) == 0 && i_rant_transport_want_new_any(st),
          "want set: back from parked it grows again");
    i_rant_transport_want_new_clear(st);
    CHECK(i_rant_transport_topic_retire(st, 1) == 0 && !i_rant_transport_want_new_any(st)
          && !ws_meets(st, "ws/v", RANT_SUB_ONLY, RANT_KIND_VARIABLE),
          "want set: a retired topic meets nothing and does not grow it");

    n = ws_interest(buf, sizeof buf, "ws/t", RANT_SUB_ONLY, RANT_KIND_TOPIC);
    CHECK(n > 7 && i_rant_transport_interest_meets(st, rant_bytes(buf, n))
          && !i_rant_transport_interest_meets(st, rant_bytes(buf, 6)),
          "want set: an entry cut short meets nothing, as an apply would refuse it");
}

/* The filter of a peer judged no match, and the pairings our topics gained since the node last
   took them: a pairing the peer takes is always found, one it lacks only rarely, the gained ones
   are kept across rebuilds until cleared, and out of memory every filter may meet us. */
static int wf_fail;
static void *wf_alloc(void *user, void *ptr, size_t size){
    return wf_fail && size ? NULL : rant_allocator_alloc(user, ptr, size);
}
/* The filter of a peer with n topics wf/p00.. in role, or of one topic name when n is 0. */
static void wf_filter(uint8_t *filter, const char *name, int n, uint8_t role){
    static uint8_t pmem[1<<18], amem[1<<15], buf[2048];
    static RantAllocator A;
    static char names[24][16];
    i_RantTopicDef d[24]; i_RantTransportConfig tc; i_RantTransportState *p; int k;
    A = rant_allocator_static(amem, sizeof amem);
    memset(d, 0, sizeof d);
    for (k = 0; k < (n ? n : 1); k++){
        if (n) sprintf(names[k], "wf/p%02d", k);
        d[k].name = n ? names[k] : name; d[k].role = role;
    }
    memset(&tc, 0, sizeof tc); tc.topics = d; tc.n_topics = (uint16_t)(n ? n : 1); tc.max_peers = 1;
    tc.allocator = rant_allocator_alloc; tc.user = &A;
    p = i_rant_transport_init(pmem, sizeof pmem, &tc);
    memset(filter, 0, RANT_DISCOVERY_FILTER_BYTES);
    if (p) i_rant_transport_interest_fold(rant_bytes(buf, i_rant_transport_build_interest(p, buf, sizeof buf)),
                                          filter, RANT_DISCOVERY_FILTER_BYTES);
}
/* 1 when a node that just made one topic name in role may meet a peer of this filter. */
static int wf_maybe(const uint8_t *filter, const char *name, uint8_t role){
    static uint8_t mem[1<<16], amem[1<<12];
    static RantAllocator A;
    i_RantTopicDef d; i_RantTransportConfig tc; i_RantTransportState *s;
    A = rant_allocator_static(amem, sizeof amem);
    memset(&d, 0, sizeof d); d.name = name; d.role = role;
    memset(&tc, 0, sizeof tc); tc.topics = &d; tc.n_topics = 1; tc.max_peers = 1;
    tc.allocator = rant_allocator_alloc; tc.user = &A;
    s = i_rant_transport_init(mem, sizeof mem, &tc);
    return s && i_rant_transport_want_new_maybe(s, filter, RANT_DISCOVERY_FILTER_BYTES);
}
/* The false hit rate of a filter holding a 20 item peer, three bits per item: 60 bits set. */
static double wf_expected(void){
    double clear = 1.0, m = 8.0 * RANT_DISCOVERY_FILTER_BYTES; int i;
    for (i = 0; i < 60; i++) clear *= 1.0 - 1.0 / m;
    return (1.0 - clear) * (1.0 - clear) * (1.0 - clear);
}
static void want_filter_checks(void){
    static uint8_t tmem[1<<17], amem[1<<14];
    static RantAllocator A;
    uint8_t pubs[RANT_DISCOVERY_FILTER_BYTES], subs[RANT_DISCOVERY_FILTER_BYTES], fa[RANT_DISCOVERY_FILTER_BYTES],
            fb[RANT_DISCOVERY_FILTER_BYTES], fc[RANT_DISCOVERY_FILTER_BYTES], none[RANT_DISCOVERY_FILTER_BYTES];
    i_RantTransportConfig tc; i_RantTransportState *st; i_RantTopicDef d;
    char name[16]; int k, missed = 0, wrong = 0, hits = 0;
    double p = wf_expected();
    wf_filter(pubs, NULL, 20, RANT_PUB_ONLY);
    wf_filter(subs, NULL, 20, RANT_SUB_ONLY);
    for (k = 0; k < 20; k++){
        sprintf(name, "wf/p%02d", k);
        missed += !wf_maybe(pubs, name, RANT_SUB_ONLY) + !wf_maybe(subs, name, RANT_PUB_ONLY);
        wrong  += wf_maybe(pubs, name, RANT_PUB_ONLY) + wf_maybe(subs, name, RANT_SUB_ONLY);
    }
    CHECK(missed == 0, "want filter: every pairing a peer takes is found, both sides (%d missed)", missed);
    CHECK(wrong <= 2.0 * 40.0 * p + 2.0, "want filter: the side counts, a publisher's filter holds no subscriber (%d of 40)", wrong);
    for (k = 0; k < 400; k++){
        sprintf(name, "wf/q%03d", k);
        hits += wf_maybe(pubs, name, RANT_SUB_ONLY) + wf_maybe(subs, name, RANT_PUB_ONLY);
    }
    CHECK(hits <= 2.0 * 800.0 * p + 8.0, "want filter: a name a small peer lacks rarely hits (%d of 800, %.1f%% expected)",
          hits, 100.0 * p);

    wf_filter(fa, "wf/a", 0, RANT_SUB_ONLY);
    wf_filter(fb, "wf/b", 0, RANT_SUB_ONLY);
    wf_filter(fc, "wf/c", 0, RANT_SUB_ONLY);
    memset(none, 0, sizeof none);
    A = rant_allocator_static(amem, sizeof amem);
    memset(&tc, 0, sizeof tc); tc.n_topics = 4; tc.max_peers = 2;
    tc.allocator = wf_alloc; tc.user = &A;
    st = i_rant_transport_init(tmem, sizeof tmem, &tc);
    CHECK(st != NULL, "want filter: transport init");
    if (!st) return;
    memset(&d, 0, sizeof d); d.name = "wf/a"; d.role = RANT_PUB_ONLY;
    (void)i_rant_transport_topic_define(st, 0, &d);
    (void)ws_meets(st, "wf/zz", RANT_SUB_ONLY, RANT_KIND_TOPIC);     /* a judge's rebuild between the two */
    d.name = "wf/b";
    (void)i_rant_transport_topic_define(st, 1, &d);
    CHECK(i_rant_transport_want_new_maybe(st, fa, sizeof fa) && i_rant_transport_want_new_maybe(st, fb, sizeof fb)
          && !i_rant_transport_want_new_maybe(st, none, sizeof none),
          "want filter: the pairings two rebuilds gained are both kept, an empty filter meets neither");
    i_rant_transport_want_new_clear(st);
    CHECK(!i_rant_transport_want_new_any(st) && !i_rant_transport_want_new_maybe(st, fa, sizeof fa),
          "want filter: cleared, nothing is new");
    CHECK(i_rant_transport_set_role(st, 0, RANT_INACTIVE) == 0 && !i_rant_transport_want_new_any(st),
          "want filter: a parked topic gains nothing");
    (void)i_rant_transport_set_role(st, 0, RANT_PUB_ONLY);
    CHECK(i_rant_transport_want_new_maybe(st, fa, sizeof fa) && !i_rant_transport_want_new_maybe(st, fb, sizeof fb),
          "want filter: a topic back from parked is new again, the other is not");
    i_rant_transport_want_new_clear(st);
    d.name = "wf/c";
    (void)i_rant_transport_topic_define(st, 2, &d);
    wf_fail = 1;
    CHECK(i_rant_transport_want_new_any(st) && i_rant_transport_want_new_maybe(st, none, sizeof none),
          "want filter: out of memory every filter may meet a new pairing");
    wf_fail = 0;
    i_rant_transport_want_new_clear(st);
    CHECK(i_rant_transport_want_new_maybe(st, fc, sizeof fc) && !i_rant_transport_want_new_maybe(st, fa, sizeof fa),
          "want filter: the rebuild that works again gains what the failed one held back");
    i_rant_transport_want_new_clear(st);
    CHECK(!i_rant_transport_want_new_any(st) && !i_rant_transport_want_new_maybe(st, none, sizeof none),
          "want filter: once the node took those, nothing is new");
    i_rant_transport_destroy(st);
}

/* Two pools behind one transport user, so a check sees which hook each buffer came from. */
typedef struct { RantAllocator main, bulk; } bk_Pools;
static void *bk_main(void *u, void *ptr, size_t size){ return rant_allocator_alloc(&((bk_Pools*)u)->main, ptr, size); }
static void *bk_bulk(void *u, void *ptr, size_t size){ return rant_allocator_alloc(&((bk_Pools*)u)->bulk, ptr, size); }
static int bk_recv; static size_t bk_len;
static int bk_on_msg(void *u, uint16_t ch, uint32_t from, RantBytes d){
    (void)u; (void)ch; (void)from; bk_recv++; bk_len = d.len; return 0;
}
static i_RantTransportState *bk_side(i_RantTopicDef *def, i_RantTransportConfig *cfg, bk_Pools *pl,
                                     void **mem, RantRole role, RantQos q){
    size_t need;
    memset(def, 0, sizeof *def); def->name = "bk/big"; def->qos = q; def->role = role;
    memset(cfg, 0, sizeof *cfg);
    cfg->topics = def; cfg->n_topics = 1; cfg->max_peers = 2; cfg->on_message = bk_on_msg;
    cfg->allocator = bk_main; cfg->bulk = bk_bulk; cfg->user = pl;
    pl->main = rant_allocator_heap(0); pl->bulk = rant_allocator_heap(0);
    need = i_rant_transport_required_memory(cfg);
    *mem = malloc(need);
    return *mem ? i_rant_transport_init(*mem, need, cfg) : NULL;
}

/* The bulk hook takes the history samples and the reassembly, the main one the lanes and
   the peer maps, and each frees into the pool it came from. */
static void bulk_checks(void){
    static uint8_t msg[20000];
    i_RantTopicDef wd, rd; i_RantTransportConfig wc, rc; bk_Pools wp, rp; void *wm = NULL, *rm = NULL;
    i_RantTransportState *w, *r; RantQos q; uint8_t buf[RANT_DGRAM_MAX]; uint32_t to; size_t ol;
    size_t wmain, rmain, wbulk, rbulk; uint64_t clk = 1000000u; int i;
    memset(&q, 0, sizeof q); q.reliability = RANT_RELIABLE; q.keep_last = 1;
    w = bk_side(&wd, &wc, &wp, &wm, RANT_PUB_ONLY, q);
    r = bk_side(&rd, &rc, &rp, &rm, RANT_SUB_ONLY, q);
    CHECK(w && r, "bulk: two transports open");
    if (!w || !r) goto done;
    i_rant_transport_peer_add(w, 2u, RANT_FRAG_SIZE);
    i_rant_transport_peer_add(r, 1u, RANT_FRAG_SIZE);
    test_apply_verified(r, 1u, w);
    test_apply_verified(w, 2u, r);
    CHECK(i_rant_transport_publisher_match_count(w, 0) == 1 && wp.bulk.in_use == 0 && rp.bulk.in_use == 0
          && wp.main.in_use > 0 && rp.main.in_use > 0,
          "bulk: a match puts its lanes and maps in the main and nothing in the bulk (main %u %u)",
          (unsigned)wp.main.in_use, (unsigned)rp.main.in_use);
    wmain = wp.main.in_use; rmain = rp.main.in_use;
    bk_recv = 0;
    CHECK(i_rant_transport_send(w, 0, rant_bytes(msg, sizeof msg), clk) == RANT_OK, "bulk: a 20 kB send commits");
    for (i = 0; i < 50 && !bk_recv; i++){
        while (i_rant_transport_poll_send(w, &to, buf, sizeof buf, &ol, clk))
            i_rant_transport_on_datagram(r, 1u, rant_bytes(buf, ol), clk);
        while (i_rant_transport_poll_send(r, &to, buf, sizeof buf, &ol, clk))
            i_rant_transport_on_datagram(w, 2u, rant_bytes(buf, ol), clk);
        clk += 1000u;
    }
    wbulk = wp.bulk.in_use; rbulk = rp.bulk.in_use;
    CHECK(bk_recv == 1 && bk_len >= sizeof msg && wbulk >= sizeof msg && rbulk >= sizeof msg
          && wp.main.in_use == wmain && rp.main.in_use == rmain,
          "bulk: the history sample and the reassembly are in the bulk, the mains unmoved (recv %d of %u, bulk %u %u, main %+d %+d)",
          bk_recv, (unsigned)bk_len, (unsigned)wbulk, (unsigned)rbulk, (int)(wp.main.in_use - wmain), (int)(rp.main.in_use - rmain));
    i_rant_transport_peer_remove(r, 1u);
    i_rant_transport_topic_retire(w, 0);
    CHECK(rp.bulk.in_use == 0 && wp.bulk.in_use == 0 && rp.main.in_use < rmain,
          "bulk: a peer removed and a topic retired give the bulk blocks back to the bulk (bulk %u %u)",
          (unsigned)wp.bulk.in_use, (unsigned)rp.bulk.in_use);
    i_rant_transport_destroy(w); i_rant_transport_destroy(r); w = r = NULL;
    CHECK(wp.main.in_use == 0 && rp.main.in_use == 0 && wp.bulk.in_use == 0 && rp.bulk.in_use == 0,
          "bulk: destroy leaves each pool empty (main %u %u)", (unsigned)wp.main.in_use, (unsigned)rp.main.in_use);
done:
    if (w) i_rant_transport_destroy(w);
    if (r) i_rant_transport_destroy(r);
    free(wm); free(rm);
    rant_allocator_reset(&wp.main); rant_allocator_reset(&wp.bulk);
    rant_allocator_reset(&rp.main); rant_allocator_reset(&rp.bulk);
}

static const TestCase cases[] = {
    { "basics", basics_checks },
    { "bulk", bulk_checks },
    { "want_set", want_set_checks },
    { "want_filter", want_filter_checks },
    { "rate", rate_checks },
    { "lapped", lapped_checks },
    { "rtt", rtt_checks },
    { "ahead", ahead_checks },
    { "qos_match", qos_match_checks },
    { "best_effort_flow", beff_flow_checks },
    { "tap_framing", tap_framing_checks },
};

TEST_MAIN(cases)
