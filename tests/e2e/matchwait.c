/* The send path match wait: writer authoritative repair of a sample sent before the peer
 * verified, the one time window, and the loud drop when the wait is off.
 * spec/testing.md says what each case pins. */
#include "support/test.h"

/* The match wait checks: a first send racing the announce and detail cycle, a
 * disabled wait dropping loudly, and writer authoritative delivery (spec/testing.md). */
/* a started node counts on its service thread, mw_last is read once mw_recv shows it */
static long long mw_recv, mw_lost, mw_unmatched;
static char mw_last[64];
static void mw_on_message(const RantMsg *m){
    size_t c = m->data.len < sizeof mw_last - 1 ? m->data.len : sizeof mw_last - 1;
    memcpy(mw_last, m->data.data, c); mw_last[c] = 0;
    test_atomic_add(&mw_recv, 1);
}
static void mw_on_event(const RantEvent *ev){
    if (ev->kind == RANT_MSG_LOST) test_atomic_add(&mw_lost, 1);
    if (ev->kind == RANT_ERROR && ev->error == RANT_E_UNMATCHED_SEND) test_atomic_add(&mw_unmatched, 1);
}

static void matchwait_checks(void){
    RantAddr seed; int t;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;

    /* (a) WRITER-AUTHORITATIVE HEAL: B never receives DETAIL_RESPs, so it cannot verify A
       while A matches B. Unblocking lets B verify and repair must deliver the original sample. */
    { /* a fixed port, only so the tap can drop by port. The domain base keeps parallel
         programs apart */
      const uint16_t MW_PORT = test_free_port((uint16_t)(40000u + test_domain_base % 20000u));
      RantAllocator aa = rant_allocator_heap(0);
      RantAllocator ba = rant_allocator_heap(0);
      RantNodeOpts ao, bo; RantNode *A, *B; RantTopic *at=NULL, *bt=NULL;
      memset(&bo,0,sizeof bo); bo.domain=TEST_DOMAIN+24; bo.disable_shm=1; bo.discovery.max_peers=4;
      bo.discovery.announce_interval_us=200000;
      bo.net.multicast_interface="127.0.0.1"; bo.net.seed_peers=&seed; bo.net.n_seed_peers=1;
      ao=bo; bo.net.data_port=MW_PORT;
      mw_recv=0; mw_lost=0; mw_last[0]=0;
      B = rant_node_open(&ba, "mw-b", mw_on_message, mw_on_event, &bo);
      CHECK(B!=NULL, "matchwait: sub node opens (is port %d free?)", (int)MW_PORT);
      if (B) bt = rant_node_create_topic(B, "mw-auth", RANT_SUB_ONLY, NULL,
                      &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE } });
      wire_tap_write(&wire_tap.drop_detail_resp_port, MW_PORT);   /* B stays PENDING on every candidate */
      A = rant_node_open(&aa, "mw-a", NULL, NULL, &ao);
      at = A ? rant_node_create_topic(A, "mw-auth", RANT_PUB_ONLY, NULL,
                      &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE, .keep_last=8,
                                               .heartbeat_us=50000 } }) : NULL;
      CHECK(A && at && bt, "matchwait: nodes + topics up");
      if (A && B && at && bt){
          int r;
          PUMP_UNTIL(A, B, 24000, rant_topic_match_count(at)!=0);
          CHECK(rant_topic_match_count(at)==1,
                   "matchwait: writer side matched while B's responses are blocked (%d)",
                   rant_topic_match_count(at));
          CHECK(rant_topic_pending_count(bt) > 0,
                   "matchwait: reader side still PENDING (%d)", rant_topic_pending_count(bt));
          r = rant_topic_send(at, rant_bytes("authoritative", 13), NULL);
          CHECK(r==RANT_OK, "matchwait: matched send commits (%d)", r);
          test_pump(A, B, 600);
          CHECK(test_atomic_load(&mw_recv)==0, "matchwait: unverified reader drops the DATA (recv=%lld)",
                   test_atomic_load(&mw_recv));
          wire_tap_write(&wire_tap.drop_detail_resp_port, 0);      /* B's next re-ask verifies A */
          PUMP_UNTIL(A, B, 24000, test_atomic_load(&mw_recv)!=0);
          { long long got = test_atomic_load(&mw_recv);
            CHECK(got==1 && strcmp(mw_last,"authoritative")==0,
                     "matchwait: repair delivers the pre-verify sample (recv=%lld '%s')", got, mw_last); }
          CHECK(test_atomic_load(&mw_lost)==0, "matchwait: healed with no MSG_LOST (%lld)",
                   test_atomic_load(&mw_lost));
          CHECK(rant_topic_pending_count(at)==0 && rant_topic_ready(at)==1,
                   "matchwait: probes converge (pending=%d ready=%d)",
                   rant_topic_pending_count(at), rant_topic_ready(at));
      }
      wire_tap_write(&wire_tap.drop_detail_resp_port, 0);
      if (A) rant_node_close(A,1);
      if (B) rant_node_close(B,1);
      rant_allocator_reset(&aa); rant_allocator_reset(&ba);
    }

    /* (a2) THE WINDOW: A never receives B's DETAIL_RESPs, so B stays a candidate forever. The
       wait bridges A's own coming up only: one window after the create, then no send waits,
       and a subscriber that appears later never blocks a send at all. */
    { const uint16_t MW_PORT2 = test_free_port((uint16_t)(40000u + (test_domain_base + 1u) % 20000u));
      RantAllocator aa = rant_allocator_heap(0);
      RantAllocator ba = rant_allocator_heap(0);
      RantNodeOpts ao, bo; RantNode *A, *B; RantTopic *poison=NULL, *late=NULL;
      memset(&bo,0,sizeof bo); bo.domain=TEST_DOMAIN+13; bo.disable_shm=1; bo.discovery.max_peers=4;
      bo.discovery.announce_interval_us=200000;
      bo.net.multicast_interface="127.0.0.1"; bo.net.seed_peers=&seed; bo.net.n_seed_peers=1;
      ao=bo; ao.net.data_port=MW_PORT2; ao.match_wait_ms=300;
      mw_unmatched=0;
      B = rant_node_open(&ba, "mw-wb", mw_on_message, mw_on_event, &bo);
      if (B) (void)rant_node_create_topic(B, "mw-poison", RANT_SUB_ONLY, NULL, NULL);
      if (B) rant_node_start(B);
      wire_tap_write(&wire_tap.drop_detail_resp_port, MW_PORT2);
      A = rant_node_open(&aa, "mw-wa", NULL, mw_on_event, &ao);
      CHECK(A && B, "matchwait: window pair up (is port %d free?)", (int)MW_PORT2);
      if (A && B){
          uint64_t t0, first, worst = 0;
          poison = rant_node_create_topic(A, "mw-poison", RANT_PUB_ONLY, NULL, NULL);
          late   = rant_node_create_topic(A, "mw-late", RANT_PUB_ONLY, NULL, NULL);
          t0 = i_rant_plat_now_us();
          (void)rant_topic_send(poison, rant_bytes("x",1), NULL);
          first = i_rant_plat_now_us() - t0;
          CHECK(first >= 100000u && first < 600000u,
                   "matchwait: the first send waits out the window once (%.0f ms)", first/1000.0);
          for (t=0;t<5;t++){
              uint64_t s0 = i_rant_plat_now_us(), dt;
              (void)rant_topic_send(poison, rant_bytes("x",1), NULL);
              dt = i_rant_plat_now_us() - s0;
              if (dt > worst) worst = dt;
              rant_node_poll(A, 2);
          }
          CHECK(worst < 50000u, "matchwait: no later send waits on the unresolved peer (worst %.1f ms)",
                   worst/1000.0);
          CHECK(rant_topic_match_count(poison)==0, "matchwait: the peer never resolved (%d)",
                   rant_topic_match_count(poison));
          /* a subscriber to mw-late appears long after A created it */
          (void)rant_node_create_topic(B, "mw-late", RANT_SUB_ONLY, NULL, NULL);
          { uint64_t end = i_rant_plat_now_us() + 500000u;
            while (i_rant_plat_now_us() < end) rant_node_poll(A, 2); }    /* A hears the candidate */
          t0 = i_rant_plat_now_us();
          (void)rant_topic_send(late, rant_bytes("x",1), NULL);
          first = i_rant_plat_now_us() - t0;
          CHECK(first < 50000u, "matchwait: a late subscriber never blocks a send (%.1f ms)",
                   first/1000.0);
      }
      wire_tap_write(&wire_tap.drop_detail_resp_port, 0);
      if (B) rant_node_stop(B);
      if (A) rant_node_close(A,1);
      if (B) rant_node_close(B,1);
      rant_allocator_reset(&aa); rant_allocator_reset(&ba);
    }

#ifdef RANT_THREADS
    /* (b) FIRST SEND against the forming match: a catch_up 0 publish fired right after
       create_topic must wait for the present subscriber's match, commit and deliver. */
    { RantAllocator aa = rant_allocator_heap(0);
      RantAllocator ba = rant_allocator_heap(0);
      RantNodeOpts o; RantNode *A, *B; RantTopic *at=NULL, *bt=NULL;
      memset(&o,0,sizeof o); o.domain=TEST_DOMAIN+25; o.disable_shm=1; o.discovery.max_peers=4;
      o.net.multicast_interface="127.0.0.1"; o.net.seed_peers=&seed; o.net.n_seed_peers=1;
      mw_recv=0; mw_lost=0; mw_unmatched=0; mw_last[0]=0;
      B = rant_node_open(&ba, "mw-sub", mw_on_message, mw_on_event, &o);
      bt = B ? rant_node_create_topic(B, "mw-first", RANT_SUB_ONLY, NULL,
                   &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE } }) : NULL;
      CHECK(B && bt, "matchwait: first-send sub up");
      if (B) rant_node_start(B);
      A = rant_node_open(&aa, "mw-pub", NULL, mw_on_event, &o);
      at = A ? rant_node_create_topic(A, "mw-first", RANT_PUB_ONLY, NULL,
                   &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE } }) : NULL;
      CHECK(A && at, "matchwait: first-send pub up");
      if (A && B && at && bt){
          int r = rant_topic_send(at, rant_bytes("first", 5), NULL);   /* immediately: the race */
          CHECK(r==RANT_OK, "matchwait: racing first send commits (%d)", r);
          CHECK(rant_topic_match_count(at) > 0,
                   "matchwait: ...after waiting out the match (%d)", rant_topic_match_count(at));
          PUMP_UNTIL(A, NULL, 6000, test_atomic_load(&mw_recv)!=0);
          { long long got = test_atomic_load(&mw_recv);
            CHECK(got==1 && strcmp(mw_last,"first")==0,
                     "matchwait: subscriber got the racing first send (recv=%lld '%s')", got, mw_last); }
          CHECK(test_atomic_load(&mw_unmatched)==0, "matchwait: no timeout diagnostic (%lld)",
                   test_atomic_load(&mw_unmatched));
          /* a topic NOBODY consumes must not stall once the gather has settled: the
             converged state is memoized, so this send early-outs as it always did */
          test_pump(A, NULL, 600);     /* let the post-open gather settle */
          { RantTopic *v = rant_node_create_topic(A, "mw-void", RANT_PUB_ONLY, NULL, NULL);
            uint64_t t0, el;
            CHECK(v!=NULL, "matchwait: void topic up");
            t0 = i_rant_plat_now_us();
            r = v ? rant_topic_send(v, rant_bytes("x",1), NULL) : -1;
            el = i_rant_plat_now_us() - t0;
            CHECK(r==RANT_OK && el < 250000u,
                     "matchwait: no-consumer send returns fast (%d, %luus)", r, (unsigned long)el); }
      }
      if (B) rant_node_stop(B);
      if (A) rant_node_close(A,1);
      if (B) rant_node_close(B,1);
      rant_allocator_reset(&aa); rant_allocator_reset(&ba);
    }
#endif

    /* (c) CONTROL, the wait disabled: the same racing send commits at once to zero
       subscribers and is gone, but RANT_E_UNMATCHED_SEND fires on the way out. */
    { RantAllocator aa = rant_allocator_heap(0);
      RantAllocator ba = rant_allocator_heap(0);
      RantNodeOpts o, ao; RantNode *A, *B; RantTopic *at=NULL, *bt=NULL;
      memset(&o,0,sizeof o); o.domain=TEST_DOMAIN+26; o.disable_shm=1; o.discovery.max_peers=4;
      o.net.multicast_interface="127.0.0.1"; o.net.seed_peers=&seed; o.net.n_seed_peers=1;
      ao=o; ao.match_wait_ms=-1;
      mw_recv=0; mw_unmatched=0;
      B = rant_node_open(&ba, "mw-sub2", mw_on_message, NULL, &o);
      bt = B ? rant_node_create_topic(B, "mw-off", RANT_SUB_ONLY, NULL,
                   &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE } }) : NULL;
      A = rant_node_open(&aa, "mw-off-pub", NULL, mw_on_event, &ao);
      at = A ? rant_node_create_topic(A, "mw-off", RANT_PUB_ONLY, NULL,
                   &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE } }) : NULL;
      CHECK(A && B && at && bt, "matchwait: knob-off pair up");
      if (A && B && at && bt){
          int r = rant_topic_send(at, rant_bytes("gone", 4), NULL);   /* no wait: commits to zero */
          CHECK(r==RANT_OK && test_atomic_load(&mw_unmatched)==1,
                   "matchwait: disabled wait drops LOUDLY (r=%d events=%lld)", r, test_atomic_load(&mw_unmatched));
          PUMP_UNTIL(A, B, 16000, rant_topic_match_count(at)!=0);
          CHECK(rant_topic_match_count(at)==1, "matchwait: the match still forms after (%d)",
                   rant_topic_match_count(at));
          test_pump(A, B, 600);
          CHECK(test_atomic_load(&mw_recv)==0, "matchwait: the dropped send never resurrects (recv=%lld)",
                   test_atomic_load(&mw_recv));
      }
      if (A) rant_node_close(A,1);
      if (B) rant_node_close(B,1);
      rant_allocator_reset(&aa); rant_allocator_reset(&ba);
    }
}

static const TestCase cases[] = {
    { "matchwait", matchwait_checks },
};

TEST_MAIN(cases)
