/* Functions and variables between two nodes: calls, replies, deferral, timeouts, forcing,
 * the first write that rides the match wait, duplicate authority, retire and reuse, and loud
 * refusals. spec/testing.md says what each case pins. */
#include "support/test.h"

/* The function phases: a provider node and a caller node in one process
 * (spec/testing.md). */
static volatile int   pf_reply_done;
static RantCallStatus pf_reply_status;
static uint32_t         pf_reply_val;
static char             pf_reply_msg[RANT_CALL_MSG_MAX + 1]; static size_t pf_reply_msg_len;
static void pf_on_reply(const RantResponse *r){
    pf_reply_status = r->status;
    pf_reply_val = r->data.len>=4 ? i_rant_le_r32(r->data.data) : 0;
    pf_reply_msg_len = r->message.len <= RANT_CALL_MSG_MAX ? r->message.len : RANT_CALL_MSG_MAX;
    if (pf_reply_msg_len) memcpy(pf_reply_msg, r->message.data, pf_reply_msg_len);
    pf_reply_msg[pf_reply_msg_len] = 0;
    pf_reply_done = 1;
}
/* handlers run on a started provider's service thread too */
static long long pf_calls;
static void pf_add_handler(RantRequest *req, void *user){
    uint8_t out[4];
    uint32_t v = req->data.len>=4 ? i_rant_le_r32(req->data.data) : 0;
    (void)user; test_atomic_add(&pf_calls, 1);
    i_rant_le_w32(out, v+1);
    rant_request_reply(req, rant_bytes(out,4));
}
static void pf_empty_handler(RantRequest *req, void *user){ (void)req;(void)user; test_atomic_add(&pf_calls, 1); }
/* fail with an over long message and a structured payload: pins the wire carried
 * APP_ERROR text, the truncating cap, and that data still rides beside the message */
static void pf_fail_handler(RantRequest *req, void *user){
    char big[301]; int i; uint8_t out[4]; (void)user; test_atomic_add(&pf_calls, 1);
    for (i=0;i<300;i++) big[i] = (char)('a' + i%26);
    big[300] = 0;
    i_rant_le_w32(out, 13);
    rant_request_fail(req, big, rant_bytes(out,4));
}
/* second caller's reply capture (two-caller directed-isolation test) */
static volatile int   pf_reply2_done;
static RantCallStatus pf_reply2_status;
static uint32_t         pf_reply2_val;
static void pf_on_reply2(const RantResponse *r){
    pf_reply2_status = r->status;
    pf_reply2_val = r->data.len>=4 ? i_rant_le_r32(r->data.data) : 0;
    pf_reply2_done = 1;
}
/* burst capture: counts completions, sums returned values, flags any non-OK */
static volatile int pf_burst_done; static uint32_t pf_burst_sum; static int pf_burst_bad;
static RantCallStatus pf_burst_last_bad;
static void pf_on_reply_burst(const RantResponse *r){
    if (r->status == RANT_CALL_OK && r->data.len>=4) pf_burst_sum += i_rant_le_r32(r->data.data);
    else { pf_burst_bad++; pf_burst_last_bad = r->status; }
    pf_burst_done++;
}
static long long pf_defer_token;
static void pf_defer_handler(RantRequest *req, void *user){
    (void)user; test_atomic_add(&pf_calls, 1);
    test_atomic_store(&pf_defer_token, (long long)rant_request_defer(req));
}
/* variable on_change / on_write capture */
typedef struct { int n; uint32_t val, seq, source; uint8_t forced; } PfVarEvt;
static void pf_on_var_update(const RantVariableUpdate *u, void *user){
    PfVarEvt *e = (PfVarEvt*)user;
    e->n++; e->val = u->value.len>=4 ? i_rant_le_r32(u->value.data) : 0;
    e->seq = u->write_seq; e->source = u->source; e->forced = u->forced;
}
/* cancel-at-close capture: a call pending at close must get exactly one CANCELLED outcome */
static volatile int pf_cancel_count; static RantCallStatus pf_cancel_status;
static void pf_on_cancel(const RantResponse *r){ pf_cancel_status = r->status; pf_cancel_count++; }
/* retire from inside a callback must be refused (the role flip would rematch mid-delivery) */
static int pf_retire_in_cb_rc;
static void pf_on_var_retire_attempt(const RantVariableUpdate *u, void *user){
    (void)user;
    pf_retire_in_cb_rc = rant_variable_retire(u->variable);
}
/* reentrant on_write: the first write of 100 immediately re-sets to 200, once */
static RantVariable *pf_reent_var; static int pf_reent_done;
static void pf_on_var_reenter(const RantVariableUpdate *u, void *user){
    (void)user;
    if (!pf_reent_done && u->value.len>=4 && i_rant_le_r32(u->value.data)==100){
        uint8_t nb[4];
        pf_reent_done = 1;
        i_rant_le_w32(nb, 200);
        rant_variable_set(pf_reent_var, rant_bytes(nb,4));
    }
}

/* a write burst from inside a callback: every set after the first runs reentrant, so the
 * value channel's ring is the only thing between the burst and silent loss */
#define PF_VBURST_N 8
static RantVariable *pf_vburst_var; static int pf_vburst_ran;
static void pf_on_var_burst(const RantVariableUpdate *u, void *user){
    uint8_t b[4]; uint32_t i;
    (void)u; (void)user;
    if (pf_vburst_ran) return;
    pf_vburst_ran = 1;
    for (i=1;i<=PF_VBURST_N;i++){ i_rant_le_w32(b,i); rant_variable_set(pf_vburst_var, rant_bytes(b,4)); }
}
static uint32_t pf_vburst_seen;   /* bit per value observed at the remote */
static void pf_on_var_burst_rx(const RantVariableUpdate *u, void *user){
    uint32_t v;
    (void)user;
    if (u->value.len < 4) return;
    v = i_rant_le_r32(u->value.data);
    if (v>=1 && v<=PF_VBURST_N) pf_vburst_seen |= 1u << (v-1);
}

static void patterns_checks(void){
    RantAllocator pa = rant_allocator_heap(0);
    RantAllocator ca = rant_allocator_heap(0);
    RantNodeOpts po, co; RantNode *P=NULL, *C=NULL; RantAddr seed;
    RantFunction *prov, *call_add, *pe, *ce, *pd, *cd, *pnh, *cnh, *ghost, *pfm, *cfm;
    uint16_t dom = TEST_DOMAIN+20; int t;

    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=dom; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    co=po;
    co.fetch_details = 1;   /* C doubles as the reflection observer: full entity names */
    P = rant_node_open(&pa, "fn-prov", NULL, NULL, &po);
    C = rant_node_open(&ca, "fn-call", NULL, NULL, &co);
    CHECK(P && C, "patterns: nodes open");
    if (!(P && C)){ if(P)rant_node_close(P,0); if(C)rant_node_close(C,0); return; }

    prov = rant_node_create_function_definition(P, "add",  NULL, NULL, pf_add_handler,   NULL, NULL);
    call_add = rant_node_create_remote_function(C, "add",  NULL, NULL, NULL);
    pe   = rant_node_create_function_definition(P, "noop", NULL, NULL, pf_empty_handler, NULL, NULL);
    ce   = rant_node_create_remote_function(C, "noop", NULL, NULL, NULL);
    pd   = rant_node_create_function_definition(P, "defr", NULL, NULL, pf_defer_handler, NULL, NULL);
    cd   = rant_node_create_remote_function(C, "defr", NULL, NULL, NULL);
    pnh  = rant_node_create_function_definition(P, "nohd", NULL, NULL, NULL /* no handler */, NULL, NULL);
    cnh  = rant_node_create_remote_function(C, "nohd", NULL, NULL, NULL);
    ghost= rant_node_create_remote_function(C, "ghost",NULL, NULL, &(RantFunctionOpts){ .timeout_us = 150000u });
    pfm  = rant_node_create_function_definition(P, "failm", NULL, NULL, pf_fail_handler, NULL, NULL);
    cfm  = rant_node_create_remote_function(C, "failm", NULL, NULL, NULL);
    CHECK(prov&&call_add&&pe&&ce&&pd&&cd&&pnh&&cnh&&ghost&&pfm&&cfm, "patterns: functions created/opened");
    (void)pfm;
    (void)pe;(void)ce;

    PUMP_UNTIL(P, C, 8000, rant_function_match_count(call_add)!=0);
    CHECK(rant_function_match_count(call_add)==1, "patterns: provider matched (%d)",
             rant_function_match_count(call_add));

    /* settle solicits and blocks until every live peer answered. The peer must answer while
       we block, so P runs its service thread for the duration. */
    { int sr;
      rant_node_start(P);
      sr = rant_node_settle(C, 3000);
      rant_node_stop(P);
      CHECK(sr == 1, "patterns: rant_node_settle settles (%d)", sr); }

    /* async call: add(41) gives 42, status OK */
    { uint8_t req[4]; i_rant_le_w32(req,41); pf_reply_done=0;
      rant_function_call_async(call_add, rant_bytes(req,4), pf_on_reply, NULL, NULL);
      PUMP_UNTIL(P, C, 3200, pf_reply_done);
      CHECK(pf_reply_done && pf_reply_status==RANT_CALL_OK && pf_reply_val==42,
               "patterns: async add(41)=42 OK (done=%d st=%d val=%u)", pf_reply_done, pf_reply_status, pf_reply_val);
      CHECK(pf_reply_msg_len==0, "patterns: OK response message empty (len=%u)",
               (unsigned)pf_reply_msg_len); }

    /* fail with a message: APP_ERROR text rides the response header (truncated at the
       255 cap, never refused) beside a structured payload */
    { size_t i; int msg_ok;
      pf_reply_done=0;
      rant_function_call_async(cfm, rant_bytes(NULL,0), pf_on_reply, NULL, NULL);
      PUMP_UNTIL(P, C, 3200, pf_reply_done);
      msg_ok = pf_reply_msg_len == RANT_CALL_MSG_MAX;
      for (i=0; msg_ok && i<pf_reply_msg_len; i++)
          if (pf_reply_msg[i] != (char)('a' + i%26)) msg_ok = 0;
      CHECK(pf_reply_done && pf_reply_status==RANT_CALL_APP_ERROR,
               "patterns: fail -> APP_ERROR (done=%d st=%d)", pf_reply_done, pf_reply_status);
      CHECK(msg_ok, "patterns: 300-byte message truncated to cap, content intact (len=%u)",
               (unsigned)pf_reply_msg_len);
      CHECK(pf_reply_val==13, "patterns: structured payload rides beside the message (val=%u)",
               pf_reply_val); }

    /* empty ack: the handler returns without replying, auto OK with an empty payload */
    { pf_reply_done=0; test_atomic_store(&pf_calls, 0);
      rant_function_call_async(ce, rant_bytes(NULL,0), pf_on_reply, NULL, NULL);
      PUMP_UNTIL(P, C, 3200, pf_reply_done);
      CHECK(pf_reply_done && pf_reply_status==RANT_CALL_OK && pf_reply_val==0,
               "patterns: empty-ack auto OK (done=%d st=%d)", pf_reply_done, pf_reply_status); }

    /* deferred: handler defers, we complete later with a value */
    { uint64_t tok;
      pf_reply_done=0; test_atomic_store(&pf_defer_token, 0);
      rant_function_call_async(cd, rant_bytes(NULL,0), pf_on_reply, NULL, NULL);
      PUMP_UNTIL(P, C, 3200, test_atomic_load(&pf_defer_token));
      tok = (uint64_t)test_atomic_load(&pf_defer_token);
      CHECK(tok!=0, "patterns: handler deferred (token=%llu)", (unsigned long long)tok);
      { uint8_t out[4]; i_rant_le_w32(out,99);
        rant_function_complete(pd, tok, RANT_CALL_OK, "deferred done", rant_bytes(out,4)); }
      PUMP_UNTIL(P, C, 3200, pf_reply_done);
      CHECK(pf_reply_done && pf_reply_status==RANT_CALL_OK && pf_reply_val==99,
               "patterns: deferred completion delivers 99 (done=%d val=%u)", pf_reply_done, pf_reply_val);
      CHECK(pf_reply_msg_len==13 && memcmp(pf_reply_msg,"deferred done",13)==0,
               "patterns: complete() message carried on OK (\"%s\")", pf_reply_msg); }

    /* no handler: the provider has a NULL on_request, NO_HANDLER */
    { pf_reply_done=0;
      rant_function_call_async(cnh, rant_bytes(NULL,0), pf_on_reply, NULL, NULL);
      PUMP_UNTIL(P, C, 3200, pf_reply_done);
      CHECK(pf_reply_done && pf_reply_status==RANT_CALL_NO_HANDLER,
               "patterns: no-handler -> NO_HANDLER (done=%d st=%d)", pf_reply_done, pf_reply_status);
      CHECK(pf_reply_msg_len==10 && memcmp(pf_reply_msg,"no handler",10)==0,
               "patterns: empty wire message defaults to status text (\"%s\")", pf_reply_msg); }

    /* no provider for "ghost": the deadline synthesizes NO_PROVIDER, never TIMEOUT */
    { pf_reply_done=0;
      rant_function_call_async(ghost, rant_bytes(NULL,0), pf_on_reply, NULL, NULL);
      PUMP_UNTIL(P, C, 2000, pf_reply_done);
      CHECK(pf_reply_done && pf_reply_status==RANT_CALL_NO_PROVIDER,
               "patterns: no-provider -> NO_PROVIDER (done=%d st=%d)", pf_reply_done, pf_reply_status);
      CHECK(pf_reply_msg_len==11 && memcmp(pf_reply_msg,"no provider",11)==0,
               "patterns: synthesized outcome carries status text (\"%s\")", pf_reply_msg); }

    /* call before the match forms: a fresh function pair called immediately. The request
       must queue and flush when the provider matches, never drop into a timeout. */
    { RantFunction *pe2, *ce2; int cr; uint8_t req[4];
      pe2 = rant_node_create_function_definition(P, "early", NULL, NULL, pf_add_handler, NULL, NULL);
      ce2 = rant_node_create_remote_function(C, "early", NULL, NULL, NULL);
      CHECK(pe2 && ce2, "patterns: early function pair created");
      i_rant_le_w32(req, 6); pf_reply_done = 0;
      cr = rant_function_call_async(ce2, rant_bytes(req,4), pf_on_reply, NULL, NULL);
      CHECK(cr==RANT_OK, "patterns: early call accepted (%d)", cr);
      PUMP_UNTIL(P, C, 8000, pf_reply_done);
      CHECK(pf_reply_done && pf_reply_status==RANT_CALL_OK && pf_reply_val==7,
               "patterns: early call flushed on match -> 7 (done=%d st=%d val=%u)",
               pf_reply_done, pf_reply_status, pf_reply_val); }

    /* two callers answered in one provider tick: both replies commit back to back before any
       TX runs, and each caller must receive its own, since call ids are per caller counters */
    { RantAllocator c2a = rant_allocator_heap(0);
      RantNodeOpts c2o = co; RantNode *C2 = rant_node_open(&c2a, "fn-call2", NULL, NULL, &c2o);
      RantFunction *call2 = C2 ? rant_node_create_remote_function(C2, "add", NULL, NULL, NULL) : NULL;
      CHECK(C2 && call2, "patterns: second caller open");
      if (C2 && call2){
          uint8_t r1[4], r2[4]; int i2;
          for (t=0;t<2000 && rant_function_match_count(call2)==0;t++){ test_pump(P,C,1); rant_node_poll(C2,1); }
          CHECK(rant_function_match_count(call2)==1, "patterns: second caller matched");
          /* park both requests at the provider before it polls once */
          i_rant_le_w32(r1,100); i_rant_le_w32(r2,200);
          pf_reply_done=0; pf_reply2_done=0;
          rant_function_call_async(call_add, rant_bytes(r1,4), pf_on_reply,      NULL, NULL);
          rant_function_call_async(call2,      rant_bytes(r2,4), pf_on_reply2, NULL, NULL);
          rant_node_poll(C,0); rant_node_poll(C2,0);          /* flush both requests out */
          for (i2=0;i2<800 && !(pf_reply_done && pf_reply2_done);i2++){
              rant_node_poll(P,2); rant_node_poll(C,2); rant_node_poll(C2,2);
          }
          CHECK(pf_reply_done && pf_reply_status==RANT_CALL_OK && pf_reply_val==101,
                   "patterns: caller1 got ITS reply (done=%d st=%d val=%u)",
                   pf_reply_done, pf_reply_status, pf_reply_val);
          CHECK(pf_reply2_done && pf_reply2_status==RANT_CALL_OK && pf_reply2_val==201,
                   "patterns: caller2 got ITS reply (done=%d st=%d val=%u)",
                   pf_reply2_done, pf_reply2_status, pf_reply2_val);
          rant_node_close(C2,1);
      } else if (C2) rant_node_close(C2,1);
      rant_allocator_reset(&c2a); }

#ifdef RANT_THREADS
    /* a burst past keep_last with the provider on its own service thread: the caller's sends
       engage backpressure without the pattern layer holding the node lock, every call completes */
    { uint8_t req[4]; int i2; uint32_t expect_sum=0;
      rant_node_start(P);
      pf_burst_done=0; pf_burst_sum=0; pf_burst_bad=0;
      for (i2=0;i2<14;i2++){
          int cr;   /* the call has a side effect, so it stays out of CHECK */
          i_rant_le_w32(req,(uint32_t)(1000+i2)); expect_sum += (uint32_t)(1000+i2+1);
          cr = rant_function_call_async(call_add, rant_bytes(req,4), pf_on_reply_burst, NULL, NULL);
          CHECK(cr==RANT_OK, "patterns: burst call %d accepted (%d)", i2, cr);
      }
      PUMP_UNTIL(C, NULL, 8000, pf_burst_done >= 14);
      CHECK(pf_burst_done==14 && pf_burst_bad==0 && pf_burst_sum==expect_sum,
               "patterns: burst 14/keep_last 10 all OK (done=%d bad=%d sum=%u want=%u)",
               pf_burst_done, pf_burst_bad, pf_burst_sum, expect_sum);
      CHECK(rant_node_evicted_unsent(C)==0, "patterns: burst evicted nothing (%u)",
               rant_node_evicted_unsent(C));
      rant_node_stop(P); }

    /* a sync call that times out locally, then the deferred reply lands late: the pending
       entry must be unlinked, the late reply dropped, and a later sync call still works */
    { RantResponse rep; int rc; uint64_t tok;
      rant_node_start(P);
      test_atomic_store(&pf_defer_token, 0);
      rc = rant_function_call(cd, rant_bytes(NULL,0), &rep, 120, NULL);
      CHECK(rc==0 && rep.status==RANT_CALL_TIMEOUT,
               "patterns: sync local timeout (rc=%d st=%d)", rc, rep.status);
      CHECK(rep.message.len==7 && memcmp(rep.message.data,"timeout",7)==0,
               "patterns: sync timeout carries status text");
      PUMP_UNTIL(C, NULL, 2000, test_atomic_load(&pf_defer_token));
      tok = (uint64_t)test_atomic_load(&pf_defer_token);
      CHECK(tok!=0, "patterns: deferred token arrived");
      if (tok){ uint8_t out[4]; i_rant_le_w32(out,7);
          rant_function_complete(pd, tok, RANT_CALL_OK, NULL, rant_bytes(out,4)); }
      test_pump(C, NULL, 400);     /* late reply arrives: must be dropped safely */
      pf_reply_done=0;
      { uint8_t req[4]; i_rant_le_w32(req,60);
        rc = rant_function_call(call_add, rant_bytes(req,4), &rep, 1000, NULL); }
      CHECK(rc==1 && rep.status==RANT_CALL_OK && rep.data.len>=4 && i_rant_le_r32(rep.data.data)==61,
               "patterns: sync works after late-reply drop (rc=%d st=%d)", rc, rep.status);
      rant_node_stop(P); }
#endif

    /* variables: catch-up, convergence, read-only refusal, force/absorb/unforce */
    { RantVariable *ov, *av; RantBytes gv; uint8_t b[4];
      i_rant_le_w32(b,20);
      ov = rant_node_create_variable_definition(P, "temp", NULL, &(RantVariableOpts){ .initial=rant_bytes(b,4), .allow_force=1 });
      av = rant_node_create_remote_variable(C, "temp", NULL, NULL);
      CHECK(ov && av, "var: created/opened");
      PUMP_UNTIL(P, C, 8000, rant_variable_match_count(ov)!=0);
      CHECK(rant_variable_match_count(ov)==1, "var: accessor matched (%d)", rant_variable_match_count(ov));
      PUMP_UNTIL(P, C, 2400, rant_variable_get(av,&gv));
      CHECK(rant_variable_get(av,&gv) && gv.len==4 && i_rant_le_r32(gv.data)==20, "var: catch_up initial=20");
      i_rant_le_w32(b,25); rant_variable_set(ov, rant_bytes(b,4));
      PUMP_UNTIL(P, C, 2400, rant_variable_get(av,&gv)&&gv.len==4&&i_rant_le_r32(gv.data)==25);
      CHECK(rant_variable_get(av,&gv)&&i_rant_le_r32(gv.data)==25, "var: owner set converges (25)");
      i_rant_le_w32(b,30);
      { int sr = rant_variable_set(av, rant_bytes(b,4)); CHECK(sr==RANT_OK, "var: accessor set ok (%d)", sr); }
      PUMP_UNTIL(P, C, 2400, rant_variable_get(ov,&gv)&&gv.len==4&&i_rant_le_r32(gv.data)==30);
      CHECK(rant_variable_get(ov,&gv)&&i_rant_le_r32(gv.data)==30, "var: accessor set applied at owner (30)");
      i_rant_le_w32(b,99); rant_variable_force(ov, rant_bytes(b,4));
      PUMP_UNTIL(P, C, 2400, rant_variable_forced(av)&&rant_variable_get(av,&gv)&&i_rant_le_r32(gv.data)==99);
      CHECK(rant_variable_forced(av)&&rant_variable_get(av,&gv)&&i_rant_le_r32(gv.data)==99,
               "var: force visible at accessor (99, forced)");
      i_rant_le_w32(b,50); rant_variable_set(av, rant_bytes(b,4));         /* absorbed into the shadow */
      test_pump(P,C,80);
      CHECK(rant_variable_get(av,&gv)&&i_rant_le_r32(gv.data)==99, "var: set absorbed while forced (still 99)");
      rant_variable_unforce(ov);
      PUMP_UNTIL(P, C, 2400, !rant_variable_forced(av)&&rant_variable_get(av,&gv)&&i_rant_le_r32(gv.data)==50);
      CHECK(!rant_variable_forced(av)&&rant_variable_get(av,&gv)&&i_rant_le_r32(gv.data)==50,
               "var: unforce restores latest absorbed (50)"); }
    { RantVariable *ro_o, *ro_a; RantBytes gv; uint8_t b[4]; int sr; i_rant_le_w32(b,7);
      ro_o = rant_node_create_variable_definition(P, "rovar", NULL, &(RantVariableOpts){ .initial=rant_bytes(b,4), .access=RANT_VAR_READONLY });
      ro_a = rant_node_create_remote_variable(C, "rovar", NULL, NULL);
      CHECK(ro_o&&ro_a, "var: read-only created/opened");
      /* the owner's value arrives only once the accessor verified it, read only included */
      PUMP_UNTIL(P, C, 4000, rant_variable_get(ro_a, &gv));
      i_rant_le_w32(b,8); sr = rant_variable_set(ro_a, rant_bytes(b,4));
      CHECK(sr==RANT_ERR_ROLE, "var: read-only set refused (%d)", sr); }
    { RantVariable *orphan = rant_node_create_remote_variable(C, "nobody-owns-this", NULL, NULL);
      uint8_t b[4]; int sr; i_rant_le_w32(b,1);
      CHECK(orphan != NULL, "var: orphan accessor opens");
      sr = orphan ? rant_variable_set(orphan, rant_bytes(b,4)) : 0;
      CHECK(sr==RANT_ERR_NO_TOPIC, "var: set with no owner -> NO_TOPIC (%d)", sr); }
    { /* typed variable, remote force and unforce: the op only unforce must pass the schema
         gate through the empty payload exemption, and remote sets while forced absorb */
      RantAllocator ma = rant_allocator_heap(0);
      RantSchema *ts = test_compile(&ma, "T { v: u32 }", NULL);
      RantVariable *to, *ta; RantBytes gv; uint8_t b[4]; int fr, ur;
      CHECK(ts != NULL, "var: typed schema compiles");
      i_rant_le_w32(b,10);
      to = rant_node_create_variable_definition(P, "ttemp", ts, &(RantVariableOpts){ .initial=rant_bytes(b,4), .allow_force=1 });
      ta = rant_node_create_remote_variable(C, "ttemp", ts, NULL);
      CHECK(to && ta, "var: typed created/opened");
      PUMP_UNTIL(P, C, 8000, rant_variable_match_count(to)!=0);
      PUMP_UNTIL(P, C, 2400, rant_variable_get(ta,&gv));
      i_rant_le_w32(b,777); fr = rant_variable_force(ta, rant_bytes(b,4));         /* remote force */
      CHECK(fr==RANT_OK, "var: typed remote force sent (%d)", fr);
      PUMP_UNTIL(P, C, 2400, rant_variable_forced(ta)&&rant_variable_get(ta,&gv)&&i_rant_le_r32(gv.data)==777);
      CHECK(rant_variable_forced(ta)&&rant_variable_get(ta,&gv)&&i_rant_le_r32(gv.data)==777,
               "var: typed force pins (777, forced)");
      ur = rant_variable_unforce(ta);     /* remote, op only payload */
      CHECK(ur==RANT_OK, "var: typed remote unforce sent (%d)", ur);
      PUMP_UNTIL(P, C, 2400, !rant_variable_forced(ta));
      CHECK(!rant_variable_forced(ta), "var: typed remote unforce applies (op-only payload)");
      rant_allocator_reset(&ma); }
    { /* on_change and on_write: change dedup, every write, replay at registration, force
         transitions, absorbed writes stay silent, and the source peer stamp */
      PfVarEvt oc, ow, ac, aw;   /* owner / accessor, change / write */
      RantVariable *eo, *ea; uint8_t b[4]; int rr;
      memset(&oc,0,sizeof oc); memset(&ow,0,sizeof ow); memset(&ac,0,sizeof ac); memset(&aw,0,sizeof aw);
      i_rant_le_w32(b,5);
      eo = rant_node_create_variable_definition(P, "evar", NULL, &(RantVariableOpts){ .initial=rant_bytes(b,4), .allow_force=1 });
      ea = rant_node_create_remote_variable(C, "evar", NULL, NULL);
      CHECK(eo && ea, "var: event pair created");
      /* registering AFTER the initial value exists replays it once, immediately */
      rr = rant_variable_on_change(eo, pf_on_var_update, &oc);
      CHECK(rr==RANT_OK && oc.n==1 && oc.val==5,
               "var: on_change replays current at registration (n=%d val=%u)", oc.n, oc.val);
      rant_variable_on_write(eo, pf_on_var_update, &ow);
      CHECK(ow.n==0, "var: on_write does not replay (writes are events, not state)");
      rant_variable_on_change(ea, pf_on_var_update, &ac);
      rant_variable_on_write(ea, pf_on_var_update, &aw);
      PUMP_UNTIL(P, C, 8000, ac.n!=0);   /* catch-up: the first value fires both */
      CHECK(ac.n==1 && aw.n==1 && ac.val==5,
               "var: accessor first value fires (change=%d write=%d val=%u)", ac.n, aw.n, ac.val);
      CHECK(ac.source!=0, "var: accessor source = the owner peer (%u)", ac.source);
      i_rant_le_w32(b,6); rant_variable_set(eo, rant_bytes(b,4));         /* local set, source 0 */
      CHECK(oc.n==2 && ow.n==1 && oc.val==6 && oc.source==0,
               "var: local set fires inline on the caller (change=%d write=%d src=%u)", oc.n, ow.n, oc.source);
      rant_variable_set(eo, rant_bytes(b,4));                   /* byte-identical re-set */
      CHECK(oc.n==2 && ow.n==2 && ow.seq==oc.seq+1,
               "var: identical re-set fires write only, seq advances (change=%d write=%d)", oc.n, ow.n);
      PUMP_UNTIL(P, C, 2400, aw.n >= 3);
      CHECK(aw.n==3 && ac.n==2 && ac.val==6,
               "var: accessor saw 3 writes / 2 changes (w=%d c=%d val=%u)", aw.n, ac.n, ac.val);
      rant_variable_force(eo, rant_bytes(b,4));       /* same bytes: the forced flip is a change */
      CHECK(oc.n==3 && oc.forced==1 && ow.n==3,
               "var: force with identical bytes still a change (n=%d forced=%u)", oc.n, oc.forced);
      PUMP_UNTIL(P, C, 2400, ac.n >= 3);
      CHECK(ac.n==3 && ac.forced==1, "var: accessor sees the forced flip (n=%d forced=%u)", ac.n, ac.forced);
      i_rant_le_w32(b,44); rant_variable_set(eo, rant_bytes(b,4));         /* absorbed into the shadow */
      test_pump(P,C,40);
      CHECK(oc.n==3 && ow.n==3, "var: absorbed write fires nothing (c=%d w=%d)", oc.n, ow.n);
      rant_variable_unforce(eo);
      CHECK(oc.n==4 && !oc.forced && oc.val==44 && ow.n==4,
               "var: unforce fires with the restored value (n=%d val=%u)", oc.n, oc.val);
      PUMP_UNTIL(P, C, 2400, ac.n >= 4);
      CHECK(ac.n==4 && !ac.forced && ac.val==44,
               "var: accessor unforce change (n=%d val=%u forced=%u)", ac.n, ac.val, ac.forced);
      i_rant_le_w32(b,70);
      { int sr = rant_variable_set(ea, rant_bytes(b,4)); CHECK(sr==RANT_OK, "var: event remote set ok (%d)", sr); }
      PUMP_UNTIL(P, C, 2400, oc.n >= 5);
      CHECK(oc.n==5 && oc.val==70 && oc.source!=0,
               "var: remote set fires at the owner with the setter's peer (src=%u)", oc.source);
      rant_variable_on_change(eo, NULL, NULL); rant_variable_on_write(eo, NULL, NULL);
      i_rant_le_w32(b,71); rant_variable_set(eo, rant_bytes(b,4));
      CHECK(oc.n==5 && ow.n==5, "var: cleared callbacks stay silent (c=%d w=%d)", oc.n, ow.n);
      PUMP_UNTIL(P, C, 2400, ac.n >= 5);   /* drain the echoes before the captures die */
      rant_variable_on_change(ea, NULL, NULL); rant_variable_on_write(ea, NULL, NULL); }

#ifdef RANT_THREADS
    /* sync call: the provider answers from its own service thread while the caller's
       sync loop drives its node */
    { RantResponse rep; int rc; uint8_t req[4]; i_rant_le_w32(req,7);
      rant_node_start(P);
      rc = rant_function_call(call_add, rant_bytes(req,4), &rep, 1000, NULL);
      CHECK(rc==1 && rep.status==RANT_CALL_OK && rep.data.len>=4 && i_rant_le_r32(rep.data.data)==8,
               "patterns: sync add(7)=8 (rc=%d st=%d)", rc, rep.status);
      rant_node_stop(P); }
#endif

    /* reflection: the entity walk folds P's channels. P hosts 5 functions and 3 variables,
       and the peer walk from C and P's local walk must both yield exactly those. */
    test_pump(P, C, 400);   /* let any straggling detail fetches settle */
    { const i_RantDiscoveryPeerView *ps; uint16_t pc = 0; uint32_t pid = 0;
      ps = test_peers(C, &pc);
      if (ps && pc) pid = ps[0].id;
      { RantIter eit; RantEntityInfo ei;
        int fns=0,vars=0,tops=0,ats=0,inc=0,temp_rw=0,rovar_ro=0,temp_forceable=0,rovar_forceable=0; size_t k;
        memset(&eit,0,sizeof eit);
        while (rant_node_entities_next(C, pid, &eit, &ei)){
            if (test_builtin(&ei)) continue;
            switch (ei.kind){
            case RANT_ENTITY_FUNCTION: fns++; break;
            case RANT_ENTITY_VARIABLE:
                vars++;
                if (ei.name.len==4 && !memcmp(ei.name.data,"temp",4)) { temp_rw  = ei.writable; temp_forceable = ei.forceable; }
                if (ei.name.len==5 && !memcmp(ei.name.data,"rovar",5)){ rovar_ro = !ei.writable; rovar_forceable = ei.forceable; }
                break;
            default: tops++; break;
            }
            inc += ei.incomplete;
            for (k=0;k<ei.name.len;k++) if (ei.name.data[k]=='@') ats++;
        }
        CHECK(fns==6 && vars==4 && tops==0,
                 "reflect: peer entities fold (fn=%d var=%d top=%d)", fns, vars, tops);
        CHECK(ats==0 && inc==0, "reflect: no internals leak (@bytes=%d incomplete=%d)", ats, inc);
        CHECK(temp_rw==1 && rovar_ro==1, "reflect: writability (temp rw=%d, rovar ro=%d)", temp_rw, rovar_ro);
        CHECK(temp_forceable==1 && rovar_forceable==0,
                 "reflect: forceability (temp allow_force=%d, rovar=%d)", temp_forceable, rovar_forceable); }
      { RantIter eit; RantEntityInfo ei; int fns=0,vars=0,tops=0,temp_forceable=0;
        memset(&eit,0,sizeof eit);
        while (rant_node_entities_next(P, RANT_SELF, &eit, &ei)){
            if (test_builtin(&ei)) continue;
            switch (ei.kind){
            case RANT_ENTITY_FUNCTION: fns++; break;
            case RANT_ENTITY_VARIABLE:
                vars++;
                if (ei.name.len==4 && !memcmp(ei.name.data,"temp",4)) temp_forceable = ei.forceable;
                break;
            default: tops++; break;
            } }
        CHECK(fns==6 && vars==4 && tops==0,
                 "reflect: local entities (fn=%d var=%d top=%d)", fns, vars, tops);
        CHECK(temp_forceable==1, "reflect: local forceability (temp allow_force=%d)", temp_forceable); } }

    { /* a reentrant set inside on_write commits to transport history before the outer set's
         deferred send, so the owner must skip the stale outer publish (spec/patterns.md) */
      RantVariable *ro, *ra; uint8_t b[4]; RantBytes cur;
      ro = rant_node_create_variable_definition(P, "rvar", NULL, NULL);
      ra = rant_node_create_remote_variable(C, "rvar", NULL, NULL);
      CHECK(ro && ra, "var: reentrancy pair created");
      pf_reent_var = ro; pf_reent_done = 0;
      rant_variable_on_write(ro, pf_on_var_reenter, NULL);
      i_rant_le_w32(b,100);
      rant_variable_set(ro, rant_bytes(b,4));
      CHECK(pf_reent_done, "var: reentrant set ran inside on_write");
      { RantBytes ov; int have = rant_variable_get(ro, &ov);
        CHECK(have && ov.len>=4 && i_rant_le_r32(ov.data)==200,
                 "var: owner store holds the newest"); }
      PUMP_UNTIL(P, C, 8000, rant_variable_get(ra, &cur) && cur.len>=4 && i_rant_le_r32(cur.data)==200);
      { int have = rant_variable_get(ra, &cur);
        CHECK(have && cur.len>=4 && i_rant_le_r32(cur.data)==200,
                 "var: remote reached the newest value"); }
      test_pump(P,C,60);   /* nothing stale may follow: the owner skipped the outer publish */
      { int have = rant_variable_get(ra, &cur);
        CHECK(have && cur.len>=4 && i_rant_le_r32(cur.data)==200,
                 "var: stale outer publish skipped, remote stays newest (%u)",
                 (have && cur.len>=4) ? i_rant_le_r32(cur.data) : 0u); }
      rant_variable_on_write(ro, NULL, NULL); }

    { /* the ring is the repair window, not the replay window: a reliable write from a callback
         cannot wait for a TX pass, so every write of the burst must fit the value channel's history */
      RantVariable *bo, *ba; uint8_t b[4]; uint32_t ev0, want = (1u<<PF_VBURST_N) - 1u;
      bo = rant_node_create_variable_definition(P, "bvar", NULL, NULL);
      ba = rant_node_create_remote_variable(C, "bvar", NULL, NULL);
      CHECK(bo && ba, "var: burst pair created");
      pf_vburst_var = bo; pf_vburst_ran = 0; pf_vburst_seen = 0;
      rant_variable_on_write(ba, pf_on_var_burst_rx, NULL);
      PUMP_UNTIL(P, C, 8000, rant_variable_match_count(bo)!=0);
      CHECK(rant_variable_match_count(bo)==1, "var: burst remote matched (%d)",
               rant_variable_match_count(bo));
      ev0 = rant_node_evicted_unsent(P);
      rant_variable_on_write(bo, pf_on_var_burst, NULL);
      i_rant_le_w32(b,0); rant_variable_set(bo, rant_bytes(b,4));         /* fires the burst inline */
      CHECK(pf_vburst_ran, "var: burst ran inside on_write");
      PUMP_UNTIL(P, C, 8000, pf_vburst_seen==want);
      CHECK(pf_vburst_seen==want, "var: every reentrant write reached the remote (0x%02x of 0x%02x)",
               pf_vburst_seen, want);
      CHECK(rant_node_evicted_unsent(P)==ev0, "var: burst evicted nothing unsent (%u)",
               rant_node_evicted_unsent(P) - ev0);
      rant_variable_on_write(bo, NULL, NULL); rant_variable_on_write(ba, NULL, NULL); }

    { /* a variable is state: a burst far past the ring never waits on a reader that is not
         acking, and the reader still ends on the newest value (spec/patterns.md) */
      RantVariable *lo, *la; uint8_t b[4]; RantBytes cur; uint64_t t0, el; uint32_t i2;
      lo = rant_node_create_variable_definition(P, "lvar", NULL, NULL);
      la = rant_node_create_remote_variable(C, "lvar", NULL, NULL);
      CHECK(lo && la, "var: latest pair created");
      PUMP_UNTIL(P, C, 8000, rant_variable_match_count(lo)!=0);
      t0 = i_rant_plat_now_us();
      for (i2=0;i2<200;i2++){ i_rant_le_w32(b,1000u+i2); rant_variable_set(lo, rant_bytes(b,4)); }
      el = i_rant_plat_now_us() - t0;
      CHECK(el < 500000u, "var: 200 writes past a silent reader never wait (%.1f ms)", el/1000.0);
      PUMP_UNTIL(P, C, 8000, rant_variable_get(la, &cur) && cur.len>=4 && i_rant_le_r32(cur.data)==1199u);
      { int have = rant_variable_get(la, &cur);
        CHECK(have && cur.len>=4 && i_rant_le_r32(cur.data)==1199u,
                 "var: the reader ends on the newest value (%u)",
                 (have && cur.len>=4) ? i_rant_le_r32(cur.data) : 0u); } }

    { /* provider side reply burst: an inline reply is a reentrant send, so the rsp ring is the
         only thing between a batch drained in one pass and lost replies (spec/testing.md) */
      uint8_t req[4]; int i2; uint32_t expect_sum, ev0;
      RantFunction *deep_p, *deep_c;
      pf_burst_done=0; pf_burst_sum=0; pf_burst_bad=0; expect_sum=0;
      for (i2=0;i2<8;i2++){
          i_rant_le_w32(req,(uint32_t)(2000+i2)); expect_sum += (uint32_t)(2000+i2+1);
          rant_function_call_async(call_add, rant_bytes(req,4), pf_on_reply_burst, NULL, NULL);
      }
      for (i2=0;i2<50;i2++) rant_node_poll(C,0);     /* park every request: P has not polled */
      ev0 = rant_node_evicted_unsent(P);
      rant_node_poll(P,20);                          /* ONE pass: drain 8, reply inline to each */
      PUMP_UNTIL(P, C, 8000, pf_burst_done >= 8);
      CHECK(pf_burst_done==8 && pf_burst_bad==0 && pf_burst_sum==expect_sum,
               "patterns: 8 inline replies in one pass all returned (done=%d bad=%d sum=%u want=%u)",
               pf_burst_done, pf_burst_bad, pf_burst_sum, expect_sum);
      CHECK(rant_node_evicted_unsent(P)==ev0, "patterns: reply burst evicted nothing unsent (%u)",
               rant_node_evicted_unsent(P) - ev0);

      deep_p = rant_node_create_function_definition(P, "deep", NULL, NULL, pf_add_handler, NULL,
                              &(RantFunctionOpts){ .keep_last = 32 });
      /* the caller needs the depth too: its req ring holds the burst while the provider has
         not polled, and at the default depth calls past it stall in backpressure */
      deep_c = rant_node_create_remote_function(C, "deep", NULL, NULL,
                              &(RantFunctionOpts){ .keep_last = 32 });
      CHECK(deep_p && deep_c, "patterns: deep-ring function pair created");
      /* both lanes must be up: the caller's req match alone would let the first replies
         commit before the provider's rsp lane exists, and rsp carries no catch_up */
      PUMP_UNTIL(P, C, 8000, rant_function_match_count(deep_c) && rant_function_match_count(deep_p));
      CHECK(rant_function_match_count(deep_c) && rant_function_match_count(deep_p),
               "patterns: deep-ring pair matched both ways (c=%d p=%d)",
               rant_function_match_count(deep_c), rant_function_match_count(deep_p));
      test_pump(P,C,50);
      { long long before = test_atomic_load(&pf_calls);
      pf_burst_done=0; pf_burst_sum=0; pf_burst_bad=0; expect_sum=0;
      for (i2=0;i2<24;i2++){
          i_rant_le_w32(req,(uint32_t)(3000+i2)); expect_sum += (uint32_t)(3000+i2+1);
          rant_function_call_async(deep_c, rant_bytes(req,4), pf_on_reply_burst, NULL, NULL);
      }
      for (i2=0;i2<80;i2++) rant_node_poll(C,0);
      rant_node_poll(P,20);
      PUMP_UNTIL(P, C, 8000, pf_burst_done >= 24);
      CHECK(pf_burst_done==24 && pf_burst_bad==0 && pf_burst_sum==expect_sum,
               "patterns: keep_last=32 carries a 24-reply pass (handled=%lld done=%d bad=%d laststatus=%d sum=%u want=%u)",
               test_atomic_load(&pf_calls) - before, pf_burst_done, pf_burst_bad, (int)pf_burst_last_bad, pf_burst_sum, expect_sum); } }

    /* a call still pending when the node closes gets one synthesized CANCELLED outcome */
    { RantFunction *never = rant_node_create_remote_function(C, "never-served", NULL, NULL,
                              &(RantFunctionOpts){ .timeout_us = 60000000u });
      CHECK(never != NULL, "patterns: cancel-at-close remote created");
      pf_cancel_count = 0; pf_cancel_status = RANT_CALL_OK;
      if (never) rant_function_call_async(never, rant_bytes(NULL,0), pf_on_cancel, NULL, NULL); }

    rant_node_close(P,0); rant_node_close(C,0);
    CHECK(pf_cancel_count==1 && pf_cancel_status==RANT_CALL_CANCELLED,
             "patterns: pending call cancelled at close (n=%d status=%d)",
             pf_cancel_count, (int)pf_cancel_status);
    rant_allocator_reset(&pa); rant_allocator_reset(&ca);
}

/* The duplicate authority diagnostic: one provider per function and one owner per
 * variable, the conflict detected off the announce interest on both rivals. */
static void dup_on_event(const RantEvent *ev){
    if (ev->kind == RANT_ERROR && ev->error == RANT_E_DUPLICATE_AUTHORITY)
        ++*(int*)ev->user;
}
static void dup_authority_checks(void){
    RantAllocator aa = rant_allocator_heap(0);
    RantAllocator ba = rant_allocator_heap(0);
    RantNodeOpts ao, bo; RantNode *A, *B; RantAddr seed;
    int a_dups = 0, b_dups = 0, t;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&ao,0,sizeof ao); ao.domain=TEST_DOMAIN+21; ao.discovery.max_peers=4;
    ao.net.multicast_interface="127.0.0.1"; ao.net.seed_peers=&seed; ao.net.n_seed_peers=1;
    bo=ao;
    ao.user_data=&a_dups; bo.user_data=&b_dups;
    A = rant_node_open(&aa, "dup-a", NULL, dup_on_event, &ao);
    B = rant_node_open(&ba, "dup-b", NULL, dup_on_event, &bo);
    CHECK(A && B, "dup: nodes open");
    if (!(A && B)){ if(A)rant_node_close(A,0); if(B)rant_node_close(B,0); return; }

    { RantVariable *va = rant_node_create_variable_definition(A, "dupvar", NULL, NULL);
      RantFunction *fa = rant_node_create_function_definition(A, "dupfn", NULL, NULL, pf_empty_handler, NULL, NULL);
      CHECK(va && fa, "dup: first authorities created");
      /* B knows A but shares nothing with it, so it holds no slot or blob of A yet */
      { RantIter it; RantPeerInfo p; int seen = 0;
        for (t=0;t<2000 && !seen;t++){
            test_pump(A,B,2);
            memset(&it,0,sizeof it);
            while (rant_node_peers_next(B, &it, &p))
                if (p.liveness == RANT_PEER_ACTIVE && p.name.len == 5 && !memcmp(p.name.data, "dup-a", 5)) seen = 1;
        }
        CHECK(seen, "dup: B knows A"); }
      CHECK(a_dups==0 && b_dups==0, "dup: quiet before the rival (a=%d b=%d)", a_dups, b_dups);

      /* the rival key fetches A's blob again, and judging it reports the rival */
      { RantVariable *vb = rant_node_create_variable_definition(B, "dupvar", NULL, NULL);
        CHECK(vb!=NULL, "dup: rival owner creates (diagnostic, not a refusal)");
        PUMP_UNTIL(A, B, 2000, b_dups >= 1);
        CHECK(b_dups==1, "dup: rival owner detected once A is judged again (%d)", b_dups); }
      { RantFunction *fb = rant_node_create_function_definition(B, "dupfn", NULL, NULL, pf_empty_handler, NULL, NULL);
        CHECK(fb!=NULL, "dup: rival provider creates");
        CHECK(b_dups==2, "dup: rival provider detected at create (%d)", b_dups); }
      PUMP_UNTIL(A, B, 8000, a_dups >= 2);
      CHECK(a_dups==2, "dup: first authority sees the rival's announce (%d)", a_dups);

      /* dedup + negative: repeated announces re-report nothing, and an accessor pairing
         with an owner is the LEGAL shape (sub side, no authority claim), never flagged */
      { RantVariable *acc    = rant_node_create_remote_variable(B, "solo", NULL, NULL);
        RantVariable *solo = rant_node_create_variable_definition(A, "solo", NULL, NULL);
        CHECK(acc && solo, "dup: solo owner + accessor create");
        test_pump(A, B, 300);
        CHECK(a_dups==2 && b_dups==2,
                 "dup: once per (entity, peer); accessor never fires (a=%d b=%d)", a_dups, b_dups); } }

    rant_node_close(A,0); rant_node_close(B,0);
    rant_allocator_reset(&aa); rant_allocator_reset(&ba);
}

/* The pattern retire lifecycle: a second same name handle is shadowed while the
 * first lives, and retire parks the predecessor so a re created handle receives. */
static void retire_checks(void){
    RantAllocator aa = rant_allocator_heap(0);
    RantAllocator ba = rant_allocator_heap(0);
    RantNodeOpts ao, bo; RantNode *A, *B; RantAddr seed;
    int t;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&ao,0,sizeof ao); ao.domain=TEST_DOMAIN+22; ao.discovery.max_peers=4;
    ao.net.multicast_interface="127.0.0.1"; ao.net.seed_peers=&seed; ao.net.n_seed_peers=1;
    bo=ao;
    A = rant_node_open(&aa, "retire-a", NULL, NULL, &ao);
    B = rant_node_open(&ba, "retire-b", NULL, NULL, &bo);
    CHECK(A && B, "retire: nodes open");
    if (!(A && B)){ if(A)rant_node_close(A,0); if(B)rant_node_close(B,0); return; }

    /* variable: retire + recreate the accessor, then the definition */
    { RantVariable *def, *acc, *acc2; RantBytes gv; uint8_t b[4]; int rr;
      i_rant_le_w32(b, 20);
      def = rant_node_create_variable_definition(A, "dial", NULL,
                &(RantVariableOpts){ .initial = rant_bytes(b,4) });
      acc = rant_node_create_remote_variable(B, "dial", NULL, NULL);
      CHECK(def && acc, "retire: variable pair created");
      PUMP_UNTIL(A, B, 8000, rant_variable_get(acc,&gv));
      CHECK(rant_variable_get(acc,&gv) && gv.len==4 && i_rant_le_r32(gv.data)==20,
               "retire: first accessor got the value");
      rr = rant_variable_retire(acc);
      CHECK(rr==RANT_OK, "retire: accessor retired (%d)", rr);
      acc2 = rant_node_create_remote_variable(B, "dial", NULL, NULL);
      CHECK(acc2 != NULL, "retire: successor accessor created");
      PUMP_UNTIL(A, B, 8000, rant_variable_get(acc2,&gv));
      CHECK(rant_variable_get(acc2,&gv) && gv.len==4 && i_rant_le_r32(gv.data)==20,
               "retire: successor RECEIVES where a live twin would be shadowed");
      i_rant_le_w32(b, 33);
      { int sr = rant_variable_set(acc2, rant_bytes(b,4));
        CHECK(sr==RANT_OK, "retire: successor set accepted (%d)", sr); }
      PUMP_UNTIL(A, B, 8000, rant_variable_get(def,&gv)&&gv.len==4&&i_rant_le_r32(gv.data)==33);
      CHECK(rant_variable_get(def,&gv)&&gv.len==4&&i_rant_le_r32(gv.data)==33,
               "retire: successor write reached the definition (33)");
      /* definition retire + recreate: the restart-shaped recipe (the old channel must
         stop matching-and-refusing so the successor can serve) */
      rr = rant_variable_retire(def);
      CHECK(rr==RANT_OK, "retire: definition retired (%d)", rr);
      i_rant_le_w32(b, 77);
      def = rant_node_create_variable_definition(A, "dial", NULL,
                &(RantVariableOpts){ .initial = rant_bytes(b,4) });
      CHECK(def != NULL, "retire: successor definition created");
      PUMP_UNTIL(A, B, 8000, rant_variable_get(acc2,&gv)&&gv.len==4&&i_rant_le_r32(gv.data)==77);
      CHECK(rant_variable_get(acc2,&gv)&&gv.len==4&&i_rant_le_r32(gv.data)==77,
               "retire: successor definition serves the accessor (77)");
      /* retire from inside a callback: refused, handle stays valid (on_change replays
         inline at registration, so the attempt runs right here on this thread) */
      pf_retire_in_cb_rc = 1234;
      rant_variable_on_change(def, pf_on_var_retire_attempt, NULL);
      CHECK(pf_retire_in_cb_rc==RANT_ERR_STATE,
               "retire: refused from a callback (%d)", pf_retire_in_cb_rc);
      rant_variable_on_change(def, NULL, NULL);
      { RantBytes chk; CHECK(rant_variable_get(def,&chk)==1,
               "retire: handle survives the refused attempt"); } }

    /* function: an outstanding call cancels at retire, a retired then recreated remote calls */
    { RantFunction *fdef, *rem, *rem2, *never; uint8_t req[4]; int rr;
      fdef = rant_node_create_function_definition(A, "calc", NULL, NULL, pf_add_handler, NULL, NULL);
      rem  = rant_node_create_remote_function(B, "calc", NULL, NULL, NULL);
      CHECK(fdef && rem, "retire: function pair created");
      PUMP_UNTIL(A, B, 8000, rant_function_match_count(rem)!=0);
      never = rant_node_create_remote_function(B, "retire-never", NULL, NULL,
                &(RantFunctionOpts){ .timeout_us = 60000000u });
      CHECK(never != NULL, "retire: never-served remote created");
      pf_cancel_count = 0; pf_cancel_status = RANT_CALL_OK;
      if (never){
          rant_function_call_async(never, rant_bytes(NULL,0), pf_on_cancel, NULL, NULL);
          rr = rant_function_retire(never);
          CHECK(rr==RANT_OK && pf_cancel_count==1 && pf_cancel_status==RANT_CALL_CANCELLED,
                   "retire: outstanding call cancelled (rc=%d n=%d st=%d)",
                   rr, pf_cancel_count, (int)pf_cancel_status); }
      rr = rant_function_retire(rem);
      CHECK(rr==RANT_OK, "retire: remote function retired (%d)", rr);
      rem2 = rant_node_create_remote_function(B, "calc", NULL, NULL, NULL);
      CHECK(rem2 != NULL, "retire: successor remote created");
      pf_reply_done = 0;
      i_rant_le_w32(req, 41);
      rant_function_call_async(rem2, rant_bytes(req,4), pf_on_reply, NULL, NULL);
      PUMP_UNTIL(A, B, 8000, pf_reply_done);
      CHECK(pf_reply_done && pf_reply_status==RANT_CALL_OK && pf_reply_val==42,
               "retire: successor remote calls (done=%d st=%d val=%u)",
               pf_reply_done, pf_reply_status, pf_reply_val); }

    rant_node_close(A,0); rant_node_close(B,0);
    rant_allocator_reset(&aa); rant_allocator_reset(&ba);
}

static volatile int loud_reply_n;
static volatile int loud_reply_status;
static void loud_on_reply(const RantResponse *r){ loud_reply_status = (int)r->status; loud_reply_n++; }
static int loud_active_peers(RantNode *n){
    RantIter it; RantPeerInfo p; int c = 0;
    memset(&it, 0, sizeof it);
    while (rant_node_peers_next(n, &it, &p)) if (p.liveness == RANT_PEER_ACTIVE) c++;
    return c;
}

/* Loud refusals: a refused create says why and keeps its strings, a live same name
 * twin is refused, a typed topic refuses bytes its schema cannot read, an accessor's
 * force against an owner without allow_force is refused with nothing sent, and the
 * blocking call and the variable wait work under a service thread. */
static void loud_checks(void){
    RantAllocator aa = rant_allocator_heap(0);
    RantAllocator ba = rant_allocator_heap(0);
    RantNodeOpts ao, bo; RantNode *A, *B; RantAddr seed; RantEvent le;
    RantSchema *pose; char txt[192]; int t;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&ao,0,sizeof ao); ao.domain=TEST_DOMAIN+23; ao.discovery.max_peers=4;
    ao.net.multicast_interface="127.0.0.1"; ao.net.seed_peers=&seed; ao.net.n_seed_peers=1;
    ao.match_wait_ms = -1;      /* the sends below reach nobody on purpose: no wait */
    bo=ao;
    A = rant_node_open(&aa, "loud-a", NULL, NULL, &ao);
    B = rant_node_open(&ba, "loud-b", NULL, NULL, &bo);
    CHECK(A && B, "loud: nodes open");
    if (!(A && B)){ if(A)rant_node_close(A,0); if(B)rant_node_close(B,0); return; }

    le = rant_last_error(A);
    rant_event_str(&le, txt, sizeof txt);
    CHECK(le.kind==RANT_ERROR && le.error==RANT_E_NONE && strcmp(txt,"no error")==0,
             "loud: a fresh node's last error reads \"no error\" (%s)", txt);
    CHECK(rant_node_create_topic(A, "a@b", RANT_PUBSUB, NULL, NULL)==NULL
             && rant_last_error(A).error==RANT_E_BAD_NAME,
             "loud: '@' in a plain topic name refused as BAD_NAME (%d)", (int)rant_last_error(A).error);
    le = rant_last_error(A);
    CHECK(le.topic_name && strcmp(le.topic_name,"a@b")==0,
             "loud: the last error keeps the name (%s)", le.topic_name ? le.topic_name : "null");

    pose = test_compile(&aa, "LoudPose { x: f32, y: f32 }", NULL);
    CHECK(pose != NULL, "loud: schema compiled");
    {   RantTopic *tp = rant_node_create_topic(A, "loud/pose", RANT_PUB_ONLY, pose, NULL);
        RantTopic *twin = rant_node_create_topic(A, "loud/pose", RANT_SUB_ONLY, pose, NULL);
        RantTopic *parked;
        uint8_t junk[3] = {1,2,3}, good[8] = {0};
        int rc_junk, rc_good;
        CHECK(tp && twin==NULL && rant_last_error(A).error==RANT_E_NAME_COLLISION,
                 "loud: a live same name topic is refused as NAME_COLLISION (%d)", (int)rant_last_error(A).error);
        parked = rant_node_create_topic(A, "loud/pose", RANT_INACTIVE, pose, NULL);
        CHECK(parked != NULL, "loud: an INACTIVE twin is still accepted (the QoS switch)");
        le = rant_last_error(A);
        rant_event_str(&le, txt, sizeof txt);
        CHECK(le.peer==0 && strstr(txt,"already created")!=NULL,
                 "loud: the collision text names this node (%s)", txt);
        rc_junk = tp ? rant_topic_send(tp, rant_bytes(junk,3), NULL) : 0;
        rc_good = tp ? rant_topic_send(tp, rant_bytes(good,8), NULL) : 0;
        CHECK(rc_junk==RANT_ERR_SCHEMA, "loud: bytes the schema cannot read refused with RANT_ERR_SCHEMA (%d)", rc_junk);
        CHECK(rc_good==RANT_OK, "loud: a well formed message still sends (%d)", rc_good);
    }
    {   /* pattern twins and the accessor force gate */
        RantVariable *v1, *v2, *acc, *knob, *kacc; RantBytes gv; uint8_t b[4]; int rc;
        i_rant_le_w32(b, 20);
        v1 = rant_node_create_variable_definition(A, "loud/dial", NULL,
                 &(RantVariableOpts){ .initial = rant_bytes(b,4) });
        v2 = rant_node_create_variable_definition(A, "loud/dial", NULL, NULL);
        CHECK(v1 && v2==NULL && rant_last_error(A).error==RANT_E_NAME_COLLISION,
                 "loud: a live same name variable definition is refused (%d)", (int)rant_last_error(A).error);
        knob = rant_node_create_variable_definition(A, "loud/knob", NULL,
                 &(RantVariableOpts){ .initial = rant_bytes(b,4), .allow_force = 1 });
        acc  = rant_node_create_remote_variable(B, "loud/dial", NULL, NULL);
        kacc = rant_node_create_remote_variable(B, "loud/knob", NULL, NULL);
        CHECK(knob && acc && kacc, "loud: variable pairs created");
        PUMP_UNTIL(A, B, 8000, rant_variable_get(acc,&gv) && rant_variable_get(kacc,&gv));
        CHECK(rant_variable_get(acc,&gv) && rant_variable_get(kacc,&gv), "loud: accessors got the values");
        i_rant_le_w32(b, 99);
        rc = rant_variable_force(acc, rant_bytes(b,4));
        CHECK(rc==RANT_ERR_ROLE, "loud: force against an owner without allow_force refused ROLE (%d)", rc);
        rc = rant_variable_unforce(acc);
        CHECK(rc==RANT_ERR_ROLE, "loud: unforce likewise (%d)", rc);
        test_pump(A, B, 400);
        CHECK(v1 && !rant_variable_forced(v1), "loud: nothing reached the owner");
        rc = rant_variable_force(kacc, rant_bytes(b,4));
        CHECK(rc==RANT_OK, "loud: force against a forceable owner sends (%d)", rc);
        PUMP_UNTIL(A, B, 8000, knob && rant_variable_forced(knob));
        CHECK(knob && rant_variable_forced(knob), "loud: the forceable owner is forced");
    }
    {   /* a queued call: an unrelated peer leaving is not its loss, its deadline ends it
           NO_PROVIDER, and the blocking form reports that outcome */
        RantAllocator ca = rant_allocator_heap(0); RantNodeOpts co = ao; RantNode *C;
        RantFunction *nobody; RantResponse rep; int rc;
        C = rant_node_open(&ca, "loud-c", NULL, NULL, &co);
        nobody = rant_node_create_remote_function(B, "loud/nobody", NULL, NULL,
                     &(RantFunctionOpts){ .timeout_us = 800000u });   /* twice the window below */
        CHECK(C && nobody, "loud: third node and never served remote created");
        for (t=0;t<3000 && loud_active_peers(B) < 2;t++){ test_pump(A,B,1); if (C) rant_node_poll(C,1); }
        CHECK(loud_active_peers(B) >= 2, "loud: B sees both peers (%d)", loud_active_peers(B));
        loud_reply_n = 0; loud_reply_status = -1;
        rc = nobody ? rant_function_call_async(nobody, rant_bytes(NULL,0), loud_on_reply, NULL, NULL) : -99;
        CHECK(rc==RANT_OK, "loud: call queued with no provider (%d)", rc);
        if (C){ rant_node_close(C,1); C = NULL; }
        test_pump(A, B, 400);
        CHECK(loud_active_peers(B) == 1 && loud_reply_n == 0,
                 "loud: the unrelated peer left and the queued call still waits (peers=%d n=%d)",
                 loud_active_peers(B), loud_reply_n);
        PUMP_UNTIL(A, B, 8000, loud_reply_n!=0);
        CHECK(loud_reply_n == 1 && loud_reply_status == (int)RANT_CALL_NO_PROVIDER,
                 "loud: the queued call ended NO_PROVIDER at its deadline (n=%d st=%d)",
                 loud_reply_n, loud_reply_status);
        memset(&rep, 0, sizeof rep);
        rc = nobody ? rant_function_call(nobody, rant_bytes(NULL,0), &rep, -1, NULL) : -99;
        CHECK(rc==1 && rep.status==RANT_CALL_NO_PROVIDER && rep.message.len==11
                 && memcmp(rep.message.data, "no provider", 11)==0,
                 "loud: the blocking call reports NO_PROVIDER (rc=%d st=%d)", rc, (int)rep.status);
        rant_allocator_reset(&ca);
    }
#ifdef RANT_THREADS
    {   /* the blocking forms under a service thread: sleep on its progress, no refusal */
        RantFunction *fdef, *rem; RantVariable *late, *lacc; RantResponse rep;
        uint8_t req[4], b[4]; int rc, s1, s2;
        fdef = rant_node_create_function_definition(A, "loud/add", NULL, NULL, pf_add_handler, NULL, NULL);
        rem  = rant_node_create_remote_function(B, "loud/add", NULL, NULL, NULL);
        lacc = rant_node_create_remote_variable(B, "loud/late", NULL, NULL);
        CHECK(fdef && rem && lacc, "loud: threaded pairs created");
        s1 = rant_node_start(A); s2 = rant_node_start(B);
        CHECK(s1==RANT_OK && s2==RANT_OK, "loud: service threads start");
        i_rant_le_w32(req, 41);
        memset(&rep, 0, sizeof rep);
        rc = rem ? rant_function_call(rem, rant_bytes(req,4), &rep, 5000, NULL) : -99;
        CHECK(rc==1 && rep.status==RANT_CALL_OK && rep.data.len==4 && i_rant_le_r32(rep.data.data)==42,
                 "loud: blocking call answers under a service thread (rc=%d st=%d)", rc, (int)rep.status);
        i_rant_le_w32(b, 5);
        late = rant_node_create_variable_definition(A, "loud/late", NULL,
                   &(RantVariableOpts){ .initial = rant_bytes(b,4) });
        rc = (late && lacc) ? rant_variable_wait(lacc, 5000) : -99;
        CHECK(rc==1, "loud: variable wait returns under a service thread (%d)", rc);
        rant_node_stop(A); rant_node_stop(B);
    }
#endif
    rant_node_close(A,0); rant_node_close(B,0);
    rant_allocator_reset(&aa); rant_allocator_reset(&ba);
}

/* How many of a peer's entities carry name, and whether one of them is a complete kind. */
static int rr_count(RantNode *obs, uint32_t pid, const char *name, RantEntityKind kind, int *complete){
    RantIter it; RantEntityInfo ei; size_t len = strlen(name); int n = 0;
    *complete = 0;
    memset(&it,0,sizeof it);
    while (rant_node_entities_next(obs, pid, &it, &ei)){
        if (ei.name.len != len || memcmp(ei.name.data, name, len) != 0) continue;
        n++;
        if (ei.kind == kind && !ei.incomplete) *complete = 1;
    }
    return n;
}

/* Index reuse: a retired slot rebound to another channel must drop the old name and
 * schema, or an observer shows the new channel under the old name until it restarts. */
static void reflect_reuse_checks(void){
    RantAllocator aa = rant_allocator_heap(0);
    RantAllocator ba = rant_allocator_heap(0);
    RantNodeOpts ao, bo; RantNode *A, *B; RantAddr seed;
    RantTopic *joints; RantFunction *fn; uint32_t pid = 0; int t, ok, n, i;
    char name[64];
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&ao,0,sizeof ao); ao.domain=TEST_DOMAIN+37; ao.discovery.max_peers=4;
    ao.net.multicast_interface="127.0.0.1"; ao.net.seed_peers=&seed; ao.net.n_seed_peers=1;
    bo=ao;
    bo.fetch_details = 1;
    A = rant_node_open(&aa, "reuse-host", NULL, NULL, &ao);
    B = rant_node_open(&ba, "reuse-obs",  NULL, NULL, &bo);
    CHECK(A && B, "reuse: nodes open");
    if (!(A && B)){ if(A)rant_node_close(A,0); if(B)rant_node_close(B,0); return; }

    joints = rant_node_create_topic(A, "rr/joints", RANT_PUB_ONLY, NULL, NULL);
    CHECK(joints != NULL, "reuse: topic created");
    for (t=0, ok=0; t<2000 && !ok; t++){
        const i_RantDiscoveryPeerView *ps; uint16_t pc;
        test_pump(A,B,2);
        ps = test_peers(B, &pc);
        if (!(ps && pc)) continue;
        pid = ps[0].id;
        rr_count(B, pid, "rr/joints", RANT_ENTITY_TOPIC, &ok);
    }
    CHECK(ok, "reuse: observer names the topic");

    CHECK(rant_topic_retire(joints) == RANT_OK, "reuse: topic retired");
    fn = rant_node_create_function_definition(A, "rr/call", NULL, NULL, pf_add_handler, NULL, NULL);
    CHECK(fn != NULL, "reuse: function takes the retired slot");
    for (t=0, ok=0; t<2000 && !ok; t++){ test_pump(A,B,2); rr_count(B, pid, "rr/call", RANT_ENTITY_FUNCTION, &ok); }
    n = rr_count(B, pid, "rr/joints", RANT_ENTITY_FUNCTION, &i);
    CHECK(ok, "reuse: the function shows whole under its own name");
    CHECK(n == 0, "reuse: nothing keeps the old name (%d)", n);

    /* every rebind appends a name, so this crosses the arena's compaction threshold */
    if (fn) rant_function_retire(fn);
    for (i=0, ok=1; i<48 && ok; i++){
        RantTopic *tp;
        snprintf(name, sizeof name, "rr/churn/a-long-enough-name-to-grow-the-arena/%02d", i);
        tp = rant_node_create_topic(A, name, RANT_PUB_ONLY, NULL, NULL);
        for (t=0, ok=0; tp && t<2000 && !ok; t++){ test_pump(A,B,2); rr_count(B, pid, name, RANT_ENTITY_TOPIC, &ok); }
        if (tp) rant_topic_retire(tp);
    }
    CHECK(ok && i == 48, "reuse: 48 rebinds each show their own name (stopped at %d)", i);

    rant_node_close(A,0); rant_node_close(B,0);
    rant_allocator_reset(&aa); rant_allocator_reset(&ba);
}

/* The variable set match wait: a fresh accessor's first write rides the send path's
 * match wait, so NO_TOPIC means the owner is genuinely absent. */
static void varwait_checks(void){
    RantAllocator aa = rant_allocator_heap(0);
    RantAllocator ba = rant_allocator_heap(0);
    RantNodeOpts ao, bo; RantNode *P, *C; RantAddr seed;
    RantVariable *vd; int t;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&ao,0,sizeof ao); ao.domain=TEST_DOMAIN+31; ao.discovery.max_peers=4;
    ao.net.multicast_interface="127.0.0.1"; ao.net.seed_peers=&seed; ao.net.n_seed_peers=1;
    bo=ao;
    P = rant_node_open(&aa, "vw-owner",    NULL, NULL, &ao);
    C = rant_node_open(&ba, "vw-accessor", NULL, NULL, &bo);
    CHECK(P && C, "varwait: nodes open");
    if (!(P && C)){ if(P)rant_node_close(P,0); if(C)rant_node_close(C,0); return; }

    { uint8_t b[4]; i_rant_le_w32(b, 20);
      vd = rant_node_create_variable_definition(P, "vw", NULL,
               &(RantVariableOpts){ .initial = rant_bytes(b,4) });
      CHECK(vd != NULL, "varwait: definition created"); }

    /* C knows P before the accessor exists, judged then as sharing nothing, so the
       accessor's first write waits for P to be judged again */
    { RantIter it; RantPeerInfo p; int seen = 0;
      for (t=0;t<2000 && !seen;t++){
          test_pump(P,C,2);
          memset(&it,0,sizeof it);
          while (rant_node_peers_next(C, &it, &p))
              if (p.liveness == RANT_PEER_ACTIVE && p.name.len == 8 && !memcmp(p.name.data, "vw-owner", 8)) seen = 1;
      }
      CHECK(seen, "varwait: C knows P"); }

    /* From here P answers only from its service thread and C is driven solely by the wait
       inside the set call. Create the accessor and write immediately. */
    rant_node_start(P);
    { RantVariable *va = rant_node_create_remote_variable(C, "vw", NULL, NULL);
      uint8_t b[4]; int sr; RantBytes gv;
      CHECK(va != NULL, "varwait: accessor created");
      i_rant_le_w32(b, 5);
      sr = rant_variable_set(va, rant_bytes(b,4));
      CHECK(sr==RANT_OK, "varwait: first write waits out the forming match (%d)", sr);
      rant_node_stop(P);
      PUMP_UNTIL(P, C, 8000, rant_variable_get(vd,&gv)&&gv.len==4&&i_rant_le_r32(gv.data)==5);
      CHECK(rant_variable_get(vd,&gv)&&gv.len==4&&i_rant_le_r32(gv.data)==5,
               "varwait: the write reached the owner (5)"); }

    /* converged verdicts stay instant: an owner nobody offers fails NO_TOPIC without
       consuming the wait bound. Let the post open gather latch first, about 400 ms. */
    test_pump(P,C,400);
    { RantVariable *orphan = rant_node_create_remote_variable(C, "vw-nobody", NULL, NULL);
      uint8_t b[4]; int sr; uint64_t t0, dt_ms;
      CHECK(orphan != NULL, "varwait: orphan accessor created");
      i_rant_le_w32(b, 1);
      t0 = i_rant_plat_now_us();
      sr = orphan ? rant_variable_set(orphan, rant_bytes(b,4)) : 0;
      dt_ms = (i_rant_plat_now_us() - t0) / 1000u;
      CHECK(sr==RANT_ERR_NO_TOPIC, "varwait: absent owner still NO_TOPIC (%d)", sr);
      CHECK(dt_ms < 250, "varwait: converged verdict is instant (%u ms)", (unsigned)dt_ms); }

    rant_node_close(P,0); rant_node_close(C,0);
    rant_allocator_reset(&aa); rant_allocator_reset(&ba);
}

/* SYNC CAPTURE: a blocking call's response capture runs under the caller's lock, so the
   call sees it answered or still pending, never mid copy (spec/patterns.md) */
#define SC_BIG 200000
static RantNode *sc_caller;
static long long sc_armed, sc_unlocked;
static int sc_calls;
/* the caller's page hook: a page call without the caller's lock is counted, and the first
   stalls 300 ms, which once let a call time out while its capture still wrote its frame */
static void *sc_pages(void *ptr, size_t size){
    if (test_atomic_load(&sc_armed) && sc_caller
        && !(sc_caller->lock_held && sc_caller->lock_owner == i_rant_plat_thread_id())
        && test_atomic_add(&sc_unlocked, 1) == 1)
        test_sleep_ms(300);
    if (!size){ free(ptr); return NULL; }
    return realloc(ptr, size);
}
/* answers the first call with SC_BIG bytes of 'A' and never answers a later one */
static void sc_on_request(RantRequest *req, void *user){
    static uint8_t payload[SC_BIG];
    (void)user;
    if (++sc_calls == 1){
        memset(payload, 'A', sizeof payload);
        rant_request_reply(req, rant_bytes(payload, sizeof payload));
    } else (void)rant_request_defer(req);
}
static void sync_capture_checks(void){
#ifndef RANT_THREADS
    test_skip("needs service threads");
#else
    RantAllocator pa = rant_allocator_heap(0), ca = rant_allocator_dynamic(sc_pages, 0);
    RantNodeOpts o; RantNode *p, *c; RantFunction *def = NULL, *fn = NULL;
    RantResponse r1, r2; int rc1, rc2; uint64_t t1, t2;
    memset(&o, 0, sizeof o);
    o.domain = TEST_DOMAIN+40; o.disable_shm = 1; o.discovery.max_peers = 4;
    o.net.multicast_interface = "127.0.0.1";
    p = rant_node_open(&pa, "sc-provider", NULL, NULL, &o);
    c = rant_node_open(&ca, "sc-caller", NULL, NULL, &o);
    CHECK(p && c, "sync_capture: nodes open");
    if (p && c){
        def = rant_node_create_function_definition(p, "sc/fn", NULL, NULL, sc_on_request, NULL, NULL);
        fn = rant_node_create_remote_function(c, "sc/fn", NULL, NULL, NULL);
    }
    CHECK(def && fn, "sync_capture: function pair created");
    if (def && fn){
        rant_node_start(p); rant_node_start(c);
        WAIT_UNTIL(8000, rant_function_match_count(fn) > 0);
        CHECK(rant_function_match_count(fn) > 0, "sync_capture: caller matched");
        sc_caller = c; test_atomic_store(&sc_armed, 1);
        rc1 = rant_function_call(fn, rant_bytes("1", 1), &r1, 100, NULL);
        CHECK(rc1 == 1 ? r1.data.len == SC_BIG && r1.data.data[0] == 'A' : r1.status == RANT_CALL_TIMEOUT,
                 "sync_capture: the first call is answered whole or times out (rc=%d len=%u)",
                 rc1, (unsigned)r1.data.len);
        t1 = i_rant_plat_now_us();
        rc2 = rant_function_call(fn, rant_bytes("2", 1), &r2, 300, NULL);
        t2 = i_rant_plat_now_us();
        test_atomic_store(&sc_armed, 0);
        CHECK(rc2 == 0 && r2.data.len == 0 && t2 - t1 >= 300000u,
                 "sync_capture: an unanswered call runs its whole timeout (rc=%d len=%u after %.0f ms)",
                 rc2, (unsigned)r2.data.len, (t2 - t1) / 1000.0);
        CHECK(test_atomic_load(&sc_unlocked) == 0,
                 "sync_capture: the caller never allocates with its lock released (%lld)",
                 test_atomic_load(&sc_unlocked));
    }
    if (c) rant_node_close(c, 0);
    if (p) rant_node_close(p, 0);
    sc_caller = NULL;
    rant_allocator_reset(&ca); rant_allocator_reset(&pa);
#endif
}


/* The one announce phase: a pattern handle's channels reach peers together, so a provider
 * never knows a caller's req without its rsp and prg. Each create, and each step of a
 * retire, moves the announce version once however many channels it touches. */
static uint32_t oa_version(RantNode *n){ return i_rant_discovery_meta_version(i_rant_discovery_state(n->discovery)); }
static void oa_answer(RantRequest *r, void *user){ (void)user; rant_request_reply(r, rant_bytes(NULL, 0)); }
static void one_announce_checks(void){
    RantAllocator aa = rant_allocator_heap(0);
    RantNodeOpts o; RantNode *A; uint32_t v; RantFunction *f; RantVariable *var;
    memset(&o,0,sizeof o); o.domain=TEST_DOMAIN+24; o.net.multicast_interface="127.0.0.1"; o.max_topics=32;
    A = rant_node_open(&aa, "oa-a", NULL, NULL, &o);
    CHECK(A != NULL, "one_announce: node open");
    if (!A) return;
    v = oa_version(A);
    f = rant_node_create_remote_task(A, "oa/rt", NULL, NULL, NULL, NULL);
    CHECK(f && oa_version(A) == v + 1, "one_announce: a remote task's 3 channels in one announce (+%u)", oa_version(A) - v);
    v = oa_version(A);
    CHECK(rant_function_retire(f) == RANT_OK && oa_version(A) == v + 2,
             "one_announce: its retire parks all at once, then frees all at once (+%u)", oa_version(A) - v);
    v = oa_version(A);
    f = rant_node_create_task_definition(A, "oa/td", NULL, NULL, NULL, oa_answer, NULL, NULL);
    CHECK(f && oa_version(A) == v + 1, "one_announce: a task definition in one announce (+%u)", oa_version(A) - v);
    v = oa_version(A);
    CHECK(rant_node_create_remote_function(A, "oa/rf", NULL, NULL, NULL) && oa_version(A) == v + 1,
             "one_announce: a remote function in one announce (+%u)", oa_version(A) - v);
    v = oa_version(A);
    CHECK(rant_node_create_function_definition(A, "oa/fd", NULL, NULL, oa_answer, NULL, NULL) && oa_version(A) == v + 1,
             "one_announce: a function definition in one announce (+%u)", oa_version(A) - v);
    v = oa_version(A);
    var = rant_node_create_remote_variable(A, "oa/rv", NULL, NULL);
    CHECK(var && oa_version(A) == v + 1, "one_announce: a remote variable in one announce (+%u)", oa_version(A) - v);
    v = oa_version(A);
    CHECK(rant_variable_retire(var) == RANT_OK && oa_version(A) == v + 2,
             "one_announce: a variable retire in two whole steps (+%u)", oa_version(A) - v);
    v = oa_version(A);
    CHECK(rant_node_create_topic(A, "oa/t", RANT_PUB_ONLY, NULL, NULL) && oa_version(A) == v + 1,
             "one_announce: a plain topic still announces at once (+%u)", oa_version(A) - v);
    rant_node_close(A, 0);
    rant_allocator_reset(&aa);
}

/* The handle gate: a differing caller req schema sized so its detail answer holds req alone
 * and rsp follows a page later. No request may run before its reply lane exists. */
#define HG_N 20
static RantFunction *hg_defs[HG_N];
static long long hg_handled, hg_no_lane, hg_unmatched, hg_ok, hg_bad;
static void hg_event(const RantEvent *e){
    if (e->kind == RANT_ERROR && e->error == RANT_E_UNMATCHED_SEND) test_atomic_add(&hg_unmatched, 1);
}
static void hg_handler(RantRequest *r, void *user){
    test_atomic_add(&hg_handled, 1);
    if (rant_function_match_count(hg_defs[(intptr_t)user]) == 0) test_atomic_add(&hg_no_lane, 1);
    rant_request_reply(r, rant_bytes(NULL, 0));
}
static void hg_on_response(const RantResponse *r){
    test_atomic_add(r->status == RANT_CALL_OK ? &hg_ok : &hg_bad, 1);
}
static RantSchema *hg_schema(RantAllocator *a, int nf, int pad){
    static char txt[8192]; size_t o = 0; int i;
    o += (size_t)sprintf(txt + o, "Req { a: u32");
    for (i = 0; i < nf; i++) o += (size_t)sprintf(txt + o, ", extra_field_number_%03d_with_a_long_name: u32", i);
    if (pad){ o += (size_t)sprintf(txt + o, ", p"); for (i = 0; i < pad; i++) txt[o++] = 'x'; o += (size_t)sprintf(txt + o, ": u32"); }
    sprintf(txt + o, " }");
    return test_compile(a, txt, NULL);
}
static void handle_gate_checks(void){
    RantAllocator aa = rant_allocator_heap(0), ba = rant_allocator_heap(0), ma = rant_allocator_heap(0);
    RantNodeOpts ao, bo; RantNode *A, *B; RantAddr seed;
    RantSchema *small, *big = NULL; uint8_t *payload = NULL; int i, nf, pad, issued = 0;
    small = test_compile(&ma, "Req { a: u32 }", NULL);
    /* 1325 to 1350 wire bytes: the req entry fills one 1390 byte page, rsp cannot follow */
    for (nf = 0; nf < 64 && !big; nf++){
        RantSchema *s = hg_schema(&ma, nf, 0);
        size_t w = s ? rant_schema_wire(s).len : 0;
        if (w > 1350) break;
        for (pad = 0; pad < 64 && !big && w + 64 > 1325; pad++){
            RantSchema *p = hg_schema(&ma, nf, pad);
            size_t pw = p ? rant_schema_wire(p).len : 0;
            if (pw >= 1325 && pw <= 1350) big = p;
        }
    }
    CHECK(small && big, "handle_gate: a caller req schema of 1325 to 1350 wire bytes");
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&ao,0,sizeof ao); ao.domain=TEST_DOMAIN+27; ao.discovery.max_peers=4;
    ao.net.multicast_interface="127.0.0.1"; ao.net.seed_peers=&seed; ao.net.n_seed_peers=1;
    bo = ao;
    ao.match_wait_ms = -1;     /* a reply never waits out a forming lane, so a lost one shows */
    A = big ? rant_node_open(&aa, "hg-a", NULL, hg_event, &ao) : NULL;
    B = big ? rant_node_open(&ba, "hg-b", NULL, NULL, &bo) : NULL;
    CHECK(!big || (A && B), "handle_gate: nodes open");
    if (A && B) payload = (uint8_t*)calloc(1, rant_schema_size(big));
    if (payload){
        for (i = 0; i < HG_N; i++){
            char name[32]; sprintf(name, "gate/f%02d", i);
            hg_defs[i] = rant_node_create_function_definition(A, name, small, NULL, hg_handler,
                                                              (void*)(intptr_t)i, NULL);
            CHECK(hg_defs[i] != NULL, "handle_gate: definition %s", name);
        }
        for (i = 0; i < HG_N; i++){   /* each call right after its remote, before A knows its rsp */
            char name[32]; RantFunction *rem;
            sprintf(name, "gate/f%02d", i);
            rem = rant_node_create_remote_function(B, name, big, NULL, NULL);
            if (!rem || rant_function_call_async(rem, rant_bytes(payload, rant_schema_size(big)),
                                                 hg_on_response, NULL, NULL) != RANT_OK) continue;
            issued++;
            PUMP_UNTIL(A, B, 3000, test_atomic_load(&hg_ok) + test_atomic_load(&hg_bad) >= issued);
        }
        CHECK(issued == HG_N && test_atomic_load(&hg_ok) == HG_N,
                 "handle_gate: every call answered (%lld of %d, %d issued)", test_atomic_load(&hg_ok), HG_N, issued);
        CHECK(test_atomic_load(&hg_no_lane) == 0,
                 "handle_gate: no request ran before its reply lane (%lld of %lld)",
                 test_atomic_load(&hg_no_lane), test_atomic_load(&hg_handled));
        CHECK(test_atomic_load(&hg_unmatched) == 0,
                 "handle_gate: no reply went to a missing lane (%lld)", test_atomic_load(&hg_unmatched));
    }
    free(payload);
    if (A) rant_node_close(A,0);
    if (B) rant_node_close(B,0);
    rant_allocator_reset(&aa); rant_allocator_reset(&ba); rant_allocator_reset(&ma);
}

/* Refresh on a live channel: a reflect remote adopts its owner's type while the owner writes
 * nonstop, so the retype frees lanes the remote's service thread receives on. Each round
 * takes a new name, since a handle re types once per owner generation. */
#define RL_ROUNDS 100
static RantVariable *rl_remote;
static long long rl_armed, rl_rc;
static void rl_on_message(const RantMsg *m){
    (void)m;
    if (test_atomic_load(&rl_armed) != 1) return;
    test_atomic_store(&rl_rc, rant_variable_refresh(rl_remote));
    test_atomic_store(&rl_armed, 2);
}
/* 1 once n's mesh view holds the owner of the variable name */
static int rl_known(RantNode *n, const char *name){
    const RantSchema *ms = NULL; uint64_t gen = 0;
    return i_rant_node_reflect_pick(n, RANT_ENTITY_VARIABLE, name, 0, 0, &ms, NULL, &gen) && gen != 0;
}
static void refresh_live_checks(void){
#ifndef RANT_THREADS
    test_skip("needs service threads");
#else
    RantAllocator aa = rant_allocator_heap(0), ba = rant_allocator_heap(0);
    RantNodeOpts o; RantNode *A, *B; const RantSchema *s = NULL;
    RantTopic *tick = NULL, *sub = NULL; RantVariable *own = NULL, *rem = NULL;
    int k, adopted = 0, live = 0;
    memset(&o, 0, sizeof o);
    o.domain = TEST_DOMAIN+41; o.disable_shm = 1; o.discovery.max_peers = 4;
    o.net.multicast_interface = "127.0.0.1";
    A = rant_node_open(&aa, "rl-owner", NULL, NULL, &o);
    B = rant_node_open(&ba, "rl-remote", rl_on_message, NULL, &o);
    CHECK(A && B, "refresh_live: nodes open");
    if (A && B){
        s = rant_node_schema(A, "RlValue { y: f64 }");
        tick = rant_node_create_topic(A, "rl/tick", RANT_PUB_ONLY, NULL, NULL);
        sub = rant_node_create_topic(B, "rl/tick", RANT_SUB_ONLY, NULL, NULL);
        rant_node_start(A); rant_node_start(B);
    }
    for (k = 0; s && k < RL_ROUNDS; k++){
        char name[32]; double y = 0; int r = 0, saw = 0, typed;
        uint64_t end = i_rant_plat_now_us() + 5000000u;
        snprintf(name, sizeof name, "rl/v%d", k);
        own = rant_node_create_variable_definition(A, name, s, NULL);
        rem = rant_node_create_remote_variable(B, name, NULL, &(RantVariableOpts){ .reflect_from_mesh = 1 });
        /* a remote made after the owner's details arrived is typed at create, with no retype */
        while (own && rem && !rant_variable_schema(rem) && i_rant_plat_now_us() < end){
            RantBytes v; int i;
            for (i = 0; i < 20; i++){ y += 1; rant_variable_set(own, rant_bytes(&y, sizeof y)); }
            if (rant_variable_get(rem, &v) == 1) saw = 1;
            r = rant_variable_refresh(rem);
        }
        typed = rem && rant_variable_schema(rem);
        if (rem) rant_variable_retire(rem);
        if (own) rant_variable_retire(own);
        if (!typed) break;
        adopted++;
        if (r == 1 && saw) live++;
    }
    CHECK(adopted == RL_ROUNDS, "refresh_live: every round took the owner's type (%d of %d)",
             adopted, RL_ROUNDS);
    CHECK(live > 0, "refresh_live: the retype ran while values arrived (%d rounds)", live);

    /* from a callback the refresh is refused: the retype would free lanes in use there. The
       remote comes first, so it is never typed at create */
    own = rem = NULL;
    if (s && tick && sub){
        rem = rant_node_create_remote_variable(B, "rl/cb", NULL, &(RantVariableOpts){ .reflect_from_mesh = 1 });
        own = rant_node_create_variable_definition(A, "rl/cb", s, NULL);
    }
    if (own && rem){
        WAIT_UNTIL(5000, rant_topic_match_count(tick) > 0 && rl_known(B, "rl/cb"));
        rl_remote = rem; test_atomic_store(&rl_armed, 1);
        for (k = 0; k < 500 && test_atomic_load(&rl_armed) != 2; k++){
            rant_topic_send(tick, rant_bytes("t", 1), NULL);
            test_sleep_ms(10);
        }
        CHECK(test_atomic_load(&rl_armed) == 2 && test_atomic_load(&rl_rc) == RANT_ERR_STATE,
                 "refresh_live: a refresh from a callback is refused (%lld)", test_atomic_load(&rl_rc));
        CHECK(rant_variable_refresh(rem) == 1, "refresh_live: the same refresh outside it re types");
    }
    if (A) rant_node_close(A, 0);
    if (B) rant_node_close(B, 0);
    rl_remote = NULL; test_atomic_store(&rl_armed, 0);
    rant_allocator_reset(&aa); rant_allocator_reset(&ba);
#endif
}

static const TestCase cases[] = {
    { "patterns", patterns_checks },
    { "variable_wait", varwait_checks },
    { "dup_authority", dup_authority_checks },
    { "retire", retire_checks },
    { "reflect_reuse", reflect_reuse_checks },
    { "loud", loud_checks },
    { "sync_capture", sync_capture_checks },
    { "one_announce", one_announce_checks },
    { "handle_gate", handle_gate_checks },
    { "refresh_live", refresh_live_checks },
};

TEST_MAIN(cases)
