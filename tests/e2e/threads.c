/* Service threads: reliable senders with no poll, bursts, a capped lane, a hostile
 * socket, the waker, unlocked handlers, reentrancy and stop under load.
 * spec/testing.md says what each case pins. */
#include "support/test.h"

#define TH_SENDERS 3
#define TH_MSGS    1000u

/* the sub's service thread counts, the test reads with test_atomic_load while it runs */
static long long th_recv, th_order_bad, th_lost, th_evicted_evt;
static unsigned long th_next_seq[TH_SENDERS];   /* only the sub's service thread writes */

static void th_on_message(const RantMsg *m){
    if (m->data.len >= 8){
        const uint8_t *p = (const uint8_t*)m->data.data;
        uint32_t tid = (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
        uint32_t seq = (uint32_t)p[4] | ((uint32_t)p[5]<<8) | ((uint32_t)p[6]<<16) | ((uint32_t)p[7]<<24);
        if (tid < TH_SENDERS){
            if (seq != th_next_seq[tid]) test_atomic_add(&th_order_bad, 1);
            th_next_seq[tid] = seq + 1;
        }
        test_atomic_add(&th_recv, 1);
    }
}
static void th_on_event(const RantEvent *ev){
    if (ev->kind == RANT_MSG_LOST) test_atomic_add(&th_lost, (long long)ev->lost_count);
    else if (ev->kind == RANT_ERROR && ev->error == RANT_E_EVICTED_UNSENT) test_atomic_add(&th_evicted_evt, 1);
}

typedef struct { RantTopic *ch; uint32_t id; } th_sender_arg;
static void th_sender(void *arg){
    th_sender_arg *a = (th_sender_arg*)arg;
    uint8_t buf[8]; uint32_t i;
    for (i=0;i<TH_MSGS;i++){
        buf[0]=(uint8_t)a->id; buf[1]=(uint8_t)(a->id>>8); buf[2]=(uint8_t)(a->id>>16); buf[3]=(uint8_t)(a->id>>24);
        buf[4]=(uint8_t)i; buf[5]=(uint8_t)(i>>8); buf[6]=(uint8_t)(i>>16); buf[7]=(uint8_t)(i>>24);
        rant_topic_send(a->ch, rant_bytes(buf, sizeof buf), NULL);
    }
}

/* REENTRANT state: node B echoes every request onto its reply topic from the
 * callback, and probes the forbidden reentrant calls exactly once */
static RantNode *th_echo_b;
static RantTopic *th_echo_rep;
static long long th_echo_replies;
static volatile int th_cb_send_rc = -100, th_cb_create_refused = -1, th_cb_setrole_rc = -100;
static void th_echo_b_on_msg(const RantMsg *m){
    th_cb_send_rc = rant_topic_send(th_echo_rep, m->data, NULL);
    if (th_cb_create_refused < 0){
        th_cb_create_refused = (rant_node_create_topic(th_echo_b, "th/na", RANT_PUBSUB, NULL, NULL) == NULL);
        th_cb_setrole_rc = rant_topic_set_role(th_echo_rep, RANT_PUB_ONLY);
    }
}
static void th_echo_a_on_msg(const RantMsg *m){ (void)m; test_atomic_add(&th_echo_replies, 1); }

/* STOP UNDER LOAD: hammer a reliable topic until told to stop */
static long long th_hammer_stop;
/* a slow inline handler: 30 ms of work, plus a create from inside, which stays refused */
static volatile unsigned long th_slow_runs, th_echo_recv; static volatile int th_slow_create_refused;
/* A handler holds while th_hold is set, up to a second, so a send on another thread that
   waited for it would take that second: a cause, not a race against a CI runner's clock. */
static volatile int th_hold, th_in_handler;
static void th_hold_here(void){
    uint64_t end = i_rant_plat_now_us() + 1000000u;
    th_in_handler = 1;
    while (th_hold && i_rant_plat_now_us() < end) test_sleep_ms(1);
    test_sleep_ms(30);
    th_in_handler = 0;
}
/* Holds a handler, measures sends on w meanwhile, then lets it go. 0 when no handler came. */
static uint64_t th_send_while_held(RantNode *w, int topic, void (*trigger)(void *), void *arg){
    uint8_t payload[16]; uint64_t worst = 0, end; int k;
    memset(payload, 0, sizeof payload);
    th_hold = 1;
    trigger(arg);
    end = i_rant_plat_now_us() + 2000000u;
    while (!th_in_handler && i_rant_plat_now_us() < end) test_sleep_ms(1);
    if (!th_in_handler){ th_hold = 0; return 0; }
    for (k=0;k<5;k++){
        uint64_t t0 = i_rant_plat_now_us(), dt;
        test_send(w, topic, payload, sizeof payload);
        dt = i_rant_plat_now_us() - t0;
        if (dt > worst) worst = dt;
    }
    th_hold = 0;
    end = i_rant_plat_now_us() + 2000000u;
    while (th_in_handler && i_rant_plat_now_us() < end) test_sleep_ms(1);
    return worst ? worst : 1;
}
static void th_slow_on_message(const RantMsg *m){
    uint8_t b[8]; int k;
    th_slow_runs++;
    if (!rant_node_create_topic(m->node, "th/inside", RANT_PUB_ONLY, NULL, NULL)) th_slow_create_refused = 1;
    /* a burst from inside the handler on a depth 1 topic: each send must leave before the next */
    for (k=0;k<8;k++){ memset(b, k, sizeof b); test_send(m->node, 2, b, sizeof b); }
    th_hold_here();
}
static void th_echo_on_message(const RantMsg *m){ if (m->topic_index == 2) th_echo_recv++; }
/* a slow function handler on the service thread, and a caller thread driving it */
static volatile unsigned long th_fn_runs, th_fn_ok;
static void th_slow_on_request(RantRequest *rq, void *user){
    (void)user; th_fn_runs++; th_hold_here(); rant_request_reply(rq, rant_bytes("ok", 2));
}
/* one call per go, from a thread of its own, since a call waits for its reply */
static volatile int th_call_go, th_call_quit;
static void th_caller(void *arg){
    while (!th_call_quit){
        RantResponse rsp;
        if (!th_call_go){ test_sleep_ms(1); continue; }
        th_call_go = 0;
        if (rant_function_call((RantFunction*)arg, rant_bytes("q",1), &rsp, 3000, NULL) == 1
            && rsp.status == RANT_CALL_OK) th_fn_ok++;
    }
}
static void th_trigger_call(void *arg){ (void)arg; th_call_go = 1; }
static void th_trigger_slow(void *arg){
    uint8_t payload[16]; memset(payload, 0, sizeof payload);
    test_send((RantNode*)arg, 1, payload, sizeof payload);
}
/* a create from a third thread while the handler is out: it waits, then succeeds */
static volatile RantTopic *th_late_topic; static volatile uint64_t th_late_us;
static void th_late_creator(void *arg){
    uint64_t t0;
    test_sleep_ms(215);                 /* lands inside a handler run */
    t0 = i_rant_plat_now_us();
    th_late_topic = rant_node_create_topic((RantNode*)arg, "th/late", RANT_PUB_ONLY, NULL, NULL);
    th_late_us = i_rant_plat_now_us() - t0;
}
static void th_hammer(void *arg){
    uint8_t buf[8]; memset(buf, 0x77, sizeof buf);
    while (!test_atomic_load(&th_hammer_stop)) rant_topic_send((RantTopic*)arg, rant_bytes(buf, sizeof buf), NULL);
}

static void threaded_checks(void){
    uint8_t payload[16]; unsigned i;
    memset(payload, 0x33, sizeof payload);

    /* RELIABLE: exactly-once ordered delivery under multithreaded send */
    { i_RantTopicDef cd[1]; RantNodeOpts o; RantNode *w, *r;
      memset(cd, 0, sizeof cd);
      cd[0].name = "th/rel"; cd[0].role = RANT_PUB_ONLY;
      cd[0].qos.reliability = RANT_RELIABLE; cd[0].qos.keep_last = 8;
      cd[0].qos.max_message_bytes = 32; cd[0].qos.heartbeat_us = 50000;
      cd[0].qos.backpressure_wait_us = 500000;
      memset(&o, 0, sizeof o);
      o.domain = TEST_DOMAIN+4; o.disable_shm = 1; o.discovery.max_peers = 4; o.net.multicast_interface="127.0.0.1";
      w = test_node_open("th-pub", NULL, th_on_event, o, cd, 1);
      cd[0].role = RANT_SUB_ONLY;
      r = test_node_open("th-sub", th_on_message, th_on_event, o, cd, 1);
      CHECK(w && r, "threaded: nodes open");
      if (w && r){
          th_recv = th_order_bad = th_lost = th_evicted_evt = 0;
          memset(th_next_seq, 0, sizeof th_next_seq);
          /* the starts have side effects, so they stay out of CHECK */
          { int s1 = rant_node_start(w), s2 = rant_node_start(r), again, poll_rc;
            again = rant_node_start(w);
            poll_rc = rant_node_poll(w, 0);
            CHECK(s1 == RANT_OK && s2 == RANT_OK, "threaded: service threads start");
            CHECK(again == RANT_ERR_STATE, "threaded: double start refused");
            CHECK(poll_rc == RANT_ERR_STATE, "threaded: foreign poll refused while started");
            CHECK(rant_node_is_started(w) == 1, "threaded: is_started");
          }
          /* the service threads own discovery + matching: no polling from here on */
          { uint64_t end = i_rant_plat_now_us() + 5000000u;
            while (test_match_count(w, 0) == 0 && i_rant_plat_now_us() < end)
                test_sleep_ms(5); }
          CHECK(test_match_count(w, 0) == 1, "threaded: match formed by the services");
          {   i_RantThread th[TH_SENDERS]; th_sender_arg ta[TH_SENDERS]; uint32_t t;
              for (t=0;t<TH_SENDERS;t++){
                  ta[t].ch = rant_node_topic(w, 0); ta[t].id = t;
                  i_rant_plat_thread_start(&th[t], th_sender, &ta[t]);
              }
              for (t=0;t<TH_SENDERS;t++) i_rant_plat_thread_join(&th[t]);
          }
          { int drained = test_drain(w, 0, 10000);
            CHECK(drained == 1, "threaded: drain completes"); }
          { uint64_t end = i_rant_plat_now_us() + 3000000u;      /* delivered before acked, settle */
            while (test_atomic_load(&th_recv) < (long long)TH_SENDERS*TH_MSGS && i_rant_plat_now_us() < end)
                test_sleep_ms(5); }
          rant_node_stop(r); rant_node_stop(w);                    /* join: counters now settled */
          CHECK(th_recv == (long long)TH_SENDERS*TH_MSGS,
                   "threaded: exactly-once delivery (%lld, want %lld)",
                   th_recv, (long long)TH_SENDERS*TH_MSGS);
          CHECK(th_order_bad == 0, "threaded: per-thread order kept (bad=%lld)", th_order_bad);
          CHECK(th_lost == 0, "threaded: no loss (lost=%lld)", th_lost);
          CHECK(th_evicted_evt == 0 && rant_node_evicted_unsent(w) == 0,
                   "threaded: no unsent eviction (evt=%lld cnt=%u)",
                   th_evicted_evt, rant_node_evicted_unsent(w));
          rant_node_close(r, 1); rant_node_close(w, 1);
      }
    }

    /* BURST, HOSTILE and WAKER on one best effort pair */
    { i_RantTopicDef cd[1]; RantNodeOpts o; RantNode *w, *r;
      memset(cd, 0, sizeof cd);
      cd[0].name = "th/burst"; cd[0].role = RANT_PUB_ONLY;
      cd[0].qos.keep_last = 4;   /* best-effort */
      cd[0].qos.max_message_bytes = 32; cd[0].qos.heartbeat_us = 50000;
      memset(&o, 0, sizeof o);
      o.domain = TEST_DOMAIN+5; o.disable_shm = 1; o.discovery.max_peers = 4; o.net.multicast_interface="127.0.0.1";
      w = test_node_open("th-bpub", NULL, th_on_event, o, cd, 1);
      cd[0].role = RANT_SUB_ONLY;
      r = test_node_open("th-bsub", th_on_message, th_on_event, o, cd, 1);
      CHECK(w && r, "burst: nodes open");
      if (w && r){
          th_recv = th_lost = th_evicted_evt = 0;
          rant_node_start(w); rant_node_start(r);
          { uint64_t end = i_rant_plat_now_us() + 5000000u;
            while (test_match_count(w, 0) == 0 && i_rant_plat_now_us() < end)
                test_sleep_ms(5); }

          /* 64 back-to-back sends, keep_last 4: every one must reach the wire, since
             each send transmits before it returns */
          for (i=0;i<64;i++) test_send(w, 0, payload, sizeof payload);
          { uint64_t end = i_rant_plat_now_us() + 3000000u;
            while (test_atomic_load(&th_recv) < 64 && i_rant_plat_now_us() < end) test_sleep_ms(5); }
          { long long got = test_atomic_load(&th_recv);
            CHECK(got == 64, "burst: all 64 delivered past a depth-4 ring (%lld)", got); }
          CHECK(rant_node_evicted_unsent(w) == 0, "burst: nothing evicted unsent (%u)",
                   rant_node_evicted_unsent(w));

          /* WAKER: quiesce, then one send must land well inside the announce capped sleep.
             Typical is sub ms, the margin absorbs a loaded CI box losing the CPU. */
          test_sleep_ms(300);
          { long long r0 = test_atomic_load(&th_recv); uint64_t t0 = i_rant_plat_now_us(), dt;
            test_send(w, 0, payload, sizeof payload);
            while (test_atomic_load(&th_recv) == r0 && i_rant_plat_now_us() - t0 < 1000000u) { /* spin */ }
            dt = i_rant_plat_now_us() - t0;
            CHECK(test_atomic_load(&th_recv) == r0+1 && dt < 400000u,
                     "waker: idle-node send delivered in %.1f ms", dt/1000.0);
          }

          /* HOSTILE: transport TX forced to would block. The first pass parks one datagram in
             tx_hold, past that the burst overwrites unsent history, which must surface as evicted. */
          { long long r0 = test_atomic_load(&th_recv), got; uint32_t e0 = rant_node_evicted_unsent(w);
            uint32_t evicted;
            wire_tap_write(&wire_tap.block_data, 1);
            for (i=0;i<64;i++) test_send(w, 0, payload, sizeof payload);
            evicted = rant_node_evicted_unsent(w) - e0;
            CHECK(evicted >= 1, "hostile: blocked TX surfaces RANT_E_EVICTED_UNSENT (%u)", evicted);
            wire_tap_write(&wire_tap.block_data, 0);
            /* the service retries the held datagram and drains the ring on its next pass, which
               the announce cadence bounds */
            { uint64_t end = i_rant_plat_now_us() + 3000000u;
              while (test_atomic_load(&th_recv) - r0 + evicted < 64 && i_rant_plat_now_us() < end)
                  test_sleep_ms(10); }
            got = test_atomic_load(&th_recv) - r0;
            CHECK(got + evicted == 64,
                     "hostile: every send delivered or accounted (recv=%lld evicted=%u)", got, evicted);
          }
          /* lock/unlock smoke: bracket a peer-view read while the services run */
          { uint16_t cnt = 0;
            rant_node_lock(w);
            (void)test_peers(w, &cnt);
            rant_node_unlock(w);
            CHECK(cnt >= 1, "lock: bracketed peer view reads (%u peers)", cnt);
          }
          rant_node_close(r, 1); rant_node_close(w, 1);
      }
    }

    /* CAPPED: a rate capped subscriber holds samples back on its lane, which must never
       slow a depth 1 publisher. A send that waited for the lane's tick would take up to
       200 ms here, far past a busy runner's stall. */
    { i_RantTopicDef cd[1]; RantNodeOpts o; RantNode *w, *r; uint8_t payload[16];
      memset(payload, 0, sizeof payload);
      memset(cd, 0, sizeof cd);
      cd[0].name = "th/capped"; cd[0].role = RANT_PUB_ONLY;
      cd[0].qos.keep_last = 1; cd[0].qos.max_message_bytes = 32;
      memset(&o, 0, sizeof o);
      o.domain = TEST_DOMAIN+12; o.disable_shm = 1; o.discovery.max_peers = 4; o.net.multicast_interface="127.0.0.1";
      w = test_node_open("th-cpub", NULL, th_on_event, o, cd, 1);
      cd[0].role = RANT_SUB_ONLY; cd[0].qos.max_rate_hz = 5;
      r = test_node_open("th-csub", th_on_message, th_on_event, o, cd, 1);
      CHECK(w && r, "capped: nodes open");
      if (w && r){
          uint64_t worst = 0, began; double secs; long long most, got; int i;
          th_recv = 0;
          rant_node_start(w); rant_node_start(r);
          { uint64_t end = i_rant_plat_now_us() + 5000000u;
            while (test_match_count(w, 0) == 0 && i_rant_plat_now_us() < end)
                test_sleep_ms(5); }
          test_sleep_ms(300);                     /* the rate section rides an announce */
          began = i_rant_plat_now_us();
          for (i=0;i<600;i++){
              uint64_t t0 = i_rant_plat_now_us(), dt;
              test_send(w, 0, payload, sizeof payload);
              dt = i_rant_plat_now_us() - t0;
              if (dt > worst) worst = dt;
              while (i_rant_plat_now_us() - t0 < 1000u) {}   /* a sleep can last a 15.6 ms tick */
          }
          /* the cap is judged over the real span, a busy publisher may still send slower */
          secs = (double)(i_rant_plat_now_us() - began) / 1e6;
          most = (long long)(5.0 * secs * 1.25) + 2;
          CHECK(worst < 100000u, "capped: no send waits for the capped lane (worst %.2f ms)",
                   worst/1000.0);
          got = test_atomic_load(&th_recv);
          CHECK(got >= 2 && got <= most && got < 600,
                   "capped: the subscriber is still paced (%lld of 600 in %.0f ms, at most %lld)",
                   got, secs * 1000.0, most);
          CHECK(rant_node_evicted_unsent(w) == 0, "capped: a held back sample is not an eviction (%u)",
                   rant_node_evicted_unsent(w));
          /* a lone held back sample leaves at the lane's 200 ms tick: the send that armed the
             tick must wake the service thread, which otherwise sleeps out its older plan of
             seconds */
          { uint64_t worst_gap = 0; int pair;
            for (pair=0;pair<3;pair++){
                long long r0; uint64_t t0, dt;
                test_sleep_ms(600);     /* past the lane's last tick, so the first leaves at once */
                r0 = test_atomic_load(&th_recv);
                test_send(w, 0, payload, sizeof payload); test_sleep_ms(1);
                test_send(w, 0, payload, sizeof payload);
                t0 = i_rant_plat_now_us();
                while (test_atomic_load(&th_recv) < r0 + 2 && i_rant_plat_now_us() - t0 < 1000000u) test_sleep_ms(1);
                dt = i_rant_plat_now_us() - t0;
                if (test_atomic_load(&th_recv) < r0 + 2) dt = 1000000u;
                if (dt > worst_gap) worst_gap = dt;
            }
            CHECK(worst_gap < 1000000u, "capped: a held back sample leaves at its tick (worst %.0f ms)",
                     worst_gap/1000.0); }
          rant_node_close(r, 1); rant_node_close(w, 1);
      }
    }

    /* UNLOCKED HANDLER: an inline handler held on the service thread must not delay a send
       on another thread of the same node. A create from a third thread waits for the
       handler and succeeds, a create from inside the handler stays refused. */
    { i_RantTopicDef cw[3], cr[3]; RantNodeOpts o; RantNode *w, *r; uint8_t payload[16];
      memset(payload, 0, sizeof payload);
      memset(cw, 0, sizeof cw);
      cw[0].name = "th/fast"; cw[0].role = RANT_PUB_ONLY; cw[0].qos.keep_last = 1; cw[0].qos.max_message_bytes = 32;
      cw[1].name = "th/slow"; cw[1].role = RANT_SUB_ONLY; cw[1].qos.keep_last = 4; cw[1].qos.max_message_bytes = 32;
      cw[2].name = "th/echo"; cw[2].role = RANT_PUB_ONLY; cw[2].qos.keep_last = 1; cw[2].qos.max_message_bytes = 32;
      memcpy(cr, cw, sizeof cw); cr[0].role = RANT_SUB_ONLY; cr[1].role = RANT_PUB_ONLY; cr[2].role = RANT_SUB_ONLY;
      memset(&o, 0, sizeof o);
      o.domain = TEST_DOMAIN+14; o.disable_shm = 1; o.discovery.max_peers = 4; o.max_topics = 8; o.net.multicast_interface="127.0.0.1";
      w = test_node_open("th-upub", th_slow_on_message, th_on_event, o, cw, 3);
      r = test_node_open("th-usub", th_echo_on_message, th_on_event, o, cr, 3);
      CHECK(w && r, "unlocked: nodes open");
      if (w && r){
          i_RantThread creator; uint64_t worst = 0, end; int held;
          th_slow_runs = 0; th_echo_recv = 0; th_slow_create_refused = 0; th_late_topic = NULL; th_late_us = 0;
          rant_node_start(w); rant_node_start(r);
          end = i_rant_plat_now_us() + 5000000u;
          while ((test_match_count(w, 0) == 0 || test_match_count(r, 1) == 0
                  || test_match_count(w, 2) == 0) && i_rant_plat_now_us() < end) test_sleep_ms(5);
          i_rant_plat_thread_start(&creator, th_late_creator, w);
          for (held = 0; held < 10; held++){
              uint64_t dt = th_send_while_held(w, 0, th_trigger_slow, r);
              if (!dt) break;
              if (dt > worst) worst = dt;
          }
          i_rant_plat_thread_join(&creator);
          CHECK(held == 10, "unlocked: the slow handler ran (%d held of 10, %lu runs)", held, th_slow_runs);
          CHECK(worst < 500000u, "unlocked: no send waited for the handler (worst %.1f ms)", worst/1000.0);
          CHECK(th_slow_create_refused, "unlocked: a create from inside the handler is refused");
          CHECK(th_late_topic != NULL, "unlocked: a create from a third thread succeeds (%.1f ms)",
                   th_late_us/1000.0);
          /* the same for a pattern handler: a 30 ms function body on w, called from a thread
             on r, while w keeps sending at 1 kHz from this thread */
          { RantFunction *def = rant_node_create_function_definition(w, "th/fn", NULL, NULL, th_slow_on_request, NULL, NULL);
            RantFunction *rem = rant_node_create_remote_function(r, "th/fn", NULL, NULL, NULL);
            i_RantThread caller; uint64_t fworst = 0;
            th_fn_runs = 0; th_fn_ok = 0;
            end = i_rant_plat_now_us() + 5000000u;
            while (def && rem && rant_function_match_count(rem) == 0 && i_rant_plat_now_us() < end) test_sleep_ms(5);
            CHECK(def && rem && rant_function_match_count(rem) == 1, "unlocked: function pair matched");
            th_call_go = 0; th_call_quit = 0;
            i_rant_plat_thread_start(&caller, th_caller, rem);
            for (held = 0; held < 10; held++){
                uint64_t dt = th_send_while_held(w, 0, th_trigger_call, NULL);
                if (!dt) break;
                if (dt > fworst) fworst = dt;
            }
            end = i_rant_plat_now_us() + 3000000u;
            while (th_fn_ok < (unsigned long)held && i_rant_plat_now_us() < end) test_sleep_ms(1);
            th_call_quit = 1;
            i_rant_plat_thread_join(&caller);
            CHECK(held == 10 && th_fn_ok == 10 && th_fn_runs == 10, "unlocked: ten calls answered through the slow handler (%lu ok, %lu runs)",
                     th_fn_ok, th_fn_runs);
            CHECK(fworst < 500000u, "unlocked: no send waited for the function handler (worst %.1f ms)", fworst/1000.0);
            if (rem) rant_function_retire(rem);
            if (def) rant_function_retire(def); }
          test_sleep_ms(100);
          CHECK(th_echo_recv == th_slow_runs * 8 && rant_node_evicted_unsent(w) == 0,
                   "unlocked: a burst from inside the handler all left on a depth 1 topic (%lu of %lu, evicted %u)",
                   th_echo_recv, th_slow_runs * 8, rant_node_evicted_unsent(w));
          rant_node_close(r, 1); rant_node_close(w, 1);
      }
    }

    /* REENTRANT: echo from the callback, forbidden calls refuse loudly. The ring must be
       deeper than the request burst since a reentrant send never waits for a TX pass. */
    { i_RantTopicDef ca[2], cb[2]; RantNodeOpts o; RantNode *a, *b;
      memset(ca, 0, sizeof ca);
      ca[0].name = "th/req"; ca[0].role = RANT_PUB_ONLY;
      ca[0].qos.reliability = RANT_RELIABLE; ca[0].qos.keep_last = 16;
      ca[0].qos.max_message_bytes = 32; ca[0].qos.heartbeat_us = 50000;
      ca[1] = ca[0]; ca[1].name = "th/rep"; ca[1].role = RANT_SUB_ONLY;
      memcpy(cb, ca, sizeof ca);
      cb[0].role = RANT_SUB_ONLY; cb[1].role = RANT_PUB_ONLY;
      memset(&o, 0, sizeof o);
      o.domain = TEST_DOMAIN+6; o.disable_shm = 1; o.discovery.max_peers = 4; o.net.multicast_interface="127.0.0.1";
      a = test_node_open("th-echo-a", th_echo_a_on_msg, NULL, o, ca, 2);
      b = test_node_open("th-echo-b", th_echo_b_on_msg, NULL, o, cb, 2);
      CHECK(a && b, "reentrant: nodes open");
      if (a && b){
          th_echo_b = b; th_echo_rep = rant_node_topic(b, 1);
          th_echo_replies = 0; th_cb_send_rc = -100; th_cb_create_refused = -1; th_cb_setrole_rc = -100;
          rant_node_start(a); rant_node_start(b);
          { uint64_t end = i_rant_plat_now_us() + 5000000u;
            while ((test_match_count(a, 0) == 0 || test_match_count(b, 1) == 0)
                   && i_rant_plat_now_us() < end)
                test_sleep_ms(5); }
          for (i=0;i<10;i++) test_send(a, 0, payload, sizeof payload);
          { uint64_t end = i_rant_plat_now_us() + 3000000u;
            while (test_atomic_load(&th_echo_replies) < 10 && i_rant_plat_now_us() < end) test_sleep_ms(5); }
          rant_node_stop(b); rant_node_stop(a);
          CHECK(th_echo_replies == 10, "reentrant: callback send echoes (%lld/10)", th_echo_replies);
          CHECK(th_cb_send_rc == RANT_OK, "reentrant: callback send returns RANT_OK (%d)", th_cb_send_rc);
          CHECK(th_cb_create_refused == 1, "reentrant: callback create_topic refused");
          CHECK(th_cb_setrole_rc == RANT_ERR_STATE, "reentrant: callback set_role refused (%d)", th_cb_setrole_rc);
          rant_node_close(b, 1); rant_node_close(a, 1);
      }
    }

    /* STOP-UNDER-LOAD: hammer threads parked in the backpressure wait while stop
       broadcasts them loose, repeated under a watchdog. A hang here is the deadlock detector. */
    { uint64_t phase_t0 = i_rant_plat_now_us(); int iter;
      for (iter=0; iter<3; iter++){
          i_RantTopicDef cd[1]; RantNodeOpts o; RantNode *w, *r;
          i_RantThread h1, h2;
          memset(cd, 0, sizeof cd);
          cd[0].name = "th/stop"; cd[0].role = RANT_PUB_ONLY;
          cd[0].qos.reliability = RANT_RELIABLE; cd[0].qos.keep_last = 4;
          cd[0].qos.max_message_bytes = 32; cd[0].qos.heartbeat_us = 50000;
          cd[0].qos.backpressure_wait_us = 300000;
          memset(&o, 0, sizeof o);
          o.domain = (uint16_t)(TEST_DOMAIN+7+iter); o.disable_shm = 1; o.discovery.max_peers = 4; o.net.multicast_interface="127.0.0.1";
          w = test_node_open("th-hpub", NULL, NULL, o, cd, 1);
          cd[0].role = RANT_SUB_ONLY;
          r = test_node_open("th-hsub", NULL, NULL, o, cd, 1);
          if (!w || !r){ CHECK(0, "stop-load: nodes open (iter %d)", iter); break; }
          { uint64_t end = i_rant_plat_now_us() + 5000000u;     /* match, then silence the reader */
            while (test_match_count(w, 0) == 0 && i_rant_plat_now_us() < end)
                test_pump(w, r, 10); }
          rant_node_start(w);                      /* reader stays unpolled: never acks */
          test_atomic_store(&th_hammer_stop, 0);
          i_rant_plat_thread_start(&h1, th_hammer, rant_node_topic(w, 0));
          i_rant_plat_thread_start(&h2, th_hammer, rant_node_topic(w, 0));
          test_sleep_ms(50);                         /* hammers now parked in the wait */
          rant_node_stop(w);                       /* broadcasts the waiters loose */
          test_atomic_store(&th_hammer_stop, 1);
          i_rant_plat_thread_join(&h1);
          i_rant_plat_thread_join(&h2);
          rant_node_close(r, 0);
          rant_node_close(w, 0);
      }
      CHECK(i_rant_plat_now_us() - phase_t0 < 30000000u,
               "stop-load: 3 stop-under-fire cycles, no hang (%.1f s)",
               (i_rant_plat_now_us() - phase_t0)/1e6);
    }
}

static const TestCase cases[] = {
    { "threads", threaded_checks },
};

TEST_MAIN(cases)
