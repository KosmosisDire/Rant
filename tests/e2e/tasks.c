/* Tasks between nodes: progress, cancel both ways, no_cancel, the bare return, and the
 * matrix of callers, providers, loss, retire and churn. spec/testing.md says more. */
#include "support/test.h"

/* The task smoke phase: defer, RUNNING first progress, cancel both ways, no_cancel
 * refused locally, an inline reply and the bare return APP_ERROR. The matrix follows. */
static volatile int   tk_reply_done;
static RantCallStatus tk_reply_status;
static uint32_t         tk_reply_val;
static char             tk_reply_msg[RANT_CALL_MSG_MAX + 1]; static size_t tk_reply_msg_len;
static void tk_on_reply(const RantResponse *r){
    tk_reply_status = r->status;
    tk_reply_val = r->data.len>=4 ? i_rant_le_r32(r->data.data) : 0;
    tk_reply_msg_len = r->message.len <= RANT_CALL_MSG_MAX ? r->message.len : RANT_CALL_MSG_MAX;
    if (tk_reply_msg_len) memcpy(tk_reply_msg, r->message.data, tk_reply_msg_len);
    tk_reply_msg[tk_reply_msg_len] = 0;
    tk_reply_done = 1;
}
/* progress capture: the RUNNING ack is the empty first update, payloads must ascend */
static int tk_prog_n, tk_prog_first_empty, tk_prog_in_order;
static uint32_t tk_prog_last_val, tk_prog_call_id;
static void tk_on_progress(const RantProgress *p){
    if (tk_prog_n == 0) tk_prog_first_empty = (p->data.len == 0);
    if (p->data.len >= 4){
        uint32_t v = i_rant_le_r32(p->data.data);
        if (v <= tk_prog_last_val) tk_prog_in_order = 0;
        tk_prog_last_val = v;
    }
    tk_prog_call_id = p->call_id;
    tk_prog_n++;
}
static volatile uint64_t tk_token;
static void tk_defer_handler(RantRequest *req, void *user){ (void)user; tk_token = rant_request_defer(req); }
static void tk_inline_handler(RantRequest *req, void *user){
    uint8_t out[4]; (void)user;
    i_rant_le_w32(out, req->data.len>=4 ? i_rant_le_r32(req->data.data)+1 : 0);
    rant_request_reply(req, rant_bytes(out,4));
}
static void tk_bare_handler(RantRequest *req, void *user){ (void)req; (void)user; }
static volatile int tk_cancel_fired; static volatile uint64_t tk_cancel_token;
static void tk_on_cancel_cb(uint64_t token, void *user){ (void)user; tk_cancel_token = token; tk_cancel_fired = 1; }

static void task_checks(void){
    RantAllocator pa = rant_allocator_heap(0);
    RantAllocator ca = rant_allocator_heap(0);
    RantNodeOpts po, co; RantNode *P=NULL, *C=NULL; RantAddr seed;
    RantFunction *pxfer, *cxfer, *plong, *clong, *pnc, *cnc, *pinl, *cinl, *pbare, *cbare;
    uint16_t dom = TEST_DOMAIN+32; int t;

    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=dom; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    co=po;
    P = rant_node_open(&pa, "task-prov", NULL, NULL, &po);
    C = rant_node_open(&ca, "task-call", NULL, NULL, &co);
    CHECK(P && C, "task: nodes open");
    if (!(P && C)){ if(P)rant_node_close(P,0); if(C)rant_node_close(C,0); return; }

    pxfer = rant_node_create_task_definition(P, "xfer", NULL, NULL, NULL, tk_defer_handler, NULL, NULL);
    cxfer = rant_node_create_remote_task(C, "xfer", NULL, NULL, NULL, NULL);
    plong = rant_node_create_task_definition(P, "long", NULL, NULL, NULL, tk_defer_handler, NULL, NULL);
    clong = rant_node_create_remote_task(C, "long", NULL, NULL, NULL, NULL);
    pnc   = rant_node_create_task_definition(P, "nocan", NULL, NULL, NULL, tk_defer_handler, NULL,
                                             &(RantTaskOpts){ .no_cancel = 1 });
    cnc   = rant_node_create_remote_task(C, "nocan", NULL, NULL, NULL, NULL);
    pinl  = rant_node_create_task_definition(P, "inl", NULL, NULL, NULL, tk_inline_handler, NULL, NULL);
    cinl  = rant_node_create_remote_task(C, "inl", NULL, NULL, NULL, NULL);
    pbare = rant_node_create_task_definition(P, "bare", NULL, NULL, NULL, tk_bare_handler, NULL, NULL);
    cbare = rant_node_create_remote_task(C, "bare", NULL, NULL, NULL, NULL);
    CHECK(pxfer&&cxfer&&plong&&clong&&pnc&&cnc&&pinl&&cinl&&pbare&&cbare, "task: pairs created");

    PUMP_UNTIL(P, C, 8000, rant_function_match_count(cxfer)!=0);
    CHECK(rant_function_match_count(cxfer)==1, "task: provider matched (%d)",
             rant_function_match_count(cxfer));

    /* defer + 3 progress updates + terminal OK: RUNNING fires on_progress first (empty),
       payloads arrive in order, id_out names the call */
    { uint8_t req[4], pb[4]; uint32_t id = 0; int rc, i2;
      i_rant_le_w32(req, 5);
      tk_reply_done=0; tk_token=0;
      tk_prog_n=0; tk_prog_first_empty=0; tk_prog_in_order=1; tk_prog_last_val=0; tk_prog_call_id=0;
      rc = rant_function_call_async(cxfer, rant_bytes(req,4), tk_on_reply, NULL,
              &(RantCallOpts){ .on_progress = tk_on_progress, .id_out = &id });
      CHECK(rc==RANT_OK && id!=0, "task: call accepted, id_out filled (rc=%d id=%u)", rc, id);
      PUMP_UNTIL(P, C, 8000, tk_token);
      CHECK(tk_token!=0, "task: handler deferred");
      PUMP_UNTIL(P, C, 3200, tk_prog_n >= 1);
      CHECK(tk_prog_n>=1 && tk_prog_first_empty,
               "task: RUNNING fired on_progress first, empty (n=%d empty=%d)",
               tk_prog_n, tk_prog_first_empty);
      for (i2=1;i2<=3;i2++){
          int pr;   /* each update lands before the next is sent, pinning arrival order */
          i_rant_le_w32(pb, (uint32_t)(100*i2));
          pr = rant_function_progress(pxfer, tk_token, rant_bytes(pb,4));
          CHECK(pr==RANT_OK, "task: progress %d sent (%d)", i2, pr);
          PUMP_UNTIL(P, C, 2000, tk_prog_n >= 1+i2);
      }
      i_rant_le_w32(pb, 777);
      rc = rant_function_complete(pxfer, tk_token, RANT_CALL_OK, NULL, rant_bytes(pb,4));
      CHECK(rc==RANT_OK, "task: complete OK sent (%d)", rc);
      PUMP_UNTIL(P, C, 3200, tk_reply_done);
      CHECK(tk_reply_done && tk_reply_status==RANT_CALL_OK && tk_reply_val==777,
               "task: terminal OK (done=%d st=%d val=%u)", tk_reply_done, tk_reply_status, tk_reply_val);
      CHECK(tk_prog_n==4 && tk_prog_in_order && tk_prog_last_val==300 && tk_prog_call_id==id,
               "task: 3 payloads in order after RUNNING (n=%d order=%d last=%u call=%u want=%u)",
               tk_prog_n, tk_prog_in_order, tk_prog_last_val, tk_prog_call_id, id);
      { int sr = rant_function_cancelled(pxfer, tk_token);     /* completed: the token is stale */
        CHECK(sr==RANT_ERR_STATE, "task: stale token refused (%d)", sr); } }

    /* cancel honored: long task, rant_function_cancel, the definition sees the flag +
       on_cancel, completes CANCELLED, the caller receives it */
    { uint32_t id = 0; int rc;
      tk_reply_done=0; tk_token=0; tk_cancel_fired=0; tk_cancel_token=0;
      rant_function_on_cancel(plong, tk_on_cancel_cb, NULL);
      rc = rant_function_call_async(clong, rant_bytes(NULL,0), tk_on_reply, NULL,
              &(RantCallOpts){ .id_out = &id });
      CHECK(rc==RANT_OK, "task: long call accepted (%d)", rc);
      PUMP_UNTIL(P, C, 8000, tk_token);
      CHECK(tk_token!=0, "task: long deferred");
      { int sr = rant_function_cancelled(plong, tk_token);
        CHECK(sr==0, "task: not yet cancelled (%d)", sr); }
      rc = rant_function_cancel(clong, id);
      CHECK(rc==RANT_OK, "task: cancel sent (%d)", rc);
      PUMP_UNTIL(P, C, 3200, tk_cancel_fired);
      CHECK(tk_cancel_fired && tk_cancel_token==tk_token,
               "task: on_cancel fired with the token (fired=%d)", tk_cancel_fired);
      { int sr = rant_function_cancelled(plong, tk_token);
        CHECK(sr==1, "task: cancelled flag set (%d)", sr); }
      rc = rant_function_complete(plong, tk_token, RANT_CALL_CANCELLED, "stopped", rant_bytes(NULL,0));
      CHECK(rc==RANT_OK, "task: complete CANCELLED sent (%d)", rc);
      PUMP_UNTIL(P, C, 3200, tk_reply_done);
      CHECK(tk_reply_done && tk_reply_status==RANT_CALL_CANCELLED
               && tk_reply_msg_len==7 && memcmp(tk_reply_msg,"stopped",7)==0,
               "task: caller sees wire CANCELLED \"stopped\" (st=%d msg=\"%s\")",
               tk_reply_status, tk_reply_msg); }

    /* .no_cancel: the caller's cancel refuses locally (RANT_ERR_ROLE), nothing reaches the
       definition, and the task still completes normally */
    { uint32_t id = 0; int rc, cr;
      tk_reply_done=0; tk_token=0; tk_cancel_fired=0;
      rant_function_on_cancel(pnc, tk_on_cancel_cb, NULL);
      rc = rant_function_call_async(cnc, rant_bytes(NULL,0), tk_on_reply, NULL,
              &(RantCallOpts){ .id_out = &id });
      PUMP_UNTIL(P, C, 8000, tk_token);
      CHECK(rc==RANT_OK && tk_token!=0, "task: no_cancel call running (rc=%d)", rc);
      cr = rant_function_cancel(cnc, id);
      CHECK(cr==RANT_ERR_ROLE, "task: cancel refused locally with RANT_ERR_ROLE (%d)", cr);
      test_pump(P,C,60);
      { int sr = rant_function_cancelled(pnc, tk_token);
        CHECK(!tk_cancel_fired && sr==0,
                 "task: nothing reached the definition (fired=%d flag=%d)", tk_cancel_fired, sr); }
      cr = rant_function_complete(pnc, tk_token, RANT_CALL_OK, NULL, rant_bytes(NULL,0));
      PUMP_UNTIL(P, C, 3200, tk_reply_done);
      CHECK(cr==RANT_OK && tk_reply_done && tk_reply_status==RANT_CALL_OK,
               "task: no_cancel task completes OK (st=%d)", tk_reply_status); }

    /* a task handler answering immediately via rant_request_reply works like a function */
    { uint8_t req[4]; int rc; i_rant_le_w32(req, 41);
      tk_reply_done=0;
      rc = rant_function_call_async(cinl, rant_bytes(req,4), tk_on_reply, NULL, NULL);
      CHECK(rc==RANT_OK, "task: inline call accepted (%d)", rc);
      PUMP_UNTIL(P, C, 8000, tk_reply_done);
      CHECK(tk_reply_done && tk_reply_status==RANT_CALL_OK && tk_reply_val==42,
               "task: inline reply -> 42 OK (st=%d val=%u)", tk_reply_status, tk_reply_val); }

    /* bare return: a task's return is not its answer */
    { int rc;
      tk_reply_done=0;
      rc = rant_function_call_async(cbare, rant_bytes(NULL,0), tk_on_reply, NULL, NULL);
      CHECK(rc==RANT_OK, "task: bare call accepted (%d)", rc);
      PUMP_UNTIL(P, C, 8000, tk_reply_done);
      CHECK(tk_reply_done && tk_reply_status==RANT_CALL_APP_ERROR,
               "task: bare return synthesizes APP_ERROR (st=%d)", tk_reply_status);
      CHECK(strcmp(tk_reply_msg, "handler returned no result")==0,
               "task: synthesized message (\"%s\")", tk_reply_msg); }

    rant_node_close(P,0); rant_node_close(C,0);
    rant_allocator_reset(&pa); rant_allocator_reset(&ca);
}

/* The task matrix: concurrent caller demux, overlapping calls, provider selection,
 * peer loss, retire and close mid run, third party cancel, churn and reflection. */

/* pump a node set (NULL entries skipped, e.g. a mid-phase closed provider) */
static void txm_pump(RantNode **ns, int n, int ms){
    uint64_t end = i_rant_plat_now_us() + (uint64_t)ms*1000u;
    while (i_rant_plat_now_us() < end){
        int i, first = 1;
        for (i=0;i<n;i++) if (ns[i]){ rant_node_poll(ns[i], first ? 1 : 0); first = 0; }
    }
}

/* caller-side progress capture (rides RantCallOpts.progress_user) */
typedef struct {
    int n, first_empty, payloads, wrong_tag, misattr;
    uint32_t want_tag;        /* nonzero: every payload must carry this tag */
    uint32_t tags_seen;       /* bitmask of payload tags */
    uint32_t vals[4]; int nvals;   /* first payload values, arrival order */
    uint32_t call_of_tag[8];  /* nonzero: a payload of tag i must carry this call_id */
    const RantSchema *ps;     /* prg schema (NULL = raw le32 payload) */
} TxmProg;
static void txm_on_prog(const RantProgress *p){
    TxmProg *c = (TxmProg*)p->user;
    if (c->n == 0) c->first_empty = (p->data.len == 0);
    c->n++;
    if (!p->data.len) return;   /* the RUNNING ack */
    c->payloads++;
    {   uint32_t tag, v;
        if (c->ps){ tag = (uint32_t)rant_get_uint(p->data, c->ps, "tag");
                    v   = (uint32_t)rant_get_uint(p->data, c->ps, "v"); }
        else      { tag = 0; v = p->data.len >= 4 ? i_rant_le_r32(p->data.data) : 0; }
        if (tag < 32) c->tags_seen |= 1u << tag;
        if (c->want_tag && tag != c->want_tag) c->wrong_tag++;
        if (tag < 8 && c->call_of_tag[tag] && c->call_of_tag[tag] != p->call_id) c->misattr++;
        if (c->nvals < 4) c->vals[c->nvals++] = v;
    }
}
/* file-transfer capture: chunks must arrive complete and strictly ascending (1..n) */
static int txm_file_n, txm_file_seq_ok;
static void txm_on_file_prog(const RantProgress *p){
    if (!p->data.len) return;
    { uint32_t v = p->data.len >= 4 ? i_rant_le_r32(p->data.data) : 0;
      if (v != (uint32_t)txm_file_n + 1) txm_file_seq_ok = 0;
      txm_file_n++; }
}
/* caller-side response capture (rides the call's user pointer) */
typedef struct {
    volatile int done; RantCallStatus status; uint32_t provider;
    uint32_t tag, v, raw32;
    char msg[64]; size_t msg_len;
    const RantSchema *rs;       /* rsp schema (NULL = raw le32 payload) */
} TxmRsp;
static void txm_on_rsp(const RantResponse *r){
    TxmRsp *c = (TxmRsp*)r->user;
    c->status = r->status; c->provider = r->provider;
    if (r->data.len >= 4) c->raw32 = i_rant_le_r32(r->data.data);
    if (c->rs && r->schema){ c->tag = (uint32_t)rant_get_uint(r->data, c->rs, "tag");
                             c->v   = (uint32_t)rant_get_uint(r->data, c->rs, "v"); }
    c->msg_len = r->message.len < sizeof c->msg - 1 ? r->message.len : sizeof c->msg - 1;
    if (c->msg_len) memcpy(c->msg, r->message.data, c->msg_len);
    c->msg[c->msg_len] = 0;
    c->done = 1;
}
/* provider handlers */
static volatile uint64_t txm_tok[8];   /* mix: live token per request tag */
static void txm_mix_handler(RantRequest *req, void *user){
    uint32_t tag = req->schema ? (uint32_t)rant_get_uint(req->data, req->schema, "tag") : 0;
    (void)user;
    if (tag < 8) txm_tok[tag] = rant_request_defer(req);
}
static void txm_defer_handler(RantRequest *req, void *user){
    *(volatile uint64_t*)user = rant_request_defer(req);
}
static int txm_sel_p, txm_sel_p2;
static void txm_sel_handler(RantRequest *req, void *user){
    ++*(int*)user;
    rant_request_reply(req, rant_bytes(NULL,0));
}
static volatile uint64_t txm_cancel_tok; static volatile int txm_cancel_n;
static void txm_on_cancel(uint64_t token, void *user){ (void)user; txm_cancel_tok = token; txm_cancel_n++; }
static volatile uint64_t txm_file_tok, txm_ret_tok, txm_shut_tok, txm_churn_tok, txm_drop_tok;
/* raw @prg tap (the explorer's progress view): 8-byte [caller_lo][call_id] header */
static int txm_tap_n, txm_tap_bad_hdr, txm_tap_nlo;
static uint32_t txm_tap_lo[4];
static void txm_tap_on_msg(void *user, const RantMsg *msg){
    (void)user;
    if (msg->header.len != 8){ txm_tap_bad_hdr++; return; }
    { uint32_t lo = i_rant_le_r32(msg->header.data); int i;
      for (i=0;i<txm_tap_nlo;i++) if (txm_tap_lo[i]==lo) break;
      if (i==txm_tap_nlo && txm_tap_nlo<4) txm_tap_lo[txm_tap_nlo++] = lo; }
    txm_tap_n++;
}
static uint32_t txm_peer_by_name(RantNode *n, const char *name){
    uint16_t cnt, i; size_t nl = strlen(name);
    const i_RantDiscoveryPeerView *ps = test_peers(n, &cnt);
    if (!ps) return 0;
    for (i=0;i<cnt;i++)
        if (ps[i].name.len==nl && memcmp(ps[i].name.data,name,nl)==0) return ps[i].id;
    return 0;
}
static RantBytes txm_enc(const RantSchema *s, uint32_t tag, uint32_t v, int has_v,
                         uint8_t *buf, size_t cap){
    rant_schema_message_default(s, buf, cap);
    rant_set_uint(buf, cap, s, "tag", tag);
    if (has_v) rant_set_uint(buf, cap, s, "v", v);
    return rant_bytes(buf, rant_schema_size(s));
}

static void taskx_checks(void){
    RantAllocator ap    = rant_allocator_heap(0);
    RantAllocator ap2 = rant_allocator_heap(0);
    RantAllocator ac1 = rant_allocator_heap(0);
    RantAllocator ac2 = rant_allocator_heap(0);
    RantAllocator ax    = rant_allocator_heap(0);
    RantAllocator ma    = rant_allocator_heap(0);
    RantNodeOpts o; RantAddr seed;
    RantNode *P, *P2, *C1, *C2, *X, *ns[5];
    RantSchema *req_s, *prg_s, *rsp_s;
    RantFunction *pmix, *pnoc, *pfile, *psel, *pret, *pret2, *psel2, *pshut;
    RantFunction *cmix1, *csel, *cfile, *cret, *cret2, *cshut, *cmix2, *xmix;
    RantTopic *xtap, *xreq;
    uint32_t p2_id_c1, p_id_x;
    int t;

    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&o,0,sizeof o); o.domain=(uint16_t)(TEST_DOMAIN+34); o.discovery.max_peers=6;
    o.net.multicast_interface="127.0.0.1"; o.net.seed_peers=&seed; o.net.n_seed_peers=1;
    P  = rant_node_open(&ap,    "txm-prov",  NULL, NULL, &o);
    P2 = rant_node_open(&ap2, "txm-prov2", NULL, NULL, &o);
    { RantNodeOpts o1 = o; o1.fetch_details = 1;     /* C1 doubles as the reflection observer */
      C1 = rant_node_open(&ac1, "txm-c1", NULL, NULL, &o1); }
    C2 = rant_node_open(&ac2, "txm-c2",  NULL, NULL, &o);
    X  = rant_node_open(&ax,    "txm-obs", NULL, NULL, &o);
    CHECK(P && P2 && C1 && C2 && X, "taskx: nodes open");
    if (!(P && P2 && C1 && C2 && X)){
        if(P)rant_node_close(P,0); if(P2)rant_node_close(P2,0); if(C1)rant_node_close(C1,0);
        if(C2)rant_node_close(C2,0); if(X)rant_node_close(X,0);
        return;
    }
    ns[0]=P; ns[1]=P2; ns[2]=C1; ns[3]=C2; ns[4]=X;

    /* three DISTINCT schemas so a crossed req/prg/rsp plumb cannot hide */
    req_s = test_compile(&ma, "TmReq { tag: u32 }", NULL);
    prg_s = test_compile(&ma, "TmPrg { tag: u32, v: u32 }", NULL);
    rsp_s = test_compile(&ma, "TmRsp { tag: u32, v: u32, note: u32 }", NULL);
    CHECK(req_s && prg_s && rsp_s, "taskx: schemas compiled");

    /* X's raw wire, the explorer's recipe. A normal remote handle on top of it is refused,
       since a live same name twin would only be shadowed. */
    { RantTopicOpts topt; memset(&topt,0,sizeof topt);
      topt.qos.reliability = RANT_RELIABLE; topt.qos.keep_last = 4;
      txm_tap_n=0; txm_tap_bad_hdr=0; txm_tap_nlo=0;
      xtap = i_rant_node_create_pattern_topic(X, "mix@prg", RANT_SUB_ONLY, prg_s, &topt,
                          RANT_KIND_TASK_PRG, 8, 0, 0, txm_tap_on_msg, NULL);
      xreq = i_rant_node_create_pattern_topic(X, "mix@req", RANT_PUB_ONLY, req_s, &topt,
                          RANT_KIND_TASK_REQ, 5, 0, 0, NULL, NULL);
      xmix = rant_node_create_remote_task(X, "mix", req_s, prg_s, rsp_s, NULL);
      CHECK(xtap && xreq && xmix==NULL && rant_last_error(X).error==RANT_E_NAME_COLLISION,
               "taskx: raw tap + raw req created, a same name remote refused (%d)",
               (int)rant_last_error(X).error); }

    pmix  = rant_node_create_task_definition(P, "mix", req_s, prg_s, rsp_s, txm_mix_handler, NULL, NULL);
    pnoc  = rant_node_create_task_definition(P, "noc", NULL, NULL, NULL, NULL, NULL,
                                             &(RantTaskOpts){ .no_cancel = 1, .exclusive = 1 });
    pfile = rant_node_create_task_definition(P, "file", NULL, NULL, NULL, txm_defer_handler,
                                             (void*)&txm_file_tok, NULL);
    psel  = rant_node_create_task_definition(P, "sel", NULL, NULL, NULL, txm_sel_handler,
                                             &txm_sel_p, &(RantTaskOpts){ .multi = 1 });
    pret  = rant_node_create_task_definition(P, "ret", NULL, NULL, NULL, txm_defer_handler,
                                             (void*)&txm_ret_tok, NULL);
    pret2 = rant_node_create_task_definition(P, "ret2", NULL, NULL, NULL, txm_defer_handler,
                                             (void*)&txm_ret_tok, NULL);
    psel2 = rant_node_create_task_definition(P2, "sel", NULL, NULL, NULL, txm_sel_handler,
                                             &txm_sel_p2, &(RantTaskOpts){ .multi = 1 });
    pshut = rant_node_create_task_definition(P2, "shut", NULL, NULL, NULL, txm_defer_handler,
                                             (void*)&txm_shut_tok, NULL);
    cmix1 = rant_node_create_remote_task(C1, "mix", req_s, prg_s, rsp_s, NULL);
    csel  = rant_node_create_remote_task(C1, "sel", NULL, NULL, NULL, &(RantTaskOpts){ .multi = 1 });
    cfile = rant_node_create_remote_task(C1, "file", NULL, NULL, NULL, NULL);
    cret  = rant_node_create_remote_task(C1, "ret", NULL, NULL, NULL, NULL);
    cret2 = rant_node_create_remote_task(C1, "ret2", NULL, NULL, NULL, NULL);
    cshut = rant_node_create_remote_task(C1, "shut", NULL, NULL, NULL, NULL);
    cmix2 = rant_node_create_remote_task(C2, "mix", req_s, prg_s, rsp_s, NULL);
    CHECK(pmix&&pnoc&&pfile&&psel&&pret&&pret2&&psel2&&pshut&&cmix1&&csel&&cfile&&cret&&cret2
             &&cshut&&cmix2, "taskx: entities created");
    rant_function_on_cancel(pmix, txm_on_cancel, NULL);

    for (t=0;t<3000 && !(rant_function_match_count(cmix1)==1 && rant_function_match_count(cmix2)==1
           && rant_function_match_count(csel)==2 && rant_function_match_count(cfile)==1
           && rant_function_match_count(cret)==1 && rant_function_match_count(cret2)==1
           && rant_function_match_count(cshut)==1
           && i_rant_topic_source_match_count(xtap)>=1 && rant_topic_match_count(xreq)>=1);t++)
        txm_pump(ns,5,2);
    CHECK(rant_function_match_count(cmix1)==1 && rant_function_match_count(cmix2)==1
             && rant_function_match_count(csel)==2 && i_rant_topic_source_match_count(xtap)>=1
             && rant_topic_match_count(xreq)>=1,
             "taskx: full mesh matched (mix=%d/%d sel=%d tap=%d raw=%d)",
             rant_function_match_count(cmix1), rant_function_match_count(cmix2),
             rant_function_match_count(csel), i_rant_topic_source_match_count(xtap),
             rant_topic_match_count(xreq));
    p2_id_c1 = txm_peer_by_name(C1, "txm-prov2");
    p_id_x   = txm_peer_by_name(X,  "txm-prov");
    CHECK(p2_id_c1 && p_id_x, "taskx: provider peer ids resolved (%u/%u)", p2_id_c1, p_id_x);

    /* CONCURRENT CALLER DEMUX: C1 and C2 both fire their first call, so both call ids are
       1. P defers both with distinct payloads and each caller must see only its own. */
    { static TxmProg g1, g2; static TxmRsp r1, r2;
      uint8_t b1[64], b2[64]; uint32_t id1=0, id2=0; int rc1, rc2;
      memset(&g1,0,sizeof g1); memset(&g2,0,sizeof g2);
      memset(&r1,0,sizeof r1); memset(&r2,0,sizeof r2);
      g1.ps = prg_s; g1.want_tag = 1; g2.ps = prg_s; g2.want_tag = 2;
      r1.rs = rsp_s; r2.rs = rsp_s;
      txm_tok[1]=0; txm_tok[2]=0;
      rc1 = rant_function_call_async(cmix1, txm_enc(req_s,1,0,0,b1,sizeof b1), txm_on_rsp, &r1,
              &(RantCallOpts){ .on_progress = txm_on_prog, .progress_user = &g1, .id_out = &id1 });
      rc2 = rant_function_call_async(cmix2, txm_enc(req_s,2,0,0,b2,sizeof b2), txm_on_rsp, &r2,
              &(RantCallOpts){ .on_progress = txm_on_prog, .progress_user = &g2, .id_out = &id2 });
      CHECK(rc1==RANT_OK && rc2==RANT_OK && id1==1 && id2==1,
               "taskx: first calls collide on call_id 1 (rc=%d/%d id=%u/%u)", rc1, rc2, id1, id2);
      for (t=0;t<2000 && !(txm_tok[1] && txm_tok[2]);t++) txm_pump(ns,5,2);
      CHECK(txm_tok[1] && txm_tok[2], "taskx: both requests deferred");
      for (t=0;t<800 && !(g1.n>=1 && g2.n>=1);t++) txm_pump(ns,5,2);
      CHECK(g1.n>=1 && g1.first_empty && g2.n>=1 && g2.first_empty,
               "taskx: both RUNNING acks fired on_progress first, empty (%d/%d)", g1.n, g2.n);
      { static const uint32_t seq[4][2] = { {1,10},{2,11},{1,20},{2,21} };
        uint8_t pb[64]; int i2, w1=0, w2=0, prs=1;
        for (i2=0;i2<4;i2++){
            uint32_t tg = seq[i2][0], v = seq[i2][1];
            if (rant_function_progress(pmix, txm_tok[tg],
                                       txm_enc(prg_s,tg,v,1,pb,sizeof pb)) != RANT_OK) prs = 0;
            if (tg==1) w1++; else w2++;
            for (t=0;t<400 && !((tg==1?g1.payloads:g2.payloads) >= (tg==1?w1:w2));t++)
                txm_pump(ns,5,2);
        }
        CHECK(prs, "taskx: interleaved progress sends accepted"); }
      CHECK(g1.payloads==2 && g1.wrong_tag==0 && g1.tags_seen==(1u<<1)
               && g1.vals[0]==10 && g1.vals[1]==20,
               "taskx: C1 saw ONLY its own payloads (n=%d wrong=%u seen=%#x v=%u,%u)",
               g1.payloads, g1.wrong_tag, g1.tags_seen, g1.vals[0], g1.vals[1]);
      CHECK(g2.payloads==2 && g2.wrong_tag==0 && g2.tags_seen==(1u<<2)
               && g2.vals[0]==11 && g2.vals[1]==21,
               "taskx: C2 saw ONLY its own payloads (n=%d wrong=%u seen=%#x v=%u,%u)",
               g2.payloads, g2.wrong_tag, g2.tags_seen, g2.vals[0], g2.vals[1]);
      { int c1r = rant_function_complete(pmix, txm_tok[1], RANT_CALL_OK, NULL,
                                         txm_enc(rsp_s,1,100,1,b1,sizeof b1));
        int c2r = rant_function_complete(pmix, txm_tok[2], RANT_CALL_OK, NULL,
                                         txm_enc(rsp_s,2,200,1,b2,sizeof b2));
        CHECK(c1r==RANT_OK && c2r==RANT_OK, "taskx: both completes sent (%d/%d)", c1r, c2r); }
      for (t=0;t<800 && !(r1.done && r2.done);t++) txm_pump(ns,5,2);
      CHECK(r1.done && r1.status==RANT_CALL_OK && r1.tag==1 && r1.v==100,
               "taskx: C1 terminal routed (st=%d tag=%u v=%u)", r1.status, r1.tag, r1.v);
      CHECK(r2.done && r2.status==RANT_CALL_OK && r2.tag==2 && r2.v==200,
               "taskx: C2 terminal routed (st=%d tag=%u v=%u)", r2.status, r2.tag, r2.v);
      /* The raw tap observed both callers' progress with distinct caller_lo values. X's
         normal remote handle has no outstanding call, so nothing can fire progress on it. */
      txm_pump(ns,5,20);
      { uint32_t lo1 = i_rant_le_r32(i_rant_node_uuid(C1)), lo2 = i_rant_le_r32(i_rant_node_uuid(C2));
        int has1=0, has2=0, i2;
        for (i2=0;i2<txm_tap_nlo;i2++){ if (txm_tap_lo[i2]==lo1) has1=1; if (txm_tap_lo[i2]==lo2) has2=1; }
        CHECK(txm_tap_n>=4 && txm_tap_bad_hdr==0 && txm_tap_nlo==2 && has1 && has2 && lo1!=lo2,
                 "taskx: raw tap saw both callers' caller_lo (n=%d bad=%d nlo=%d)",
                 txm_tap_n, txm_tap_bad_hdr, txm_tap_nlo); } }

    /* Two OVERLAPPING calls from ONE caller: two live tokens on P, interleaved
       progress attributed per call_id, both terminals */
    { static TxmProg g; static TxmRsp r3, r4;
      uint8_t b[64]; uint32_t id3=0, id4=0;
      memset(&g,0,sizeof g); memset(&r3,0,sizeof r3); memset(&r4,0,sizeof r4);
      g.ps = prg_s; r3.rs = rsp_s; r4.rs = rsp_s;
      txm_tok[3]=0; txm_tok[4]=0;
      rant_function_call_async(cmix2, txm_enc(req_s,3,0,0,b,sizeof b), txm_on_rsp, &r3,
          &(RantCallOpts){ .on_progress = txm_on_prog, .progress_user = &g, .id_out = &id3 });
      rant_function_call_async(cmix2, txm_enc(req_s,4,0,0,b,sizeof b), txm_on_rsp, &r4,
          &(RantCallOpts){ .on_progress = txm_on_prog, .progress_user = &g, .id_out = &id4 });
      CHECK(id3 && id4 && id3!=id4, "taskx: overlapping ids distinct (%u/%u)", id3, id4);
      g.call_of_tag[3]=id3; g.call_of_tag[4]=id4;
      for (t=0;t<2000 && !(txm_tok[3] && txm_tok[4]);t++) txm_pump(ns,5,2);
      CHECK(txm_tok[3] && txm_tok[4], "taskx: two live tokens on the provider");
      { static const uint32_t seq[4][2] = { {3,30},{4,40},{3,31},{4,41} }; int i2;
        for (i2=0;i2<4;i2++){
            (void)rant_function_progress(pmix, txm_tok[seq[i2][0]],
                                         txm_enc(prg_s,seq[i2][0],seq[i2][1],1,b,sizeof b));
            for (t=0;t<400 && g.payloads < i2+1;t++) txm_pump(ns,5,2);
        } }
      CHECK(g.payloads==4 && g.misattr==0 && g.tags_seen==((1u<<3)|(1u<<4)),
               "taskx: interleaved progress attributed per call_id (n=%d misattr=%u)",
               g.payloads, g.misattr);
      rant_function_complete(pmix, txm_tok[3], RANT_CALL_OK, NULL, txm_enc(rsp_s,3,300,1,b,sizeof b));
      rant_function_complete(pmix, txm_tok[4], RANT_CALL_APP_ERROR, "worked anyway",
                             txm_enc(rsp_s,4,400,1,b,sizeof b));
      for (t=0;t<800 && !(r3.done && r4.done);t++) txm_pump(ns,5,2);
      CHECK(r3.done && r3.status==RANT_CALL_OK && r3.tag==3 && r3.v==300,
               "taskx: first overlapped terminal (st=%d tag=%u)", r3.status, r3.tag);
      CHECK(r4.done && r4.status==RANT_CALL_APP_ERROR && r4.tag==4 && r4.v==400
               && strcmp(r4.msg,"worked anyway")==0,
               "taskx: second overlapped terminal, message + data (st=%d msg=\"%s\")",
               r4.status, r4.msg); }

    /* PROVIDER SELECTION: an explicit RantCallOpts.provider reaches exactly the chosen
       provider, provider 0 auto directs at the oldest matched */
    { static TxmRsp r; int before;
      memset(&r,0,sizeof r);
      txm_sel_p=0; txm_sel_p2=0;
      rant_function_call_async(csel, rant_bytes(NULL,0), txm_on_rsp, &r,
          &(RantCallOpts){ .provider = p2_id_c1 });
      for (t=0;t<800 && !r.done;t++) txm_pump(ns,5,2);
      txm_pump(ns,5,20);   /* the unchosen provider must stay silent */
      CHECK(r.done && r.status==RANT_CALL_OK && r.provider==p2_id_c1
               && txm_sel_p2==1 && txm_sel_p==0,
               "taskx: explicit provider reached only P2 (p=%d p2=%d prov=%u)",
               txm_sel_p, txm_sel_p2, r.provider);
      before = txm_sel_p + txm_sel_p2;
      memset(&r,0,sizeof r);
      rant_function_call_async(csel, rant_bytes(NULL,0), txm_on_rsp, &r, NULL);
      for (t=0;t<800 && !r.done;t++) txm_pump(ns,5,2);
      txm_pump(ns,5,20);
      CHECK(r.done && r.status==RANT_CALL_OK && txm_sel_p + txm_sel_p2 == before+1,
               "taskx: provider 0 auto-directed at ONE provider (p=%d p2=%d)",
               txm_sel_p, txm_sel_p2); }

    /* THIRD-PARTY CANCEL, the explorer path: X crafts the raw CANCEL op naming C1's live
       call. The provider's on_cancel fires for exactly that call, C2's call is untouched. */
    { static TxmProg g5, g6; static TxmRsp r5, r6;
      uint8_t b[64]; uint32_t id5=0, id6=0;
      memset(&g5,0,sizeof g5); memset(&g6,0,sizeof g6);
      memset(&r5,0,sizeof r5); memset(&r6,0,sizeof r6);
      g5.ps=prg_s; g6.ps=prg_s; r5.rs=rsp_s; r6.rs=rsp_s;
      txm_tok[5]=0; txm_tok[6]=0; txm_cancel_n=0; txm_cancel_tok=0;
      rant_function_call_async(cmix1, txm_enc(req_s,5,0,0,b,sizeof b), txm_on_rsp, &r5,
          &(RantCallOpts){ .on_progress = txm_on_prog, .progress_user = &g5, .id_out = &id5 });
      rant_function_call_async(cmix2, txm_enc(req_s,6,0,0,b,sizeof b), txm_on_rsp, &r6,
          &(RantCallOpts){ .on_progress = txm_on_prog, .progress_user = &g6, .id_out = &id6 });
      for (t=0;t<2000 && !(txm_tok[5] && txm_tok[6]);t++) txm_pump(ns,5,2);
      CHECK(txm_tok[5] && txm_tok[6] && id5!=0, "taskx: victim + bystander calls running");
      { uint8_t hdr[5], pl[4]; int sr;
        i_rant_le_w32(hdr, id5); hdr[4] = 1;     /* op 1 = CANCEL */
        i_rant_le_w32(pl, i_rant_le_r32(i_rant_node_uuid(C1)));         /* the victim's caller_lo */
        sr = i_rant_topic_send_to(xreq, p_id_x, rant_bytes(hdr,5), rant_bytes(pl,4));
        CHECK(sr==RANT_OK, "taskx: raw third-party cancel sent (%d)", sr); }
      for (t=0;t<800 && !txm_cancel_n;t++) txm_pump(ns,5,2);
      CHECK(txm_cancel_n==1 && txm_cancel_tok==txm_tok[5],
               "taskx: on_cancel fired for the victim call (n=%d)", txm_cancel_n);
      { int c5 = rant_function_cancelled(pmix, txm_tok[5]);
        int c6 = rant_function_cancelled(pmix, txm_tok[6]);
        CHECK(c5==1 && c6==0, "taskx: only the victim's flag set (%d/%d)", c5, c6); }
      rant_function_complete(pmix, txm_tok[5], RANT_CALL_CANCELLED, "stopped", rant_bytes(NULL,0));
      rant_function_complete(pmix, txm_tok[6], RANT_CALL_OK, NULL, txm_enc(rsp_s,6,600,1,b,sizeof b));
      for (t=0;t<800 && !(r5.done && r6.done);t++) txm_pump(ns,5,2);
      CHECK(r5.done && r5.status==RANT_CALL_CANCELLED && strcmp(r5.msg,"stopped")==0,
               "taskx: victim caller sees CANCELLED (st=%d msg=\"%s\")", r5.status, r5.msg);
      CHECK(r6.done && r6.status==RANT_CALL_OK && r6.v==600,
               "taskx: bystander call unaffected (st=%d v=%u)", r6.status, r6.v); }

    /* RETIRE MID-RUN through both delivery paths: the wire CANCELLED, and the caller side
       severed lane backstop when the announce arrives first (spec/testing.md). */
    { static TxmProg g; static TxmRsp r;
      uint8_t b[8]; uint32_t id=0; uint64_t dead;
      memset(&g,0,sizeof g); memset(&r,0,sizeof r);
      txm_ret_tok = 0;
      rant_function_call_async(cret, rant_bytes(NULL,0), txm_on_rsp, &r,
          &(RantCallOpts){ .on_progress = txm_on_prog, .progress_user = &g, .id_out = &id });
      for (t=0;t<2000 && !(txm_ret_tok && g.n>=1);t++) txm_pump(ns,5,2);
      CHECK(txm_ret_tok && g.n>=1, "taskx: retire victim RUNNING");
      dead = txm_ret_tok;
      { int rr = rant_function_retire(pret);
        CHECK(rr==RANT_OK, "taskx: definition retired mid-run (%d)", rr); }
      pret = NULL;
      PUMP_UNTIL(C1, NULL, 2000, r.done);     /* caller first: the wire path */
      CHECK(r.done && r.status==RANT_CALL_CANCELLED && strcmp(r.msg,"provider retired")==0,
               "taskx: caller got wire CANCELLED \"provider retired\" (st=%d msg=\"%s\")",
               r.done ? (int)r.status : -1, r.msg);
      i_rant_le_w32(b, 0);
      { int a = rant_function_progress (pmix, dead, rant_bytes(b,4));
        int c = rant_function_cancelled(pmix, dead);
        int d = rant_function_complete (pmix, dead, RANT_CALL_OK, NULL, rant_bytes(NULL,0));
        CHECK(a==RANT_ERR_STATE && c==RANT_ERR_STATE && d==RANT_ERR_STATE,
                 "taskx: dead token refused everywhere (%d/%d/%d)", a, c, d); }
      /* round 2: provider pumped first, so the retire announce beats the wire reply */
      memset(&g,0,sizeof g); memset(&r,0,sizeof r);
      txm_ret_tok = 0;
      rant_function_call_async(cret2, rant_bytes(NULL,0), txm_on_rsp, &r,
          &(RantCallOpts){ .on_progress = txm_on_prog, .progress_user = &g, .id_out = &id });
      for (t=0;t<2000 && !(txm_ret_tok && g.n>=1);t++) txm_pump(ns,5,2);
      CHECK(txm_ret_tok && g.n>=1, "taskx: second retire victim RUNNING");
      { int rr = rant_function_retire(pret2);
        CHECK(rr==RANT_OK, "taskx: second definition retired (%d)", rr); }
      pret2 = NULL;
      for (t=0;t<800 && !r.done;t++) txm_pump(ns,5,2);   /* P polls first each pass */
      CHECK(r.done && r.status==RANT_CALL_CANCELLED && strcmp(r.msg,"provider retired")==0,
               "taskx: severed lane still resolves CANCELLED (st=%d msg=\"%s\")",
               r.done ? (int)r.status : -1, r.msg); }

    /* CLOSE MID-RUN: the provider closes with the call RUNNING. No BYE is sent, so the
       outcome can only be the close hook's CANCELLED "node closing". */
    { static TxmProg g; static TxmRsp r;
      uint32_t id=0;
      memset(&g,0,sizeof g); memset(&r,0,sizeof r);
      txm_shut_tok = 0;
      rant_function_call_async(cshut, rant_bytes(NULL,0), txm_on_rsp, &r,
          &(RantCallOpts){ .on_progress = txm_on_prog, .progress_user = &g, .id_out = &id });
      for (t=0;t<2000 && !(txm_shut_tok && g.n>=1);t++) txm_pump(ns,5,2);
      CHECK(txm_shut_tok && g.n>=1, "taskx: close victim RUNNING");
      rant_node_close(P2, 0);
      ns[1] = NULL; P2 = NULL;
      for (t=0;t<800 && !r.done;t++) txm_pump(ns,5,2);
      CHECK(r.done && r.status==RANT_CALL_CANCELLED && strcmp(r.msg,"node closing")==0,
               "taskx: caller resolved at provider close (st=%d msg=\"%s\")",
               r.done ? (int)r.status : -1, r.msg); }

    /* FILE-TRANSFER SHAPE: 50 reliable 4 byte chunks as progress, one per app loop pass,
       then the terminal summary. Reliable progress loses nothing and stays in order. */
    { static TxmRsp r; uint8_t b[8]; uint32_t id=0; int i2, sent_ok=1;
      memset(&r,0,sizeof r);
      txm_file_tok=0; txm_file_n=0; txm_file_seq_ok=1;
      rant_function_call_async(cfile, rant_bytes(NULL,0), txm_on_rsp, &r,
          &(RantCallOpts){ .on_progress = txm_on_file_prog, .id_out = &id });
      for (t=0;t<2000 && !txm_file_tok;t++) txm_pump(ns,5,2);
      CHECK(txm_file_tok!=0, "taskx: transfer deferred");
      for (i2=1;i2<=50;i2++){
          i_rant_le_w32(b, (uint32_t)i2);
          if (rant_function_progress(pfile, txm_file_tok, rant_bytes(b,4)) != RANT_OK) sent_ok = 0;
          for (t=0;t<400 && txm_file_n < i2;t++) txm_pump(ns,5,2);
      }
      CHECK(sent_ok, "taskx: every chunk send accepted");
      CHECK(txm_file_n==50 && txm_file_seq_ok,
               "taskx: 50 chunks, strictly ascending (n=%d ok=%d)", txm_file_n, txm_file_seq_ok);
      i_rant_le_w32(b, 50);
      rant_function_complete(pfile, txm_file_tok, RANT_CALL_OK, "done", rant_bytes(b,4));
      for (t=0;t<800 && !r.done;t++) txm_pump(ns,5,2);
      CHECK(r.done && r.status==RANT_CALL_OK && r.raw32==50 && strcmp(r.msg,"done")==0,
               "taskx: terminal summary after the chunks (st=%d n=%u)", r.status, r.raw32); }

    /* TASK CHURN: retire remote and definition, re-create the SAME name, and a full
       defer + progress + complete round trip works again, three cycles */
    { int cyc;
      for (cyc=0; cyc<3; cyc++){
          RantFunction *d, *c; static TxmProg g; static TxmRsp r;
          uint8_t b[8]; uint32_t id=0;
          memset(&g,0,sizeof g); memset(&r,0,sizeof r);
          txm_churn_tok = 0;
          d = rant_node_create_task_definition(P, "churn", NULL, NULL, NULL,
                  txm_defer_handler, (void*)&txm_churn_tok, NULL);
          c = rant_node_create_remote_task(C1, "churn", NULL, NULL, NULL, NULL);
          CHECK(d && c, "taskx: churn cycle %d pair created", cyc);
          if (!(d && c)) break;
          for (t=0;t<2000 && rant_function_match_count(c)==0;t++) txm_pump(ns,5,2);
          rant_function_call_async(c, rant_bytes(NULL,0), txm_on_rsp, &r,
              &(RantCallOpts){ .on_progress = txm_on_prog, .progress_user = &g, .id_out = &id });
          for (t=0;t<2000 && !txm_churn_tok;t++) txm_pump(ns,5,2);
          if (txm_churn_tok){
              i_rant_le_w32(b, (uint32_t)(cyc+1));
              (void)rant_function_progress(d, txm_churn_tok, rant_bytes(b,4));
              for (t=0;t<400 && g.payloads<1;t++) txm_pump(ns,5,2);
              i_rant_le_w32(b, (uint32_t)(100+cyc));
              rant_function_complete(d, txm_churn_tok, RANT_CALL_OK, NULL, rant_bytes(b,4));
          }
          for (t=0;t<800 && !r.done;t++) txm_pump(ns,5,2);
          CHECK(r.done && r.status==RANT_CALL_OK && r.raw32==(uint32_t)(100+cyc)
                   && g.payloads==1,
                   "taskx: churn cycle %d round trip (st=%d v=%u prg=%d)",
                   cyc, r.done ? (int)r.status : -1, r.raw32, g.payloads);
          rant_function_retire(c); rant_function_retire(d);
      } }

    /* REFLECTION: from C1, one RANT_ENTITY_TASK per task name on P, attrs decoded, the
       three schema slots plumbed distinctly. The retired names must be gone. */
    { uint32_t pid = txm_peer_by_name(C1, "txm-prov");
      int tries, tasks=0, others=0, ats=0, inc=0;
      int have_mix=0, have_sel=0, have_noc=0, have_file=0;
      RantEntityInfo mix_e, sel_e, noc_e, file_e;
      memset(&mix_e,0,sizeof mix_e); memset(&sel_e,0,sizeof sel_e);
      memset(&noc_e,0,sizeof noc_e); memset(&file_e,0,sizeof file_e);
      CHECK(pid!=0, "taskx: provider peer visible at C1");
      for (tries=0; tries<400; tries++){   /* let straggling detail fetches settle */
          RantIter it; RantEntityInfo ei; size_t k;
          tasks=others=ats=inc=0; have_mix=have_sel=have_noc=have_file=0;
          memset(&it,0,sizeof it);
          while (rant_node_entities_next(C1, pid, &it, &ei)){
              if (test_builtin(&ei)) continue;
              if (ei.kind==RANT_ENTITY_TASK) tasks++; else others++;
              inc += ei.incomplete;
              for (k=0;k<ei.name.len;k++) if (ei.name.data[k]=='@') ats++;
              if (ei.name.len==3 && !memcmp(ei.name.data,"mix",3)){ mix_e=ei; have_mix=1; }
              if (ei.name.len==3 && !memcmp(ei.name.data,"sel",3)){ sel_e=ei; have_sel=1; }
              if (ei.name.len==3 && !memcmp(ei.name.data,"noc",3)){ noc_e=ei; have_noc=1; }
              if (ei.name.len==4 && !memcmp(ei.name.data,"file",4)){ file_e=ei; have_file=1; }
          }
          if (tasks==4 && others==0 && inc==0 && have_mix && have_sel && have_noc && have_file
              && mix_e.schema && mix_e.rsp_schema && mix_e.progress_schema
              && noc_e.exclusive) break;
          txm_pump(ns,5,2);
      }
      CHECK(tasks==4 && others==0 && ats==0 && inc==0,
               "taskx: reflect: one task entity per name, folded (tasks=%d others=%d @=%d inc=%d)",
               tasks, others, ats, inc);
      CHECK(have_mix && mix_e.provides && mix_e.cancellable==1 && mix_e.exclusive==0
               && mix_e.multi==0,
               "taskx: reflect: mix attrs default (cancel=%d excl=%d multi=%d)",
               mix_e.cancellable, mix_e.exclusive, mix_e.multi);
      CHECK(have_noc && noc_e.cancellable==0 && noc_e.exclusive==1,
               "taskx: reflect: .no_cancel + .exclusive mirrored (cancel=%d excl=%d)",
               noc_e.cancellable, noc_e.exclusive);
      CHECK(have_sel && sel_e.multi==1 && sel_e.cancellable==1,
               "taskx: reflect: .multi mirrored (multi=%d cancel=%d)", sel_e.multi, sel_e.cancellable);
      CHECK(have_mix && mix_e.schema && mix_e.rsp_schema && mix_e.progress_schema
               && mix_e.schema_hash==rant_schema_hash(req_s)
               && mix_e.rsp_schema_hash==rant_schema_hash(rsp_s)
               && mix_e.progress_schema_hash==rant_schema_hash(prg_s),
               "taskx: reflect: req/prg/rsp schemas plumbed distinctly "
               "(req %llx/%llx prg %llx/%llx rsp %llx/%llx)",
               (unsigned long long)mix_e.schema_hash, (unsigned long long)rant_schema_hash(req_s),
               (unsigned long long)mix_e.progress_schema_hash, (unsigned long long)rant_schema_hash(prg_s),
               (unsigned long long)mix_e.rsp_schema_hash, (unsigned long long)rant_schema_hash(rsp_s));
      CHECK(have_file && file_e.cancellable==1,
               "taskx: reflect: untyped task cancellable by default (%d)", file_e.cancellable); }

    rant_node_close(P,0); rant_node_close(C1,0); rant_node_close(C2,0); rant_node_close(X,0);
    rant_allocator_reset(&ap); rant_allocator_reset(&ap2); rant_allocator_reset(&ac1);
    rant_allocator_reset(&ac2); rant_allocator_reset(&ax);

    /* PEER LOSS MID-RUN on a short timeout pair: RUNNING dropped the deadline, then the
       provider stops pumping. The caller's reap must answer PEER_LOST. */
    { RantAllocator apd = rant_allocator_heap(0);
      RantAllocator acd = rant_allocator_heap(0);
      RantNodeOpts od; RantNode *PD, *CD, *pair[2];
      RantFunction *pd, *cd; static TxmProg g; static TxmRsp r;
      uint32_t id=0; uint64_t give_up;
      memset(&od,0,sizeof od); od.domain=(uint16_t)(TEST_DOMAIN+36); od.discovery.max_peers=4;
      od.discovery.announce_interval_us=100000; od.discovery.peer_timeout_us=600000;
      od.net.multicast_interface="127.0.0.1"; od.net.seed_peers=&seed; od.net.n_seed_peers=1;
      PD = rant_node_open(&apd, "txm-drop-p", NULL, NULL, &od);
      CD = rant_node_open(&acd, "txm-drop-c", NULL, NULL, &od);
      CHECK(PD && CD, "taskx: drop pair open");
      if (PD && CD){
          txm_drop_tok = 0;
          pd = rant_node_create_task_definition(PD, "goner", NULL, NULL, NULL,
                  txm_defer_handler, (void*)&txm_drop_tok, NULL);
          cd = rant_node_create_remote_task(CD, "goner", NULL, NULL, NULL,
                  &(RantTaskOpts){ .timeout_us = 300000 });
          CHECK(pd && cd, "taskx: drop pair created");
          memset(&g,0,sizeof g); memset(&r,0,sizeof r);
          pair[0]=PD; pair[1]=CD;
          for (t=0;t<2000 && rant_function_match_count(cd)==0;t++) txm_pump(pair,2,2);
          rant_function_call_async(cd, rant_bytes(NULL,0), txm_on_rsp, &r,
              &(RantCallOpts){ .on_progress = txm_on_prog, .progress_user = &g, .id_out = &id });
          for (t=0;t<2000 && !(txm_drop_tok && g.n>=1);t++) txm_pump(pair,2,2);
          CHECK(txm_drop_tok && g.n>=1, "taskx: drop victim RUNNING (deadline dropped)");
          /* the provider goes silent: only the caller's discovery reap can answer now */
          give_up = i_rant_plat_now_us() + 3000000u;
          while (!r.done && i_rant_plat_now_us() < give_up) rant_node_poll(CD, 5);
          CHECK(r.done && r.status==RANT_CALL_PEER_LOST,
                   "taskx: RUNNING call fails PEER_LOST on the drop, not TIMEOUT (st=%d)",
                   r.done ? (int)r.status : -1);
      }
      if (PD) rant_node_close(PD,0);
      if (CD) rant_node_close(CD,0);
      rant_allocator_reset(&apd); rant_allocator_reset(&acd); }

    rant_allocator_reset(&ma);
}

static const TestCase cases[] = {
    { "task", task_checks },
    { "task_matrix", taskx_checks },
};

TEST_MAIN(cases)
