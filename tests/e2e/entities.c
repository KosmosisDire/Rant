/* Entities over time: the reflection walk across dropped peers, a retire and reuse churn
 * soak, and the builtin log and meta topics. spec/testing.md says what each case pins. */
#include "support/test.h"

/* The dropped peer reflection gate: the entity walk must refuse dropped peers
 * unless include_dropped is set, or observers grow ghost entities after a restart. */
static void reflect_dropped_checks(void){
    RantAllocator aa = rant_allocator_heap(0);
    RantAllocator ba = rant_allocator_heap(0);
    RantNodeOpts ao, bo; RantNode *A, *B; RantAddr seed;
    RantVariable *def; uint32_t pid = 0; int t;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&ao,0,sizeof ao); ao.domain=TEST_DOMAIN+23; ao.discovery.max_peers=4;
    ao.net.multicast_interface="127.0.0.1"; ao.net.seed_peers=&seed; ao.net.n_seed_peers=1;
    ao.discovery.announce_interval_us = 100000;   /* fast cadence so the drop is quick */
    ao.discovery.peer_timeout_us      = 500000;
    bo=ao;
    bo.fetch_details = 1;   /* B is the observer: names + schemas in the detail cache */
    A = rant_node_open(&aa, "ghost-host", NULL, NULL, &ao);
    B = rant_node_open(&ba, "observer",   NULL, NULL, &bo);
    CHECK(A && B, "ghost: nodes open");
    if (!(A && B)){ if(A)rant_node_close(A,0); if(B)rant_node_close(B,0); return; }

    def = rant_node_create_variable_definition(A, "gdial", NULL, NULL);
    CHECK(def != NULL, "ghost: variable definition created");
    { int ents = 0, unnamed = 1;
      /* an entity shows by its hash before its details name it, so wait for every name */
      for (t=0;t<2000 && (!ents || unnamed);t++){
          RantIter eit; RantEntityInfo ei; const i_RantDiscoveryPeerView *ps; uint16_t pc;
          test_pump(A,B,2);
          ps = test_peers(B, &pc);
          if (!(ps && pc)) continue;
          pid = ps[0].id;
          ents = unnamed = 0;
          memset(&eit,0,sizeof eit);
          while (rant_node_entities_next(B, pid, &eit, &ei)){
              ents += !test_builtin(&ei);
              unnamed += !ei.name.len;
          }
      }
      CHECK(ents == 1, "ghost: live peer enumerates its entity (%d)", ents); }

    rant_node_close(A, 0);     /* silent death, no BYE: B must DROP (not forget) the peer */
    { uint64_t end = i_rant_plat_now_us() + 1200000u;     /* > peer_timeout */
      while (i_rant_plat_now_us() < end) rant_node_poll(B, 5); }
    { const i_RantDiscoveryPeerView *ps; uint16_t pc, i; int dropped = 0;
      ps = test_peers(B, &pc);
      for (i=0; ps && i<pc; i++)
          if (ps[i].id == pid && ps[i].liveness == RANT_PEER_DROPPED) dropped = 1;
      CHECK(dropped, "ghost: peer is DROPPED yet still listed (by design)"); }
    { RantIter eit; RantEntityInfo ei; int ents = 0, mesh = 0;
      memset(&eit,0,sizeof eit);
      while (rant_node_entities_next(B, pid, &eit, &ei)) ents += !test_builtin(&ei);
      CHECK(ents == 1, "ghost: the per-node walk serves the dropped peer's last view (%d)", ents);
      memset(&eit,0,sizeof eit);
      while (rant_node_mesh_next(B, &eit, &ei))
          if (ei.name.len==5 && !memcmp(ei.name.data,"gdial",5)) mesh++;
      CHECK(mesh == 0, "ghost: the mesh walk never counts a dropped peer (%d)", mesh); }

    rant_node_close(B,0);
    rant_allocator_reset(&aa); rant_allocator_reset(&ba);
}

/* The retire and reuse churn soak: retire and create cycles reuse slots, so
 * nothing grows over sustained churn (spec/testing.md lists the case matrix). */
static volatile unsigned long ch_c_recv, ch_u_recv, ch_t_recv, ch_c_mismatch;
static volatile uint32_t ch_c_last;
static uint32_t ch_seq;
static void ch_on_message(const RantMsg *m){
    const char *who = (const char *)m->user;
    if (!who || m->data.len < 4) return;
    if      (who[0]=='C'){ ch_c_recv++; ch_c_last = i_rant_le_r32(m->data.data); }
    else if (who[0]=='U') ch_u_recv++;
    else if (who[0]=='T') ch_t_recv++;
}
static void ch_on_event(const RantEvent *ev){
    if (ev->kind == RANT_ERROR && ev->error == RANT_E_SCHEMA_MISMATCH
        && ev->user && ((const char*)ev->user)[0]=='C') ch_c_mismatch++;
}
static void ch_pump(RantNode **ns, int n, int ms){
    uint64_t end = i_rant_plat_now_us() + (uint64_t)ms*1000u;
    while (i_rant_plat_now_us() < end){
        int i;
        for (i=0;i<n;i++) if (ns[i]) rant_node_poll(ns[i], 1);
    }
}
static void ch_send(RantTopic *beat, int nbytes){
    uint8_t pb[8];
    memset(pb, 0, sizeof pb);
    i_rant_le_w32(pb, ++ch_seq);
    rant_topic_send(beat, rant_bytes(pb, (size_t)nbytes), NULL);
}

static void churn_checks(void){
    RantAllocator ma = rant_allocator_heap(0);
    RantAllocator pa = rant_allocator_heap(0);
    RantAllocator ca = rant_allocator_heap(0);
    RantAllocator ua = rant_allocator_heap(0);
    RantNodeOpts po, co2, uo; RantNode *P, *C, *U; RantAddr seed;
    RantSchema *G1, *G2, *G3;
    RantTopic *beat, *csub; RantTopicOpts topts;
    RantNode *nodes[4];
    uint16_t base_idx, c_idx, p_hi0=0, c_hi0=0;
    uint32_t p_int0=0, c_int0=0;
    size_t p_mem0=0, c_mem0=0, u_mem0=0, mem=0, peak=0; uint64_t calls=0;
    int t, cyc, idx_ok=1;

    ch_c_recv = ch_u_recv = ch_t_recv = ch_c_mismatch = 0;
    ch_c_last = 0; ch_seq = 100;
    G1 = test_compile(&ma, "Beat { v: u32 }", NULL);
    G2 = test_compile(&ma, "Beat { w: u32 }", NULL);
    G3 = test_compile(&ma, "Beat { v: u32, extra: u32 }", NULL);
    CHECK(G1 && G2 && G3, "churn: schemas compile");
    if (!(G1 && G2 && G3)){ rant_allocator_reset(&ma); return; }

    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=TEST_DOMAIN+33; po.discovery.max_peers=6;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    po.discovery.announce_interval_us = 100000;   /* fast cadence: rebinds re-verify quickly */
    co2=po; uo=po;
    po.user_data=(void*)"P"; co2.user_data=(void*)"C"; uo.user_data=(void*)"U";
    P = rant_node_open(&pa, "churn-pub",     NULL,          ch_on_event, &po);
    C = rant_node_open(&ca, "churn-typed",   ch_on_message, ch_on_event, &co2);
    U = rant_node_open(&ua, "churn-untyped", ch_on_message, ch_on_event, &uo);
    CHECK(P && C && U, "churn: nodes open");
    if (!(P && C && U)){
        if(P)rant_node_close(P,0); if(C)rant_node_close(C,0); if(U)rant_node_close(U,0);
        rant_allocator_reset(&pa); rant_allocator_reset(&ca); rant_allocator_reset(&ua);
        rant_allocator_reset(&ma); return;
    }
    nodes[0]=P; nodes[1]=C; nodes[2]=U; nodes[3]=NULL;

    memset(&topts,0,sizeof topts);
    topts.qos.reliability = RANT_RELIABLE; topts.qos.catch_up = 1;
    beat = rant_node_create_topic(P, "beat", RANT_PUB_ONLY, G1, &topts);
    csub = rant_node_create_topic(C, "beat", RANT_SUB_ONLY, G1, &topts);
    (void)rant_node_create_topic(U, "beat", RANT_SUB_ONLY, NULL, &topts);
    CHECK(beat && csub, "churn: survivor topics created");
    base_idx = rant_topic_index(beat); c_idx = rant_topic_index(csub);

    /* warmup: first delivery, then one identical retire/create cycle so every lazy
       allocation (lane pool, detail buffers, peer maps) is in the baseline */
    ch_send(beat, 4);
    for (t=0;t<4000 && (ch_c_last != ch_seq || ch_u_recv == 0);t++) ch_pump(nodes,3,2);
    CHECK(ch_c_last == ch_seq && ch_u_recv > 0, "churn: warmup delivery (typed + untyped)");
    CHECK(rant_topic_retire(beat) == RANT_OK, "churn: warmup retire");
    beat = rant_node_create_topic(P, "beat", RANT_PUB_ONLY, G1, &topts);
    CHECK(beat && rant_topic_index(beat) == base_idx,
             "churn: identical re-create REUSES the slot (idx %u)", base_idx);
    ch_send(beat, 4);
    for (t=0;t<4000 && ch_c_last != ch_seq;t++) ch_pump(nodes,3,2);
    CHECK(ch_c_last == ch_seq, "churn: delivery across the warmup cycle");
    {   /* saturate the @rant/log/error keep_last ring, which mirrors every retype refusal: its
           16 slots fill once and plateau, so the baselines must include them at full size */
        char big[200]; int k;
        memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0;
        for (k=0;k<20;k++){
            rant_node_log(P, RANT_LOG_ERROR, "%s", big);
            rant_node_log(C, RANT_LOG_ERROR, "%s", big);
            rant_node_log(U, RANT_LOG_ERROR, "%s", big);
        }
    }
    rant_node_mem_stats(P, &p_mem0, &peak, &calls);
    rant_node_mem_stats(C, &c_mem0, &peak, &calls);
    rant_node_mem_stats(U, &u_mem0, &peak, &calls);
    p_hi0 = i_rant_node_topic_count(P); c_hi0 = i_rant_node_topic_count(C);
    p_int0 = i_rant_transport_interest_size(P->transport);
    c_int0 = i_rant_transport_interest_size(C->transport);

    /* A: identical binding churn. Every cycle must reuse the slot, relink with no round
       trip, and deliver. Nothing may grow. */
    { int deliver_ok = 1;
      for (cyc=0;cyc<25;cyc++){
          unsigned long u0 = ch_u_recv;
          if (rant_topic_retire(beat) != RANT_OK){ idx_ok = 0; break; }
          beat = rant_node_create_topic(P, "beat", RANT_PUB_ONLY, G1, &topts);
          if (!beat || rant_topic_index(beat) != base_idx){ idx_ok = 0; break; }
          ch_send(beat, 4);
          for (t=0;t<4000 && (ch_c_last != ch_seq || ch_u_recv == u0);t++) ch_pump(nodes,3,2);
          if (ch_c_last != ch_seq || ch_u_recv == u0){ deliver_ok = 0; break; }
      }
      CHECK(idx_ok, "churn-A: 25 identical cycles reuse the slot");
      CHECK(deliver_ok, "churn-A: every cycle delivered (typed + untyped)");
      rant_node_mem_stats(P, &mem, &peak, &calls);
      CHECK(mem <= p_mem0 + 1024, "churn-A: publisher memory plateaus (%u -> %u)",
               (unsigned)p_mem0, (unsigned)mem);
      rant_node_mem_stats(C, &mem, &peak, &calls);
      CHECK(mem <= c_mem0 + 1024, "churn-A: subscriber memory plateaus (%u -> %u)",
               (unsigned)c_mem0, (unsigned)mem);
      CHECK(i_rant_node_topic_count(P) == p_hi0, "churn-A: topic table did not grow");
      CHECK(i_rant_transport_interest_size(P->transport) == p_int0,
               "churn-A: announce size unchanged (%u)", p_int0); }

    /* B: retype churn. The publisher rebinds the slot to an incompatible schema: the typed
       subscriber refuses loudly, the untyped one follows, then the subscriber adopts. */
    { int deliver_ok = 1, u_ok = 1, mm_ok = 1, iso_ok = 1;
      uint32_t p_int1 = 0, c_int1 = 0;
      size_t p_memb = 0, c_memb = 0;   /* re-baselined once the retype caches warm */
      for (cyc=0;cyc<10;cyc++){
          RantSchema *ns = (cyc & 1) ? G1 : G2;
          unsigned long mm0 = ch_c_mismatch, u0 = ch_u_recv, c0 = ch_c_recv;
          if (rant_topic_retire(beat) != RANT_OK){ idx_ok = 0; break; }
          beat = rant_node_create_topic(P, "beat", RANT_PUB_ONLY, ns, &topts);
          if (!beat || rant_topic_index(beat) != base_idx){ idx_ok = 0; break; }
          ch_send(beat, 4);
          for (t=0;t<4000 && (ch_u_recv == u0 || ch_c_mismatch == mm0);t++) ch_pump(nodes,3,2);
          if (ch_u_recv == u0) u_ok = 0;
          if (ch_c_mismatch == mm0) mm_ok = 0;
          if (ch_c_recv != c0) iso_ok = 0;   /* never a cross-schema delivery */
          if (rant_topic_retire(csub) != RANT_OK){ idx_ok = 0; break; }
          csub = rant_node_create_topic(C, "beat", RANT_SUB_ONLY, ns, &topts);
          if (!csub || rant_topic_index(csub) != c_idx){ idx_ok = 0; break; }
          for (t=0;t<4000 && ch_c_last != ch_seq;t++) ch_pump(nodes,3,2);
          if (ch_c_last != ch_seq){ deliver_ok = 0; break; }
          if (cyc == 0){ p_int1 = i_rant_transport_interest_size(P->transport);
                         c_int1 = i_rant_transport_interest_size(C->transport); }
          if (cyc == 1){ rant_node_mem_stats(P, &p_memb, &peak, &calls);
                         rant_node_mem_stats(C, &c_memb, &peak, &calls); }
      }
      CHECK(idx_ok, "churn-B: retype cycles keep reusing both slots");
      CHECK(mm_ok, "churn-B: typed subscriber refuses each retype loudly");
      CHECK(iso_ok, "churn-B: no cross-schema delivery to the typed subscriber");
      CHECK(u_ok, "churn-B: untyped subscriber follows every shape");
      CHECK(deliver_ok, "churn-B: adopted re-create receives the current value (catch-up)");
      CHECK(p_int1 == p_int0 + 3 && i_rant_transport_interest_size(P->transport) == p_int1,
               "churn-B: announce grows once by the 3 B gen entry, then holds (%u -> %u)",
               p_int0, p_int1);
      CHECK(c_int1 == c_int0 + 3 && i_rant_transport_interest_size(C->transport) == c_int1,
               "churn-B: subscriber announce likewise (%u -> %u)", c_int0, c_int1);
      /* the plateau is judged against the warmed baseline two full retype round trips in,
         after the one time allocations, and the 8 cycles after it must add nothing */
      rant_node_mem_stats(P, &mem, &peak, &calls);
      CHECK(mem <= p_memb + 512, "churn-B: publisher memory plateaus (%u -> %u)",
               (unsigned)p_memb, (unsigned)mem);
      rant_node_mem_stats(C, &mem, &peak, &calls);
      CHECK(mem <= c_memb + 512, "churn-B: subscriber memory plateaus (%u -> %u)",
               (unsigned)c_memb, (unsigned)mem); }

    /* C: compatible retype. The publisher rebinds to a SUPERSET of the subscriber's
       schema: the typed subscriber keeps receiving with no action and no refusal. */
    { unsigned long mm0 = ch_c_mismatch;
      CHECK(rant_topic_retire(beat) == RANT_OK, "churn-C: retire before the superset rebind");
      beat = rant_node_create_topic(P, "beat", RANT_PUB_ONLY, G3, &topts);
      CHECK(beat && rant_topic_index(beat) == base_idx, "churn-C: superset re-create reuses the slot");
      ch_send(beat, 8);
      for (t=0;t<4000 && ch_c_last != ch_seq;t++) ch_pump(nodes,3,2);
      CHECK(ch_c_last == ch_seq, "churn-C: subset-compatible rebind keeps the typed subscriber");
      CHECK(ch_c_mismatch == mm0, "churn-C: no refusal fired for the compatible rebind"); }

    /* D: transient node waves. Odd waves bring a rival same name publisher of another shape,
       even waves a late joining subscriber. Survivor state must plateau across all of it. */
    { int wave_ok = 1, cross_ok = 1, late_ok = 1, live_ok = 1;
      size_t p_mem1 = 0, c_mem1 = 0, u_mem1 = 0;
      for (cyc=0;cyc<6;cyc++){
          RantAllocator ta = rant_allocator_heap(0);
          RantNodeOpts to2 = po; RantNode *T; RantTopic *tt;
          unsigned long u0 = ch_u_recv, t0c = ch_t_recv;
          to2.user_data = (void*)"T";
          T = rant_node_open(&ta, "churn-transient", ch_on_message, ch_on_event, &to2);
          if (!T){ wave_ok = 0; rant_allocator_reset(&ta); break; }
          nodes[3] = T;
          if (cyc & 1){
              tt = rant_node_create_topic(T, "beat", RANT_PUB_ONLY, G2, &topts);
              if (!tt){ wave_ok = 0; }
              else {
                  for (t=0;t<4000 && rant_topic_match_count(tt) < 1;t++) ch_pump(nodes,4,2);
                  {   uint8_t pb[4]; i_rant_le_w32(pb, 0xBADBEEFu);     /* must never reach C */
                      rant_topic_send(tt, rant_bytes(pb, 4), NULL); }
                  for (t=0;t<4000 && ch_u_recv == u0;t++) ch_pump(nodes,4,2);
                  if (ch_u_recv == u0) wave_ok = 0;
                  ch_send(beat, 8);   /* the survivor stream runs beside the rival */
                  for (t=0;t<4000 && ch_c_last != ch_seq;t++) ch_pump(nodes,4,2);
                  if (ch_c_last != ch_seq) cross_ok = 0;
              }
          } else {
              tt = rant_node_create_topic(T, "beat", RANT_SUB_ONLY, NULL, &topts);
              if (!tt) wave_ok = 0;
              else {
                  for (t=0;t<4000 && ch_t_recv == t0c;t++) ch_pump(nodes,4,2);
                  if (ch_t_recv == t0c) late_ok = 0;   /* catch-up hands it the current value */
              }
          }
          rant_node_close(T, 1);
          nodes[3] = NULL;
          rant_allocator_reset(&ta);
          ch_send(beat, 8);   /* survivors deliver between waves */
          for (t=0;t<4000 && ch_c_last != ch_seq;t++) ch_pump(nodes,3,2);
          if (ch_c_last != ch_seq) live_ok = 0;
          if (cyc == 0){
              rant_node_mem_stats(P, &p_mem1, &peak, &calls);
              rant_node_mem_stats(C, &c_mem1, &peak, &calls);
              rant_node_mem_stats(U, &u_mem1, &peak, &calls);
          }
      }
      CHECK(wave_ok, "churn-D: every wave node joined and exchanged data");
      CHECK(cross_ok, "churn-D: rival same-name publisher never cross-wires the typed stream");
      CHECK(late_ok, "churn-D: late joiners catch up to the current value");
      CHECK(live_ok, "churn-D: survivor subscriptions deliver through every wave");
      rant_node_mem_stats(P, &mem, &peak, &calls);
      CHECK(mem <= p_mem1 + 512, "churn-D: publisher memory plateaus over peer churn (%u -> %u)",
               (unsigned)p_mem1, (unsigned)mem);
      rant_node_mem_stats(C, &mem, &peak, &calls);
      CHECK(mem <= c_mem1 + 512, "churn-D: typed subscriber memory plateaus (%u -> %u)",
               (unsigned)c_mem1, (unsigned)mem);
      rant_node_mem_stats(U, &mem, &peak, &calls);
      CHECK(mem <= u_mem1 + 512, "churn-D: untyped subscriber memory plateaus (%u -> %u)",
               (unsigned)u_mem1, (unsigned)mem);
      CHECK(i_rant_node_topic_count(P) == p_hi0 && i_rant_node_topic_count(C) == c_hi0,
               "churn-D: topic tables never grew"); }

    rant_node_close(P,0); rant_node_close(C,0); rant_node_close(U,0);
    rant_allocator_reset(&pa); rant_allocator_reset(&ca); rant_allocator_reset(&ua);
    rant_allocator_reset(&ma);
}

/* B's log lines as they reach on_message, and whether a queue dispatch ran them */
static int ml_n, ml_boom0, ml_mirror, ml_live, ml_in_dispatch, ml_dispatching;
static void ml_on_msg(const RantMsg *m){
    RantString txt = rant_get_string(m->data, m->schema, "text");
    uint64_t wall = rant_get_uint(m->data, m->schema, "wall_us");
    char tb[64]; size_t tl = txt.len < sizeof tb - 1 ? txt.len : sizeof tb - 1;
    memcpy(tb, txt.data, tl); tb[tl] = '\0';
    ml_n++;
    if (ml_dispatching) ml_in_dispatch++;
    if (strcmp(tb, "boom 0") == 0 && wall) ml_boom0 = 1;
    if (strstr(tb, "unmatched-send")) ml_mirror = 1;
    if (strcmp(tb, "live") == 0) ml_live = 1;
}

/* The metalog phase: A logs before anyone listens and triggers a mirrored error, B late
 * joins the error level on a queue and reads both, then reads a live line inline, then
 * calls A's @rant/meta directed at A. */
static void metalog_checks(void){
    RantAllocator aa = rant_allocator_heap(0);
    RantAllocator ba = rant_allocator_heap(0);
    RantNodeOpts ao, bo; RantNode *A=NULL, *B=NULL; RantAddr seed;
    int t, i;

    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&ao,0,sizeof ao); ao.domain=TEST_DOMAIN+35; ao.discovery.max_peers=4;
    ao.net.multicast_interface="127.0.0.1"; ao.net.seed_peers=&seed; ao.net.n_seed_peers=1;
    bo=ao;
    ao.match_wait_ms = -1;    /* so the unmatched send below commits + fires immediately */
    A = rant_node_open(&aa, "meta-a", NULL, NULL, &ao);
    B = rant_node_open(&ba, "meta-b", ml_on_msg, NULL, &bo);
    CHECK(A && B, "metalog: nodes open");
    if (!(A && B)){ if(A)rant_node_close(A,0); if(B)rant_node_close(B,0);
                    rant_allocator_reset(&ba); rant_allocator_reset(&aa); return; }

    CHECK(rant_node_log_topic(A, RANT_LOG_ERROR) && rant_node_log_topic(A, RANT_LOG_WARN)
             && rant_node_log_topic(A, RANT_LOG_INFO), "metalog: log topics exist");
    CHECK(rant_node_meta_function(A) != NULL && rant_node_meta_function(B) != NULL,
             "metalog: meta endpoint hosted");

    /* log BEFORE any subscriber exists: the lines land in KEEP_LAST history and a late
       joiner replays them (catch_up = keep_last) */
    for (i=0;i<3;i++){
        int lr = rant_node_log(A, RANT_LOG_ERROR, "boom %d", i);   /* a side effect, kept out of CHECK */
        CHECK(lr == RANT_OK, "metalog: log %d accepted (%d)", i, lr);
    }
    { uint64_t txm=0;
      rant_topic_counts(rant_node_log_topic(A, RANT_LOG_ERROR), &txm, NULL, NULL, NULL);
      CHECK(txm >= 3, "metalog: tx counter counts the lines (%u)", (unsigned)txm); }

    /* an internal error mirrors onto @rant/log/error: an unmatched send right after open
       (gather unsettled, wait disabled) fires RANT_E_UNMATCHED_SEND */
    { RantTopic *src = rant_node_create_topic(A, "mirror-src", RANT_PUB_ONLY, NULL, NULL);
      uint8_t payload[4] = {1,2,3,4};
      CHECK(src != NULL, "metalog: mirror-src created");
      if (src) rant_topic_send(src, rant_bytes(payload, 4), NULL);
      rant_node_poll(A, 0);     /* flush the mirror ring into the log topic */
    }

    /* late subscriber: B parks its log lines on a queue, then widens its own handle of the
       error level to PUBSUB, so the replay parks and runs only at a dispatch */
    { RantTopic *eh = rant_node_log_topic(B, RANT_LOG_ERROR);
      RantQueue *lq = rant_node_create_queue(B), *other = rant_node_create_queue(A);
      int rc = rant_node_set_log_queue(B, other);
      CHECK(rc == RANT_ERR_STATE, "metalog: another node's queue refused (%d)", rc);
      rc = rant_node_set_log_queue(B, lq);
      CHECK(lq && rc == RANT_OK, "metalog: log queue set (%d)", rc);
      rc = eh ? rant_topic_set_role(eh, RANT_PUBSUB) : -1;
      CHECK(rc == 0, "metalog: log subscribe (%d)", rc);
      for (t=0;t<1500 && !(ml_boom0 && ml_mirror);t++){
          test_pump(A,B,2);
          ml_dispatching = 1;
          (void)rant_queue_dispatch(lq, 0, 0);
          ml_dispatching = 0;
      }
      CHECK(ml_n >= 4 && ml_boom0 && ml_mirror && ml_in_dispatch == ml_n,
               "metalog: replay + mirrored error dispatched (n=%d boom0=%d mirror=%d queued=%d)",
               ml_n, ml_boom0, ml_mirror, ml_in_dispatch);

      /* NULL makes the lines inline again: a live line reaches on_message from the poll */
      rc = rant_node_set_log_queue(B, NULL);
      CHECK(rc == RANT_OK, "metalog: log queue cleared (%d)", rc);
      ml_in_dispatch = 0;
      rc = rant_node_log(A, RANT_LOG_ERROR, "live");
      CHECK(rc == RANT_OK, "metalog: live line accepted (%d)", rc);
      PUMP_UNTIL(A, B, 6000, ml_live);
      CHECK(ml_live && ml_in_dispatch == 0, "metalog: live line delivered inline (%d)", ml_live); }

    /* @rant/meta: B calls A's endpoint directed at A's peer id. A answers on its service
       thread while B blocks in the call */
    { const i_RantDiscoveryPeerView *ps; uint16_t cnt=0; uint32_t idA=0;
      RantResponse rep; int rc;
      ps = test_peers(B, &cnt);
      for (i=0;i<(int)cnt;i++)
          if (ps[i].name.len==6 && memcmp(ps[i].name.data,"meta-a",6)==0) idA = ps[i].id;
      CHECK(idA != 0, "metalog: found A's peer id (%u)", idA);
      rant_node_start(A);
      rc = rant_function_call(rant_node_meta_function(B), rant_bytes(NULL,0), &rep, 3000,
                              &(RantCallOpts){ .provider = idA });
      rant_node_stop(A);
      CHECK(rc==1 && rep.status==RANT_CALL_OK && rep.data.len>0 && rep.schema,
               "metalog: meta call answered (rc=%d st=%d len=%u)",
               rc, rep.status, (unsigned)rep.data.len);
      if (rc==1 && rep.status==RANT_CALL_OK && rep.schema){
          RantBytes info = rant_get_map(rep.data, rep.schema, "info");
          RantValue nodev, namev, topv, upt;
          int ok_node   = rant_map_get(info, "node", &nodev);
          int ok_name   = ok_node && rant_map_get(nodev.bytes, "name", &namev);
          int ok_topics = rant_map_get(info, "topics", &topv);
          CHECK(ok_name && namev.bytes.len==6 && memcmp(namev.bytes.data,"meta-a",6)==0,
                   "metalog: snapshot node.name == meta-a (%d)", ok_name);
          { /* only APP topics ride the snapshot: mirror-src, never the @rant/ builtins */
            RantValue row, nm; uint16_t r; int hid = 0;
            for (r = 0; ok_topics && r < topv.count; r++)
                if (rant_map_array_at(topv.bytes, r, &row) && rant_map_get(row.bytes, "name", &nm)
                    && nm.bytes.len >= 6 && !memcmp(nm.bytes.data, "@rant/", 6)) hid++;
            CHECK(ok_topics && topv.count == 1 && hid == 0,
                     "metalog: snapshot lists app topics only (%u rows, %d hidden)",
                     (unsigned)(ok_topics?topv.count:0), hid); }
          CHECK(ok_node && rant_map_get(nodev.bytes, "uptime_us", &upt) && upt.v.u > 0,
                   "metalog: snapshot uptime present");
      }

      /* the builtins are listed like any entity: A's walks see mirror-src beside them, B
         hosts only builtins, and B's walk of A carries A's meta function */
      { RantIter eit; RantEntityInfo ei; int la=0, lb=0, pa=0, ba=0, bb=0, meta=0;
        memset(&eit,0,sizeof eit);
        while (rant_node_entities_next(A, RANT_SELF, &eit, &ei)){ if (test_builtin(&ei)) ba++; else la++; }
        memset(&eit,0,sizeof eit);
        while (rant_node_entities_next(B, RANT_SELF, &eit, &ei)){ if (test_builtin(&ei)) bb++; else lb++; }
        memset(&eit,0,sizeof eit);
        while (rant_node_entities_next(B, idA, &eit, &ei)){
            if (!test_builtin(&ei)) pa++;
            else if (ei.kind==RANT_ENTITY_FUNCTION && ei.name.len==10
                     && !memcmp(ei.name.data,"@rant/meta",10) && ei.provides && !ei.incomplete) meta++;
        }
        CHECK(la==1 && lb==0 && pa==1,
                 "metalog: entity walks list the app's own (A=%d B=%d peerA=%d)", la, lb, pa);
        CHECK(ba>=4 && bb>=4 && meta==1,
                 "metalog: entity walks list the builtins (A=%d B=%d peerA meta=%d)", ba, bb, meta); }

      /* the hidden namespace is reserved: a leading '@' is refused in every constructor */
      { RantVariable *ev = rant_node_create_variable_definition(A, "@rant/evil", NULL, NULL);
        RantTopic *tp = rant_node_create_topic(A, "a@b", RANT_PUB_ONLY, NULL, NULL);
        CHECK(ev == NULL && tp == NULL, "metalog: reserved '@' names refused"); } }

    rant_node_close(B,1); rant_node_close(A,1);
    rant_allocator_reset(&ba); rant_allocator_reset(&aa);
}

static const TestCase cases[] = {
    { "reflect_dropped", reflect_dropped_checks },
    { "churn", churn_checks },
    { "log_and_meta", metalog_checks },
};

TEST_MAIN(cases)
