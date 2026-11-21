/* Call observers: every step of a function's and a task's calls reaches a node that neither
 * calls nor provides, costs nothing unwatched and never slows the provider. spec/testing.md
 * says more. */
#include "support/test.h"

#define OB_MAX 256

typedef struct {
    RantCallEventKind kind;
    RantCallStatus    status;
    uint32_t          caller_lo, call_id, val;
    size_t            data_len;
    const RantSchema *schema;
    uint64_t          written_us;
    char              name[64], provider[33], caller[33], message[256];
} ObEvent;

/* n goes through atomic.h, since a service thread appends while the test reads */
typedef struct { ObEvent ev[OB_MAX]; long long n; } ObLog;
static int ob_n(const ObLog *l){ return (int)test_atomic_load(&l->n); }

static void ob_copy(char *out, size_t cap, RantString s){
    size_t l = s.len < cap - 1 ? s.len : cap - 1;
    if (l) memcpy(out, s.data, l);
    out[l] = 0;
}

static void ob_on_event(const RantCallEvent *e, void *user){
    ObLog *l = (ObLog*)user; ObEvent *o;
    if (ob_n(l) >= OB_MAX) return;
    o = &l->ev[ob_n(l)];
    o->kind = e->kind; o->status = e->status; o->caller_lo = e->caller_lo; o->call_id = e->call_id;
    o->data_len = e->data.len; o->val = e->data.len >= 4 ? i_rant_le_r32(e->data.data) : 0;
    o->schema = e->schema; o->written_us = e->written_us;
    ob_copy(o->name, sizeof o->name, e->name);
    ob_copy(o->provider, sizeof o->provider, e->provider_name);
    ob_copy(o->caller, sizeof o->caller, e->caller_name);
    ob_copy(o->message, sizeof o->message, e->message);
    test_atomic_add(&l->n, 1);
}

/* four nodes on loopback, the test's own domain each. W calls with a schema of its own */
typedef struct { RantAllocator pa, ca, oa, wa; RantNode *P, *C, *O, *W; } ObMesh;

static RantMsgFn ob_watch_on_message;     /* what the observer node hears beyond its handles */

static int ob_open(ObMesh *m, uint16_t dom, int fetch_details){
    RantNodeOpts po, oo; RantAddr seed;
    memset(&seed, 0, sizeof seed); seed.ip[0] = 127; seed.ip[3] = 1; seed.ip_len = 4;
    memset(&po, 0, sizeof po); po.domain = dom; po.discovery.max_peers = 6;
    po.net.multicast_interface = "127.0.0.1"; po.net.seed_peers = &seed; po.net.n_seed_peers = 1;
    oo = po; oo.fetch_details = (uint8_t)fetch_details;
    m->pa = rant_allocator_heap(0); m->ca = rant_allocator_heap(0); m->oa = rant_allocator_heap(0);
    m->wa = rant_allocator_heap(0);
    m->P = rant_node_open(&m->pa, "ob-prov", NULL, NULL, &po);
    m->C = rant_node_open(&m->ca, "ob-call", NULL, NULL, &po);
    m->O = rant_node_open(&m->oa, "ob-watch", ob_watch_on_message, NULL, &oo);
    m->W = rant_node_open(&m->wa, "ob-wide", NULL, NULL, &po);
    CHECK(m->P && m->C && m->O && m->W, "observers: nodes open");
    return m->P && m->C && m->O && m->W;
}
static void ob_close(ObMesh *m){
    if (m->W) rant_node_close(m->W, 0);
    if (m->O) rant_node_close(m->O, 0);
    if (m->C) rant_node_close(m->C, 0);
    if (m->P) rant_node_close(m->P, 0);
}
static void ob_pump(ObMesh *m, int rounds){
    int i;
    for (i = 0; i < rounds; i++){
        rant_node_poll(m->P, 1); rant_node_poll(m->C, 1); rant_node_poll(m->O, 1); rant_node_poll(m->W, 1);
    }
}
#define OB_UNTIL(m, ms, cond) do { int ob_t_; for (ob_t_ = 0; ob_t_ < (ms) / 4 && !(cond); ob_t_++) ob_pump((m), 1); } while (0)

static long long ob_reply_done; static RantCallStatus ob_reply_status;
static void ob_on_reply(const RantResponse *r){ ob_reply_status = r->status; test_atomic_add(&ob_reply_done, 1); }

static void ob_echo(RantRequest *req, void *user){
    uint8_t out[4]; (void)user;
    i_rant_le_w32(out, req->data.len >= 4 ? i_rant_le_r32(req->data.data) + 1 : 0);
    rant_request_reply(req, rant_bytes(out, 4));
}
static void ob_refuse(RantRequest *req, void *user){ (void)user; rant_request_fail(req, "not now", rant_bytes(NULL, 0)); }
static long long ob_token;
static void ob_defer(RantRequest *req, void *user){ (void)user; test_atomic_store(&ob_token, (long long)rant_request_defer(req)); }

/* A function's request and answer, a refusal with its text, and a missing handler with the
 * default text. A call the observer joined too late for still names its caller. */
static void function_steps(void){
    static ObLog log; ObMesh m; RantFunction *pf, *cf, *pr, *cr, *pn, *cn;
    RantCallObserver *of, *or_, *on; uint8_t req[4]; int rc;
    memset(&m, 0, sizeof m); test_atomic_store(&log.n, 0);
    if (!ob_open(&m, (uint16_t)(TEST_DOMAIN + 0), 0)){ ob_close(&m); return; }
    pf = rant_node_create_function_definition(m.P, "ob/echo", NULL, NULL, ob_echo, NULL, NULL);
    cf = rant_node_create_remote_function(m.C, "ob/echo", NULL, NULL, NULL);
    pr = rant_node_create_function_definition(m.P, "ob/refuse", NULL, NULL, ob_refuse, NULL, NULL);
    cr = rant_node_create_remote_function(m.C, "ob/refuse", NULL, NULL, NULL);
    pn = rant_node_create_function_definition(m.P, "ob/none", NULL, NULL, NULL, NULL, NULL);
    cn = rant_node_create_remote_function(m.C, "ob/none", NULL, NULL, NULL);
    of = rant_node_create_call_observer(m.O, "ob/echo", NULL, NULL, NULL, ob_on_event, &log, NULL);
    or_ = rant_node_create_call_observer(m.O, "ob/refuse", NULL, NULL, NULL, ob_on_event, &log, NULL);
    on = rant_node_create_call_observer(m.O, "ob/none", NULL, NULL, NULL, ob_on_event, &log, NULL);
    CHECK(pf && cf && pr && cr && pn && cn && of && or_ && on, "function: handles created");
    if (!(pf && cf && pr && cr && pn && cn && of && or_ && on)){ ob_close(&m); return; }
    OB_UNTIL(&m, 8000, rant_call_observer_match_count(of) == 1 && rant_call_observer_match_count(or_) == 1
             && rant_call_observer_match_count(on) == 1 && rant_function_match_count(cf) == 1
             && rant_function_match_count(cr) == 1 && rant_function_match_count(cn) == 1
             && rant_topic_match_count(pf->obs) == 1);
    CHECK(rant_call_observer_match_count(of) == 1, "function: the definition matched (%d)", rant_call_observer_match_count(of));

    i_rant_le_w32(req, 41); test_atomic_store(&ob_reply_done, 0);
    rc = rant_function_call_async(cf, rant_bytes(req, 4), ob_on_reply, NULL, NULL);
    CHECK(rc == RANT_OK, "function: call accepted (%d)", rc);
    OB_UNTIL(&m, 4000, ob_n(&log) >= 2 && test_atomic_load(&ob_reply_done));
    CHECK(test_atomic_load(&ob_reply_done) && ob_reply_status == RANT_CALL_OK, "function: the caller got its answer");
    CHECK(ob_n(&log) == 2, "function: a call and its end, no more (%d)", ob_n(&log));
    if (ob_n(&log) >= 2){
        ObEvent *a = &log.ev[0], *b = &log.ev[1];
        CHECK(a->kind == RANT_CALL_EVENT_CALL && a->val == 41 && a->data_len == 4, "function: the request arrives (%d %u)", (int)a->kind, a->val);
        CHECK(b->kind == RANT_CALL_EVENT_END && b->status == RANT_CALL_OK && b->val == 42 && b->message[0] == 0,
              "function: the response arrives (%d %d %u)", (int)b->kind, (int)b->status, b->val);
        CHECK(a->call_id != 0 && a->call_id == b->call_id && a->caller_lo != 0 && a->caller_lo == b->caller_lo,
              "function: both name one call");
        CHECK(strcmp(a->name, "ob/echo") == 0 && strcmp(a->provider, "ob-prov") == 0 && strcmp(a->caller, "ob-call") == 0
              && strcmp(b->caller, "ob-call") == 0, "function: names (%s %s %s)", a->name, a->provider, a->caller);
        CHECK(a->written_us != 0 && b->written_us >= a->written_us, "function: stamped in order");
    }

    test_atomic_store(&log.n, 0); test_atomic_store(&ob_reply_done, 0);
    rant_function_call_async(cr, rant_bytes(NULL, 0), ob_on_reply, NULL, NULL);
    OB_UNTIL(&m, 4000, ob_n(&log) >= 2);
    CHECK(ob_n(&log) == 2 && log.ev[1].status == RANT_CALL_APP_ERROR && strcmp(log.ev[1].message, "not now") == 0,
          "function: a refusal keeps its text (%d %s)", log.n, ob_n(&log) > 1 ? log.ev[1].message : "");

    test_atomic_store(&log.n, 0);
    rant_function_call_async(cn, rant_bytes(NULL, 0), ob_on_reply, NULL, NULL);
    OB_UNTIL(&m, 4000, ob_n(&log) >= 2);
    CHECK(ob_n(&log) == 2 && log.ev[1].status == RANT_CALL_NO_HANDLER && strcmp(log.ev[1].message, "no handler") == 0,
          "function: no handler reads the default text (%d %s)", log.n, ob_n(&log) > 1 ? log.ev[1].message : "");

    rc = rant_call_observer_retire(of);
    CHECK(rc == RANT_OK, "function: observer retired (%d)", rc);
    OB_UNTIL(&m, 4000, rant_topic_match_count(pf->obs) == 0);
    CHECK(rant_topic_match_count(pf->obs) == 0, "function: the definition saw it go");
    test_atomic_store(&log.n, 0); test_atomic_store(&ob_reply_done, 0);
    rant_function_call_async(cf, rant_bytes(req, 4), ob_on_reply, NULL, NULL);
    OB_UNTIL(&m, 4000, test_atomic_load(&ob_reply_done));
    ob_pump(&m, 30);
    CHECK(test_atomic_load(&ob_reply_done) && ob_n(&log) == 0, "function: nothing after the retire (%d)", ob_n(&log));
    ob_close(&m);
}

/* A task's whole life: RUNNING, progress in order with the request first, a cancel naming who
 * asked, and the end. A definition retired mid call ends it for the observer too. */
static void task_steps(void){
    static ObLog log; ObMesh m; RantFunction *pt, *ct, *pz, *cz;
    RantCallObserver *ot, *oz; uint32_t id = 0; int rc, i; uint8_t b[4];
    memset(&m, 0, sizeof m); test_atomic_store(&log.n, 0);
    if (!ob_open(&m, (uint16_t)(TEST_DOMAIN + 1), 0)){ ob_close(&m); return; }
    pt = rant_node_create_task_definition(m.P, "ob/job", NULL, NULL, NULL, ob_defer, NULL, NULL);
    ct = rant_node_create_remote_task(m.C, "ob/job", NULL, NULL, NULL, NULL);
    pz = rant_node_create_task_definition(m.P, "ob/gone", NULL, NULL, NULL, ob_defer, NULL, NULL);
    cz = rant_node_create_remote_task(m.C, "ob/gone", NULL, NULL, NULL, NULL);
    ot = rant_node_create_call_observer(m.O, "ob/job", NULL, NULL, NULL, ob_on_event, &log, NULL);
    oz = rant_node_create_call_observer(m.O, "ob/gone", NULL, NULL, NULL, ob_on_event, &log, NULL);
    CHECK(pt && ct && pz && cz && ot && oz, "task: handles created");
    if (!(pt && ct && pz && cz && ot && oz)){ ob_close(&m); return; }
    OB_UNTIL(&m, 8000, rant_topic_match_count(pt->obs) == 1 && rant_topic_match_count(pz->obs) == 1
             && rant_call_observer_match_count(ot) == 1 && rant_function_match_count(ct) == 1
             && rant_function_match_count(cz) == 1);
    CHECK(rant_function_match_count(ct) == 1, "task: the observer takes no calls, one provider (%d)", rant_function_match_count(ct));

    test_atomic_store(&ob_token, 0); test_atomic_store(&ob_reply_done, 0);
    rc = rant_function_call_async(ct, rant_bytes(NULL, 0), ob_on_reply, NULL, &(RantCallOpts){ .id_out = &id });
    CHECK(rc == RANT_OK, "task: call accepted (%d)", rc);
    OB_UNTIL(&m, 4000, (uint64_t)test_atomic_load(&ob_token) && ob_n(&log) >= 2);
    for (i = 1; i <= 3; i++){
        i_rant_le_w32(b, (uint32_t)(i * 100));
        rc = rant_function_progress(pt, (uint64_t)test_atomic_load(&ob_token), rant_bytes(b, 4));
        CHECK(rc == RANT_OK, "task: progress sent (%d)", rc);
    }
    OB_UNTIL(&m, 4000, ob_n(&log) >= 5);
    rc = rant_function_cancel(ct, id);
    CHECK(rc == RANT_OK, "task: cancel sent (%d)", rc);
    OB_UNTIL(&m, 4000, ob_n(&log) >= 6);
    rc = rant_function_complete(pt, (uint64_t)test_atomic_load(&ob_token), RANT_CALL_CANCELLED, "stopped", rant_bytes(NULL, 0));
    CHECK(rc == RANT_OK, "task: complete sent (%d)", rc);
    OB_UNTIL(&m, 4000, ob_n(&log) >= 7 && test_atomic_load(&ob_reply_done));
    CHECK(ob_n(&log) == 7, "task: seven steps (%d)", ob_n(&log));
    if (ob_n(&log) == 7){
        CHECK(log.ev[0].kind == RANT_CALL_EVENT_CALL && log.ev[0].call_id == id, "task: the call first");
        CHECK(log.ev[1].kind == RANT_CALL_EVENT_RUNNING && log.ev[1].status == RANT_CALL_RUNNING, "task: then RUNNING");
        CHECK(log.ev[2].kind == RANT_CALL_EVENT_PROGRESS && log.ev[2].val == 100 && log.ev[3].val == 200 && log.ev[4].val == 300,
              "task: progress in order (%u %u %u)", log.ev[2].val, log.ev[3].val, log.ev[4].val);
        CHECK(log.ev[5].kind == RANT_CALL_EVENT_CANCEL && strcmp(log.ev[5].message, "ob-call") == 0
              && strcmp(log.ev[5].caller, "ob-call") == 0, "task: the cancel names who asked (%s)", log.ev[5].message);
        CHECK(log.ev[6].kind == RANT_CALL_EVENT_END && log.ev[6].status == RANT_CALL_CANCELLED
              && strcmp(log.ev[6].message, "stopped") == 0, "task: the end (%d %s)", (int)log.ev[6].status, log.ev[6].message);
        for (i = 1; i < 7; i++)
            CHECK(log.ev[i].call_id == id && log.ev[i].caller_lo == log.ev[0].caller_lo, "task: step %d names the call", i);
    }

    /* the definition goes mid call. Its last report races the retire announce, which a node
       handles first, so the call may end unseen here. The observer stays well either way */
    test_atomic_store(&log.n, 0); test_atomic_store(&ob_token, 0); test_atomic_store(&ob_reply_done, 0);
    rant_function_call_async(cz, rant_bytes(NULL, 0), ob_on_reply, NULL, NULL);
    OB_UNTIL(&m, 4000, (uint64_t)test_atomic_load(&ob_token) && ob_n(&log) >= 2);
    rc = rant_function_retire(pz);
    CHECK(rc == RANT_OK, "task: definition retired (%d)", rc);
    OB_UNTIL(&m, 2000, rant_call_observer_match_count(oz) == 0);
    CHECK(rant_call_observer_match_count(oz) == 0, "task: the observer saw the definition go");
    ob_pump(&m, 30);
    CHECK(ob_n(&log) == 2 || (ob_n(&log) == 3 && log.ev[2].kind == RANT_CALL_EVENT_END && log.ev[2].status == RANT_CALL_CANCELLED),
          "task: a retire reports at most its end (%d)", ob_n(&log));
    pz = rant_node_create_task_definition(m.P, "ob/gone", NULL, NULL, NULL, ob_defer, NULL, NULL);
    OB_UNTIL(&m, 4000, pz && rant_function_match_count(cz) == 1 && rant_topic_match_count(pz->obs) == 1);
    test_atomic_store(&log.n, 0); test_atomic_store(&ob_token, 0);
    rant_function_call_async(cz, rant_bytes(NULL, 0), ob_on_reply, NULL, NULL);
    OB_UNTIL(&m, 4000, (uint64_t)test_atomic_load(&ob_token) && ob_n(&log) >= 2);
    CHECK(ob_n(&log) == 2 && log.ev[0].kind == RANT_CALL_EVENT_CALL, "task: a new definition is watched at once (%d)", ob_n(&log));
    ob_close(&m);
}

/* The one trap of a reader on @req: an observer older than the provider must not look like
 * one. It adds no entity and takes no call. */
static void no_provider_lookalike(void){
    static ObLog log; ObMesh m; RantCallObserver *ob; RantFunction *pt, *ct; RantEntityInfo e; RantIter it; int found = 0;
    memset(&m, 0, sizeof m); test_atomic_store(&log.n, 0);
    if (!ob_open(&m, (uint16_t)(TEST_DOMAIN + 2), 1)){ ob_close(&m); return; }
    ob = rant_node_create_call_observer(m.O, "ob/first", NULL, NULL, NULL, ob_on_event, &log, NULL);
    ob_pump(&m, 40);
    pt = rant_node_create_task_definition(m.P, "ob/first", NULL, NULL, NULL, ob_defer, NULL, NULL);
    ct = rant_node_create_remote_task(m.C, "ob/first", NULL, NULL, NULL, NULL);
    CHECK(ob && pt && ct, "lookalike: handles created");
    if (!(ob && pt && ct)){ ob_close(&m); return; }
    OB_UNTIL(&m, 8000, rant_function_match_count(ct) == 1 && rant_call_observer_match_count(ob) == 1);
    CHECK(rant_function_match_count(ct) == 1, "lookalike: one provider (%d)", rant_function_match_count(ct));
    test_atomic_store(&ob_token, 0);
    rant_function_call_async(ct, rant_bytes(NULL, 0), ob_on_reply, NULL, NULL);
    OB_UNTIL(&m, 4000, (uint64_t)test_atomic_load(&ob_token) != 0);
    CHECK((uint64_t)test_atomic_load(&ob_token) != 0, "lookalike: the real provider got the call");
    ob_pump(&m, 40);

    memset(&it, 0, sizeof it);
    while (rant_node_mesh_next(m.O, &it, &e))
        if (e.name.len == 8 && memcmp(e.name.data, "ob/first", 8) == 0){
            found++;
            CHECK(e.kind == RANT_ENTITY_TASK && !e.incomplete && e.providers == 1 && e.consumers == 1,
                  "lookalike: one task, whole, the observer counted neither way (%d %d %d %d)",
                  (int)e.kind, e.incomplete, e.providers, e.consumers);
        }
    CHECK(found == 1, "lookalike: one entity by that name (%d)", found);
    memset(&it, 0, sizeof it);
    found = 0;
    while (rant_node_entities_next(m.O, RANT_SELF, &it, &e)) if (e.name.len == 8 && memcmp(e.name.data, "ob/first", 8) == 0) found++;
    CHECK(found == 0, "lookalike: the observer's own walk lists no such entity (%d)", found);
    ob_close(&m);
}

/* A definition and an observer of one name would be twins on one node. */
static void refused_twin(void){
    RantAllocator a = rant_allocator_heap(0); RantNode *n; RantFunction *f; RantCallObserver *o; static ObLog log;
    RantNodeOpts opt; memset(&opt, 0, sizeof opt); opt.domain = (uint16_t)(TEST_DOMAIN + 3);
    opt.net.multicast_interface = "127.0.0.1";
    n = rant_node_open(&a, "ob-solo", NULL, NULL, &opt);
    CHECK(n != NULL, "twin: node open");
    if (!n) return;
    f = rant_node_create_function_definition(n, "tw", NULL, NULL, ob_echo, NULL, NULL);
    o = rant_node_create_call_observer(n, "tw", NULL, NULL, NULL, ob_on_event, &log, NULL);
    CHECK(f && !o && rant_last_error(n).error == RANT_E_NAME_COLLISION, "twin: refused (%d)", (int)rant_last_error(n).error);
    CHECK(rant_node_create_call_observer(n, "other", NULL, NULL, NULL, NULL, NULL, NULL) == NULL, "twin: on_event is required");
    CHECK(rant_node_create_call_observer(n, "@rant/meta", NULL, NULL, NULL, ob_on_event, &log, NULL) == NULL, "twin: a builtin cannot be watched");
    rant_node_close(n, 0);
}

/* Unwatched means uncommitted: the channel's sequence line never moves. */
static void no_cost_unwatched(void){
    ObMesh m; RantFunction *pf, *cf; uint8_t req[4]; int i;
    memset(&m, 0, sizeof m);
    if (!ob_open(&m, (uint16_t)(TEST_DOMAIN + 4), 0)){ ob_close(&m); return; }
    pf = rant_node_create_function_definition(m.P, "ob/free", NULL, NULL, ob_echo, NULL, NULL);
    cf = rant_node_create_remote_function(m.C, "ob/free", NULL, NULL, NULL);
    CHECK(pf && cf, "unwatched: handles created");
    if (!(pf && cf)){ ob_close(&m); return; }
    OB_UNTIL(&m, 8000, rant_function_match_count(cf) == 1);
    {   uint64_t before = i_rant_topic_seqno(pf->obs);
        for (i = 0; i < 20; i++){
            i_rant_le_w32(req, (uint32_t)i); test_atomic_store(&ob_reply_done, 0);
            rant_function_call_async(cf, rant_bytes(req, 4), ob_on_reply, NULL, NULL);
            OB_UNTIL(&m, 2000, test_atomic_load(&ob_reply_done));
            CHECK(test_atomic_load(&ob_reply_done), "unwatched: call %d answered", i);
        }
        CHECK(i_rant_topic_seqno(pf->obs) == before, "unwatched: nothing committed (%llu to %llu)",
              (unsigned long long)before, (unsigned long long)i_rant_topic_seqno(pf->obs));
    }
    ob_close(&m);
}

/* A schema is only offered when the report's hash names it: a passed one, the provider's through
 * the mesh, and never one of another shape. A caller's wider request stays raw. */
static void schemas(void){
    static ObLog given, mesh, wrong; ObMesh m; RantFunction *pf, *cf, *cw;
    const RantSchema *req, *rsp, *wide, *other;
    RantCallObserver *og, *om, *ow; uint8_t msg[16]; size_t msg_len; int rc;
    memset(&m, 0, sizeof m); test_atomic_store(&given.n, 0); test_atomic_store(&mesh.n, 0); test_atomic_store(&wrong.n, 0);
    if (!ob_open(&m, (uint16_t)(TEST_DOMAIN + 5), 1)){ ob_close(&m); return; }
    req = rant_node_schema(m.P, "ObReq { a: u32 }");
    rsp = rant_node_schema(m.P, "ObRsp { b: u32 }");
    pf = rant_node_create_function_definition(m.P, "ob/typed", req, rsp, ob_echo, NULL, NULL);
    cf = rant_node_create_remote_function(m.C, "ob/typed", rant_node_schema(m.C, "ObReq { a: u32 }"),
                                          rant_node_schema(m.C, "ObRsp { b: u32 }"), NULL);
    other = rant_node_schema(m.O, "ObOther { z: u64, y: u64 }");
    og = rant_node_create_call_observer(m.O, "ob/typed", rant_node_schema(m.O, "ObReq { a: u32 }"), NULL,
                                        rant_node_schema(m.O, "ObRsp { b: u32 }"), ob_on_event, &given, NULL);
    CHECK(pf && cf && other && og, "schemas: handles created");
    if (!(pf && cf && other && og)){ ob_close(&m); return; }
    OB_UNTIL(&m, 8000, rant_function_match_count(cf) == 1 && rant_topic_match_count(pf->obs) == 1);
    i_rant_le_w32(msg, 7); test_atomic_store(&ob_reply_done, 0);
    rc = rant_function_call_async(cf, rant_bytes(msg, 4), ob_on_reply, NULL, NULL);
    CHECK(rc == RANT_OK, "schemas: typed call accepted (%d)", rc);
    OB_UNTIL(&m, 4000, ob_n(&given) >= 2);
    CHECK(ob_n(&given) == 2 && given.ev[0].schema && given.ev[1].schema
          && rant_schema_hash(given.ev[0].schema) == rant_schema_hash(req)
          && rant_schema_hash(given.ev[1].schema) == rant_schema_hash(rsp), "schemas: passed ones are offered");
    rc = rant_call_observer_retire(og);
    CHECK(rc == RANT_OK, "schemas: retired (%d)", rc);
    OB_UNTIL(&m, 4000, rant_topic_match_count(pf->obs) == 0);

    om = rant_node_create_call_observer(m.O, "ob/typed", NULL, NULL, NULL, ob_on_event, &mesh,
                                        &(RantCallObserverOpts){ .reflect_from_mesh = 1 });
    CHECK(om != NULL, "schemas: reflecting observer");
    if (!om){ ob_close(&m); return; }
    OB_UNTIL(&m, 4000, rant_topic_match_count(pf->obs) == 1);
    ob_pump(&m, 60);
    test_atomic_store(&ob_reply_done, 0);
    rant_function_call_async(cf, rant_bytes(msg, 4), ob_on_reply, NULL, NULL);
    OB_UNTIL(&m, 4000, ob_n(&mesh) >= 2);
    CHECK(ob_n(&mesh) == 2 && mesh.ev[0].schema && mesh.ev[1].schema
          && rant_schema_hash(mesh.ev[0].schema) == rant_schema_hash(req)
          && rant_schema_hash(mesh.ev[1].schema) == rant_schema_hash(rsp), "schemas: the mesh's are offered");
    rc = rant_call_observer_retire(om);
    CHECK(rc == RANT_OK, "schemas: retired too (%d)", rc);
    OB_UNTIL(&m, 4000, rant_topic_match_count(pf->obs) == 0);

    ow = rant_node_create_call_observer(m.O, "ob/typed", other, NULL, other, ob_on_event, &wrong, NULL);
    CHECK(ow != NULL, "schemas: wrong shape observer");
    if (!ow){ ob_close(&m); return; }
    OB_UNTIL(&m, 4000, rant_topic_match_count(pf->obs) == 1);
    test_atomic_store(&wrong.n, 0); test_atomic_store(&ob_reply_done, 0);
    rant_function_call_async(cf, rant_bytes(msg, 4), ob_on_reply, NULL, NULL);
    OB_UNTIL(&m, 4000, ob_n(&wrong) >= 2);
    CHECK(ob_n(&wrong) == 2 && !wrong.ev[0].schema && !wrong.ev[1].schema && wrong.ev[0].data_len == 4,
          "schemas: another shape is never offered, the bytes still arrive");

    /* a wider caller: its field a comes second here, so only the raw bytes are honest */
    wide = rant_node_schema(m.W, "ObReq { x: u32, a: u32 }");
    cw = rant_node_create_remote_function(m.W, "ob/typed", wide, NULL, NULL);
    CHECK(wide && cw, "schemas: wide caller");
    if (!cw){ ob_close(&m); return; }
    OB_UNTIL(&m, 4000, rant_function_match_count(cw) == 1);
    CHECK(rant_function_match_count(cw) == 1, "schemas: wide caller matched (%d)", rant_function_match_count(cw));
    i_rant_le_w32(msg, 1); i_rant_le_w32(msg + 4, 2); msg_len = 8;
    test_atomic_store(&wrong.n, 0); test_atomic_store(&ob_reply_done, 0);
    rant_function_call_async(cw, rant_bytes(msg, msg_len), ob_on_reply, NULL, NULL);
    OB_UNTIL(&m, 4000, ob_n(&wrong) >= 1);
    CHECK(ob_n(&wrong) >= 1 && wrong.ev[0].kind == RANT_CALL_EVENT_CALL && !wrong.ev[0].schema && wrong.ev[0].data_len == 8,
          "schemas: a wider request stays raw (%d)", ob_n(&wrong));
    ob_close(&m);
}

/* With a queue the callback runs at dispatch, not inside the poll. A retire from it is refused. */
static volatile int ob_q_seen, ob_q_retire_rc; static RantCallObserver *ob_q_handle;
static void ob_q_event(const RantCallEvent *e, void *user){
    (void)e; (void)user;
    ob_q_seen++;
    ob_q_retire_rc = rant_call_observer_retire(ob_q_handle);
}
static void queued(void){
    ObMesh m; RantFunction *pf, *cf; RantQueue *q; int rc; uint8_t req[4];
    memset(&m, 0, sizeof m); ob_q_seen = 0; ob_q_retire_rc = 0;
    if (!ob_open(&m, (uint16_t)(TEST_DOMAIN + 6), 0)){ ob_close(&m); return; }
    pf = rant_node_create_function_definition(m.P, "ob/q", NULL, NULL, ob_echo, NULL, NULL);
    cf = rant_node_create_remote_function(m.C, "ob/q", NULL, NULL, NULL);
    q = rant_node_create_queue(m.O);
    ob_q_handle = rant_node_create_call_observer(m.O, "ob/q", NULL, NULL, NULL, ob_q_event, NULL,
                                                 &(RantCallObserverOpts){ .queue = q });
    CHECK(pf && cf && q && ob_q_handle, "queued: handles created");
    if (!(pf && cf && q && ob_q_handle)){ ob_close(&m); return; }
    OB_UNTIL(&m, 8000, rant_function_match_count(cf) == 1 && rant_topic_match_count(pf->obs) == 1);
    i_rant_le_w32(req, 1); test_atomic_store(&ob_reply_done, 0);
    rant_function_call_async(cf, rant_bytes(req, 4), ob_on_reply, NULL, NULL);
    OB_UNTIL(&m, 4000, test_atomic_load(&ob_reply_done));
    ob_pump(&m, 60);
    CHECK(ob_q_seen == 0, "queued: nothing runs before a dispatch (%d)", ob_q_seen);
    rc = rant_queue_dispatch(q, 0, 0);
    CHECK(rc == 2 && ob_q_seen == 2, "queued: both steps run at dispatch (rc=%d seen=%d)", rc, ob_q_seen);
    CHECK(ob_q_retire_rc == RANT_ERR_STATE, "queued: a retire from the callback is refused (%d)", ob_q_retire_rc);
    rc = rant_call_observer_retire(ob_q_handle);
    CHECK(rc == RANT_OK, "queued: retired from outside (%d)", rc);
    ob_close(&m);
}

/* An observer that stops polling never slows a provider, and a burst one pass drains all lands. */
static void slow_observer(void){
    static ObLog log; ObMesh m; RantFunction *pf, *cf; RantCallObserver *ob; int i, answered = 0; uint8_t req[4];
    uint64_t t0, took;
    memset(&m, 0, sizeof m); test_atomic_store(&log.n, 0);
    if (!ob_open(&m, (uint16_t)(TEST_DOMAIN + 7), 0)){ ob_close(&m); return; }
    pf = rant_node_create_function_definition(m.P, "ob/slow", NULL, NULL, ob_echo, NULL, NULL);
    cf = rant_node_create_remote_function(m.C, "ob/slow", NULL, NULL, NULL);
    ob = rant_node_create_call_observer(m.O, "ob/slow", NULL, NULL, NULL, ob_on_event, &log, NULL);
    CHECK(pf && cf && ob, "slow: handles created");
    if (!(pf && cf && ob)){ ob_close(&m); return; }
    OB_UNTIL(&m, 8000, rant_function_match_count(cf) == 1 && rant_topic_match_count(pf->obs) == 1);

    /* eight calls before the provider polls once: sixteen steps fit its ring */
    test_atomic_store(&ob_reply_done, 0);
    for (i = 0; i < 8; i++){
        i_rant_le_w32(req, (uint32_t)i);
        rant_function_call_async(cf, rant_bytes(req, 4), ob_on_reply, NULL, NULL);
    }
    OB_UNTIL(&m, 4000, ob_n(&log) >= 16 && test_atomic_load(&ob_reply_done) == 8);
    CHECK(test_atomic_load(&ob_reply_done) == 8 && ob_n(&log) == 16, "slow: a burst lands whole (%d answers, %d steps)", test_atomic_load(&ob_reply_done), ob_n(&log));

    /* the observer stops. Far more calls than the ring holds still answer at full speed */
    t0 = i_rant_plat_now_us();
    for (i = 0; i < 200; i++){
        int t;
        i_rant_le_w32(req, (uint32_t)i); test_atomic_store(&ob_reply_done, 0);
        rant_function_call_async(cf, rant_bytes(req, 4), ob_on_reply, NULL, NULL);
        for (t = 0; t < 2000 && !ob_reply_done; t++){ rant_node_poll(m.P, 1); rant_node_poll(m.C, 1); }
        if (test_atomic_load(&ob_reply_done)) answered++;
    }
    took = i_rant_plat_now_us() - t0;
    CHECK(answered == 200, "slow: every call answered with the observer stopped (%d)", answered);
    CHECK(took < 10000000u, "slow: and without waiting on it (%llu us)", (unsigned long long)took);

    /* it comes back and resumes on the newest steps */
    test_atomic_store(&log.n, 0); ob_pump(&m, 200);
    test_atomic_store(&ob_reply_done, 0); test_atomic_store(&log.n, 0);
    rant_function_call_async(cf, rant_bytes(req, 4), ob_on_reply, NULL, NULL);
    OB_UNTIL(&m, 8000, test_atomic_load(&ob_reply_done) && ob_n(&log) >= 2);
    CHECK(ob_n(&log) >= 2 && log.ev[ob_n(&log) - 1].kind == RANT_CALL_EVENT_END, "slow: it recovers (%d)", ob_n(&log));
    ob_close(&m);
}

/* Every node on its service thread, the provider working from this one: the observer's callback
 * runs on its own thread, and its retire waits for that callback. */
static void threaded(void){
    static ObLog log; ObMesh m; RantFunction *pt, *ct; RantCallObserver *ob; uint32_t id = 0; int rc, i; uint8_t b[4];
    memset(&m, 0, sizeof m); test_atomic_store(&log.n, 0);
    if (!ob_open(&m, (uint16_t)(TEST_DOMAIN + 8), 0)){ ob_close(&m); return; }
    pt = rant_node_create_task_definition(m.P, "ob/thr", NULL, NULL, NULL, ob_defer, NULL, NULL);
    ct = rant_node_create_remote_task(m.C, "ob/thr", NULL, NULL, NULL, NULL);
    ob = rant_node_create_call_observer(m.O, "ob/thr", NULL, NULL, NULL, ob_on_event, &log, NULL);
    CHECK(pt && ct && ob, "threaded: handles created");
    if (!(pt && ct && ob)){ ob_close(&m); return; }
    rant_node_start(m.P); rant_node_start(m.C); rant_node_start(m.O); rant_node_start(m.W);
    WAIT_UNTIL(8000, rant_function_match_count(ct) == 1 && rant_topic_match_count(pt->obs) == 1);
    test_atomic_store(&ob_token, 0); test_atomic_store(&ob_reply_done, 0);
    rc = rant_function_call_async(ct, rant_bytes(NULL, 0), ob_on_reply, NULL, &(RantCallOpts){ .id_out = &id });
    CHECK(rc == RANT_OK, "threaded: call accepted (%d)", rc);
    WAIT_UNTIL(4000, (uint64_t)test_atomic_load(&ob_token) != 0);
    for (i = 0; i < 50; i++){
        i_rant_le_w32(b, (uint32_t)i);
        rc = rant_function_progress(pt, (uint64_t)test_atomic_load(&ob_token), rant_bytes(b, 4));
        CHECK(rc == RANT_OK, "threaded: progress %d (%d)", i, rc);
        test_sleep_ms(1);
    }
    rc = rant_function_complete(pt, (uint64_t)test_atomic_load(&ob_token), RANT_CALL_OK, NULL, rant_bytes(NULL, 0));
    CHECK(rc == RANT_OK, "threaded: complete (%d)", rc);
    WAIT_UNTIL(4000, ob_n(&log) >= 53 && test_atomic_load(&ob_reply_done));
    CHECK(ob_n(&log) == 53, "threaded: call, RUNNING, 50 updates and the end (%d)", ob_n(&log));
    if (ob_n(&log) == 53){
        int in_order = 1;
        for (i = 0; i < 50; i++) if (log.ev[2 + i].kind != RANT_CALL_EVENT_PROGRESS || log.ev[2 + i].val != (uint32_t)i) in_order = 0;
        CHECK(in_order && log.ev[52].kind == RANT_CALL_EVENT_END, "threaded: in order");
    }
    rc = rant_call_observer_retire(ob);
    CHECK(rc == RANT_OK, "threaded: retired (%d)", rc);
    ob_close(&m);
}

/* What only a caller can know goes on its @rant/log, once per failed call and never for @rant/meta. */
static volatile int ob_lines_end; static char ob_line[256];
static void ob_on_line(const RantMsg *msg){
    RantString t;
    if (msg->topic_name.len != 9 || memcmp(msg->topic_name.data, "@rant/log", 9) != 0) return;
    t = rant_get_string(msg->data, msg->schema, "text");
    if (t.len >= 8 && memcmp(t.data, "call of ", 8) == 0){
        size_t l = t.len < sizeof ob_line - 1 ? t.len : sizeof ob_line - 1;
        memcpy(ob_line, t.data, l); ob_line[l] = 0;
        ob_lines_end++;
    }
}
static void caller_failures(void){
    ObMesh m; RantFunction *cn; RantTopic *lines; int rc; RantFunctionOpts fo;
    memset(&m, 0, sizeof m); ob_lines_end = 0; ob_line[0] = 0;
    ob_watch_on_message = ob_on_line;
    rc = ob_open(&m, (uint16_t)(TEST_DOMAIN + 9), 0);
    ob_watch_on_message = NULL;
    if (!rc){ ob_close(&m); return; }
    lines = rant_node_log_topic(m.O);
    CHECK(lines && rant_topic_set_role(lines, RANT_PUBSUB) == RANT_OK, "failures: reading the log");
    memset(&fo, 0, sizeof fo); fo.timeout_us = 100000;
    cn = rant_node_create_remote_function(m.C, "ob/nobody", NULL, NULL, &fo);
    CHECK(cn != NULL, "failures: remote created");
    if (!cn){ ob_close(&m); return; }
    OB_UNTIL(&m, 4000, rant_topic_match_count(rant_node_log_topic(m.C)) >= 1);
    test_atomic_store(&ob_reply_done, 0); ob_reply_status = RANT_CALL_OK;
    rc = rant_function_call_async(cn, rant_bytes(NULL, 0), ob_on_reply, NULL, NULL);
    CHECK(rc == RANT_OK, "failures: call accepted (%d)", rc);
    OB_UNTIL(&m, 4000, test_atomic_load(&ob_reply_done) && ob_lines_end >= 1);
    CHECK(test_atomic_load(&ob_reply_done) && ob_reply_status == RANT_CALL_NO_PROVIDER, "failures: no provider (%d)", (int)ob_reply_status);
    CHECK(ob_lines_end == 1 && strcmp(ob_line, "call of ob/nobody ended: no provider") == 0,
          "failures: one line says so (%d: %s)", ob_lines_end, ob_line);
    ob_close(&m);
}

/* A report that lies about its lengths, or names an event that does not exist, is dropped whole,
 * and the good one after it still lands. */
static void malformed_reports(void){
    static ObLog log; ObMesh m; RantCallObserver *ob; RantTopic *raw; RantTopicOpts topt;
    uint8_t buf[64]; uint32_t i;
    memset(&m, 0, sizeof m); test_atomic_store(&log.n, 0);
    if (!ob_open(&m, (uint16_t)(TEST_DOMAIN + 10), 0)){ ob_close(&m); return; }
    ob = rant_node_create_call_observer(m.O, "ob/bad", NULL, NULL, NULL, ob_on_event, &log, NULL);
    memset(&topt, 0, sizeof topt); topt.qos.reliability = RANT_RELIABLE; topt.qos.keep_last = 16;
    raw = i_rant_node_create_pattern_topic(m.P, "ob/bad@obs", RANT_PUB_ONLY, NULL, &topt,
                                           RANT_KIND_CALL_OBS, RANT__OBS_PREFIX, 0, 0, NULL, NULL);
    CHECK(ob && raw, "malformed: handles created");
    if (!(ob && raw)){ ob_close(&m); return; }
    OB_UNTIL(&m, 8000, rant_topic_match_count(raw) == 1 && rant_call_observer_match_count(ob) == 1);
    CHECK(rant_topic_match_count(raw) == 1, "malformed: matched");
    memset(buf, 0, sizeof buf);
    (void)i_rant_topic_send_hdr(raw, rant_bytes(buf, RANT__OBS_PREFIX), rant_bytes(NULL, 0));     /* no strings at all */
    buf[RANT__OBS_PREFIX] = 200;     /* a caller name longer than the report */
    (void)i_rant_topic_send_hdr(raw, rant_bytes(buf, RANT__OBS_PREFIX + 2u), rant_bytes(NULL, 0));
    buf[RANT__OBS_PREFIX] = 0; buf[RANT__OBS_PREFIX + 1] = 200;     /* a text longer than the report */
    (void)i_rant_topic_send_hdr(raw, rant_bytes(buf, RANT__OBS_PREFIX + 2u), rant_bytes(NULL, 0));
    buf[RANT__OBS_PREFIX + 1] = 0; buf[8] = 200;     /* an event that does not exist */
    (void)i_rant_topic_send_hdr(raw, rant_bytes(buf, RANT__OBS_PREFIX + 2u), rant_bytes(NULL, 0));
    (void)i_rant_topic_send_hdr(raw, rant_bytes(buf, 3), rant_bytes(NULL, 0));     /* shorter than the prefix */
    ob_pump(&m, 100);
    CHECK(ob_n(&log) == 0, "malformed: all dropped (%d)", ob_n(&log));
    buf[8] = RANT_CALL_EVENT_END; i_rant_le_w32(buf + 4, 77);
    (void)i_rant_topic_send_hdr(raw, rant_bytes(buf, RANT__OBS_PREFIX + 2u), rant_bytes(NULL, 0));
    for (i = 0; i < 2000u && ob_n(&log) < 1; i++) ob_pump(&m, 1);
    CHECK(ob_n(&log) == 1 && log.ev[0].kind == RANT_CALL_EVENT_END && log.ev[0].call_id == 77,
          "malformed: a good one still lands (%d)", ob_n(&log));
    ob_close(&m);
}

static const TestCase cases[] = {
    { "function_steps", function_steps },
    { "task_steps", task_steps },
    { "no_provider_lookalike", no_provider_lookalike },
    { "refused_twin", refused_twin },
    { "no_cost_unwatched", no_cost_unwatched },
    { "schemas", schemas },
    { "queued", queued },
    { "slow_observer", slow_observer },
    { "threaded", threaded },
    { "caller_failures", caller_failures },
    { "malformed_reports", malformed_reports },
};

TEST_MAIN(cases)
