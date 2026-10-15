/* The selftest: every phase of the core that runs real nodes on loopback, one case per
 * phase. spec/testing.md catalogues them. */
#include "support/test.h"

/* topic handles are array indices (declaration order in ch[]) */
#define ST_CH_GAP   0   /* reliable, depth 4, no backpressure  */
#define ST_CH_BLOCK 1   /* reliable, depth 4, slow_reader_wait 100 ms */
#define ST_CH_DYN   2   /* reliable, depth 4, reader starts RANT_INACTIVE */
#define ST_CH_BLOCK2 3  /* like BLOCK but repair_delay>0: the reader's ack is
                           timer-armed, so it relies on the periodic sweep */
#define ST_DEPTH    4
#define ST_BLOCK_US 100000u
#define ST_NACK_US  5000u
#define ST_NCH      40

#ifdef RANT_SHM

/* (3) full nodes on loopback: SHM across size classes (byte-exact + shm_tx/rx), and
   the inline fallback for a non-SHM-capable subscriber. */
static int shmn_recv; static size_t shmn_len; static unsigned long shmn_sum;
static void shmn_on_message(const RantMsg *msg){
    const unsigned char *p=(const unsigned char*)msg->data.data; size_t i, len=msg->data.len; unsigned long s=0;
    for(i=0;i<len;i++) s+=p[i];
    shmn_recv++; shmn_len=len; shmn_sum=s;
}
static RantNode *shmn_open(int is_pub, int shm_capable, uint16_t domain){
    static i_RantTopicDef ch[2]; static int slot;
    i_RantTopicDef *d=&ch[slot++ & 1]; RantAddr seed; RantNodeOpts opts;
    RantQos q; memset(&q,0,sizeof q);
    q.reliability=RANT_RELIABLE; q.keep_last=4; q.catch_up=1;
    memset(d,0,sizeof *d); d->name="shmnode"; d->qos=q; d->role=is_pub?RANT_PUB_ONLY:RANT_SUB_ONLY;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&opts,0,sizeof opts); opts.domain=domain;   /* dynamic mode is SHM capable */
    opts.disable_shm = (uint8_t)(!shm_capable);   /* a non-SHM peer forces the inline UDP path */
    opts.discovery.max_peers=4;
    opts.net.multicast_interface="127.0.0.1"; opts.net.seed_peers=&seed; opts.net.n_seed_peers=1;
    return test_node_open(NULL, is_pub?NULL:shmn_on_message, NULL, opts, d, 1);
}
static void shm_node_checks(void){
    static unsigned char buf[6*1024*1024];
    RantNode *P,*S; int i; uint32_t tx=0, rx=0; size_t sizes[3];
    P=shmn_open(1,1,(uint16_t)(TEST_DOMAIN+40)); S=shmn_open(0,1,(uint16_t)(TEST_DOMAIN+40));
    CHECK(P&&S, "shm-node: SHM-capable pub + sub open");
    if (P&&S){
        for (i=0;i<800 && test_match_count(P,0)==0;i++){ rant_node_poll(P,2); rant_node_poll(S,2); }
        CHECK(test_match_count(P,0)>0, "shm-node: matched");
        /* sizes[0] fits one datagram so it ships inline, not SHM. The two larger ones would
           fragment, so they take the SHM path. */
        sizes[0]=200; sizes[1]=300*1024; sizes[2]=4*1024*1024;
        for (i=0;i<3;i++){
            unsigned long want=0; size_t j; int before=shmn_recv, t;
            for (j=0;j<sizes[i];j++){ buf[j]=(unsigned char)((j*31u+(unsigned)i+1)&0xFF); want+=buf[j]; }
            test_send(P,0,buf,sizes[i]);
            for (t=0;t<500 && shmn_recv==before;t++){ rant_node_poll(P,2); rant_node_poll(S,2); }
            CHECK(shmn_recv==before+1 && shmn_len==sizes[i] && shmn_sum==want,
                     "shm-node: byte-exact %lu bytes", (unsigned long)sizes[i]);
            if (i==0){ tx=0; rant_node_shm_stats(P,&tx,NULL);
                CHECK(tx==0, "shm-node: sub-fragment %lu B went inline, not SHM (tx=%u)",
                         (unsigned long)sizes[i], tx); }
        }
        rant_node_shm_stats(P,&tx,NULL); rant_node_shm_stats(S,NULL,&rx);
        CHECK(tx==2 && rx==2, "shm-node: 2 fragmenting msgs over SHM, small inline (tx=%u rx=%u)", tx, rx);
        rant_node_close(P,1); rant_node_close(S,1);
    }
    /* inline fallback: a non-SHM-capable subscriber forces inline UDP */
    shmn_recv=0;
    P=shmn_open(1,1,(uint16_t)(TEST_DOMAIN+42)); S=shmn_open(0,0,(uint16_t)(TEST_DOMAIN+42));
    if (P&&S){
        unsigned long want=0; size_t j; int before, t;
        for (i=0;i<800 && test_match_count(P,0)==0;i++){ rant_node_poll(P,2); rant_node_poll(S,2); }
        for (j=0;j<50*1024;j++){ buf[j]=(unsigned char)((j*31u+9)&0xFF); want+=buf[j]; }
        before=shmn_recv; test_send(P,0,buf,50*1024);
        for (t=0;t<500 && shmn_recv==before;t++){ rant_node_poll(P,2); rant_node_poll(S,2); }
        CHECK(shmn_recv==before+1 && shmn_sum==want, "shm-node: non-SHM sub -> inline byte-exact");
        tx=0; rant_node_shm_stats(P,&tx,NULL);
        CHECK(tx==0, "shm-node: no SHM used for the non-SHM reader (tx=%u)", tx);
        rant_node_close(P,1); rant_node_close(S,1);
    }
}

#endif /* RANT_SHM */

/* Exercise the staged cleanup paths of rant_node_open: force a failure at a different
 * stage each time, assert NULL, then confirm a normal open still works. */
/* open-failure event sink: capture the RANT_ERROR kind fired during a failing open */
static int of_seen_error;
static void of_on_event(const RantEvent *ev){ if (ev->kind == RANT_ERROR) of_seen_error = (int)ev->error; }

static void open_fail_checks(void){
    static uint8_t mem[1<<20];
    RantNode *n;
    of_seen_error = RANT_E_NONE;

    /* create fail: an over long topic name is rejected by rant_node_create_topic. The node
       opened fine and stays usable. */
    {   static char longname[RANT_TOPIC_NAME_MAX + 8]; RantTopic *c;
        RantAllocator a = rant_allocator_static(mem, sizeof mem);
        memset(longname, 'x', sizeof longname - 1); longname[sizeof longname - 1] = 0;
        n = rant_node_open(&a, NULL, NULL, NULL, &(RantNodeOpts){ .domain=TEST_DOMAIN });
        CHECK(n != NULL, "open-fail: node opens for create-fail check");
        c = n ? rant_node_create_topic(n, longname, RANT_PUBSUB, NULL, NULL) : NULL;
        CHECK(c == NULL, "open-fail: over-long topic name -> create_channel NULL");
        if (n) rant_node_close(n, 0);
    }

    /* a non multicast discovery group fails the IGMP join, so the node unwinds through
       fail_sock and rant_last_error(NULL) names MCAST_JOIN with no handle to query */
    {   RantAllocator a = rant_allocator_static(mem, sizeof mem);
        RantEvent err; char line[160];
        n = rant_node_open(&a, NULL, NULL, NULL,
            &(RantNodeOpts){ .domain=TEST_DOMAIN, .net={ .discovery_group="1.2.3.4" } });
        CHECK(n == NULL, "open-fail: non-multicast discovery group -> NULL");
        err = rant_last_error(NULL);
        CHECK(err.kind == RANT_ERROR && err.error == RANT_E_MCAST_JOIN,
                 "open-fail: last_error is RANT_E_MCAST_JOIN (%s)", rant_event_str(&err, line, sizeof line));
        if (n) rant_node_close(n, 0);
    }

    /* fail_sock: occupy an ephemeral port, then aim the node's data socket at it. The bind
       takes no reuse, so it collides and last_error names RANT_E_BIND with port and errno. */
    {   i_RantSock occupy;
        i_rant_plat_startup();
        occupy = i_rant_plat_udp_open();
        if (occupy != RANT_SOCK_BAD && i_rant_plat_bind(occupy, 0, 0, 0)){
            uint16_t port = i_rant_plat_local_port(occupy);
            RantAllocator a = rant_allocator_static(mem, sizeof mem);
            RantEvent err; char line[160];
            n = rant_node_open(&a, NULL, NULL, NULL,
                &(RantNodeOpts){ .domain=TEST_DOMAIN, .net={ .data_port=port } });
            CHECK(n == NULL, "open-fail: data-port collision -> NULL (fail_sock)");
            err = rant_last_error(NULL);
            CHECK(err.kind == RANT_ERROR && err.error == RANT_E_BIND && err.port == port,
                     "open-fail: last_error is RANT_E_BIND port=%u (%s)", port, rant_event_str(&err, line, sizeof line));
            if (n) rant_node_close(n, 0);
        }
        if (occupy != RANT_SOCK_BAD) i_rant_plat_close(occupy);
        i_rant_plat_cleanup();
    }

    /* an on_event handler also receives the open failure directly (no handle needed): the
       node fires it on the passed-in callback before returning NULL. */
    {   RantAllocator a = rant_allocator_static(mem, sizeof mem);
        n = rant_node_open(&a, NULL, NULL, of_on_event,
            &(RantNodeOpts){ .domain=TEST_DOMAIN, .net={ .discovery_group="1.2.3.4" } });
        CHECK(n == NULL, "open-fail: open still returns NULL with on_event set");
        CHECK(of_seen_error == RANT_E_MCAST_JOIN,
                 "open-fail: on_event received the failure (error=%d)", of_seen_error);
        if (n) rant_node_close(n, 0);
    }

    /* after the failed opens a normal open must still succeed (cleanup balanced) */
    {   RantAllocator a = rant_allocator_static(mem, sizeof mem);
        n = rant_node_open(&a, NULL, NULL, NULL, &(RantNodeOpts){ .domain=TEST_DOMAIN });
        CHECK(n != NULL, "open-fail: normal open still works after failures");
        if (n) rant_node_close(n, 0);
    }
}

/* An SHM capable node rewraps the transport callbacks to forward the app's user_data, so
 * a transport fired event must reach on_event with that same user_data (spec/testing.md). */
static void *evu_user; static int evu_collisions;
static void evu_on_event(const RantEvent *ev){
    if (ev->kind == RANT_ERROR && ev->error == RANT_E_NAME_COLLISION){ evu_user = ev->user; evu_collisions++; }
}
static void event_user_checks(void){
 static int sentinel;
    const char *A="iuZA9tcJzAG", *B="5wVGxhTCmOC";   /* both to one identity, see collide.c */
    i_RantTopicDef cw, cr; RantNodeOpts wo, ro; RantNode *w, *r;
    uint8_t payload[16]; int i; memset(payload,0x5A,sizeof payload);
    memset(&cw,0,sizeof cw);
    cw.name=A; cw.role=RANT_PUB_ONLY;
    cw.qos.reliability=RANT_RELIABLE; cw.qos.keep_last=1; cw.qos.catch_up=1;
    cw.qos.max_message_bytes=32; cw.qos.heartbeat_us=50000;
    cr=cw; cr.name=B; cr.role=RANT_SUB_ONLY;
    wo = (RantNodeOpts){ .domain=TEST_DOMAIN+5, .discovery={ .max_peers=4 } };
    ro = wo;
    ro.user_data=&sentinel;   /* dynamic mode, so SHM capable */
    evu_user=NULL; evu_collisions=0;
    w=test_node_open(NULL,NULL,NULL,wo,&cw,1);
    r=test_node_open(NULL,NULL,evu_on_event,ro,&cr,1);
    CHECK(w && r, "event-user: nodes open");
    if (w && r){
        for (i=0;i<120 && evu_collisions==0;i++){ test_send(w,0,payload,16); rant_node_poll(w,0); rant_node_poll(r,20); }
        CHECK(evu_collisions >= 1, "event-user: collision event fired (%d)", evu_collisions);
        CHECK(evu_user == (void*)&sentinel,
                 "event-user: on_event gets user_data not node ptr (got %p want %p)", evu_user, (void*)&sentinel);
        rant_node_close(r,1); rant_node_close(w,1);
    }
}

/* Dynamic growth: creating topics past the reserve relocates the node into a bigger arena.
 * Stream on topic 0 across several grows and assert nothing was lost, duplicated or reordered. */
static int dg_recv[24]; static int dg_seq_ok; static int dg_next0;
static void dg_on_message(const RantMsg *msg){
    if (msg->topic_index < 24) dg_recv[msg->topic_index]++;
    if (msg->topic_index == 0 && msg->data.len >= 1){
        int s = ((const uint8_t*)msg->data.data)[0];
        if (s != dg_next0) dg_seq_ok = 0;            /* gap, dup or reorder */
        dg_next0 = s + 1;
    }
}
static void dynamic_grow_checks(void){
    static const char *names[12] = {"dg/0","dg/1","dg/2","dg/3","dg/4","dg/5",
                                    "dg/6","dg/7","dg/8","dg/9","dg/10","dg/11"};
    RantAllocator pa = rant_allocator_heap(0), sa = rant_allocator_heap(0);
    RantNodeOpts po, so; RantNode *P=NULL, *S=NULL; RantTopic *pc0=NULL, *pcN;
    RantTopicOpts co; RantAddr seed; uint8_t payload[8]; int i, t;
    memset(&co,0,sizeof co); co.qos.reliability=RANT_RELIABLE; co.qos.keep_last=32;
    co.qos.catch_up=32; co.qos.heartbeat_us=50000;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=TEST_DOMAIN+7; po.max_topics=2; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po;
    for (i=0;i<24;i++) dg_recv[i]=0;
    dg_seq_ok=1; dg_next0=0;
    P = rant_node_open(&pa, "dg-pub", NULL, NULL, &po);
    S = rant_node_open(&sa, "dg-sub", dg_on_message, NULL, &so);
    CHECK(P&&S, "dyn-grow: nodes open (max_topics=2)");
    if (!(P&&S)){ if(P)rant_node_close(P,0); if(S)rant_node_close(S,0); return; }
    pc0 = rant_node_create_topic(P, names[0], RANT_PUB_ONLY, NULL, &co);
    rant_node_create_topic(S, names[0], RANT_SUB_ONLY, NULL, &co);
    CHECK(pc0 != NULL, "dyn-grow: topic 0 created");
    for (t=0;t<800 && rant_topic_match_count(pc0)==0;t++){ rant_node_poll(P,2); rant_node_poll(S,2); }
    CHECK(rant_topic_match_count(pc0)>0, "dyn-grow: topic 0 matched");
    for (i=0;i<5;i++){ payload[0]=(uint8_t)i; rant_topic_send(pc0,rant_bytes(payload,1), NULL); rant_node_poll(P,1); rant_node_poll(S,2); }
    /* create topics 1 to 11 on both: several grows, max_topics 2, 4, 8, 16 */
    for (i=1;i<12;i++){ rant_node_create_topic(P, names[i], RANT_PUB_ONLY, NULL, &co);
                        rant_node_create_topic(S, names[i], RANT_SUB_ONLY, NULL, &co);
                        rant_node_poll(P,1); rant_node_poll(S,1); }
    CHECK(rant_topic_match_count(pc0)>0, "dyn-grow: topic 0 still matched after grows");
    for (i=5;i<10;i++){ payload[0]=(uint8_t)i; rant_topic_send(pc0,rant_bytes(payload,1), NULL); rant_node_poll(P,1); rant_node_poll(S,2); }
    for (t=0;t<400 && dg_recv[0]<10;t++){ rant_node_poll(P,1); rant_node_poll(S,2); }
    CHECK(dg_recv[0]==10, "dyn-grow: all 10 on topic 0 delivered across grows (got %d)", dg_recv[0]);
    CHECK(dg_seq_ok, "dyn-grow: topic 0 in-order, no loss/dup across grows");
    pcN = rant_node_topic(P, 11);                    /* a topic created AFTER a grow */
    for (i=0;i<3;i++){ payload[0]=0xAA; if(pcN) rant_topic_send(pcN,rant_bytes(payload,1), NULL); rant_node_poll(P,1); rant_node_poll(S,2); }
    for (t=0;t<200 && dg_recv[11]<3;t++){ rant_node_poll(P,1); rant_node_poll(S,2); }
    CHECK(dg_recv[11]==3, "dyn-grow: post-grow topic delivers (got %d)", dg_recv[11]);
    rant_node_close(P,1); rant_node_close(S,1);
}

/* (18) announce interest: one positional [u32 hash][u8 flags] entry per topic slot. An
   inactive topic is not yielded but holds its position (spec/testing.md). */
static void schema_advert_checks(void){
    RantAllocator pa = rant_allocator_heap(0);
    RantAllocator sa = rant_allocator_heap(0);
    RantAllocator ma = rant_allocator_heap(0);       /* caller side schema */
    RantNodeOpts po, so; RantNode *P=NULL, *S=NULL; RantTopic *pc;
    RantTopicOpts co; RantAddr seed; RantSchema *sch=NULL;
    uint32_t pose_h = (uint32_t)i_rant_topic_id("sch/pose");
    uint32_t raw_h  = (uint32_t)i_rant_topic_id("sch/raw");
    uint32_t late_h = (uint32_t)i_rant_topic_id("sch/late");
    int t, pose_ok=0, raw_ok=0;
    const i_RantDiscoveryPeerView *peers; uint16_t n_peers=0;

    sch = test_compile(&ma, "Reading { x: f64, y: f64, tags: u8[16] }", NULL);
    CHECK(sch!=NULL, "announce: schema compiles");
    if (!sch) return;

    memset(&co,0,sizeof co); co.qos.keep_last=4;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=TEST_DOMAIN+8; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po;
    P = rant_node_open(&pa, "sch-pub", NULL, NULL, &po);
    S = rant_node_open(&sa, "sch-sub", NULL, NULL, &so);
    CHECK(P&&S, "announce: nodes open");
    if (!(P&&S)){ if(P)rant_node_close(P,0); if(S)rant_node_close(S,0); rant_allocator_reset(&ma); return; }
    pc = rant_node_create_topic(P, "sch/pose", RANT_PUB_ONLY, sch, &co);     /* index 0, typed */
    rant_node_create_topic(P, "sch/raw",  RANT_PUB_ONLY, NULL, &co);         /* index 1, raw */
    rant_node_create_topic(S, "sch/pose", RANT_SUB_ONLY, sch, &co);
    CHECK(pc!=NULL, "announce: topics created");
    {   RantEvent le; RantBytes w = rant_schema_wire(sch);
        CHECK(pc && rant_node_schema_parse(P, w.data, w.len) == rant_topic_schema(pc),
                 "announce: a create registers its schema in the node, one handle per shape");
        CHECK(rant_node_schema(P, "RantLog { x: u8 }") == NULL, "announce: a builtin name is reserved");
        le = rant_last_error(P);
        CHECK(le.error == RANT_E_BAD_SCHEMA && le.schema_detail
                 && strstr(le.schema_detail, "another shape near: RantLog") != NULL,
                 "announce: the refusal says why and where (%s)", le.schema_detail ? le.schema_detail : "null");
        CHECK(rant_node_schema(P, "Open {") == NULL && rant_last_error(P).schema_detail
                 && strstr(rant_last_error(P).schema_detail, "expected } at the end") != NULL,
                 "announce: a truncated text is refused at the end (%s)",
                 rant_last_error(P).schema_detail ? rant_last_error(P).schema_detail : "null");
        CHECK(rant_node_schema(P, "Reading { p: Nope }") == NULL && rant_last_error(P).schema_detail
                 && strstr(rant_last_error(P).schema_detail, "unknown type near: Nope") != NULL,
                 "announce: an unknown type is named (%s)",
                 rant_last_error(P).schema_detail ? rant_last_error(P).schema_detail : "null");
        CHECK(rant_node_schema(P, "Reading { x: f64 }") != NULL && rant_node_schema(P, "Reading") != NULL,
                 "announce: a definition made on the node names the next compile");
    }
    i_rant_schema_free(sch, rant_allocator_alloc, &ma);       /* the node registered its own: freed NOW */

    /* a typed-typed match through hash nomination + detail verification */
    for (t=0;t<800 && rant_topic_match_count(pc)==0;t++){ rant_node_poll(P,2); rant_node_poll(S,2); }
    CHECK(rant_topic_match_count(pc)>0, "announce: typed match formed via detail exchange");

    peers = test_peers(S, &n_peers);   /* S's view of P */
    CHECK(peers && n_peers==1, "announce: subscriber sees one peer (%u)", n_peers);
    if (peers && n_peers==1){
        i_RantInterestIter it; i_RantTopicEntry tp;
        memset(&it,0,sizeof it);
        while (i_rant_node_peer_interest_next(&peers[0], &it, &tp)){
            if (!tp.is_pub) continue;
            if (tp.index==0 && tp.hash==pose_h && tp.role==RANT_PUB_ONLY) pose_ok=1;
            if (tp.index==1 && tp.hash==raw_h  && tp.role==RANT_PUB_ONLY) raw_ok=1;
        }
        CHECK(pose_ok && raw_ok, "announce: positional indices carry the 32-bit hashes");

        /* the runtime flip flow: an inactive topic holds its position but is not yielded,
           flipping it to publish advertises the same index */
        {   RantTopic *lc = rant_node_create_topic(P, "sch/late", RANT_INACTIVE, NULL, &co);
            CHECK(lc!=NULL, "announce: inactive topic created");
            for (t=0;t<200;t++){ rant_node_poll(P,2); rant_node_poll(S,2); }
            {   i_RantInterestIter it2; i_RantTopicEntry tp2; int seen=0;
                peers = test_peers(S, &n_peers);
                memset(&it2,0,sizeof it2);
                while (i_rant_node_peer_interest_next(&peers[0], &it2, &tp2))
                    if (tp2.hash==late_h) seen=1;
                CHECK(!seen, "announce: inactive topic not advertised");
            }
            rant_topic_set_role(lc, RANT_PUB_ONLY);
            for (t=0;t<400;t++){ rant_node_poll(P,2); rant_node_poll(S,2); }
            {   i_RantInterestIter it2; i_RantTopicEntry tp2; uint16_t late_alias=0xFFFF;
                peers = test_peers(S, &n_peers);
                memset(&it2,0,sizeof it2);
                while (i_rant_node_peer_interest_next(&peers[0], &it2, &tp2))
                    if (tp2.is_pub && tp2.hash==late_h) late_alias=tp2.index;
                CHECK(late_alias==2, "announce: role flip advertises the held position (index %u)",
                         late_alias);
            }
        }
    }
    rant_node_close(P,1); rant_node_close(S,1);
    rant_allocator_reset(&ma);
}

/* (19) reader side subset binding: a subset subscriber matches with a rebased schema, an
   incompatible one is refused on both sides, a wrong size message is dropped. */
static int      sb_recv, sb_schema_ok;
static uint64_t sb_stamp; static double sb_y;
static unsigned long sb_mismatch_n;
static void sb_on_message(const RantMsg *msg){
    sb_recv++;
    if (!msg->schema) return;
    sb_schema_ok = (rant_schema_size(msg->schema) == 25 && rant_schema_field_count(msg->schema) == 2);
    sb_y     = rant_get_f64 (msg->data, msg->schema, "y");
    sb_stamp = rant_get_uint(msg->data, msg->schema, "stamp");
}
static void sb_on_event(const RantEvent *ev){
    if (ev->kind == RANT_ERROR && ev->error == RANT_E_SCHEMA_MISMATCH) sb_mismatch_n++;
}
static void schema_bind_checks(void){
    RantAllocator pa = rant_allocator_heap(0);
    RantAllocator sa = rant_allocator_heap(0);
    RantAllocator ma = rant_allocator_heap(0);
    RantNodeOpts po, so; RantNode *P=NULL, *S=NULL;
    RantTopic *pc, *pc_bad; RantTopicOpts co; RantAddr seed;
    RantSchema *W, *R, *WB, *RB; int t;
    /* writer: the full Reading. reader: a reordered subset of it */
    W  = test_compile(&ma,
             "Reading { stamp: u64, x: f64, y: f64, tag: u8 }", NULL);      /* 25 B */
    R  = test_compile(&ma,
             "Reading { y: f64, stamp: u64 }", NULL);
    WB = test_compile(&ma, "Bad { v: u64 }", NULL);
    RB = test_compile(&ma, "Bad { v: f64 }", NULL);
    CHECK(W && R && WB && RB, "schema-bind: schemas compile");
    CHECK(W && R && rant_schema_subset(R, W) && !rant_schema_subset(W, R),
             "schema-bind: subset is one-way (reader within writer)");
    if (!(W && R && WB && RB)){ rant_allocator_reset(&ma); return; }

    memset(&co,0,sizeof co); co.qos.keep_last=4;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=TEST_DOMAIN+9; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po;
    sb_recv=0; sb_schema_ok=0; sb_stamp=0; sb_y=0.0; sb_mismatch_n=0;
    P = rant_node_open(&pa, "sb-pub", NULL,          sb_on_event, &po);
    S = rant_node_open(&sa, "sb-sub", sb_on_message, sb_on_event, &so);
    CHECK(P&&S, "schema-bind: nodes open");
    if (!(P&&S)){ if(P)rant_node_close(P,0); if(S)rant_node_close(S,0); rant_allocator_reset(&ma); return; }
    pc     = rant_node_create_topic(P, "sb/pose", RANT_PUB_ONLY, W,    &co);
    pc_bad = rant_node_create_topic(P, "sb/bad",  RANT_PUB_ONLY, WB, &co);
    rant_node_create_topic(S, "sb/pose", RANT_SUB_ONLY, R,    &co);
    rant_node_create_topic(S, "sb/bad",  RANT_SUB_ONLY, RB, &co);
    CHECK(pc && pc_bad, "schema-bind: topics created");

    for (t=0;t<800 && rant_topic_match_count(pc)==0;t++){ rant_node_poll(P,2); rant_node_poll(S,2); }
    CHECK(rant_topic_match_count(pc)==1, "schema-bind: subset reader matched");
    CHECK(rant_topic_match_count(pc_bad)==0, "schema-bind: kind-conflict reader refused");
    CHECK(sb_mismatch_n>=1, "schema-bind: refusal surfaced (%lu RANT_E_SCHEMA_MISMATCH)", sb_mismatch_n);

    {   /* publish one Reading packed in the writer's layout. The reader decodes through the
           rebased schema with its own indices */
        uint8_t buf[25]; uint64_t bits; double x=1.5, y=-2.25;
        i_rant_le_w64(buf, 0x1122334455667788ULL);                /* stamp @0 */
        memcpy(&bits,&x,8); i_rant_le_w64(buf+8,    bits);        /* x     @8 */
        memcpy(&bits,&y,8); i_rant_le_w64(buf+16, bits);          /* y     @16 */
        buf[24]=7;                                                /* tag   @24 */
        rant_topic_send(pc, rant_bytes(buf, sizeof buf), NULL);
        for (t=0;t<400 && sb_recv==0;t++){ rant_node_poll(P,1); rant_node_poll(S,2); }
        CHECK(sb_recv==1, "schema-bind: subset message delivered");
        CHECK(sb_schema_ok, "schema-bind: RantMsg.schema is the rebased view (writer size, reader fields)");
        CHECK(sb_y==-2.25 && sb_stamp==0x1122334455667788ULL,
                 "schema-bind: reader indices read the writer's offsets (y=%.2f)", sb_y);
    }
    {   /* a message that does not fit the sender's schema is refused at the sender */
        unsigned long before = sb_mismatch_n;
        uint8_t junk[3] = {1,2,3};
        int rc = rant_topic_send(pc, rant_bytes(junk, sizeof junk), NULL);
        for (t=0;t<50;t++){ rant_node_poll(P,1); rant_node_poll(S,2); }
        CHECK(rc==RANT_ERR_SCHEMA && sb_recv==1 && sb_mismatch_n==before,
                 "schema-bind: wrong-size message refused at the sender (rc=%d recv=%d)", rc, sb_recv);
    }
    rant_node_close(P,1); rant_node_close(S,1);
    rant_allocator_reset(&ma);
}

/* (19a2) a large procedurally built enum, 1024 options with a u16 backing. Both nodes
   define the identical list, so the hashes match and no schema wire is sent. */
#define BE_N 1024
static int be_recv; static int64_t be_val; static char be_label[32];
static void be_on_message(const RantMsg *msg){
    RantString nm; size_t k;
    if (!msg->schema) return;
    be_recv++;
    be_val = rant_get_int(msg->data, msg->schema, "job");
    nm = rant_get_enum(msg->data, msg->schema, "job");
    k = nm.len < sizeof be_label - 1 ? nm.len : sizeof be_label - 1;
    if (nm.data) memcpy(be_label, nm.data, k);
    be_label[k] = '\0';
}
static RantSchema *be_build_schema(RantAllocator *a){
    static char text[64 + BE_N * 10];
    int i, at = sprintf(text, "Jobs { id: u32, job: enum<u16> {");
    for (i=0;i<BE_N;i++) at += sprintf(text + at, " job_%04d,", i);
    sprintf(text + at, " } }");
    return test_compile(a, text, NULL);
}
static void schema_bigenum_checks(void){
    RantAllocator pa = rant_allocator_heap(0);
    RantAllocator sa = rant_allocator_heap(0);
    RantAllocator ma = rant_allocator_heap(0);
    RantNodeOpts po, so; RantNode *P=NULL, *S=NULL; RantTopic *pc; RantTopicOpts co;
    RantAddr seed; RantSchema *es; int t, ji;

    es = be_build_schema(&ma);
    CHECK(es!=NULL, "bigenum: 1024-option schema compiles");
    if (!es){ rant_allocator_reset(&ma); return; }
    ji = rant_schema_field_index(es, "job");

    {   RantString nm; int64_t vv; RantSchema *back; RantBytes wire;
        CHECK(rant_schema_enum_count(es,(uint16_t)ji)==BE_N,
                 "bigenum: option count carries past 255 (%u)",
                 rant_schema_enum_count(es,(uint16_t)ji));
        CHECK(rant_schema_enum_variant(es,(uint16_t)ji,1000,&vv,&nm)
                 && vv==1000 && nm.len==7 && memcmp(nm.data,"Job1000",7)==0,
                 "bigenum: high-index variant reflects");
        CHECK(rant_enum_name_of(es,(uint16_t)ji,1023).len==7
                 && rant_enum_value_of(es,(uint16_t)ji,"job_0500",&vv) && vv==500,
                 "bigenum: name<->value at the tail");
        wire = rant_schema_wire(es);
        CHECK(wire.len > RANT_DGRAM_MAX,
                 "bigenum: schema wire exceeds one datagram (%u bytes)", (unsigned)wire.len);
        back = i_rant_schema_parse(wire.data, wire.len, rant_allocator_alloc, &ma);
        CHECK(back && rant_schema_hash(back)==rant_schema_hash(es)
                 && rant_schema_enum_count(back,(uint16_t)ji)==BE_N,
                 "bigenum: wire round-trips (parse preserves hash + count)");
        if (back) i_rant_schema_free(back, rant_allocator_alloc, &ma);
    }

    memset(&co,0,sizeof co); co.qos.keep_last=4;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=TEST_DOMAIN+10; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po;
    be_recv=0; be_val=-1; be_label[0]='\0';
    P = rant_node_open(&pa, "be-pub", NULL, NULL, &po);
    S = rant_node_open(&sa, "be-sub", be_on_message, NULL, &so);
    CHECK(P&&S, "bigenum: nodes open");
    if (!(P&&S)){ if(P)rant_node_close(P,0); if(S)rant_node_close(S,0);
                  i_rant_schema_free(es,rant_allocator_alloc,&ma); rant_allocator_reset(&ma); return; }
    pc = rant_node_create_topic(P, "be/jobs", RANT_PUB_ONLY, es, &co);
    rant_node_create_topic(S, "be/jobs", RANT_SUB_ONLY, es, &co);
    CHECK(pc!=NULL, "bigenum: topic created");

    for (t=0;t<800 && rant_topic_match_count(pc)==0;t++){ rant_node_poll(P,2); rant_node_poll(S,2); }
    CHECK(rant_topic_match_count(pc)>0, "bigenum: typed match reached the peer");

    {   uint8_t buf[16];
        rant_schema_message_default(es, buf, sizeof buf);
        rant_set_uint(buf, sizeof buf, es, "id", 42);
        CHECK(rant_set_enum(buf, sizeof buf, es, "job", "job_1000"),
                 "bigenum: set the value by its human name");
        rant_topic_send(pc, rant_bytes(buf, rant_schema_msg_len(es, buf, sizeof buf)), NULL);
        for (t=0;t<400 && be_recv==0;t++){ rant_node_poll(P,1); rant_node_poll(S,2); }
        CHECK(be_recv==1 && be_val==1000 && strcmp(be_label,"Job1000")==0,
                 "bigenum: value delivered + resolved to its name (val=%lld, label=%s)",
                 (long long)be_val, be_label);
    }
    rant_node_close(P,1); rant_node_close(S,1);
    i_rant_schema_free(es, rant_allocator_alloc, &ma);
    rant_allocator_reset(&ma);
}

/* (19a3) primitive rooted schemas: one bare type, anonymous, addressed by the empty path,
   the same wire in any language (spec/testing.md). Every root kind is covered. */
static const uint64_t SR_HASH_BOOL  = 0xee90234f61d2520bULL;   /* `bool`  canonical wire hash */
static const uint64_t SR_HASH_F32ARR = 0x314844e3386a1fc4ULL;  /* `f32[]` canonical wire hash */

/* one bare-type spelling: compiles, prints back to the identical text, recompiles to the
   same wire, and reflects as a single anonymous field with the expected layout */
static void sr_root_case(RantAllocator *ma, const char *text, uint8_t kind,
                         uint32_t size, uint32_t msg_min){
    RantSchema *s = test_compile(ma, text, NULL);
    RantSchema *back = NULL; RantSchemaFieldInfo fi; char buf[128];
    CHECK(s != NULL, "schema-root: '%s' compiles", text);
    if (!s) return;
    rant_schema_print(s, buf, sizeof buf);
    CHECK(strncmp(buf, text, strlen(text)) == 0 && strcmp(buf + strlen(text), "\n") == 0,
             "schema-root: '%s' prints back bare (got '%s')", text, buf);
    CHECK(rant_schema_print(s, NULL, 0) == (uint32_t)strlen(buf),
             "schema-root: '%s' print measures with (NULL,0)", text);
    back = test_compile(ma, buf, NULL);
    CHECK(back && rant_schema_hash(back) == rant_schema_hash(s),
             "schema-root: '%s' print recompiles to the same wire (hash)", text);
    CHECK(rant_schema_field_count(s) == 1 && rant_schema_field_at(s, 0, &fi)
             && fi.kind == kind && fi.name.len == 0 && fi.name.data != NULL
             && fi.depth == 0 && fi.offset == 0,
             "schema-root: '%s' is one anonymous field (kind=%u)", text, fi.kind);
    CHECK(rant_schema_name(s).len == 0 && rant_schema_field_index(s, "") == 0,
             "schema-root: '%s' root is unnamed, the empty path resolves it", text);
    CHECK(rant_schema_size(s) == size && rant_schema_msg_min(s) == msg_min,
             "schema-root: '%s' layout (size=%u min=%u)", text,
             rant_schema_size(s), rant_schema_msg_min(s));
    if (back) i_rant_schema_free(back, rant_allocator_alloc, ma);
    i_rant_schema_free(s, rant_allocator_alloc, ma);
}

static int sr_flag_recv, sr_flag_val, sr_note_recv, sr_samples_recv, sr_map_recv, sr_enum_recv;
static int sr_raw_recv, sr_raw_ok;
static unsigned long sr_mismatch_n;
static char sr_note[32], sr_mode[16];
static float sr_s0, sr_s2; static size_t sr_ns;
static uint64_t sr_battery;
/* one handler for every root kind: the delivered schema's single field says which */
static void sr_on_message(const RantMsg *msg){
    RantSchemaFieldInfo fi;
    if (!msg->schema || !rant_schema_field_at(msg->schema, 0, &fi)) return;
    switch (fi.kind){
        case RANT_BOOL:
            sr_flag_recv++; sr_flag_val = (int)rant_get_uint(msg->data, msg->schema, "");
            break;
        case RANT_VSTR: {
            RantString v = rant_get_string(msg->data, msg->schema, "");
            size_t k = v.len < sizeof sr_note - 1 ? v.len : sizeof sr_note - 1;
            if (v.data) memcpy(sr_note, v.data, k);
            sr_note[k] = '\0'; sr_note_recv++;
            break;
        }
        case RANT_VARR: {
            RantBytes a = rant_get_array(msg->data, msg->schema, "");
            sr_ns = a.len / sizeof(float);
            if (sr_ns >= 3){ memcpy(&sr_s0, a.data, 4); memcpy(&sr_s2, a.data + 8, 4); }
            sr_samples_recv++;
            break;
        }
        case RANT_MAP: {
            RantValue v;
            if (rant_map_get(rant_get_map(msg->data, msg->schema, ""), "battery", &v))
                sr_battery = v.v.u;
            sr_map_recv++;
            break;
        }
        case RANT_ENUM: {
            RantString nm = rant_get_enum(msg->data, msg->schema, "");
            size_t k = nm.len < sizeof sr_mode - 1 ? nm.len : sizeof sr_mode - 1;
            if (nm.data) memcpy(sr_mode, nm.data, k);
            sr_mode[k] = '\0'; sr_enum_recv++;
            break;
        }
        default: break;
    }
}
/* the schema-less reader: everything it knows comes from the sender's schema */
static void sr_on_raw(const RantMsg *msg){
    RantSchemaFieldInfo fi;
    sr_raw_recv++;
    sr_raw_ok = msg->schema && rant_schema_field_count(msg->schema) == 1
             && rant_schema_name(msg->schema).len == 0
             && rant_schema_field_at(msg->schema, 0, &fi) && fi.kind == RANT_BOOL
             && fi.name.len == 0
             && rant_get_uint(msg->data, msg->schema, "") == 1;
}
static void sr_on_event(const RantEvent *ev){
    if (ev->kind == RANT_ERROR && ev->error == RANT_E_SCHEMA_MISMATCH) sr_mismatch_n++;
}
static void schema_root_checks(void){
    RantAllocator pa = rant_allocator_heap(0);
    RantAllocator sa = rant_allocator_heap(0);
    RantAllocator qa = rant_allocator_heap(0);
    RantAllocator ma = rant_allocator_heap(0);
    RantNodeOpts po, so, qo; RantNode *P=NULL, *S=NULL, *Q=NULL;
    RantTopicOpts co; RantAddr seed;
    RantSchema *sb, *su8, *sstr, *sarr, *smap, *senum, *swrap;
    RantTopic *pflag, *pnote, *psamples, *pextras, *pmode, *pbad, *pwrap;
    int t;

    sr_root_case(&ma, "bool",   RANT_BOOL, 1, 1);
    sr_root_case(&ma, "u8",     RANT_U8,     1, 1);
    sr_root_case(&ma, "i16",    RANT_I16,    2, 2);
    sr_root_case(&ma, "u32",    RANT_U32,    4, 4);
    sr_root_case(&ma, "i64",    RANT_I64,    8, 8);
    sr_root_case(&ma, "f32",    RANT_F32,    4, 4);
    sr_root_case(&ma, "f64",    RANT_F64,    8, 8);
    sr_root_case(&ma, "u8[16]", RANT_ARR, 16, 16);
    sr_root_case(&ma, "string<64>",   RANT_STR, 66, 66);
    sr_root_case(&ma, "string<8>[4]", RANT_ARR, 40, 40);
    sr_root_case(&ma, "string", RANT_VSTR, 0, 4);
    sr_root_case(&ma, "f32[]",  RANT_VARR, 0, 4);
    sr_root_case(&ma, "string<8>[]", RANT_VARR, 0, 4);
    sr_root_case(&ma, "map",    RANT_MAP,    0, 4);
    sr_root_case(&ma, "enum<u8> { Idle = 0, Run = 1, Fault = 2 }", RANT_ENUM, 1, 1);

    sb    = test_compile(&ma, "bool", NULL);
    su8   = test_compile(&ma, "u8", NULL);
    sstr  = test_compile(&ma, "string", NULL);
    sarr  = test_compile(&ma, "f32[]", NULL);
    smap  = test_compile(&ma, "map", NULL);
    senum = test_compile(&ma, "enum<u8> { Idle, Run, Fault }", NULL);
    swrap = test_compile(&ma, "Wrap { v: bool }", NULL);
    CHECK(sb && su8 && sstr && sarr && smap && senum && swrap,
             "schema-root: the e2e schemas compile");
    if (!(sb && su8 && sstr && sarr && smap && senum && swrap)){ rant_allocator_reset(&ma); return; }

    /* THE canonical-hash pin: a bare type's wire is its kind alone, so every language's
       `bool` topic hashes to this and cross-language matching costs zero detail bytes. */
    CHECK(rant_schema_hash(sb) == SR_HASH_BOOL,
             "schema-root: `bool` canonical hash %016llx", (unsigned long long)rant_schema_hash(sb));
    CHECK(rant_schema_hash(sarr) == SR_HASH_F32ARR,
             "schema-root: `f32[]` canonical hash %016llx", (unsigned long long)rant_schema_hash(sarr));
    CHECK(rant_schema_wire(sb).len == 3, "schema-root: `bool` wire is 3 bytes (%u)",
             (unsigned)rant_schema_wire(sb).len);

    CHECK(test_compile(&ma, "Temperature: f32", NULL) == NULL
          && test_compile(&ma, "bool bool", NULL) == NULL,
             "schema-root: a named bare root and trailing garbage are compile errors");
    {   /* the wire: a bare root round trips, a named one is an alias, but the name may only
           ride the header, so a named type in root position stays refused */
        RantBytes w = rant_schema_wire(sarr);
        RantSchema *rt = i_rant_schema_parse(w.data, w.len, rant_allocator_alloc, &ma);
        RantSchema *alias; uint8_t named[6];
        CHECK(rt && rant_schema_hash(rt) == rant_schema_hash(sarr)
                 && rant_schema_field_count(rt) == 1,
                 "schema-root: a bare root parses back from its wire");
        if (rt) i_rant_schema_free(rt, rant_allocator_alloc, &ma);
        named[0] = (uint8_t)RANT_SCHEMA_WIRE_VERSION;     /* [ver][namelen 1]['x'][BOOL] */
        named[1] = 1; named[2] = 'x'; named[3] = (uint8_t)RANT_BOOL;
        alias = i_rant_schema_parse(named, 4, rant_allocator_alloc, &ma);
        CHECK(alias && rant_schema_name(alias).len == 1
                 && rant_schema_hash(alias) != rant_schema_hash(sb),
                 "schema-root: a named bare root on the wire is an alias, distinct from `bool`");
        if (alias) i_rant_schema_free(alias, rant_allocator_alloc, &ma);
        named[1] = 0;                                   /* [ver][0][NAMED][1]['x'][BOOL] */
        named[2] = (uint8_t)RANT_NAMED; named[3] = 1; named[4] = 'x'; named[5] = (uint8_t)RANT_BOOL;
        CHECK(i_rant_schema_parse(named, sizeof named, rant_allocator_alloc, &ma) == NULL,
                 "schema-root: a NAMED type in root position is refused (the header names it)");
    }

    {   /* empty-path and flat-index-0 access, every kind */
        uint8_t m[128]; RantValue v; RantBytes got; RantString sv;
        float xs[3]; uint8_t body[32]; RantMapWriter mw; uint32_t bl;
        xs[0]=1.5f; xs[1]=2.5f; xs[2]=-3.0f;
        CHECK(rant_schema_message_default(sb, m, sizeof m) && m[0] == 0,
                 "schema-root: bare default is the zero value");
        CHECK(rant_set_uint(m, sizeof m, sb, "", 1)
                 && rant_get_uint(rant_bytes(m, 1), sb, "") == 1
                 && rant_schema_msg_len(sb, m, sizeof m) == 1
                 && rant_schema_validate(sb, rant_bytes(m, 1)),
                 "schema-root: bool set/get through the empty path");
        memset(&v, 0, sizeof v);
        CHECK(rant_get_value(rant_bytes(m, 1), sb, 0, &v) && v.kind == RANT_BOOL && v.v.u == 1,
                 "schema-root: rant_get_value at flat index 0");
        v.v.u = 0;
        CHECK(rant_set_value(m, sizeof m, sb, 0, &v)
                 && rant_get_uint(rant_bytes(m, 1), sb, "") == 0,
                 "schema-root: rant_set_value at flat index 0");
        CHECK(rant_schema_message_default(sarr, m, sizeof m)
                 && rant_set_array(m, sizeof m, sarr, "", rant_bytes(xs, sizeof xs))
                 && rant_schema_msg_len(sarr, m, sizeof m) == 4 + 12,
                 "schema-root: f32[] frame set through the empty path");
        got = rant_get_array(rant_bytes(m, 16), sarr, "");
        CHECK(got.len == 12 && memcmp(got.data, xs, 12) == 0, "schema-root: f32[] reads back");
        CHECK(rant_schema_message_default(sstr, m, sizeof m)
                 && rant_set_string(m, sizeof m, sstr, "", rant_cstr("bare")),
                 "schema-root: string frame set through the empty path");
        sv = rant_get_string(rant_bytes(m, rant_schema_msg_len(sstr, m, sizeof m)), sstr, "");
        CHECK(sv.len == 4 && memcmp(sv.data, "bare", 4) == 0, "schema-root: string reads back");
        mw = rant_map_begin(body, sizeof body);
        rant_map_put_uint(&mw, "battery", 87);
        bl = rant_map_finish(&mw);
        CHECK(rant_schema_message_default(smap, m, sizeof m)
                 && rant_set_map(m, sizeof m, smap, "", rant_bytes(body, bl))
                 && rant_map_count(rant_get_map(rant_bytes(m, rant_schema_msg_len(smap, m, sizeof m)),
                                                smap, "")) == 1,
                 "schema-root: map body set/get through the empty path");
        CHECK(rant_schema_message_default(senum, m, sizeof m)
                 && rant_set_enum(m, sizeof m, senum, "", "Fault")
                 && rant_get_uint(rant_bytes(m, 1), senum, "") == 2
                 && rant_get_enum(rant_bytes(m, 1), senum, "").len == 5,
                 "schema-root: enum set by name, read back as value + name");
    }
    {   /* matching: the roots' types compare like fields, struct vs bare never matches */
        char why[128]; RantSchema *rb;
        CHECK(rant_schema_subset(sb, sb) == 1 && rant_schema_subset(sb, su8) == 0
              && rant_schema_subset(sb, swrap) == 0 && rant_schema_subset(swrap, sb) == 0,
                 "schema-root: bare roots match only their own type");
        rant_schema_subset_why(sb, su8, why, sizeof why);
        CHECK(strcmp(why, "root: reader bool, writer u8") == 0,
                 "schema-root: subset_why names both roots ('%s')", why);
        rant_schema_subset_why(swrap, sb, why, sizeof why);
        CHECK(strcmp(why, "root: reader struct 'Wrap', writer bool") == 0,
                 "schema-root: subset_why explains struct vs bare ('%s')", why);
        rb = i_rant_schema_rebase(sarr, sarr, rant_allocator_alloc, &ma);
        CHECK(rb && rant_schema_field_count(rb) == 1 && rant_schema_msg_min(rb) == 4,
                 "schema-root: rebase of a bare root stays valid");
        if (rb) i_rant_schema_free(rb, rant_allocator_alloc, &ma);
    }

    /* ---- end to end: one topic per root kind, plus the two refusals ---- */
    memset(&co,0,sizeof co); co.qos.keep_last=4; co.qos.reliability=RANT_RELIABLE;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=TEST_DOMAIN+11; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po; qo=po;
    sr_flag_recv=sr_flag_val=sr_note_recv=sr_samples_recv=sr_map_recv=sr_enum_recv=0;
    sr_raw_recv=sr_raw_ok=0; sr_mismatch_n=0; sr_ns=0; sr_battery=0;
    sr_note[0]='\0'; sr_mode[0]='\0';
    P = rant_node_open(&pa, "sr-pub", NULL,          sr_on_event, &po);
    S = rant_node_open(&sa, "sr-sub", sr_on_message, sr_on_event, &so);
    Q = rant_node_open(&qa, "sr-raw", sr_on_raw,     NULL,        &qo);
    CHECK(P&&S&&Q, "schema-root: nodes open");
    if (!(P&&S&&Q)){ if(P)rant_node_close(P,0); if(S)rant_node_close(S,0); if(Q)rant_node_close(Q,0);
                     rant_allocator_reset(&ma); return; }
    pflag    = rant_node_create_topic(P, "sr/flag",    RANT_PUB_ONLY, sb,      &co);
    pnote    = rant_node_create_topic(P, "sr/note",    RANT_PUB_ONLY, sstr,    &co);
    psamples = rant_node_create_topic(P, "sr/samples", RANT_PUB_ONLY, sarr,    &co);
    pextras  = rant_node_create_topic(P, "sr/extras",  RANT_PUB_ONLY, smap,    &co);
    pmode    = rant_node_create_topic(P, "sr/mode",    RANT_PUB_ONLY, senum, &co);
    pbad     = rant_node_create_topic(P, "sr/bad",     RANT_PUB_ONLY, sb,      &co);
    pwrap    = rant_node_create_topic(P, "sr/wrap",    RANT_PUB_ONLY, swrap, &co);
    rant_node_create_topic(S, "sr/flag",    RANT_SUB_ONLY, sb,      &co);
    rant_node_create_topic(S, "sr/note",    RANT_SUB_ONLY, sstr,    &co);
    rant_node_create_topic(S, "sr/samples", RANT_SUB_ONLY, sarr,    &co);
    rant_node_create_topic(S, "sr/extras",  RANT_SUB_ONLY, smap,    &co);
    rant_node_create_topic(S, "sr/mode",    RANT_SUB_ONLY, senum, &co);
    rant_node_create_topic(S, "sr/bad",     RANT_SUB_ONLY, su8,     &co);   /* bool writer: refused */
    rant_node_create_topic(S, "sr/wrap",    RANT_SUB_ONLY, sb,      &co);   /* refused: a struct */
    rant_node_create_topic(Q, "sr/flag",    RANT_SUB_ONLY, NULL,    &co);   /* raw reader */
    CHECK(pflag && pnote && psamples && pextras && pmode && pbad && pwrap,
             "schema-root: topics created");

    for (t=0;t<1200 && (rant_topic_match_count(pflag)==0 || rant_topic_match_count(pmode)==0
                        || rant_topic_match_count(pextras)==0);t++){
        rant_node_poll(P,2); rant_node_poll(S,2); rant_node_poll(Q,2);
    }
    CHECK(rant_topic_match_count(pflag)==2, "schema-root: bool topic matched typed + raw readers (%d)",
             rant_topic_match_count(pflag));
    CHECK(rant_topic_match_count(pnote)==1 && rant_topic_match_count(psamples)==1
             && rant_topic_match_count(pextras)==1 && rant_topic_match_count(pmode)==1,
             "schema-root: string/f32[]/map/enum roots matched");
    CHECK(rant_topic_match_count(pbad)==0 && rant_topic_match_count(pwrap)==0,
             "schema-root: bool-vs-u8 and struct-vs-bare readers refused");
    CHECK(sr_mismatch_n>=2, "schema-root: both refusals surfaced (%lu RANT_E_SCHEMA_MISMATCH)",
             sr_mismatch_n);

    {   uint8_t m[128]; float xs[3]; uint8_t body[32]; RantMapWriter mw; uint32_t bl;
        xs[0]=1.5f; xs[1]=2.5f; xs[2]=-3.0f;
        rant_schema_message_default(sb, m, sizeof m);
        rant_set_uint(m, sizeof m, sb, "", 1);
        rant_topic_send(pflag, rant_bytes(m, 1), NULL);
        rant_schema_message_default(sstr, m, sizeof m);
        rant_set_string(m, sizeof m, sstr, "", rant_cstr("bare-string"));
        rant_topic_send(pnote, rant_bytes(m, rant_schema_msg_len(sstr, m, sizeof m)), NULL);
        rant_schema_message_default(sarr, m, sizeof m);
        rant_set_array(m, sizeof m, sarr, "", rant_bytes(xs, sizeof xs));
        rant_topic_send(psamples, rant_bytes(m, rant_schema_msg_len(sarr, m, sizeof m)), NULL);
        mw = rant_map_begin(body, sizeof body);
        rant_map_put_uint(&mw, "battery", 87);
        bl = rant_map_finish(&mw);
        rant_schema_message_default(smap, m, sizeof m);
        rant_set_map(m, sizeof m, smap, "", rant_bytes(body, bl));
        rant_topic_send(pextras, rant_bytes(m, rant_schema_msg_len(smap, m, sizeof m)), NULL);
        rant_schema_message_default(senum, m, sizeof m);
        rant_set_enum(m, sizeof m, senum, "", "Fault");
        rant_topic_send(pmode, rant_bytes(m, 1), NULL);
        for (t=0;t<800 && (sr_flag_recv==0 || sr_note_recv==0 || sr_samples_recv==0
                           || sr_map_recv==0 || sr_enum_recv==0 || sr_raw_recv==0);t++){
            rant_node_poll(P,1); rant_node_poll(S,2); rant_node_poll(Q,2);
        }
    }
    CHECK(sr_flag_recv==1 && sr_flag_val==1, "schema-root: bool delivered (recv=%d val=%d)",
             sr_flag_recv, sr_flag_val);
    CHECK(sr_note_recv==1 && strcmp(sr_note,"bare-string")==0,
             "schema-root: string root delivered ('%s')", sr_note);
    CHECK(sr_samples_recv==1 && sr_ns==3 && sr_s0==1.5f && sr_s2==-3.0f,
             "schema-root: f32[] root delivered (%u elems)", (unsigned)sr_ns);
    CHECK(sr_map_recv==1 && sr_battery==87, "schema-root: map root delivered (battery=%llu)",
             (unsigned long long)sr_battery);
    CHECK(sr_enum_recv==1 && strcmp(sr_mode,"Fault")==0,
             "schema-root: enum root delivered + resolved to its name ('%s')", sr_mode);
    CHECK(sr_raw_recv==1 && sr_raw_ok,
             "schema-root: schema-less reader decodes through the sender's bare schema");

    rant_node_close(P,1); rant_node_close(S,1); rant_node_close(Q,1);
    rant_allocator_reset(&ma);
}

/* (19a5) the standard type library: names always in scope, golden bytes and hash, shape
   verifying recognition, and an end to end pair publishing Transform and Image. */
static volatile long sd_pose_recv = 0, sd_img_recv = 0, sd_bad_recv = 0, sd_mismatch = 0;
static volatile long sd_empty_recv = 0; static size_t sd_empty_len = 1;
static double sd_px = 0.0, sd_qw = 0.0;
static unsigned sd_iw = 0, sd_ih = 0, sd_ifmt = 0; static size_t sd_ilen = 0;
static void sd_on_message(const RantMsg *m){
    if (!m->schema) return;
    if (rant_schema_field_count(m->schema) == 0){
        sd_empty_len = m->data.len;
        sd_empty_recv++;
        return;
    }
    if (rant_get_f64(m->data, m->schema, "pose.position.x") != 0.0 ||
        rant_schema_field_index(m->schema, "pose.orientation.w") >= 0){
        if (rant_schema_field_index(m->schema, "pose.position.x") >= 0){
            sd_px = rant_get_f64(m->data, m->schema, "pose.position.x");
            sd_qw = rant_get_f64(m->data, m->schema, "pose.orientation.w");
            sd_pose_recv++;
            return;
        }
    }
    if (rant_schema_field_index(m->schema, "width") >= 0){
        sd_iw = (unsigned)rant_get_uint(m->data, m->schema, "width");
        sd_ih = (unsigned)rant_get_uint(m->data, m->schema, "height");
        sd_ifmt = (unsigned)rant_get_uint(m->data, m->schema, "format");
        sd_ilen = rant_get_array(m->data, m->schema, "data").len;
        sd_img_recv++;
    }
}
static void sd_on_bad(const RantMsg *m){ (void)m; sd_bad_recv++; }
static void sd_on_event(const RantEvent *ev){
    if (ev->kind == RANT_ERROR && ev->error == RANT_E_SCHEMA_MISMATCH) sd_mismatch++;
}
static void stdtypes_checks(void){
    RantAllocator ma = rant_allocator_heap(0);
    i_RantRegistry reg;
    RantAllocator pa = rant_allocator_heap(0);
    RantAllocator sa = rant_allocator_heap(0);
    RantNodeOpts po, so; RantNode *P = NULL, *S = NULL;
    RantTopicOpts co; RantAddr seed;
    RantTopic *ppose, *pimg, *pbad, *pempty; RantSchema *SPose, *SImg, *SBad, *SBadSub, *SEmpty;
    int t;

    i_rant_registry_init(&reg, rant_allocator_alloc, &ma);
    /* the whole roster compiles by name alone, and each recognizes itself */
    {   int i, ok = 1, rec = 1;
        for (i = 1; i < (int)RANT_STD_COUNT; i++){
            RantSchema *s = test_compile(&ma, rant_std_name((RantStdType)i), NULL);
            RantString n;
            if (!s){ ok = 0; continue; }
            n = rant_schema_name(s);
            if (n.len != strlen(rant_std_name((RantStdType)i))) ok = 0;
            if (i_rant_std_recognize(&reg, s) != (RantStdType)i) rec = 0;
            i_rant_schema_free(s, rant_allocator_alloc, &ma);
        }
        CHECK(ok, "stdtypes: every standard type compiles from its name alone");
        CHECK(rec, "stdtypes: every standard type recognizes itself");
    }
    /* GOLDEN BYTES: the canonical wire and hash every wrapper mirrors. Change these only
       with a deliberate wire bump. */
    {   RantSchema *s = test_compile(&ma, "Float3", NULL);
        static const uint8_t want[] = {
            8, 6, 'F','l','o','a','t','3', 12, 3,
            1, 'x', 8,  1, 'y', 8,  1, 'z', 8
        };
        RantBytes w = rant_schema_wire(s);
        CHECK(s && w.len == sizeof want && memcmp(w.data, want, sizeof want) == 0,
                 "stdtypes: Float3 golden wire (%u bytes)", (unsigned)w.len);
        CHECK(s && rant_schema_hash(s) == 0x04aa9469cd08b1ddULL,
                 "stdtypes: Float3 golden hash %016llx",
                 (unsigned long long)rant_schema_hash(s));
        CHECK(s && rant_schema_size(s) == 12, "stdtypes: Float3 is 12 message bytes");
        if (s) i_rant_schema_free(s, rant_allocator_alloc, &ma);
    }
    /* recognition verifies the SHAPE, so a peer's same-named impostor never converts */
    {   RantSchema *good = test_compile(&ma,
            "A { at: Transform, id: Uuid, pts: Float3[], plain: f32 }", NULL);
        RantSchema *bad = test_compile(&ma,
            "Uuid = u8[16]\nB { id: Uuid }", NULL);
        RantSchema *lie = i_rant_schema_parse(bad ? rant_schema_wire(bad).data : NULL,
                                            bad ? rant_schema_wire(bad).len : 0,
                                            rant_allocator_alloc, &ma);
        CHECK(good && bad && lie, "stdtypes: recognition fixtures compile");
        if (good){
            CHECK(i_rant_std_recognize_field(&reg, good, (uint16_t)rant_schema_field_index(good, "at"))
                        == RANT_STD_TRANSFORM
                     && i_rant_std_recognize_field(&reg, good, (uint16_t)rant_schema_field_index(good, "id"))
                        == RANT_STD_UUID,
                     "stdtypes: a named field recognizes by name AND shape");
            CHECK(i_rant_std_recognize_elem(&reg, good, (uint16_t)rant_schema_field_index(good, "pts"))
                        == RANT_STD_FLOAT3,
                     "stdtypes: an array's ELEMENT recognizes");
            CHECK(i_rant_std_recognize_field(&reg, good, (uint16_t)rant_schema_field_index(good, "plain"))
                        == RANT_STD_NONE
                     && i_rant_std_recognize(&reg, good) == RANT_STD_NONE,
                     "stdtypes: an anonymous field and a user root are not standard types");
        }
        if (good) i_rant_schema_free(good, rant_allocator_alloc, &ma);
        if (bad)  i_rant_schema_free(bad,    rant_allocator_alloc, &ma);
        if (lie)  i_rant_schema_free(lie,    rant_allocator_alloc, &ma);
    }
    {   /* a hand-built wire that calls itself Uuid but is 15 bytes: refused by shape */
        uint8_t w[10]; RantSchema *s;
        w[0] = (uint8_t)RANT_SCHEMA_WIRE_VERSION; w[1] = 4;
        w[2] = 'U'; w[3] = 'u'; w[4] = 'i'; w[5] = 'd';
        w[6] = (uint8_t)RANT_ARR; w[7] = 15; w[8] = 0; w[9] = (uint8_t)RANT_U8;
        s = i_rant_schema_parse(w, sizeof w, rant_allocator_alloc, &ma);
        CHECK(s && i_rant_std_recognize(&reg, s) == RANT_STD_NONE,
                 "stdtypes: a same-named wrong-shaped peer type is NOT recognized");
        if (s) i_rant_schema_free(s, rant_allocator_alloc, &ma);
    }
    /* the C mirrors line up with the wire, byte for byte */
    {   RantSchema *sp = test_compile(&ma, "Transform", NULL);
        RantTransform p = rant_transform_identity(); uint8_t m[sizeof(RantTransform)];
        p.pose.position = rant_double3(1.0, 2.0, 3.0);
        memcpy(m, &p, sizeof p);
        CHECK(sp && sizeof(RantTransform) == rant_schema_size(sp),
                 "stdtypes: sizeof(RantTransform) == the wire size (%u)",
                 sp ? rant_schema_size(sp) : 0);
        if (sp){
            RantBytes b = rant_bytes(m, sizeof m);
            CHECK(rant_get_f64(b, sp, "pose.position.y") == 2.0
                     && rant_get_f64(b, sp, "pose.orientation.w") == 1.0,
                     "stdtypes: a memcpy'd RantTransform reads back through the schema");
        }
        if (sp) i_rant_schema_free(sp, rant_allocator_alloc, &ma);
    }
    {   static const struct { const char *name; size_t size; } mirrors[] = {
            { "Pose", sizeof(RantPose) }, { "Pose2D", sizeof(RantPose2D) },
            { "Wrench", sizeof(RantWrench) },
            { "AlignedBox", sizeof(RantAlignedBox) }, { "AlignedBox2D", sizeof(RantAlignedBox2D) },
            { "OrientedBox", sizeof(RantOrientedBox) }, { "OrientedBox2D", sizeof(RantOrientedBox2D) },
            { "Plane", sizeof(RantPlane) }, { "Segment", sizeof(RantSegment) },
            { "Sphere", sizeof(RantSphere) }, { "Capsule", sizeof(RantCapsule) },
            { "Cylinder", sizeof(RantCylinder) }, { "Cone", sizeof(RantCone) },
            { "Circle", sizeof(RantCircle) },
            { "Transform2D", sizeof(RantTransform2D) }, { "Twist2D", sizeof(RantTwist2D) },
            { "Wrench2D", sizeof(RantWrench2D) }, { "Plane2D", sizeof(RantPlane2D) },
            { "Segment2D", sizeof(RantSegment2D) }, { "Capsule2D", sizeof(RantCapsule2D) }
        };
        int i, ok = 1;
        for (i = 0; i < (int)(sizeof mirrors / sizeof mirrors[0]); i++){
            RantSchema *s = test_compile(&ma, mirrors[i].name, NULL);
            if (!s || rant_schema_size(s) != mirrors[i].size){
                ok = 0;
                CHECK(0, "stdtypes: %s mirror is %u bytes, the wire %u", mirrors[i].name,
                         (unsigned)mirrors[i].size, s ? rant_schema_size(s) : 0);
            }
            if (s) i_rant_schema_free(s, rant_allocator_alloc, &ma);
        }
        CHECK(ok, "stdtypes: every shape mirror is its wire size");
    }
    {   RantSchema *cap = test_compile(&ma, "Capsule", NULL), *cyl = test_compile(&ma, "Cylinder", NULL);
        RantSchema *ob = test_compile(&ma, "OrientedBox", NULL), *e = test_compile(&ma, "Empty", NULL);
        RantOrientedBox box = {{{0}}}; uint8_t m[sizeof(RantOrientedBox)];
        CHECK(cap && cyl && !rant_schema_subset(cap, cyl) && !rant_schema_subset(cyl, cap),
                 "stdtypes: Capsule and Cylinder share bytes but never cross wire");
        CHECK(e && rant_schema_size(e) == 0 && rant_schema_field_count(e) == 0,
                 "stdtypes: Empty is zero bytes and no fields");
        box.pose.orientation = rant_quaternion_identity();
        box.size = rant_double3(1.0, 2.0, 3.0);
        memcpy(m, &box, sizeof box);
        CHECK(ob && rant_get_f64(rant_bytes(m, sizeof m), ob, "pose.orientation.w") == 1.0
                 && rant_get_f64(rant_bytes(m, sizeof m), ob, "size.z") == 3.0,
                 "stdtypes: a memcpy'd RantOrientedBox reads back through the schema");
        if (cap) i_rant_schema_free(cap, rant_allocator_alloc, &ma);
        if (cyl) i_rant_schema_free(cyl, rant_allocator_alloc, &ma);
        if (ob)  i_rant_schema_free(ob,  rant_allocator_alloc, &ma);
        if (e)   i_rant_schema_free(e,   rant_allocator_alloc, &ma);
    }
    {   RantColor c = rant_color_from_hex(0x11223344u);
        RantDouble3 v = rant_quaternion_rotate(rant_quaternion(0.0, 0.0, 1.0, 0.0),
                                               rant_double3(1.0, 0.0, 0.0));
        double len = rant_double3_length(rant_double3(3.0, 4.0, 0.0));
        CHECK(c.r == 0x11 && c.g == 0x22 && c.b == 0x33 && c.a == 0x44
                 && rant_color_to_hex(c) == 0x11223344u,
                 "stdtypes: Color hex round-trips as 0xRRGGBBAA");
        CHECK(len == 5.0, "stdtypes: vector length without libm (%g)", len);
        CHECK(v.x < -0.999 && v.x > -1.001 && v.y < 1e-9 && v.y > -1e-9,
                 "stdtypes: a 180 degree rotation about z flips x (%g, %g)", v.x, v.y);
    }
    {   /* the two values that need a platform, hence the node runtime */
        RantTimestamp t0 = rant_timestamp_now();
        RantUuid u1, u2;
        rant_uuid_new(&u1); rant_uuid_new(&u2);
        CHECK(t0 > 1600000000000000LL,       /* past 2020 in microseconds */
                 "stdtypes: rant_timestamp_now is Unix-epoch microseconds (%lld)",
                 (long long)t0);
        CHECK(!rant_uuid_is_nil(u1) && memcmp(u1.bytes, u2.bytes, 16) != 0
                 && (u1.bytes[6] & 0xF0u) == 0x40u && (u1.bytes[8] & 0xC0u) == 0x80u,
                 "stdtypes: rant_uuid_new makes distinct RFC 4122 version-4 ids");
    }

    /* ---- end to end: Transform and Image over two nodes, plus a shape impostor refused ---- */
    SPose = test_compile(&ma, "Transform", NULL);
    SImg  = test_compile(&ma, "Image", NULL);
    SBad  = test_compile(&ma, "Twist", NULL);
    SBadSub = test_compile(&ma, "Transform", NULL);
    SEmpty = test_compile(&ma, "Empty", NULL);
    CHECK(SPose && SImg && SBad && SBadSub && SEmpty, "stdtypes: e2e schemas compile");
    if (!(SPose && SImg && SBad && SBadSub && SEmpty)){ rant_allocator_reset(&ma); return; }

    memset(&co,0,sizeof co); co.qos.keep_last=4; co.qos.reliability=RANT_RELIABLE;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=TEST_DOMAIN+23; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po;
    sd_pose_recv=sd_img_recv=sd_bad_recv=sd_mismatch=sd_empty_recv=0; sd_empty_len=1;
    P = rant_node_open(&pa, "sd-pub", NULL,          sd_on_event, &po);
    S = rant_node_open(&sa, "sd-sub", sd_on_message, sd_on_event, &so);
    CHECK(P&&S, "stdtypes: nodes open");
    if (!(P&&S)){ if(P)rant_node_close(P,0); if(S)rant_node_close(S,0); rant_allocator_reset(&ma); return; }
    ppose = rant_node_create_topic(P, "sd/pose", RANT_PUB_ONLY, SPose, &co);
    pimg  = rant_node_create_topic(P, "sd/img",  RANT_PUB_ONLY, SImg,    &co);
    pbad  = rant_node_create_topic(P, "sd/bad",  RANT_PUB_ONLY, SBad,    &co);
    pempty = rant_node_create_topic(P, "sd/empty", RANT_PUB_ONLY, SEmpty, &co);
    rant_node_create_topic(S, "sd/pose", RANT_SUB_ONLY, SPose, &co);
    rant_node_create_topic(S, "sd/img",  RANT_SUB_ONLY, SImg,    &co);
    rant_node_create_topic(S, "sd/bad",  RANT_SUB_ONLY, SBadSub, &co);     /* Twist writer: refused */
    rant_node_create_topic(S, "sd/empty", RANT_SUB_ONLY, SEmpty, &co);
    CHECK(ppose && pimg && pbad && pempty, "stdtypes: topics created");

    for (t=0;t<1200 && (rant_topic_match_count(ppose)==0 || rant_topic_match_count(pimg)==0
                        || rant_topic_match_count(pempty)==0);t++){
        rant_node_poll(P,2); rant_node_poll(S,2);
    }
    CHECK(rant_topic_match_count(ppose)==1 && rant_topic_match_count(pimg)==1,
             "stdtypes: Transform and Image topics matched");
    CHECK(rant_topic_match_count(pbad)==0,
             "stdtypes: a Transform reader refuses a Twist writer");
    CHECK(sd_mismatch >= 1, "stdtypes: the refusal surfaced (%ld)", sd_mismatch);

    {   uint8_t m[sizeof(RantTransform)]; RantTransform p = rant_transform_identity();
        uint8_t *img; uint32_t need; size_t px = 64u*48u*3u;
        p.pose.position = rant_double3(4.5, -1.25, 9.0);
        memcpy(m, &p, sizeof p);
        rant_topic_send(ppose, rant_bytes(m, sizeof p), NULL);
        CHECK(rant_topic_send(pempty, rant_bytes(NULL, 0), NULL) >= 0,
                 "stdtypes: an Empty message sends");
        need = rant_schema_msg_min(SImg) + (uint32_t)px;
        img = (uint8_t *)i_rant_plat_realloc(NULL, need);
        if (img){
            size_t i;
            uint8_t *pix = (uint8_t *)i_rant_plat_realloc(NULL, px);
            rant_schema_message_default(SImg, img, need);
            rant_set_uint(img, need, SImg, "width", 64);
            rant_set_uint(img, need, SImg, "height", 48);
            rant_set_uint(img, need, SImg, "stride", 0);
            rant_set_enum(img, need, SImg, "format", "Rgb8");
            if (pix){
                for (i = 0; i < px; i++) pix[i] = (uint8_t)i;
                rant_set_array(img, need, SImg, "data", rant_bytes(pix, px));
                i_rant_plat_realloc(pix, 0);
            }
            rant_topic_send(pimg, rant_bytes(img, rant_schema_msg_len(SImg, img, need)), NULL);
            i_rant_plat_realloc(img, 0);
        }
        for (t=0;t<1200 && (sd_pose_recv==0 || sd_img_recv==0 || sd_empty_recv==0);t++){
            rant_node_poll(P,1); rant_node_poll(S,2);
        }
    }
    CHECK(sd_pose_recv==1 && sd_px==4.5 && sd_qw==1.0,
             "stdtypes: Transform delivered (recv=%ld x=%g w=%g)", sd_pose_recv, sd_px, sd_qw);
    CHECK(sd_img_recv==1 && sd_iw==64 && sd_ih==48 && sd_ifmt==(unsigned)RANT_IMAGE_RGB8
             && sd_ilen==64u*48u*3u,
             "stdtypes: Image delivered whole (%ux%u fmt=%u %u bytes)",
             sd_iw, sd_ih, sd_ifmt, (unsigned)sd_ilen);
    CHECK(sd_empty_recv==1 && sd_empty_len==0,
             "stdtypes: Empty delivered as zero bytes (recv=%ld len=%u)", sd_empty_recv, (unsigned)sd_empty_len);
    CHECK(sd_bad_recv==0, "stdtypes: the refused topic delivered nothing");
    rant_node_close(S,1); rant_node_close(P,1);
    rant_allocator_reset(&ma); rant_allocator_reset(&pa); rant_allocator_reset(&sa);
}

/* (19c) live uDTL routing: a DETAIL_REQ at the data socket is answered to its source even
   from a bare socket. Duplicates are idempotent, wrong domain and garbage are ignored. */
static void detail_live_checks(void){
    RantAllocator pa = rant_allocator_heap(0);
    RantAllocator sa = rant_allocator_heap(0);
    RantAllocator ma = rant_allocator_heap(0);
    RantNodeOpts po, so; RantNode *P=NULL, *S=NULL; RantTopicOpts co; RantAddr seed;
    RantSchema *W; RantTopic *pc;
    i_RantSock q = RANT_SOCK_BAD;
    uint8_t pip[4]={0,0,0,0}; uint16_t pport=0; uint32_t pversion=0;
    /* the oracle, copied out of the peer view BEFORE any further poll invalidates it */
    uint16_t oalias[4]; char oname[4][64]; uint8_t onlen[4]; uint64_t ohash[4]; uint16_t nw=0;
    uint16_t dom = TEST_DOMAIN+10;
    int t;

    W = test_compile(&ma, "Reading { stamp: u64, x: f64 }", NULL);
    memset(&co,0,sizeof co); co.qos.keep_last=2;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=dom; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po;
    P = rant_node_open(&pa, "dt-pub", NULL, NULL, &po);
    S = rant_node_open(&sa, "dt-sub", test_on_message, NULL, &so);
    CHECK(P && S && W, "detail-live: nodes open");
    if (!(P && S && W)){
        if (P) rant_node_close(P,0); if (S) rant_node_close(S,0);
        rant_allocator_reset(&ma); return;
    }
    pc = rant_node_create_topic(P, "dt/pose",  RANT_PUB_ONLY, W,      &co);
    rant_node_create_topic(P, "dt/plain", RANT_PUB_ONLY, NULL, &co);
    rant_node_create_topic(S, "dt/pose",  RANT_SUB_ONLY, W,      &co);
    for (t=0;t<800 && rant_topic_match_count(pc)==0;t++){ rant_node_poll(P,2); rant_node_poll(S,2); }
    CHECK(pc && rant_topic_match_count(pc)==1, "detail-live: matched");

    /* the oracle: the publisher's locator, version, and advertised topics as its v9
       blob (held by the subscriber) states them */
    {   uint16_t cnt=0, k; const i_RantDiscoveryPeerView *ps = test_peers(S, &cnt);
        const i_RantDiscoveryPeerView *pp = NULL;
        for (k=0;k<cnt;k++) if (ps[k].name.len==6 && memcmp(ps[k].name.data,"dt-pub",6)==0) pp=&ps[k];
        CHECK(pp!=NULL, "detail-live: publisher in the peer view");
        if (pp){
            i_RantInterestIter it; i_RantTopicEntry tp;
            memcpy(pip, pp->addr.ip, 4); pport = pp->addr.port; pversion = pp->meta_version;
            memset(&it,0,sizeof it);
            /* v10 entries carry 32-bit hashes only: bind each announced entry to the
               topic we created on P by hash, and expect the RESP to fill in the rest */
            while (nw<4 && i_rant_node_peer_interest_next(pp, &it, &tp)){
                if (!tp.is_pub) continue;
                oalias[nw] = tp.index;
                if (tp.hash == (uint32_t)i_rant_topic_id("dt/pose")){
                    onlen[nw]=7; memcpy(oname[nw],"dt/pose",7);  ohash[nw]=rant_schema_hash(W);
                } else if (tp.hash == (uint32_t)i_rant_topic_id("dt/plain")){
                    onlen[nw]=8; memcpy(oname[nw],"dt/plain",8); ohash[nw]=0;
                } else continue;
                nw++;
            }
        }
    }
    CHECK(nw==2 && pport!=0, "detail-live: oracle holds both pub topics (nw=%u)", nw);

    /* observer mode (opts.fetch_details): a topic-less node greedily fetches every
       peer topic's name + schema and serves the rant_node_peer_topic_* queries */
    {   RantAllocator oa = rant_allocator_heap(0);
        RantNodeOpts oo = po; RantNode *O;
        oo.fetch_details = 1;
        O = rant_node_open(&oa, "dt-obs", test_on_message, NULL, &oo);
        CHECK(O != NULL, "detail-obs: observer node opens");
        if (O){
            uint32_t pid = 0;
            int have_pose = 0, have_plain = 0, pose_typed = 0, plain_raw = 0;
            for (t=0;t<800;t++){
                uint16_t cnt=0, k; const i_RantDiscoveryPeerView *ops;
                RantIter it; RantEntityInfo ei;
                rant_node_poll(O,2); rant_node_poll(P,1); rant_node_poll(S,1);
                ops = test_peers(O, &cnt);
                pid = 0;
                for (k=0;k<cnt;k++)
                    if (ops[k].name.len==6 && memcmp(ops[k].name.data,"dt-pub",6)==0) pid = ops[k].id;
                if (!pid) continue;
                have_pose = have_plain = 0;
                memset(&it,0,sizeof it);
                while (rant_node_entities_next(O, pid, &it, &ei)){
                    if (ei.name.len==7 && memcmp(ei.name.data,"dt/pose",7)==0){
                        have_pose = 1;
                        pose_typed = ei.schema && ei.schema_hash==rant_schema_hash(W)
                                     && rant_schema_hash(ei.schema)==ei.schema_hash;
                    }
                    if (ei.name.len==8 && memcmp(ei.name.data,"dt/plain",8)==0){
                        have_plain = 1; plain_raw = !ei.schema && ei.schema_hash==0;
                    }
                }
                if (have_pose && have_plain) break;
            }
            CHECK(have_pose && have_plain, "detail-obs: greedy cache resolves both topic names");
            CHECK(pose_typed, "detail-obs: typed topic's schema cached parsed (hash matches)");
            CHECK(plain_raw, "detail-obs: raw topic cached untyped");
            /* the subscriber is authoritative about its schema too: the observer fetches a SUB_ONLY
               topic's schema exactly like a publisher's */
            {   uint32_t sid = 0; int sub_typed = 0;
                for (t=0;t<800;t++){
                    uint16_t cnt=0, k; const i_RantDiscoveryPeerView *ops;
                    RantIter it; RantEntityInfo ei;
                    rant_node_poll(O,2); rant_node_poll(P,1); rant_node_poll(S,1);
                    ops = test_peers(O, &cnt);
                    sid = 0;
                    for (k=0;k<cnt;k++)
                        if (ops[k].name.len==6 && memcmp(ops[k].name.data,"dt-sub",6)==0) sid = ops[k].id;
                    if (!sid) continue;
                    memset(&it,0,sizeof it);
                    while (rant_node_entities_next(O, sid, &it, &ei))
                        if (ei.schema && ei.schema_hash==rant_schema_hash(W)
                            && rant_schema_hash(ei.schema)==ei.schema_hash) sub_typed = 1;
                    if (sub_typed) break;
                }
                CHECK(sub_typed,
                         "detail-obs: subscriber-only topic's schema fetched (subscriber authoritative)");
            }
            /* observe then subscribe, the explorer's flow: the greedy fetch dissolved these indices
               against a topic less node, so creating the topic must re pend and re verify them */
            {   RantTopic *osub = rant_node_create_topic(O, "dt/pose", RANT_SUB_ONLY, W, &co);
                unsigned long a0 = test_messages;
                CHECK(osub != NULL, "detail-obs: late subscribe topic created");
                for (t=0;t<800 && rant_topic_match_count(pc)<2;t++){
                    rant_node_poll(O,2); rant_node_poll(P,1); rant_node_poll(S,1);
                }
                CHECK(rant_topic_match_count(pc)==2,
                         "detail-obs: observe-then-subscribe re-verifies and matches (%u readers)",
                         rant_topic_match_count(pc));
                {   uint8_t pose[16]; memset(pose, 0x33, sizeof pose);   /* stamp + x */
                    rant_topic_send(pc, rant_bytes(pose, sizeof pose), NULL);
                    for (t=0;t<400 && test_messages < a0+2;t++){
                        rant_node_poll(P,1); rant_node_poll(S,1); rant_node_poll(O,2);
                    }
                    CHECK(test_messages >= a0+2, "detail-obs: late subscriber receives (got %lu new)",
                             test_messages - a0);
                }
            }
            rant_node_close(O, 1);
        }
    }

    q = i_rant_plat_udp_open();
    if (q != RANT_SOCK_BAD){ if (!i_rant_plat_bind(q, 0, 0, 0)){ i_rant_plat_close(q); q = RANT_SOCK_BAD; } }
    CHECK(q != RANT_SOCK_BAD, "detail-live: raw requester socket");
    if (q != RANT_SOCK_BAD && nw==2 && pport){
        uint8_t req[128], r1[2048], r2[2048]; size_t rl; int n1=-1, n2=-1;
        i_RantDetailWant wants[4]; uint16_t k;
        i_rant_plat_set_nonblock(q);
        for (k=0;k<nw;k++){ wants[k].index=oalias[k]; wants[k].schema_hash=0; }
        rl = i_rant_detail_req_build(dom, pversion, wants, nw, req, sizeof req);

        /* a wrong-domain request is ignored (the node core's domain gate) */
        {   uint8_t bad[128]; size_t bl = i_rant_detail_req_build((uint16_t)(dom+1), pversion,
                                                                wants, nw, bad, sizeof bad);
            i_rant_plat_send(q, bad, bl, pip, pport);
            for (t=0;t<50;t++){ rant_node_poll(P,1); rant_node_poll(S,1);
                                if (i_rant_plat_recv(q, r1, sizeof r1, NULL, NULL) > 0){ n1=1; break; } }
            CHECK(n1<0, "detail-live: wrong-domain request ignored");
        }

        /* the real request, re-sent like a real requester until answered */
        n1 = -1;
        for (t=0;t<400 && n1<=0;t++){
            if ((t & 63)==0) i_rant_plat_send(q, req, rl, pip, pport);
            rant_node_poll(P,2); rant_node_poll(S,1);
            n1 = i_rant_plat_recv(q, r1, sizeof r1, NULL, NULL);
        }
        CHECK(n1>0, "detail-live: response reached the request's source socket");
        if (n1>0){
            RantBytes rb = rant_bytes(r1, (size_t)n1);
            i_RantDetailIter it; i_RantDetail d; int n=0, names_ok=1, hashes_ok=1, wire_ok=0;
            CHECK(i_rant_detail_kind(rb)==RANT_DETAIL_RESP && i_rant_detail_domain(rb)==dom
                  && i_rant_detail_meta_version(rb)==pversion,
                     "detail-live: header matches the announced version (%u)", pversion);
            memset(&it,0,sizeof it);
            while (i_rant_detail_next(rb, &it, &d)){
                for (k=0;k<nw;k++) if (oalias[k]==d.index) break;
                if (k==nw){ names_ok=0; continue; }
                if (d.name.len!=onlen[k] || memcmp(d.name.data, oname[k], onlen[k])!=0) names_ok=0;
                if (d.schema_hash != ohash[k]) hashes_ok=0;
                if (ohash[k] && d.schema_wire.len){
                    RantSchema *ps2 = i_rant_schema_parse(d.schema_wire.data, d.schema_wire.len,
                                                        rant_allocator_alloc, &ma);
                    if (ps2 && rant_schema_hash(ps2)==ohash[k]) wire_ok=1;
                }
                n++;
            }
            CHECK(n==(int)nw && names_ok, "detail-live: names match the announce blob (n=%d)", n);
            CHECK(hashes_ok, "detail-live: schema hashes match the announce blob");
            CHECK(wire_ok, "detail-live: typed topic's wire inlined and parses to the advertised hash");
        }

        /* duplicates are idempotent: two more asks, two byte-identical answers */
        {   int got=0; n1=n2=-1;
            for (t=0;t<400 && got<2;t++){
                if ((t & 63)==0) i_rant_plat_send(q, req, rl, pip, pport);
                rant_node_poll(P,2); rant_node_poll(S,1);
                {   int r = i_rant_plat_recv(q, got==0?r1:r2, sizeof r1, NULL, NULL);
                    if (r>0){ if (got==0) n1=r; else n2=r; got++; } }
            }
            CHECK(n1>0 && n1==n2 && memcmp(r1,r2,(size_t)n1)==0,
                     "detail-live: duplicate requests answer byte-identically");
        }

        /* a RESP and garbage uDTL bytes at the node are ignored, it still answers */
        if (n1>0) i_rant_plat_send(q, r1, (size_t)n1, pip, pport);
        {   uint8_t junk[6]={'u','D','T','L',0x7F,0x00};
            i_rant_plat_send(q, junk, sizeof junk, pip, pport); }
        n2 = -1;
        for (t=0;t<400 && n2<=0;t++){
            if ((t & 63)==0) i_rant_plat_send(q, req, rl, pip, pport);
            rant_node_poll(P,2); rant_node_poll(S,1);
            n2 = i_rant_plat_recv(q, r2, sizeof r2, NULL, NULL);
        }
        CHECK(n2>0, "detail-live: node still answers after RESP/garbage datagrams");
    }
    if (q != RANT_SOCK_BAD) i_rant_plat_close(q);
    rant_node_close(P,1); rant_node_close(S,1);
    rant_allocator_reset(&ma);
}

/* The threaded phases: service threads, condvar flow control and the waker. Phases 20 to
 * 24 are catalogued in spec/testing.md. */
#ifdef RANT_THREADS

#define TH_SENDERS 3
#define TH_MSGS    1000u

static volatile unsigned long th_recv, th_order_bad, th_lost, th_evicted_evt;
static unsigned long th_next_seq[TH_SENDERS];   /* only the sub's service thread writes */

static void th_on_message(const RantMsg *m){
    if (m->data.len >= 8){
        const uint8_t *p = (const uint8_t*)m->data.data;
        uint32_t tid = (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
        uint32_t seq = (uint32_t)p[4] | ((uint32_t)p[5]<<8) | ((uint32_t)p[6]<<16) | ((uint32_t)p[7]<<24);
        if (tid < TH_SENDERS){
            if (seq != th_next_seq[tid]) th_order_bad++;
            th_next_seq[tid] = seq + 1;
        }
        th_recv++;
    }
}
static void th_on_event(const RantEvent *ev){
    if (ev->kind == RANT_MSG_LOST) th_lost += (unsigned long)ev->lost_count;
    else if (ev->kind == RANT_ERROR && ev->error == RANT_E_EVICTED_UNSENT) th_evicted_evt++;
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

/* phase 23 state: node B echoes every request onto its reply topic from the
 * callback, and probes the forbidden reentrant calls exactly once */
static RantNode *th_echo_b;
static RantTopic *th_echo_rep;
static volatile unsigned long th_echo_replies;
static volatile int th_cb_send_rc = -100, th_cb_create_refused = -1, th_cb_setrole_rc = -100;
static void th_echo_b_on_msg(const RantMsg *m){
    th_cb_send_rc = rant_topic_send(th_echo_rep, m->data, NULL);
    if (th_cb_create_refused < 0){
        th_cb_create_refused = (rant_node_create_topic(th_echo_b, "th/na", RANT_PUBSUB, NULL, NULL) == NULL);
        th_cb_setrole_rc = rant_topic_set_role(th_echo_rep, RANT_PUB_ONLY);
    }
}
static void th_echo_a_on_msg(const RantMsg *m){ (void)m; th_echo_replies++; }

/* phase 24: hammer a reliable topic until told to stop */
static volatile int th_hammer_stop;
/* a slow inline handler: 30 ms of work, plus a create from inside, which stays refused */
static volatile unsigned long th_slow_runs, th_echo_recv; static volatile int th_slow_create_refused;
static void th_slow_on_message(const RantMsg *m){
    uint8_t b[8]; int k;
    th_slow_runs++;
    if (!rant_node_create_topic(m->node, "th/inside", RANT_PUB_ONLY, NULL, NULL)) th_slow_create_refused = 1;
    /* a burst from inside the handler on a depth 1 topic: each send must leave before the next */
    for (k=0;k<8;k++){ memset(b, k, sizeof b); test_send(m->node, 2, b, sizeof b); }
    test_sleep_ms(30);
}
static void th_echo_on_message(const RantMsg *m){ if (m->topic_index == 2) th_echo_recv++; }
/* a slow function handler on the service thread, and a caller thread driving it */
static volatile unsigned long th_fn_runs, th_fn_ok;
static void th_slow_on_request(RantRequest *rq, void *user){
    (void)user; th_fn_runs++; test_sleep_ms(30); rant_request_reply(rq, rant_bytes("ok", 2));
}
static void th_caller(void *arg){
    int k;
    for (k=0;k<10;k++){
        RantResponse rsp;
        if (rant_function_call((RantFunction*)arg, rant_bytes("q",1), &rsp, 2000, NULL) == 1
            && rsp.status == RANT_CALL_OK) th_fn_ok++;
    }
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
    while (!th_hammer_stop) rant_topic_send((RantTopic*)arg, rant_bytes(buf, sizeof buf), NULL);
}

static void threaded_checks(void){
    uint8_t payload[16]; unsigned i;
    memset(payload, 0x33, sizeof payload);

    /* 20. RELIABLE: exactly-once ordered delivery under multithreaded send */
    { i_RantTopicDef cd[1]; RantNodeOpts o; RantNode *w, *r;
      memset(cd, 0, sizeof cd);
      cd[0].name = "th/rel"; cd[0].role = RANT_PUB_ONLY;
      cd[0].qos.reliability = RANT_RELIABLE; cd[0].qos.keep_last = 8;
      cd[0].qos.max_message_bytes = 32; cd[0].qos.heartbeat_us = 50000;
      cd[0].qos.backpressure_wait_us = 500000;
      memset(&o, 0, sizeof o);
      o.domain = TEST_DOMAIN+4; o.disable_shm = 1; o.discovery.max_peers = 4;
      w = test_node_open("th-pub", NULL, th_on_event, o, cd, 1);
      cd[0].role = RANT_SUB_ONLY;
      r = test_node_open("th-sub", th_on_message, th_on_event, o, cd, 1);
      CHECK(w && r, "threaded: nodes open");
      if (w && r){
          th_recv = th_order_bad = th_lost = th_evicted_evt = 0;
          memset(th_next_seq, 0, sizeof th_next_seq);
          /* ST_CHECK evaluates its condition twice: keep side effects out of it */
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
            while (th_recv < (unsigned long)TH_SENDERS*TH_MSGS && i_rant_plat_now_us() < end)
                test_sleep_ms(5); }
          rant_node_stop(r); rant_node_stop(w);                    /* join: counters now settled */
          CHECK(th_recv == (unsigned long)TH_SENDERS*TH_MSGS,
                   "threaded: exactly-once delivery (%lu, want %lu)",
                   th_recv, (unsigned long)TH_SENDERS*TH_MSGS);
          CHECK(th_order_bad == 0, "threaded: per-thread order kept (bad=%lu)", th_order_bad);
          CHECK(th_lost == 0, "threaded: no loss (lost=%lu)", th_lost);
          CHECK(th_evicted_evt == 0 && rant_node_evicted_unsent(w) == 0,
                   "threaded: no unsent eviction (evt=%lu cnt=%u)",
                   th_evicted_evt, rant_node_evicted_unsent(w));
          rant_node_close(r, 1); rant_node_close(w, 1);
      }
    }

    /* 21 + 21b + 22. BURST / HOSTILE / WAKER on one best-effort pair */
    { i_RantTopicDef cd[1]; RantNodeOpts o; RantNode *w, *r;
      memset(cd, 0, sizeof cd);
      cd[0].name = "th/burst"; cd[0].role = RANT_PUB_ONLY;
      cd[0].qos.keep_last = 4;   /* best-effort */
      cd[0].qos.max_message_bytes = 32; cd[0].qos.heartbeat_us = 50000;
      memset(&o, 0, sizeof o);
      o.domain = TEST_DOMAIN+5; o.disable_shm = 1; o.discovery.max_peers = 4;
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

          /* 21. 64 back-to-back sends, keep_last 4: every one must reach the wire, since
             each send transmits before it returns */
          for (i=0;i<64;i++) test_send(w, 0, payload, sizeof payload);
          { uint64_t end = i_rant_plat_now_us() + 3000000u;
            while (th_recv < 64 && i_rant_plat_now_us() < end) test_sleep_ms(5); }
          CHECK(th_recv == 64, "burst: all 64 delivered past a depth-4 ring (%lu)", th_recv);
          CHECK(rant_node_evicted_unsent(w) == 0, "burst: nothing evicted unsent (%u)",
                   rant_node_evicted_unsent(w));

          /* 22. WAKER: quiesce, then one send must land well inside the announce capped sleep.
             Typical is sub ms, the margin absorbs a loaded CI box losing the CPU. */
          test_sleep_ms(300);
          { unsigned long r0 = th_recv; uint64_t t0 = i_rant_plat_now_us(), dt;
            test_send(w, 0, payload, sizeof payload);
            while (th_recv == r0 && i_rant_plat_now_us() - t0 < 1000000u) { /* spin */ }
            dt = i_rant_plat_now_us() - t0;
            CHECK(th_recv == r0+1 && dt < 400000u,
                     "waker: idle-node send delivered in %.1f ms", dt/1000.0);
          }

          /* 21b. HOSTILE: transport TX forced to would block. The first pass parks one datagram in
             tx_hold, past that the burst overwrites unsent history, which must surface as evicted. */
          { unsigned long r0 = th_recv; uint32_t e0 = rant_node_evicted_unsent(w);
            uint32_t evicted;
            wire_tap_write(&wire_tap.block_data, 1);
            for (i=0;i<64;i++) test_send(w, 0, payload, sizeof payload);
            evicted = rant_node_evicted_unsent(w) - e0;
            CHECK(evicted >= 1, "hostile: blocked TX surfaces RANT_E_EVICTED_UNSENT (%u)", evicted);
            wire_tap_write(&wire_tap.block_data, 0);
            /* the service retries the held datagram and drains the ring on its next pass, which
               the announce cadence bounds */
            { uint64_t end = i_rant_plat_now_us() + 3000000u;
              while (th_recv - r0 + evicted < 64 && i_rant_plat_now_us() < end) test_sleep_ms(10); }
            CHECK(th_recv - r0 + (unsigned long)evicted == 64,
                     "hostile: every send delivered or accounted (recv=%lu evicted=%u)",
                     th_recv - r0, evicted);
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

    /* 22b. CAPPED: a rate capped subscriber holds samples back on its lane, which must never
       slow a depth 1 publisher. A send that waits for the lane's tick takes 20 ms here. */
    { i_RantTopicDef cd[1]; RantNodeOpts o; RantNode *w, *r; uint8_t payload[16];
      memset(payload, 0, sizeof payload);
      memset(cd, 0, sizeof cd);
      cd[0].name = "th/capped"; cd[0].role = RANT_PUB_ONLY;
      cd[0].qos.keep_last = 1; cd[0].qos.max_message_bytes = 32;
      memset(&o, 0, sizeof o);
      o.domain = TEST_DOMAIN+12; o.disable_shm = 1; o.discovery.max_peers = 4;
      w = test_node_open("th-cpub", NULL, th_on_event, o, cd, 1);
      cd[0].role = RANT_SUB_ONLY; cd[0].qos.max_rate_hz = 50;
      r = test_node_open("th-csub", th_on_message, th_on_event, o, cd, 1);
      CHECK(w && r, "capped: nodes open");
      if (w && r){
          uint64_t worst = 0, began; double secs; unsigned long most; int i;
          th_recv = 0;
          rant_node_start(w); rant_node_start(r);
          { uint64_t end = i_rant_plat_now_us() + 5000000u;
            while (test_match_count(w, 0) == 0 && i_rant_plat_now_us() < end)
                test_sleep_ms(5); }
          test_sleep_ms(300);                     /* the rate section rides an announce */
          began = i_rant_plat_now_us();
          for (i=0;i<200;i++){
              uint64_t t0 = i_rant_plat_now_us(), dt;
              test_send(w, 0, payload, sizeof payload);
              dt = i_rant_plat_now_us() - t0;
              if (dt > worst) worst = dt;
              while (i_rant_plat_now_us() - t0 < 1000u) {}   /* a sleep can last a 15.6 ms tick */
          }
          /* the cap is judged over the real span, a busy publisher may still send slower */
          secs = (double)(i_rant_plat_now_us() - began) / 1e6;
          most = (unsigned long)(50.0 * secs * 1.25) + 2u;
          CHECK(worst < 10000u, "capped: no send waits for the capped lane (worst %.2f ms)",
                   worst/1000.0);
          CHECK(th_recv >= 2 && th_recv <= most && th_recv < 200,
                   "capped: the subscriber is still paced (%lu of 200 in %.0f ms, at most %lu)",
                   th_recv, secs * 1000.0, most);
          CHECK(rant_node_evicted_unsent(w) == 0, "capped: a held back sample is not an eviction (%u)",
                   rant_node_evicted_unsent(w));
          /* a lone held back sample leaves at the lane's 20 ms tick: the send that armed the
             tick must wake the service thread, which otherwise sleeps out its older plan */
          { uint64_t worst_gap = 0; int pair;
            for (pair=0;pair<3;pair++){
                unsigned long r0; uint64_t t0, dt;
                test_sleep_ms(300);
                r0 = th_recv;
                test_send(w, 0, payload, sizeof payload); test_sleep_ms(1);
                test_send(w, 0, payload, sizeof payload);
                t0 = i_rant_plat_now_us();
                while (th_recv < r0 + 2 && i_rant_plat_now_us() - t0 < 1000000u) test_sleep_ms(1);
                dt = i_rant_plat_now_us() - t0;
                if (th_recv < r0 + 2) dt = 1000000u;
                if (dt > worst_gap) worst_gap = dt;
            }
            CHECK(worst_gap < 100000u, "capped: a held back sample leaves at its tick (worst %.0f ms)",
                     worst_gap/1000.0); }
          rant_node_close(r, 1); rant_node_close(w, 1);
      }
    }

    /* 22c. UNLOCKED HANDLER: a 30 ms inline handler on the service thread must not delay a
       send on another thread of the same node. A create from a third thread waits for the
       handler and succeeds, a create from inside the handler stays refused. */
    { i_RantTopicDef cw[3], cr[3]; RantNodeOpts o; RantNode *w, *r; uint8_t payload[16];
      memset(payload, 0, sizeof payload);
      memset(cw, 0, sizeof cw);
      cw[0].name = "th/fast"; cw[0].role = RANT_PUB_ONLY; cw[0].qos.keep_last = 1; cw[0].qos.max_message_bytes = 32;
      cw[1].name = "th/slow"; cw[1].role = RANT_SUB_ONLY; cw[1].qos.keep_last = 4; cw[1].qos.max_message_bytes = 32;
      cw[2].name = "th/echo"; cw[2].role = RANT_PUB_ONLY; cw[2].qos.keep_last = 1; cw[2].qos.max_message_bytes = 32;
      memcpy(cr, cw, sizeof cw); cr[0].role = RANT_SUB_ONLY; cr[1].role = RANT_PUB_ONLY; cr[2].role = RANT_SUB_ONLY;
      memset(&o, 0, sizeof o);
      o.domain = TEST_DOMAIN+14; o.disable_shm = 1; o.discovery.max_peers = 4; o.max_topics = 8;
      w = test_node_open("th-upub", th_slow_on_message, th_on_event, o, cw, 3);
      r = test_node_open("th-usub", th_echo_on_message, th_on_event, o, cr, 3);
      CHECK(w && r, "unlocked: nodes open");
      if (w && r){
          i_RantThread creator; uint64_t worst = 0, next_slow, end;
          th_slow_runs = 0; th_echo_recv = 0; th_slow_create_refused = 0; th_late_topic = NULL; th_late_us = 0;
          rant_node_start(w); rant_node_start(r);
          end = i_rant_plat_now_us() + 5000000u;
          while ((test_match_count(w, 0) == 0 || test_match_count(r, 1) == 0
                  || test_match_count(w, 2) == 0) && i_rant_plat_now_us() < end) test_sleep_ms(5);
          i_rant_plat_thread_start(&creator, th_late_creator, w);
          end = i_rant_plat_now_us() + 1000000u; next_slow = 0;
          while (i_rant_plat_now_us() < end){
              uint64_t t0 = i_rant_plat_now_us(), dt;
              if (t0 >= next_slow){ test_send(r, 1, payload, sizeof payload); next_slow = t0 + 50000u; }
              test_send(w, 0, payload, sizeof payload);
              dt = i_rant_plat_now_us() - t0;
              if (dt > worst) worst = dt;
              test_sleep_ms(1);
          }
          i_rant_plat_thread_join(&creator);
          CHECK(th_slow_runs >= 10, "unlocked: the slow handler ran (%lu times)", th_slow_runs);
          CHECK(worst < 10000u, "unlocked: no send waited for the handler (worst %.1f ms)", worst/1000.0);
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
            i_rant_plat_thread_start(&caller, th_caller, rem);
            end = i_rant_plat_now_us() + 600000u;
            while (i_rant_plat_now_us() < end){
                uint64_t t0 = i_rant_plat_now_us(), dt;
                test_send(w, 0, payload, sizeof payload);
                dt = i_rant_plat_now_us() - t0;
                if (dt > fworst) fworst = dt;
                test_sleep_ms(1);
            }
            i_rant_plat_thread_join(&caller);
            CHECK(th_fn_ok == 10 && th_fn_runs == 10, "unlocked: ten calls answered through the slow handler (%lu ok, %lu runs)",
                     th_fn_ok, th_fn_runs);
            CHECK(fworst < 10000u, "unlocked: no send waited for the function handler (worst %.1f ms)", fworst/1000.0);
            if (rem) rant_function_retire(rem);
            if (def) rant_function_retire(def); }
          test_sleep_ms(100);
          CHECK(th_echo_recv == th_slow_runs * 8 && rant_node_evicted_unsent(w) == 0,
                   "unlocked: a burst from inside the handler all left on a depth 1 topic (%lu of %lu, evicted %u)",
                   th_echo_recv, th_slow_runs * 8, rant_node_evicted_unsent(w));
          rant_node_close(r, 1); rant_node_close(w, 1);
      }
    }

    /* 23. REENTRANT: echo from the callback, forbidden calls refuse loudly. The ring must be
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
      o.domain = TEST_DOMAIN+6; o.disable_shm = 1; o.discovery.max_peers = 4;
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
            while (th_echo_replies < 10 && i_rant_plat_now_us() < end) test_sleep_ms(5); }
          rant_node_stop(b); rant_node_stop(a);
          CHECK(th_echo_replies == 10, "reentrant: callback send echoes (%lu/10)", th_echo_replies);
          CHECK(th_cb_send_rc == RANT_OK, "reentrant: callback send returns RANT_OK (%d)", th_cb_send_rc);
          CHECK(th_cb_create_refused == 1, "reentrant: callback create_topic refused");
          CHECK(th_cb_setrole_rc == RANT_ERR_STATE, "reentrant: callback set_role refused (%d)", th_cb_setrole_rc);
          rant_node_close(b, 1); rant_node_close(a, 1);
      }
    }

    /* 24. STOP-UNDER-LOAD: hammer threads parked in the backpressure wait while stop
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
          o.domain = (uint16_t)(TEST_DOMAIN+7+iter); o.disable_shm = 1; o.discovery.max_peers = 4;
          w = test_node_open("th-hpub", NULL, NULL, o, cd, 1);
          cd[0].role = RANT_SUB_ONLY;
          r = test_node_open("th-hsub", NULL, NULL, o, cd, 1);
          if (!w || !r){ CHECK(0, "stop-load: nodes open (iter %d)", iter); break; }
          { uint64_t end = i_rant_plat_now_us() + 5000000u;     /* match, then silence the reader */
            while (test_match_count(w, 0) == 0 && i_rant_plat_now_us() < end)
                test_pump(w, r, 10); }
          rant_node_start(w);                      /* reader stays unpolled: never acks */
          th_hammer_stop = 0;
          i_rant_plat_thread_start(&h1, th_hammer, rant_node_topic(w, 0));
          i_rant_plat_thread_start(&h2, th_hammer, rant_node_topic(w, 0));
          test_sleep_ms(50);                         /* hammers now parked in the wait */
          rant_node_stop(w);                       /* broadcasts the waiters loose */
          th_hammer_stop = 1;
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
#endif /* RANT_THREADS */

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
    ao = (RantNodeOpts){ .domain=TEST_DOMAIN+4, .discovery={ .max_peers=4 } };
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

/* The pull phase: a pull topic never calls back, take pops the oldest, take_latest the
 * newest after dropping the rest, and a topic created without pull refuses take. */
/* Pattern handle schemas: what a definition declared, a function's missing progress, and
   what a reflect_from_mesh remote adopted once the mesh typed it. */
static void ps_request(RantRequest *req, void *user){ (void)user; rant_request_reply(req, rant_bytes(NULL, 0)); }
static void pattern_schema_checks(void){
    RantAllocator aa = rant_allocator_heap(0), ba = rant_allocator_heap(0);
    RantNodeOpts o;
    RantNode *a, *b;
    const RantSchema *rq, *rs, *pg;
    RantFunction *fn = NULL, *task = NULL, *remote = NULL;
    RantVariable *var = NULL;
    int t, typed = 0;
    memset(&o, 0, sizeof o);
    o.domain = TEST_DOMAIN+53; o.discovery.max_peers = 4; o.max_topics = 16; o.fetch_details = 1;
    o.net.multicast_interface = "127.0.0.1";
    a = rant_node_open(&aa, "ps-a", NULL, NULL, &o);
    b = rant_node_open(&ba, "ps-b", NULL, NULL, &o);
    CHECK(a && b, "pattern schema: nodes open");
    if (!(a && b)) goto done;
    rq = rant_node_schema(a, "PsReq { x: i32 }");
    rs = rant_node_schema(a, "PsRsp { y: f64 }");
    pg = rant_node_schema(a, "PsPrg { done: u8 }");
    fn   = rant_node_create_function_definition(a, "ps/fn", rq, rs, ps_request, NULL, NULL);
    task = rant_node_create_task_definition(a, "ps/task", rq, pg, rs, ps_request, NULL, NULL);
    var  = rant_node_create_variable_definition(a, "ps/var", rs, NULL);
    CHECK(fn && task && var, "pattern schema: handles created");
    if (!(fn && task && var)) goto done;
    CHECK(rant_function_request_schema(fn) == rq && rant_function_response_schema(fn) == rs,
             "pattern schema: a definition reports its schemas");
    CHECK(rant_function_progress_schema(fn) == NULL, "pattern schema: a function has no progress schema");
    CHECK(rant_function_progress_schema(task) == pg, "pattern schema: a task reports its progress schema");
    CHECK(rant_variable_schema(var) == rs, "pattern schema: a variable reports its schema");
    CHECK(!rant_function_request_schema(NULL) && !rant_variable_schema(NULL),
             "pattern schema: NULL handles report NULL");

    remote = rant_node_create_remote_function(b, "ps/fn", NULL, NULL,
                                              &(RantFunctionOpts){ .reflect_from_mesh = 1 });
    CHECK(remote && !rant_function_request_schema(remote), "pattern schema: a reflect remote starts untyped");
    for (t = 0; t < 2500 && remote && !typed; t++){
        test_pump(a, b, 2);
        if (rant_function_refresh(remote) == 1 || rant_function_request_schema(remote)) typed = 1;
    }
    CHECK(typed && rant_schema_hash(rant_function_request_schema(remote)) == rant_schema_hash(rq)
             && rant_schema_hash(rant_function_response_schema(remote)) == rant_schema_hash(rs),
             "pattern schema: a reflect remote reports the schemas it adopted");
done:
    if (a) rant_node_close(a, 0);
    if (b) rant_node_close(b, 0);
}

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
    for (t=0;t<2500 && rant_topic_match_count(pub)<1;t++) test_pump(a,b,2);

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
    ao = (RantNodeOpts){ .domain=TEST_DOMAIN+5, .discovery={ .max_peers=4 } };
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
       the caller's thread. ST_CHECK evaluates twice, so the starts run outside it. */
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
static int pf_calls;
static void pf_add_handler(RantRequest *req, void *user){
    uint8_t out[4];
    uint32_t v = req->data.len>=4 ? i_rant_le_r32(req->data.data) : 0;
    (void)user; pf_calls++;
    i_rant_le_w32(out, v+1);
    rant_request_reply(req, rant_bytes(out,4));
}
static void pf_empty_handler(RantRequest *req, void *user){ (void)req;(void)user; pf_calls++; }
/* fail with an over long message and a structured payload: pins the wire carried
 * APP_ERROR text, the truncating cap, and that data still rides beside the message */
static void pf_fail_handler(RantRequest *req, void *user){
    char big[301]; int i; uint8_t out[4]; (void)user; pf_calls++;
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
static volatile uint64_t pf_defer_token;
static void pf_defer_handler(RantRequest *req, void *user){ (void)user; pf_calls++; pf_defer_token = rant_request_defer(req); }
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

static void pf_pump(RantNode *a, RantNode *b, int ms){
    uint64_t end = i_rant_plat_now_us() + (uint64_t)ms*1000u;
    while (i_rant_plat_now_us() < end){ rant_node_poll(a,2); rant_node_poll(b,2); }
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

    for (t=0;t<2000 && rant_function_match_count(call_add)==0;t++) pf_pump(P,C,2);
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
      for (t=0;t<800 && !pf_reply_done;t++) pf_pump(P,C,2);
      CHECK(pf_reply_done && pf_reply_status==RANT_CALL_OK && pf_reply_val==42,
               "patterns: async add(41)=42 OK (done=%d st=%d val=%u)", pf_reply_done, pf_reply_status, pf_reply_val);
      CHECK(pf_reply_msg_len==0, "patterns: OK response message empty (len=%u)",
               (unsigned)pf_reply_msg_len); }

    /* fail with a message: APP_ERROR text rides the response header (truncated at the
       255 cap, never refused) beside a structured payload */
    { size_t i; int msg_ok;
      pf_reply_done=0;
      rant_function_call_async(cfm, rant_bytes(NULL,0), pf_on_reply, NULL, NULL);
      for (t=0;t<800 && !pf_reply_done;t++) pf_pump(P,C,2);
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
    { pf_reply_done=0; pf_calls=0;
      rant_function_call_async(ce, rant_bytes(NULL,0), pf_on_reply, NULL, NULL);
      for (t=0;t<800 && !pf_reply_done;t++) pf_pump(P,C,2);
      CHECK(pf_reply_done && pf_reply_status==RANT_CALL_OK && pf_reply_val==0,
               "patterns: empty-ack auto OK (done=%d st=%d)", pf_reply_done, pf_reply_status); }

    /* deferred: handler defers, we complete later with a value */
    { pf_reply_done=0; pf_defer_token=0;
      rant_function_call_async(cd, rant_bytes(NULL,0), pf_on_reply, NULL, NULL);
      for (t=0;t<800 && !pf_defer_token;t++) pf_pump(P,C,2);
      CHECK(pf_defer_token!=0, "patterns: handler deferred (token=%llu)", (unsigned long long)pf_defer_token);
      { uint8_t out[4]; i_rant_le_w32(out,99);
        rant_function_complete(pd, pf_defer_token, RANT_CALL_OK, "deferred done", rant_bytes(out,4)); }
      for (t=0;t<800 && !pf_reply_done;t++) pf_pump(P,C,2);
      CHECK(pf_reply_done && pf_reply_status==RANT_CALL_OK && pf_reply_val==99,
               "patterns: deferred completion delivers 99 (done=%d val=%u)", pf_reply_done, pf_reply_val);
      CHECK(pf_reply_msg_len==13 && memcmp(pf_reply_msg,"deferred done",13)==0,
               "patterns: complete() message carried on OK (\"%s\")", pf_reply_msg); }

    /* no handler: the provider has a NULL on_request, NO_HANDLER */
    { pf_reply_done=0;
      rant_function_call_async(cnh, rant_bytes(NULL,0), pf_on_reply, NULL, NULL);
      for (t=0;t<800 && !pf_reply_done;t++) pf_pump(P,C,2);
      CHECK(pf_reply_done && pf_reply_status==RANT_CALL_NO_HANDLER,
               "patterns: no-handler -> NO_HANDLER (done=%d st=%d)", pf_reply_done, pf_reply_status);
      CHECK(pf_reply_msg_len==10 && memcmp(pf_reply_msg,"no handler",10)==0,
               "patterns: empty wire message defaults to status text (\"%s\")", pf_reply_msg); }

    /* no provider for "ghost": the deadline synthesizes NO_PROVIDER, never TIMEOUT */
    { pf_reply_done=0;
      rant_function_call_async(ghost, rant_bytes(NULL,0), pf_on_reply, NULL, NULL);
      for (t=0;t<400 && !pf_reply_done;t++) pf_pump(P,C,2);
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
      for (t=0;t<2000 && !pf_reply_done;t++) pf_pump(P,C,2);
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
          for (t=0;t<2000 && rant_function_match_count(call2)==0;t++){ pf_pump(P,C,1); rant_node_poll(C2,1); }
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
          int cr;   /* hoisted: ST_CHECK evaluates its condition twice */
          i_rant_le_w32(req,(uint32_t)(1000+i2)); expect_sum += (uint32_t)(1000+i2+1);
          cr = rant_function_call_async(call_add, rant_bytes(req,4), pf_on_reply_burst, NULL, NULL);
          CHECK(cr==RANT_OK, "patterns: burst call %d accepted (%d)", i2, cr);
      }
      for (t=0;t<2000 && pf_burst_done<14;t++) rant_node_poll(C,2);
      CHECK(pf_burst_done==14 && pf_burst_bad==0 && pf_burst_sum==expect_sum,
               "patterns: burst 14/keep_last 10 all OK (done=%d bad=%d sum=%u want=%u)",
               pf_burst_done, pf_burst_bad, pf_burst_sum, expect_sum);
      CHECK(rant_node_evicted_unsent(C)==0, "patterns: burst evicted nothing (%u)",
               rant_node_evicted_unsent(C));
      rant_node_stop(P); }

    /* a sync call that times out locally, then the deferred reply lands late: the pending
       entry must be unlinked, the late reply dropped, and a later sync call still works */
    { RantResponse rep; int rc;
      rant_node_start(P);
      pf_defer_token=0;
      rc = rant_function_call(cd, rant_bytes(NULL,0), &rep, 120, NULL);
      CHECK(rc==0 && rep.status==RANT_CALL_TIMEOUT,
               "patterns: sync local timeout (rc=%d st=%d)", rc, rep.status);
      CHECK(rep.message.len==7 && memcmp(rep.message.data,"timeout",7)==0,
               "patterns: sync timeout carries status text");
      for (t=0;t<400 && !pf_defer_token;t++) rant_node_poll(C,2);
      CHECK(pf_defer_token!=0, "patterns: deferred token arrived");
      if (pf_defer_token){ uint8_t out[4]; i_rant_le_w32(out,7);
          rant_function_complete(pd, pf_defer_token, RANT_CALL_OK, NULL, rant_bytes(out,4)); }
      for (t=0;t<200;t++) rant_node_poll(C,2);     /* late reply arrives: must be dropped safely */
      pf_reply_done=0;
      { uint8_t req[4]; i_rant_le_w32(req,60);
        rc = rant_function_call(call_add, rant_bytes(req,4), &rep, 1000, NULL); }
      CHECK(rc==1 && rep.status==RANT_CALL_OK && rep.data.len>=4 && i_rant_le_r32(rep.data.data)==61,
               "patterns: sync works after late-reply drop (rc=%d st=%d)", rc, rep.status);
      rant_node_stop(P); }
#endif

    /* ---- variables: catch-up, convergence, read-only refusal, force/absorb/unforce ---- */
    { RantVariable *ov, *av; RantBytes gv; uint8_t b[4];
      i_rant_le_w32(b,20);
      ov = rant_node_create_variable_definition(P, "temp", NULL, &(RantVariableOpts){ .initial=rant_bytes(b,4), .allow_force=1 });
      av = rant_node_create_remote_variable(C, "temp", NULL, NULL);
      CHECK(ov && av, "var: created/opened");
      for (t=0;t<2000 && rant_variable_match_count(ov)==0;t++) pf_pump(P,C,2);
      CHECK(rant_variable_match_count(ov)==1, "var: accessor matched (%d)", rant_variable_match_count(ov));
      for (t=0;t<600 && !rant_variable_get(av,&gv);t++) pf_pump(P,C,2);
      CHECK(rant_variable_get(av,&gv) && gv.len==4 && i_rant_le_r32(gv.data)==20, "var: catch_up initial=20");
      i_rant_le_w32(b,25); rant_variable_set(ov, rant_bytes(b,4));
      for (t=0;t<600;t++){ pf_pump(P,C,2); if (rant_variable_get(av,&gv)&&gv.len==4&&i_rant_le_r32(gv.data)==25) break; }
      CHECK(rant_variable_get(av,&gv)&&i_rant_le_r32(gv.data)==25, "var: owner set converges (25)");
      i_rant_le_w32(b,30);
      { int sr = rant_variable_set(av, rant_bytes(b,4)); CHECK(sr==RANT_OK, "var: accessor set ok (%d)", sr); }
      for (t=0;t<600;t++){ pf_pump(P,C,2); if (rant_variable_get(ov,&gv)&&gv.len==4&&i_rant_le_r32(gv.data)==30) break; }
      CHECK(rant_variable_get(ov,&gv)&&i_rant_le_r32(gv.data)==30, "var: accessor set applied at owner (30)");
      i_rant_le_w32(b,99); rant_variable_force(ov, rant_bytes(b,4));
      for (t=0;t<600;t++){ pf_pump(P,C,2); if (rant_variable_forced(av)&&rant_variable_get(av,&gv)&&i_rant_le_r32(gv.data)==99) break; }
      CHECK(rant_variable_forced(av)&&rant_variable_get(av,&gv)&&i_rant_le_r32(gv.data)==99,
               "var: force visible at accessor (99, forced)");
      i_rant_le_w32(b,50); rant_variable_set(av, rant_bytes(b,4));         /* absorbed into the shadow */
      pf_pump(P,C,80);
      CHECK(rant_variable_get(av,&gv)&&i_rant_le_r32(gv.data)==99, "var: set absorbed while forced (still 99)");
      rant_variable_unforce(ov);
      for (t=0;t<600;t++){ pf_pump(P,C,2); if (!rant_variable_forced(av)&&rant_variable_get(av,&gv)&&i_rant_le_r32(gv.data)==50) break; }
      CHECK(!rant_variable_forced(av)&&rant_variable_get(av,&gv)&&i_rant_le_r32(gv.data)==50,
               "var: unforce restores latest absorbed (50)"); }
    { RantVariable *ro_o, *ro_a; uint8_t b[4]; int sr; i_rant_le_w32(b,7);
      ro_o = rant_node_create_variable_definition(P, "rovar", NULL, &(RantVariableOpts){ .initial=rant_bytes(b,4), .access=RANT_VAR_READONLY });
      ro_a = rant_node_create_remote_variable(C, "rovar", NULL, NULL);
      CHECK(ro_o&&ro_a, "var: read-only created/opened");
      for (t=0;t<600;t++) pf_pump(P,C,2);
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
      for (t=0;t<2000 && rant_variable_match_count(to)==0;t++) pf_pump(P,C,2);
      for (t=0;t<600 && !rant_variable_get(ta,&gv);t++) pf_pump(P,C,2);
      i_rant_le_w32(b,777); fr = rant_variable_force(ta, rant_bytes(b,4));         /* remote force */
      CHECK(fr==RANT_OK, "var: typed remote force sent (%d)", fr);
      for (t=0;t<600;t++){ pf_pump(P,C,2); if (rant_variable_forced(ta)&&rant_variable_get(ta,&gv)&&i_rant_le_r32(gv.data)==777) break; }
      CHECK(rant_variable_forced(ta)&&rant_variable_get(ta,&gv)&&i_rant_le_r32(gv.data)==777,
               "var: typed force pins (777, forced)");
      ur = rant_variable_unforce(ta);     /* remote, op only payload */
      CHECK(ur==RANT_OK, "var: typed remote unforce sent (%d)", ur);
      for (t=0;t<600;t++){ pf_pump(P,C,2); if (!rant_variable_forced(ta)) break; }
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
      for (t=0;t<2000 && ac.n==0;t++) pf_pump(P,C,2);   /* catch-up: the first value fires both */
      CHECK(ac.n==1 && aw.n==1 && ac.val==5,
               "var: accessor first value fires (change=%d write=%d val=%u)", ac.n, aw.n, ac.val);
      CHECK(ac.source!=0, "var: accessor source = the owner peer (%u)", ac.source);
      i_rant_le_w32(b,6); rant_variable_set(eo, rant_bytes(b,4));         /* local set, source 0 */
      CHECK(oc.n==2 && ow.n==1 && oc.val==6 && oc.source==0,
               "var: local set fires inline on the caller (change=%d write=%d src=%u)", oc.n, ow.n, oc.source);
      rant_variable_set(eo, rant_bytes(b,4));                   /* byte-identical re-set */
      CHECK(oc.n==2 && ow.n==2 && ow.seq==oc.seq+1,
               "var: identical re-set fires write only, seq advances (change=%d write=%d)", oc.n, ow.n);
      for (t=0;t<600 && aw.n<3;t++) pf_pump(P,C,2);
      CHECK(aw.n==3 && ac.n==2 && ac.val==6,
               "var: accessor saw 3 writes / 2 changes (w=%d c=%d val=%u)", aw.n, ac.n, ac.val);
      rant_variable_force(eo, rant_bytes(b,4));       /* same bytes: the forced flip is a change */
      CHECK(oc.n==3 && oc.forced==1 && ow.n==3,
               "var: force with identical bytes still a change (n=%d forced=%u)", oc.n, oc.forced);
      for (t=0;t<600 && ac.n<3;t++) pf_pump(P,C,2);
      CHECK(ac.n==3 && ac.forced==1, "var: accessor sees the forced flip (n=%d forced=%u)", ac.n, ac.forced);
      i_rant_le_w32(b,44); rant_variable_set(eo, rant_bytes(b,4));         /* absorbed into the shadow */
      pf_pump(P,C,40);
      CHECK(oc.n==3 && ow.n==3, "var: absorbed write fires nothing (c=%d w=%d)", oc.n, ow.n);
      rant_variable_unforce(eo);
      CHECK(oc.n==4 && !oc.forced && oc.val==44 && ow.n==4,
               "var: unforce fires with the restored value (n=%d val=%u)", oc.n, oc.val);
      for (t=0;t<600 && ac.n<4;t++) pf_pump(P,C,2);
      CHECK(ac.n==4 && !ac.forced && ac.val==44,
               "var: accessor unforce change (n=%d val=%u forced=%u)", ac.n, ac.val, ac.forced);
      i_rant_le_w32(b,70);
      { int sr = rant_variable_set(ea, rant_bytes(b,4)); CHECK(sr==RANT_OK, "var: event remote set ok (%d)", sr); }
      for (t=0;t<600 && oc.n<5;t++) pf_pump(P,C,2);
      CHECK(oc.n==5 && oc.val==70 && oc.source!=0,
               "var: remote set fires at the owner with the setter's peer (src=%u)", oc.source);
      rant_variable_on_change(eo, NULL, NULL); rant_variable_on_write(eo, NULL, NULL);
      i_rant_le_w32(b,71); rant_variable_set(eo, rant_bytes(b,4));
      CHECK(oc.n==5 && ow.n==5, "var: cleared callbacks stay silent (c=%d w=%d)", oc.n, ow.n);
      for (t=0;t<600 && ac.n<5;t++) pf_pump(P,C,2);   /* drain the echoes before the captures die */
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
    for (t=0;t<200;t++) pf_pump(P,C,2);   /* let any straggling detail fetches settle */
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
                 "var: owner store holds the newest (200)"); }
      for (t=0;t<2000;t++){ pf_pump(P,C,2);
          if (rant_variable_get(ra, &cur) && cur.len>=4 && i_rant_le_r32(cur.data)==200) break; }
      { int have = rant_variable_get(ra, &cur);
        CHECK(have && cur.len>=4 && i_rant_le_r32(cur.data)==200,
                 "var: remote reached the newest value"); }
      pf_pump(P,C,60);   /* nothing stale may follow: the owner skipped the outer publish */
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
      for (t=0;t<2000 && rant_variable_match_count(bo)==0;t++) pf_pump(P,C,2);
      CHECK(rant_variable_match_count(bo)==1, "var: burst remote matched (%d)",
               rant_variable_match_count(bo));
      ev0 = rant_node_evicted_unsent(P);
      rant_variable_on_write(bo, pf_on_var_burst, NULL);
      i_rant_le_w32(b,0); rant_variable_set(bo, rant_bytes(b,4));         /* fires the burst inline */
      CHECK(pf_vburst_ran, "var: burst ran inside on_write");
      for (t=0;t<2000 && pf_vburst_seen!=want;t++) pf_pump(P,C,2);
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
      for (t=0;t<2000 && rant_variable_match_count(lo)==0;t++) pf_pump(P,C,2);
      t0 = i_rant_plat_now_us();
      for (i2=0;i2<200;i2++){ i_rant_le_w32(b,1000u+i2); rant_variable_set(lo, rant_bytes(b,4)); }
      el = i_rant_plat_now_us() - t0;
      CHECK(el < 500000u, "var: 200 writes past a silent reader never wait (%.1f ms)", el/1000.0);
      for (t=0;t<2000;t++){ pf_pump(P,C,2);
          if (rant_variable_get(la, &cur) && cur.len>=4 && i_rant_le_r32(cur.data)==1199u) break; }
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
      for (t=0;t<2000 && pf_burst_done<8;t++) pf_pump(P,C,2);
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
      for (t=0;t<2000 && !(rant_function_match_count(deep_c) && rant_function_match_count(deep_p));t++)
          pf_pump(P,C,2);
      CHECK(rant_function_match_count(deep_c) && rant_function_match_count(deep_p),
               "patterns: deep-ring pair matched both ways (c=%d p=%d)",
               rant_function_match_count(deep_c), rant_function_match_count(deep_p));
      pf_pump(P,C,50);
      { int before = pf_calls; (void)before;
      pf_burst_done=0; pf_burst_sum=0; pf_burst_bad=0; expect_sum=0;
      for (i2=0;i2<24;i2++){
          i_rant_le_w32(req,(uint32_t)(3000+i2)); expect_sum += (uint32_t)(3000+i2+1);
          rant_function_call_async(deep_c, rant_bytes(req,4), pf_on_reply_burst, NULL, NULL);
      }
      for (i2=0;i2<80;i2++) rant_node_poll(C,0);
      rant_node_poll(P,20);
      for (t=0;t<2000 && pf_burst_done<24;t++) pf_pump(P,C,2);
      CHECK(pf_burst_done==24 && pf_burst_bad==0 && pf_burst_sum==expect_sum,
               "patterns: keep_last=32 carries a 24-reply pass (handled=%d done=%d bad=%d laststatus=%d sum=%u want=%u)",
               pf_calls - before, pf_burst_done, pf_burst_bad, (int)pf_burst_last_bad, pf_burst_sum, expect_sum); } }

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

/* The task smoke phase (19e7): defer, RUNNING first progress, cancel both ways, no_cancel
 * refused locally, an inline reply and the bare return APP_ERROR. The matrix is 19e8. */
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

    for (t=0;t<2000 && rant_function_match_count(cxfer)==0;t++) pf_pump(P,C,2);
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
      for (t=0;t<2000 && !tk_token;t++) pf_pump(P,C,2);
      CHECK(tk_token!=0, "task: handler deferred");
      for (t=0;t<800 && tk_prog_n<1;t++) pf_pump(P,C,2);
      CHECK(tk_prog_n>=1 && tk_prog_first_empty,
               "task: RUNNING fired on_progress first, empty (n=%d empty=%d)",
               tk_prog_n, tk_prog_first_empty);
      for (i2=1;i2<=3;i2++){
          int pr;   /* each update lands before the next is sent, pinning arrival order */
          i_rant_le_w32(pb, (uint32_t)(100*i2));
          pr = rant_function_progress(pxfer, tk_token, rant_bytes(pb,4));
          CHECK(pr==RANT_OK, "task: progress %d sent (%d)", i2, pr);
          for (t=0;t<400 && tk_prog_n < 1+i2;t++) pf_pump(P,C,2);
      }
      i_rant_le_w32(pb, 777);
      rc = rant_function_complete(pxfer, tk_token, RANT_CALL_OK, NULL, rant_bytes(pb,4));
      CHECK(rc==RANT_OK, "task: complete OK sent (%d)", rc);
      for (t=0;t<800 && !tk_reply_done;t++) pf_pump(P,C,2);
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
      for (t=0;t<2000 && !tk_token;t++) pf_pump(P,C,2);
      CHECK(tk_token!=0, "task: long deferred");
      { int sr = rant_function_cancelled(plong, tk_token);
        CHECK(sr==0, "task: not yet cancelled (%d)", sr); }
      rc = rant_function_cancel(clong, id);
      CHECK(rc==RANT_OK, "task: cancel sent (%d)", rc);
      for (t=0;t<800 && !tk_cancel_fired;t++) pf_pump(P,C,2);
      CHECK(tk_cancel_fired && tk_cancel_token==tk_token,
               "task: on_cancel fired with the token (fired=%d)", tk_cancel_fired);
      { int sr = rant_function_cancelled(plong, tk_token);
        CHECK(sr==1, "task: cancelled flag set (%d)", sr); }
      rc = rant_function_complete(plong, tk_token, RANT_CALL_CANCELLED, "stopped", rant_bytes(NULL,0));
      CHECK(rc==RANT_OK, "task: complete CANCELLED sent (%d)", rc);
      for (t=0;t<800 && !tk_reply_done;t++) pf_pump(P,C,2);
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
      for (t=0;t<2000 && !tk_token;t++) pf_pump(P,C,2);
      CHECK(rc==RANT_OK && tk_token!=0, "task: no_cancel call running (rc=%d)", rc);
      cr = rant_function_cancel(cnc, id);
      CHECK(cr==RANT_ERR_ROLE, "task: cancel refused locally with RANT_ERR_ROLE (%d)", cr);
      pf_pump(P,C,60);
      { int sr = rant_function_cancelled(pnc, tk_token);
        CHECK(!tk_cancel_fired && sr==0,
                 "task: nothing reached the definition (fired=%d flag=%d)", tk_cancel_fired, sr); }
      cr = rant_function_complete(pnc, tk_token, RANT_CALL_OK, NULL, rant_bytes(NULL,0));
      for (t=0;t<800 && !tk_reply_done;t++) pf_pump(P,C,2);
      CHECK(cr==RANT_OK && tk_reply_done && tk_reply_status==RANT_CALL_OK,
               "task: no_cancel task completes OK (st=%d)", tk_reply_status); }

    /* a task handler answering immediately via rant_request_reply works like a function */
    { uint8_t req[4]; int rc; i_rant_le_w32(req, 41);
      tk_reply_done=0;
      rc = rant_function_call_async(cinl, rant_bytes(req,4), tk_on_reply, NULL, NULL);
      CHECK(rc==RANT_OK, "task: inline call accepted (%d)", rc);
      for (t=0;t<2000 && !tk_reply_done;t++) pf_pump(P,C,2);
      CHECK(tk_reply_done && tk_reply_status==RANT_CALL_OK && tk_reply_val==42,
               "task: inline reply -> 42 OK (st=%d val=%u)", tk_reply_status, tk_reply_val); }

    /* bare return: a task's return is not its answer */
    { int rc;
      tk_reply_done=0;
      rc = rant_function_call_async(cbare, rant_bytes(NULL,0), tk_on_reply, NULL, NULL);
      CHECK(rc==RANT_OK, "task: bare call accepted (%d)", rc);
      for (t=0;t<2000 && !tk_reply_done;t++) pf_pump(P,C,2);
      CHECK(tk_reply_done && tk_reply_status==RANT_CALL_APP_ERROR,
               "task: bare return synthesizes APP_ERROR (st=%d)", tk_reply_status);
      CHECK(strcmp(tk_reply_msg, "handler returned no result")==0,
               "task: synthesized message (\"%s\")", tk_reply_msg); }

    rant_node_close(P,0); rant_node_close(C,0);
    rant_allocator_reset(&pa); rant_allocator_reset(&ca);
}

/* The task matrix (19e8): concurrent caller demux, overlapping calls, provider selection,
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

    /* 1. CONCURRENT CALLER DEMUX: C1 and C2 both fire their first call, so both call ids are
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
      /* 8. the raw tap observed both callers' progress with distinct caller_lo values. X's
         normal remote handle has no outstanding call, so nothing can fire progress on it. */
      txm_pump(ns,5,20);
      { uint32_t lo1 = i_rant_le_r32(i_rant_node_uuid(C1)), lo2 = i_rant_le_r32(i_rant_node_uuid(C2));
        int has1=0, has2=0, i2;
        for (i2=0;i2<txm_tap_nlo;i2++){ if (txm_tap_lo[i2]==lo1) has1=1; if (txm_tap_lo[i2]==lo2) has2=1; }
        CHECK(txm_tap_n>=4 && txm_tap_bad_hdr==0 && txm_tap_nlo==2 && has1 && has2 && lo1!=lo2,
                 "taskx: raw tap saw both callers' caller_lo (n=%d bad=%d nlo=%d)",
                 txm_tap_n, txm_tap_bad_hdr, txm_tap_nlo); } }

    /* 2. two OVERLAPPING calls from ONE caller: two live tokens on P, interleaved
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

    /* 3. PROVIDER SELECTION: an explicit RantCallOpts.provider reaches exactly the chosen
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

    /* 7. THIRD-PARTY CANCEL, the explorer path: X crafts the raw CANCEL op naming C1's live
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

    /* 5. RETIRE MID-RUN through both delivery paths: the wire CANCELLED, and the caller side
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
      for (t=0;t<400 && !r.done;t++) rant_node_poll(C1, 2);     /* caller first: the wire path */
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

    /* 6. CLOSE MID-RUN: the provider closes with the call RUNNING. No BYE is sent, so the
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

    /* 9. FILE-TRANSFER SHAPE: 50 reliable 4 byte chunks as progress, one per app loop pass,
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

    /* 10. TASK CHURN: retire remote and definition, re-create the SAME name, and a full
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

    /* 11. REFLECTION: from C1, one RANT_ENTITY_TASK per task name on P, attrs decoded, the
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

    /* 4. PEER LOSS MID-RUN on a short timeout pair: RUNNING dropped the deadline, then the
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

/* The duplicate authority diagnostic (19e2): one provider per function and one owner per
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
      /* wait until B holds A's interest: the create-time sweep's precondition */
      { uint32_t want = (uint32_t)i_rant_topic_id("dupvar"); int seen = 0;
        for (t=0;t<2000 && !seen;t++){
            const i_RantDiscoveryPeerView *ps; uint16_t pc;
            pf_pump(A,B,2);
            ps = test_peers(B,&pc);
            if (ps && pc){ i_RantInterestIter it; i_RantTopicEntry e; memset(&it,0,sizeof it);
                while (i_rant_node_peer_interest_next(&ps[0], &it, &e))
                    if (e.hash==want){ seen=1; break; } }
        }
        CHECK(seen, "dup: B holds A's interest"); }
      CHECK(a_dups==0 && b_dups==0, "dup: quiet before the rival (a=%d b=%d)", a_dups, b_dups);

      { RantVariable *vb = rant_node_create_variable_definition(B, "dupvar", NULL, NULL);
        CHECK(vb!=NULL, "dup: rival owner creates (diagnostic, not a refusal)");
        CHECK(b_dups==1, "dup: rival owner detected at create (%d)", b_dups); }
      { RantFunction *fb = rant_node_create_function_definition(B, "dupfn", NULL, NULL, pf_empty_handler, NULL, NULL);
        CHECK(fb!=NULL, "dup: rival provider creates");
        CHECK(b_dups==2, "dup: rival provider detected at create (%d)", b_dups); }
      for (t=0;t<2000 && a_dups<2;t++) pf_pump(A,B,2);
      CHECK(a_dups==2, "dup: first authority sees the rival's announce (%d)", a_dups);

      /* dedup + negative: repeated announces re-report nothing, and an accessor pairing
         with an owner is the LEGAL shape (sub side, no authority claim), never flagged */
      { RantVariable *acc    = rant_node_create_remote_variable(B, "solo", NULL, NULL);
        RantVariable *solo = rant_node_create_variable_definition(A, "solo", NULL, NULL);
        CHECK(acc && solo, "dup: solo owner + accessor create");
        for (t=0;t<150;t++) pf_pump(A,B,2);
        CHECK(a_dups==2 && b_dups==2,
                 "dup: once per (entity, peer); accessor never fires (a=%d b=%d)", a_dups, b_dups); } }

    rant_node_close(A,0); rant_node_close(B,0);
    rant_allocator_reset(&aa); rant_allocator_reset(&ba);
}

/* The pattern retire lifecycle (19e3): a second same name handle is shadowed while the
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
      for (t=0;t<2000 && !rant_variable_get(acc,&gv);t++) pf_pump(A,B,2);
      CHECK(rant_variable_get(acc,&gv) && gv.len==4 && i_rant_le_r32(gv.data)==20,
               "retire: first accessor got the value (20)");
      rr = rant_variable_retire(acc);
      CHECK(rr==RANT_OK, "retire: accessor retired (%d)", rr);
      acc2 = rant_node_create_remote_variable(B, "dial", NULL, NULL);
      CHECK(acc2 != NULL, "retire: successor accessor created");
      for (t=0;t<2000 && !rant_variable_get(acc2,&gv);t++) pf_pump(A,B,2);
      CHECK(rant_variable_get(acc2,&gv) && gv.len==4 && i_rant_le_r32(gv.data)==20,
               "retire: successor RECEIVES where a live twin would be shadowed (20)");
      i_rant_le_w32(b, 33);
      { int sr = rant_variable_set(acc2, rant_bytes(b,4));
        CHECK(sr==RANT_OK, "retire: successor set accepted (%d)", sr); }
      for (t=0;t<2000;t++){ pf_pump(A,B,2);
          if (rant_variable_get(def,&gv)&&gv.len==4&&i_rant_le_r32(gv.data)==33) break; }
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
      for (t=0;t<2000;t++){ pf_pump(A,B,2);
          if (rant_variable_get(acc2,&gv)&&gv.len==4&&i_rant_le_r32(gv.data)==77) break; }
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
      for (t=0;t<2000 && rant_function_match_count(rem)==0;t++) pf_pump(A,B,2);
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
      for (t=0;t<2000 && !pf_reply_done;t++) pf_pump(A,B,2);
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

/* Loud refusals (19e9): a refused create says why and keeps its strings, a live same name
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
        for (t=0;t<2000 && !(rant_variable_get(acc,&gv) && rant_variable_get(kacc,&gv));t++) pf_pump(A,B,2);
        CHECK(rant_variable_get(acc,&gv) && rant_variable_get(kacc,&gv), "loud: accessors got the values");
        i_rant_le_w32(b, 99);
        rc = rant_variable_force(acc, rant_bytes(b,4));
        CHECK(rc==RANT_ERR_ROLE, "loud: force against an owner without allow_force refused ROLE (%d)", rc);
        rc = rant_variable_unforce(acc);
        CHECK(rc==RANT_ERR_ROLE, "loud: unforce likewise (%d)", rc);
        for (t=0;t<200;t++) pf_pump(A,B,2);
        CHECK(v1 && !rant_variable_forced(v1), "loud: nothing reached the owner");
        rc = rant_variable_force(kacc, rant_bytes(b,4));
        CHECK(rc==RANT_OK, "loud: force against a forceable owner sends (%d)", rc);
        for (t=0;t<2000 && !(knob && rant_variable_forced(knob));t++) pf_pump(A,B,2);
        CHECK(knob && rant_variable_forced(knob), "loud: the forceable owner is forced");
    }
    {   /* a queued call: an unrelated peer leaving is not its loss, its deadline ends it
           NO_PROVIDER, and the blocking form reports that outcome */
        RantAllocator ca = rant_allocator_heap(0); RantNodeOpts co = ao; RantNode *C;
        RantFunction *nobody; RantResponse rep; int rc;
        C = rant_node_open(&ca, "loud-c", NULL, NULL, &co);
        nobody = rant_node_create_remote_function(B, "loud/nobody", NULL, NULL,
                     &(RantFunctionOpts){ .timeout_us = 1500000u });
        CHECK(C && nobody, "loud: third node and never served remote created");
        for (t=0;t<3000 && loud_active_peers(B) < 2;t++){ pf_pump(A,B,1); if (C) rant_node_poll(C,1); }
        CHECK(loud_active_peers(B) >= 2, "loud: B sees both peers (%d)", loud_active_peers(B));
        loud_reply_n = 0; loud_reply_status = -1;
        rc = nobody ? rant_function_call_async(nobody, rant_bytes(NULL,0), loud_on_reply, NULL, NULL) : -99;
        CHECK(rc==RANT_OK, "loud: call queued with no provider (%d)", rc);
        if (C){ rant_node_close(C,1); C = NULL; }
        /* bounded by the clock, not by passes: a coarse timer makes a 2 ms poll take 15 */
        { uint64_t end = i_rant_plat_now_us() + 400000u; while (i_rant_plat_now_us() < end) pf_pump(A,B,1); }
        CHECK(loud_active_peers(B) == 1 && loud_reply_n == 0,
                 "loud: the unrelated peer left and the queued call still waits (peers=%d n=%d)",
                 loud_active_peers(B), loud_reply_n);
        for (t=0;t<4000 && loud_reply_n == 0;t++) pf_pump(A,B,1);
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

/* The dropped peer reflection gate (19e4): the entity walk must refuse dropped peers
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
          pf_pump(A,B,2);
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

/* Index reuse (19e4b): a retired slot rebound to another channel must drop the old name and
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
        pf_pump(A,B,2);
        ps = test_peers(B, &pc);
        if (!(ps && pc)) continue;
        pid = ps[0].id;
        rr_count(B, pid, "rr/joints", RANT_ENTITY_TOPIC, &ok);
    }
    CHECK(ok, "reuse: observer names the topic");

    CHECK(rant_topic_retire(joints) == RANT_OK, "reuse: topic retired");
    fn = rant_node_create_function_definition(A, "rr/call", NULL, NULL, pf_add_handler, NULL, NULL);
    CHECK(fn != NULL, "reuse: function takes the retired slot");
    for (t=0, ok=0; t<2000 && !ok; t++){ pf_pump(A,B,2); rr_count(B, pid, "rr/call", RANT_ENTITY_FUNCTION, &ok); }
    n = rr_count(B, pid, "rr/joints", RANT_ENTITY_FUNCTION, &i);
    CHECK(ok, "reuse: the function shows whole under its own name");
    CHECK(n == 0, "reuse: nothing keeps the old name (%d)", n);

    /* every rebind appends a name, so this crosses the arena's compaction threshold */
    if (fn) rant_function_retire(fn);
    for (i=0, ok=1; i<48 && ok; i++){
        RantTopic *tp;
        snprintf(name, sizeof name, "rr/churn/a-long-enough-name-to-grow-the-arena/%02d", i);
        tp = rant_node_create_topic(A, name, RANT_PUB_ONLY, NULL, NULL);
        for (t=0, ok=0; tp && t<2000 && !ok; t++){ pf_pump(A,B,2); rr_count(B, pid, name, RANT_ENTITY_TOPIC, &ok); }
        if (tp) rant_topic_retire(tp);
    }
    CHECK(ok && i == 48, "reuse: 48 rebinds each show their own name (stopped at %d)", i);

    rant_node_close(A,0); rant_node_close(B,0);
    rant_allocator_reset(&aa); rant_allocator_reset(&ba);
}

/* The variable set match wait (19e5): a fresh accessor's first write rides the send path's
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

    /* C must hold P's announce nominating vw before the accessor exists, since candidates
       come from the cached blob. Verdicts are not yet fetched. */
    { uint32_t want = (uint32_t)i_rant_topic_id("vw"); int seen = 0;
      for (t=0;t<2000 && !seen;t++){
          const i_RantDiscoveryPeerView *ps; uint16_t pc;
          pf_pump(P,C,2);
          ps = test_peers(C,&pc);
          if (ps && pc){ i_RantInterestIter it; i_RantTopicEntry e; memset(&it,0,sizeof it);
              while (i_rant_node_peer_interest_next(&ps[0], &it, &e))
                  if (e.hash==want){ seen=1; break; } }
      }
      CHECK(seen, "varwait: C holds P's interest"); }

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
      for (t=0;t<2000;t++){ pf_pump(P,C,2);
          if (rant_variable_get(vd,&gv)&&gv.len==4&&i_rant_le_r32(gv.data)==5) break; }
      CHECK(rant_variable_get(vd,&gv)&&gv.len==4&&i_rant_le_r32(gv.data)==5,
               "varwait: the write reached the owner (5)"); }

    /* converged verdicts stay instant: an owner nobody offers fails NO_TOPIC without
       consuming the wait bound. Let the post open gather latch first, about 400 ms. */
    pf_pump(P,C,400);
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

/* The retire and reuse churn soak (19e6): retire and create cycles reuse slots, so
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

/* The match wait checks (19f): a first send racing the announce and detail cycle, a
 * disabled wait dropping loudly, and writer authoritative delivery (spec/testing.md). */
static volatile unsigned long mw_recv, mw_lost, mw_unmatched;
static char mw_last[64];
static void mw_on_message(const RantMsg *m){
    size_t c = m->data.len < sizeof mw_last - 1 ? m->data.len : sizeof mw_last - 1;
    memcpy(mw_last, m->data.data, c); mw_last[c] = 0;
    mw_recv++;
}
static void mw_on_event(const RantEvent *ev){
    if (ev->kind == RANT_MSG_LOST) mw_lost++;
    if (ev->kind == RANT_ERROR && ev->error == RANT_E_UNMATCHED_SEND) mw_unmatched++;
}

static void matchwait_checks(void){
    RantAddr seed; int t;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;

    /* (a) WRITER-AUTHORITATIVE HEAL: B never receives DETAIL_RESPs, so it cannot verify A
       while A matches B. Unblocking lets B verify and repair must deliver the original sample. */
    { /* per-process fixed port, like the domain base: concurrent selftests must not
         collide on the bind (the phase needs it fixed only to intercept by port) */
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
          for (t=0;t<3000 && rant_topic_match_count(at)==0;t++){ rant_node_poll(A,2); rant_node_poll(B,2); }
          CHECK(rant_topic_match_count(at)==1,
                   "matchwait: writer side matched while B's responses are blocked (%d)",
                   rant_topic_match_count(at));
          CHECK(rant_topic_pending_count(bt) > 0,
                   "matchwait: reader side still PENDING (%d)", rant_topic_pending_count(bt));
          r = rant_topic_send(at, rant_bytes("authoritative", 13), NULL);
          CHECK(r==RANT_OK, "matchwait: matched send commits (%d)", r);
          for (t=0;t<150;t++){ rant_node_poll(A,2); rant_node_poll(B,2); }
          CHECK(mw_recv==0, "matchwait: unverified reader drops the DATA (recv=%lu)", mw_recv);
          wire_tap_write(&wire_tap.drop_detail_resp_port, 0);      /* B's next re-ask verifies A */
          for (t=0;t<3000 && mw_recv==0;t++){ rant_node_poll(A,2); rant_node_poll(B,2); }
          CHECK(mw_recv==1 && strcmp(mw_last,"authoritative")==0,
                   "matchwait: repair delivers the pre-verify sample (recv=%lu '%s')", mw_recv, mw_last);
          CHECK(mw_lost==0, "matchwait: healed with no MSG_LOST (%lu)", mw_lost);
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
          for (t=0;t<1500 && mw_recv==0;t++) rant_node_poll(A,2);
          CHECK(mw_recv==1 && strcmp(mw_last,"first")==0,
                   "matchwait: subscriber got the racing first send (recv=%lu '%s')", mw_recv, mw_last);
          CHECK(mw_unmatched==0, "matchwait: no timeout diagnostic (%lu)", mw_unmatched);
          /* a topic NOBODY consumes must not stall once the gather has settled: the
             converged state is memoized, so this send early-outs as it always did */
          for (t=0;t<300;t++) rant_node_poll(A,2);     /* let the post-open gather settle */
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
          CHECK(r==RANT_OK && mw_unmatched==1,
                   "matchwait: disabled wait drops LOUDLY (r=%d events=%lu)", r, mw_unmatched);
          for (t=0;t<2000 && rant_topic_match_count(at)==0;t++){ rant_node_poll(A,2); rant_node_poll(B,2); }
          CHECK(rant_topic_match_count(at)==1, "matchwait: the match still forms after (%d)",
                   rant_topic_match_count(at));
          for (t=0;t<150;t++){ rant_node_poll(A,2); rant_node_poll(B,2); }
          CHECK(mw_recv==0, "matchwait: the dropped send never resurrects (recv=%lu)", mw_recv);
      }
      if (A) rant_node_close(A,1);
      if (B) rant_node_close(B,1);
      rant_allocator_reset(&aa); rant_allocator_reset(&ba);
    }
}

/* The relay phase: discovery for a node that cannot multicast. U opens unicast_only with
 * one seed, R's data port, and every U to B fact arrives through R's proxied announces. */
static unsigned long rly_recv = 0;
static char rly_last[64];
static void rly_on_message(const RantMsg *msg){
    size_t k = msg->data.len < sizeof rly_last - 1 ? msg->data.len : sizeof rly_last - 1;
    memcpy(rly_last, msg->data.data, k); rly_last[k] = '\0';
    rly_recv++;
}
/* is `name` an ACTIVE peer of n? (the peer view is discovery's own, valid until the next poll) */
static int rly_sees(RantNode *n, const char *name){
    uint16_t c = 0, i; size_t nl = strlen(name);
    const i_RantDiscoveryPeerView *p = test_peers(n, &c);
    for (i=0;i<c;i++)
        if (p[i].liveness == RANT_PEER_ACTIVE && p[i].name.len == nl &&
            memcmp(p[i].name.data, name, nl) == 0) return 1;
    return 0;
}
static void rly_pump(RantNode *a, RantNode *b, RantNode *c, int iters){
    int t;
    for (t=0;t<iters;t++){
        if (a) rant_node_poll(a, 2);
        if (b) rant_node_poll(b, 2);
        if (c) rant_node_poll(c, 2);
    }
}
static void relay_checks(void){
    /* R's data port is fixed so U can seed exactly one address (per-process, like the
       matchwait port: concurrent selftests must not collide on the bind) */
    const uint16_t R_PORT = (uint16_t)(20000u + test_domain_base % 15000u);
    RantAddr seed; int t;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4; seed.port=R_PORT;

    /* (a) INTRODUCTION + DATA, then the relay dies */
    { RantAllocator ua = rant_allocator_heap(0);
      RantAllocator ra = rant_allocator_heap(0);
      RantAllocator ba = rant_allocator_heap(0);
      RantNodeOpts uo, ro, bo; RantNode *U=NULL, *R=NULL, *B=NULL;
      RantTopic *ut=NULL, *bt=NULL;
      memset(&ro,0,sizeof ro); ro.domain=TEST_DOMAIN+27; ro.disable_shm=1; ro.discovery.max_peers=4;
      ro.discovery.announce_interval_us=200000;
      ro.net.multicast_interface="127.0.0.1";
      ro.net.data_port=R_PORT;                 /* the one address anybody ever states */
      bo=ro; bo.net.data_port=0;
      uo=ro; uo.net.data_port=0;
      uo.net.unicast_only=1;                    /* no group join, no group sends, RELAY_ME */
      uo.net.seed_peers=&seed; uo.net.n_seed_peers=1;
      rly_recv=0; rly_last[0]='\0';

      R = rant_node_open(&ra, "rly-relay", NULL, NULL, &ro);
      CHECK(R!=NULL, "relay: relay node opens (is port %d free?)", (int)R_PORT);
      B = rant_node_open(&ba, "rly-far", rly_on_message, NULL, &bo);
      U = rant_node_open(&ua, "rly-uni", NULL, NULL, &uo);
      CHECK(U!=NULL, "relay: unicast-only node opens with no multicast membership");
      CHECK(B!=NULL, "relay: far node opens");
      if (U && R && B){
          bt = rant_node_create_topic(B, "rly/t", RANT_SUB_ONLY, NULL,
                   &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE } });
          ut = rant_node_create_topic(U, "rly/t", RANT_PUB_ONLY, NULL,
                   &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE, .keep_last=8 } });
          CHECK(ut && bt, "relay: topics up");

          for (t=0;t<3000 && !(rly_sees(B,"rly-uni") && rly_sees(U,"rly-far"));t++)
              rly_pump(U,R,B,1);
          CHECK(rly_sees(B,"rly-uni"),
                   "relay: far node discovers the unicast-only node it cannot hear");
          CHECK(rly_sees(U,"rly-far"),
                   "relay: and the unicast-only node discovers it back");

          if (ut && bt){
              int r;
              for (t=0;t<2000 && rant_topic_match_count(ut)==0;t++) rly_pump(U,R,B,1);
              CHECK(rant_topic_match_count(ut)==1, "relay: topic matches across the introduction (%d)",
                       rant_topic_match_count(ut));
              r = rant_topic_send(ut, rant_bytes("over-unicast", 12), NULL);
              for (t=0;t<2000 && rly_recv==0;t++) rly_pump(U,R,B,1);
              CHECK(r==RANT_OK && rly_recv==1 && strcmp(rly_last,"over-unicast")==0,
                       "relay: data flows unicast end to end (r=%d recv=%lu '%s')", r, rly_recv, rly_last);
          }

          /* the relay only INTRODUCED: it is not in the data path and not needed to keep
             the pair alive, since each now unicasts its announces to the other directly */
          rant_node_close(R,1); R=NULL;
          rly_pump(U,NULL,B,400);                       /* ~0.8s: several announce intervals */
          CHECK(rly_sees(B,"rly-uni") && rly_sees(U,"rly-far"),
                   "relay: the pair outlives the relay (B sees U=%d, U sees B=%d)",
                   rly_sees(B,"rly-uni"), rly_sees(U,"rly-far"));
          CHECK(!rly_sees(B,"rly-relay") && !rly_sees(U,"rly-relay"),
                   "relay: ...and the departed relay itself is gone from both");
          if (ut && bt){
              int r = rant_topic_send(ut, rant_bytes("after-relay-died", 16), NULL);
              for (t=0;t<2000 && rly_recv<2;t++) rly_pump(U,NULL,B,1);
              CHECK(r==RANT_OK && rly_recv==2 && strcmp(rly_last,"after-relay-died")==0,
                       "relay: data still flows with the relay dead (r=%d recv=%lu '%s')",
                       r, rly_recv, rly_last);
          }
      }
      if (R) rant_node_close(R,1);
      if (U) rant_node_close(U,1);
      if (B) rant_node_close(B,1);
      rant_allocator_reset(&ua); rant_allocator_reset(&ra); rant_allocator_reset(&ba);
    }

    /* (b) CONTROL: the same unicast only node with nobody to relay it is unreachable in
       both directions, so (a) measured the relay, not the loopback */
    { RantAllocator ua = rant_allocator_heap(0);
      RantAllocator ba = rant_allocator_heap(0);
      RantNodeOpts uo, bo; RantNode *U=NULL, *B=NULL;
      memset(&bo,0,sizeof bo); bo.domain=TEST_DOMAIN+28; bo.disable_shm=1; bo.discovery.max_peers=4;
      bo.discovery.announce_interval_us=200000;
      bo.net.multicast_interface="127.0.0.1";
      uo=bo; uo.net.unicast_only=1;              /* no seeds: it can announce to nobody at all */
      U = rant_node_open(&ua, "rly-alone", NULL, NULL, &uo);
      B = rant_node_open(&ba, "rly-far2", NULL, NULL, &bo);
      CHECK(U && B, "relay: control pair up");
      if (U && B){
          rly_pump(U,B,NULL,400);                 /* ~0.8s: several announce intervals each way */
          CHECK(!rly_sees(B,"rly-alone") && !rly_sees(U,"rly-far2"),
                   "relay: unrelayed unicast-only node stays invisible (B sees U=%d, U sees B=%d)",
                   rly_sees(B,"rly-alone"), rly_sees(U,"rly-far2"));
      }
      if (U) rant_node_close(U,1);
      if (B) rant_node_close(B,1);
      rant_allocator_reset(&ua); rant_allocator_reset(&ba);
    }
}

/* The NAT phase: a unicast only node behind an outbound only NAT. U advertises a black
 * holed port, so only the observed sources can carry the mesh (spec/testing.md). */
static void nat_checks(void){
    const uint16_t R2_PORT = (uint16_t)(21000u + test_domain_base % 15000u);
    uint16_t DEAD_PORT = 0;
    RantAddr seed; int t;
    i_RantSock bh;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4; seed.port=R2_PORT;

    /* the black hole owns U's advertised port and never reads it, so every datagram sent to
       U's locator disappears like a NAT drop. OS assigned, so it never collides. */
    i_rant_plat_startup();            /* raw socket outside any node: needs the net stack up */
    bh = i_rant_plat_udp_open();
    if (bh != RANT_SOCK_BAD){
        if (i_rant_plat_bind(bh, 0, 0, 0)) DEAD_PORT = i_rant_plat_local_port(bh);
        if (!DEAD_PORT){ i_rant_plat_close(bh); bh = RANT_SOCK_BAD; }
    }
    CHECK(DEAD_PORT != 0, "nat: black-hole port binds");

    { RantAllocator ua = rant_allocator_heap(0);
      RantAllocator ra = rant_allocator_heap(0);
      RantAllocator ba = rant_allocator_heap(0);
      RantNodeOpts uo, ro, bo; RantNode *U=NULL, *R=NULL, *B=NULL;
      RantTopic *ut=NULL, *bt=NULL;
      memset(&ro,0,sizeof ro); ro.domain=TEST_DOMAIN+29; ro.disable_shm=1; ro.discovery.max_peers=4;
      ro.discovery.announce_interval_us=200000;
      ro.net.multicast_interface="127.0.0.1";
      ro.net.data_port=R2_PORT;                /* the seeded address */
      bo=ro; bo.net.data_port=0;
      uo=ro; uo.net.data_port=0;
      uo.net.unicast_only=1;
      uo.net.advertise_port=DEAD_PORT;         /* the locator lie: what a NAT'd node's blob
                                                  port amounts to on the outside */
      uo.net.seed_peers=&seed; uo.net.n_seed_peers=1;
      rly_recv=0; rly_last[0]='\0';

      R = rant_node_open(&ra, "nat-relay", NULL, NULL, &ro);
      B = rant_node_open(&ba, "nat-far", rly_on_message, NULL, &bo);
      U = rant_node_open(&ua, "nat-uni", NULL, NULL, &uo);
      CHECK(R!=NULL, "nat: relay node opens (is port %d free?)", (int)R2_PORT);
      CHECK(U!=NULL && B!=NULL, "nat: NAT'd + far nodes open");
      if (U && R && B){
          bt = rant_node_create_topic(B, "nat/t", RANT_SUB_ONLY, NULL,
                   &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE } });
          ut = rant_node_create_topic(U, "nat/t", RANT_PUB_ONLY, NULL,
                   &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE, .keep_last=8 } });
          CHECK(ut && bt, "nat: topics up");

          /* B cannot dial U (locator black-holed) and U cannot hear multicast: the pair
             can only form through R's introduction followed by U speaking first */
          for (t=0;t<3000 && !(rly_sees(B,"nat-uni") && rly_sees(U,"nat-far"));t++)
              rly_pump(U,R,B,1);
          CHECK(rly_sees(U,"nat-far"),
                   "nat: the NAT'd node is INTRODUCED to a peer it cannot hear");
          CHECK(rly_sees(B,"nat-uni"),
                   "nat: and reaches it first, so the far node binds its observed source");

          if (ut && bt){
              int r;
              for (t=0;t<2000 && rant_topic_match_count(ut)==0;t++) rly_pump(U,R,B,1);
              CHECK(rant_topic_match_count(ut)==1, "nat: topic matches through the mappings (%d)",
                       rant_topic_match_count(ut));
              r = rant_topic_send(ut, rant_bytes("through-the-nat", 15), NULL);
              for (t=0;t<2000 && rly_recv==0;t++) rly_pump(U,R,B,1);
              CHECK(r==RANT_OK && rly_recv==1 && strcmp(rly_last,"through-the-nat")==0,
                       "nat: reliable data flows despite a dead locator (r=%d recv=%lu '%s')",
                       r, rly_recv, rly_last);
          }

          /* the introducer is bootstrap only: the pair sustains itself point to point */
          rant_node_close(R,1); R=NULL;
          rly_pump(U,NULL,B,400);
          CHECK(rly_sees(B,"nat-uni") && rly_sees(U,"nat-far"),
                   "nat: the pair outlives the introducer (B sees U=%d, U sees B=%d)",
                   rly_sees(B,"nat-uni"), rly_sees(U,"nat-far"));
          if (ut && bt){
              int r = rant_topic_send(ut, rant_bytes("after-introducer-died", 21), NULL);
              for (t=0;t<2000 && rly_recv<2;t++) rly_pump(U,NULL,B,1);
              CHECK(r==RANT_OK && rly_recv==2 && strcmp(rly_last,"after-introducer-died")==0,
                       "nat: data still flows with the introducer dead (r=%d recv=%lu '%s')",
                       r, rly_recv, rly_last);
          }
      }
      if (R) rant_node_close(R,1);
      if (U) rant_node_close(U,1);
      if (B) rant_node_close(B,1);
      rant_allocator_reset(&ua); rant_allocator_reset(&ra); rant_allocator_reset(&ba);
    }
    if (bh != RANT_SOCK_BAD) i_rant_plat_close(bh);
    i_rant_plat_cleanup();
}

/* The self ip phase: stating our own locator. On one host the advertised port carries
 * the proof, it is deliberately not the port A bound. */
static int sip_addr_of(RantNode *n, const char *name, RantAddr *out){
    uint16_t c = 0, i; size_t nl = strlen(name);
    const i_RantDiscoveryPeerView *p = test_peers(n, &c);
    for (i=0;i<c;i++)
        if (p[i].liveness == RANT_PEER_ACTIVE && p[i].name.len == nl &&
            memcmp(p[i].name.data, name, nl) == 0){ *out = p[i].addr; return 1; }
    return 0;
}
static void selfip_checks(void){
    const uint16_t ADV_PORT = (uint16_t)(45000u + test_domain_base % 15000u);
    int t;
    /* (a) peers record what we STATE, not where our packets came from */
    { RantAllocator aa = rant_allocator_heap(0);
      RantAllocator ba = rant_allocator_heap(0);
      RantNodeOpts ao, bo; RantNode *A=NULL, *B=NULL; RantAddr got;
      memset(&bo,0,sizeof bo); bo.domain=TEST_DOMAIN+29; bo.disable_shm=1; bo.discovery.max_peers=4;
      bo.discovery.announce_interval_us=200000; bo.net.multicast_interface="127.0.0.1";
      ao=bo; ao.net.self_ip="127.0.0.1"; ao.net.advertise_port=ADV_PORT;
      A = rant_node_open(&aa, "sip-a", NULL, NULL, &ao);
      B = rant_node_open(&ba, "sip-b", NULL, NULL, &bo);
      CHECK(A && B, "selfip: pair up");
      if (A && B){
          memset(&got,0,sizeof got);
          for (t=0;t<3000 && !sip_addr_of(B,"sip-a",&got);t++){ rant_node_poll(A,2); rant_node_poll(B,2); }
          CHECK(got.ip_len==4 && got.ip[0]==127 && got.ip[3]==1 && got.port==ADV_PORT,
                   "selfip: peers record the stated locator (%u.%u.%u.%u:%u, want 127.0.0.1:%u)",
                   got.ip[0], got.ip[1], got.ip[2], got.ip[3],
                   (unsigned)got.port, (unsigned)ADV_PORT);
      }
      if (A) rant_node_close(A,1);
      if (B) rant_node_close(B,1);
      rant_allocator_reset(&aa); rant_allocator_reset(&ba);
    }
    /* (b) an unparseable locator is a config error, never a silent fallback to the default:
       a node advertising an unreachable address would look healthy and receive nothing */
    { RantAllocator aa = rant_allocator_heap(0);
      RantNodeOpts o; RantNode *n; RantEvent err; char line[160];
      memset(&o,0,sizeof o); o.domain=TEST_DOMAIN+29; o.net.multicast_interface="127.0.0.1";
      o.net.self_ip="not-an-ip";
      n = rant_node_open(&aa, "sip-bad", NULL, NULL, &o);
      CHECK(n==NULL, "selfip: unparseable self ip refuses the open");
      err = rant_last_error(NULL);
      CHECK(err.kind==RANT_ERROR && err.error==RANT_E_BAD_ADDRESS,
               "selfip: ...and says why (%s)", rant_event_str(&err, line, sizeof line));
      if (n) rant_node_close(n,1);
      rant_allocator_reset(&aa);
    }
}

static void launchname_checks(void){
    RantAllocator a = rant_allocator_heap(0);
    RantNodeOpts o; RantNode *n;
    memset(&o,0,sizeof o); o.domain=TEST_DOMAIN+38; o.net.multicast_interface="127.0.0.1";
    test_setenv("RANT_NODE_NAME", "launched");
    n = rant_node_open(&a, "from-code", NULL, NULL, &o);
    CHECK(n && strcmp(n->name, "launched") == 0, "launchname: RANT_NODE_NAME names the node (%s)",
             n ? n->name : "no node");
    if (n) rant_node_close(n, 0);
    rant_allocator_reset(&a);
    test_setenv("RANT_NODE_NAME", "");
    n = rant_node_open(&a, "from-code", NULL, NULL, &o);
    CHECK(n && strcmp(n->name, "from-code") == 0, "launchname: unset, the code's name stands (%s)",
             n ? n->name : "no node");
    if (n) rant_node_close(n, 0);
    rant_allocator_reset(&a);
}

/* The launch domain phase: RANT_DOMAIN fills a domain the code left at 0, never one it set,
 * and a value that is not a u16 refuses the open. */
static void launchdomain_checks(void){
    static const char *bad[] = { "70000", "12x", "-1", "0000000000000000000000000000000000001" };
    RantAllocator a = rant_allocator_heap(0);
    RantNodeOpts o; RantNode *n; char env[16]; size_t i;
    memset(&o,0,sizeof o); o.net.multicast_interface="127.0.0.1";
    snprintf(env, sizeof env, "%u", (unsigned)(TEST_DOMAIN+39));
    test_setenv("RANT_DOMAIN", env);
    n = rant_node_open(&a, "ld", NULL, NULL, &o);
    CHECK(n && n->domain == TEST_DOMAIN+39, "launchdomain: RANT_DOMAIN fills the default domain (%u)",
             n ? (unsigned)n->domain : 0u);
    if (n) rant_node_close(n, 0);
    rant_allocator_reset(&a);
    o.domain = TEST_DOMAIN+40;
    n = rant_node_open(&a, "ld", NULL, NULL, &o);
    CHECK(n && n->domain == TEST_DOMAIN+40, "launchdomain: a domain set in code stands (%u)",
             n ? (unsigned)n->domain : 0u);
    if (n) rant_node_close(n, 0);
    rant_allocator_reset(&a);
    o.domain = 0;
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++){
        RantEvent err;
        test_setenv("RANT_DOMAIN", bad[i]);
        n = rant_node_open(&a, "ld", NULL, NULL, &o);
        err = rant_last_error(NULL);
        CHECK(n == NULL && err.error == RANT_E_BAD_DOMAIN, "launchdomain: '%s' refuses the open", bad[i]);
        if (n) rant_node_close(n, 0);
        rant_allocator_reset(&a);
    }
    test_setenv("RANT_DOMAIN", "");
}

static int st_name_is(RantTopic *t, const char *want){
    RantString s = i_rant_topic_name(t);
    return t && s.len == strlen(want) && memcmp(s.data, want, s.len) == 0;
}

/* The name prefix phase: RANT_PREFIX stacks outside opts.prefix on every create, a leading
 * '/' skips both, a bad prefix refuses the open and RANT_NODE_NAME_PREFIX names the node. */
static void prefix_checks(void){
    static const char *bad_code[] = { "/x", "x/", "a@b", "012345678901234567890123456789012345678901234567890123456789012" };
    RantAllocator a = rant_allocator_heap(0);
    RantNodeOpts o; RantNode *n; RantTopic *t; RantEntityInfo info; RantEvent err; size_t i;
    memset(&o,0,sizeof o); o.domain=TEST_DOMAIN+41; o.net.multicast_interface="127.0.0.1";
    o.prefix = "robot1";
    test_setenv("RANT_PREFIX", "cellA");
    test_setenv("RANT_NODE_NAME_PREFIX", "lab");
    n = rant_node_open(&a, "arm", NULL, NULL, &o);
    CHECK(n != NULL, "prefix: open with RANT_PREFIX and opts.prefix");
    if (n){
        CHECK(strcmp(n->name, "lab/arm") == 0, "prefix: RANT_NODE_NAME_PREFIX names the node (%s)", n->name);
        t = rant_node_create_topic(n, "pose", RANT_PUB_ONLY, NULL, NULL);
        CHECK(st_name_is(t, "cellA/robot1/pose"), "prefix: a topic goes under env then code");
        t = rant_node_create_topic(n, "/clock", RANT_SUB_ONLY, NULL, NULL);
        CHECK(st_name_is(t, "clock"), "prefix: a leading '/' skips the prefix");
        CHECK(rant_node_create_function_definition(n, "add", NULL, NULL, NULL, NULL, NULL) != NULL
                 && rant_node_mesh_find(n, RANT_ENTITY_FUNCTION, "cellA/robot1/add", &info) == 1,
                 "prefix: a function goes under the prefix");
        CHECK(rant_node_create_variable_definition(n, "speed", NULL, NULL) != NULL
                 && rant_node_mesh_find(n, RANT_ENTITY_VARIABLE, "cellA/robot1/speed", &info) == 1,
                 "prefix: a variable goes under the prefix");
        CHECK(rant_node_create_topic(n, "a@b", RANT_PUB_ONLY, NULL, NULL) == NULL
                 && rant_last_error(n).error == RANT_E_BAD_NAME, "prefix: '@' in a name is refused");
        /* 13 prefix bytes leave 51, and a function needs 4 more for its suffix */
        CHECK(rant_node_create_remote_function(n, "012345678901234567890123456789012345678901234567",
                                                  NULL, NULL, NULL) == NULL
                 && rant_last_error(n).error == RANT_E_BAD_NAME, "prefix: a name too long under the prefix is refused");
        rant_node_close(n, 0);
    }
    rant_allocator_reset(&a);
    test_setenv("RANT_PREFIX", ""); test_setenv("RANT_NODE_NAME_PREFIX", "");
    for (i = 0; i < sizeof bad_code / sizeof bad_code[0]; i++){
        o.prefix = bad_code[i];
        n = rant_node_open(&a, "arm", NULL, NULL, &o);
        err = rant_last_error(NULL);
        CHECK(n == NULL && err.error == RANT_E_BAD_PREFIX, "prefix: '%s' refuses the open", bad_code[i]);
        if (n) rant_node_close(n, 0);
        rant_allocator_reset(&a);
    }
    o.prefix = NULL;
    test_setenv("RANT_PREFIX", "/cellA");
    n = rant_node_open(&a, "arm", NULL, NULL, &o);
    err = rant_last_error(NULL);
    CHECK(n == NULL && err.error == RANT_E_BAD_PREFIX, "prefix: a bad RANT_PREFIX refuses the open");
    if (n) rant_node_close(n, 0);
    rant_allocator_reset(&a);
    test_setenv("RANT_PREFIX", "");
}

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
    for (t=0;t<800 && ts_msgs[TS_CH_PLAIN]==0;t++) test_pump(A,B,2);
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
        for (t=0;t<800 && ts_msgs[TS_CH_OFF]==0;t++) test_pump(A,B,2);
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
        for (t=0;t<2000 && ts_msgs[TS_CH_BIG]==0;t++) test_pump(A,B,2);
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
        for (t=0;t<800 && ts_msgs[TS_CH_PLAIN]==0;t++) test_pump(A,B,2);
        rant_topic_counts(pt, NULL, &b1, NULL, NULL);
        CHECK(ts_msgs[TS_CH_PLAIN]==1 && ts_capture[TS_CH_PLAIN]==0,
                 "capture: an unset capture delivers 0 (%llu)",
                 (unsigned long long)ts_capture[TS_CH_PLAIN]);

        /* set: delivered exactly, written_us still a sane wall clock beside it */
        before = i_rant_plat_wall_us();
        ts_msgs[TS_CH_PLAIN]=0; ts_capture[TS_CH_PLAIN]=0;
        rant_topic_send(pt, rant_bytes(payload, sizeof payload),
                        &(RantSendOpts){ .capture_us = want });
        for (t=0;t<800 && ts_msgs[TS_CH_PLAIN]==0;t++) test_pump(A,B,2);
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
        for (t=0;t<800 && ts_msgs[TS_CH_OFF]==0;t++) test_pump(A,B,2);
        CHECK(ts_msgs[TS_CH_OFF]==1 && ts_capture[TS_CH_OFF]==0
              && ts_sent[TS_CH_OFF]==0 && ts_len[TS_CH_OFF]==sizeof payload,
                 "capture: an opted out topic carries neither stamp (cap=%llu len=%lu)",
                 (unsigned long long)ts_capture[TS_CH_OFF], (unsigned long)ts_len[TS_CH_OFF]);

        /* the fragmenting path, which is SHM where it is compiled in */
        ts_msgs[TS_CH_BIG]=0; ts_capture[TS_CH_BIG]=0;
        rant_topic_send(bt, rant_bytes(big, sizeof big),
                        &(RantSendOpts){ .capture_us = want });
        for (t=0;t<2000 && ts_msgs[TS_CH_BIG]==0;t++) test_pump(A,B,2);
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
            for (t=0;t<2000 && rant_function_match_count(fr)==0;t++) pf_pump(P,C,2);
            ts_req_done = 0; ts_req_sent = 0;
            rant_function_call_async(fr, rant_bytes(b,4), NULL, NULL, NULL);
            for (t=0;t<800 && !ts_req_done;t++) pf_pump(P,C,2);
            CHECK(ts_req_done && ts_req_sent!=0,
                     "writets: RantRequest.written_us stamped (done=%d ts=%llu)",
                     ts_req_done, (unsigned long long)ts_req_sent);
            ts_var_sent = 0;
            i_rant_le_w32(b, 42);
            for (t=0;t<2000 && rant_variable_match_count(vr)==0;t++) pf_pump(P,C,2);
            rant_variable_set(vr, rant_bytes(b,4));
            for (t=0;t<800 && ts_var_sent==0;t++) pf_pump(P,C,2);
            CHECK(ts_var_sent!=0, "writets: RantVariableUpdate.written_us stamped (%llu)",
                     (unsigned long long)ts_var_sent);
            rant_variable_on_write(vd, NULL, NULL);
        }
        if (C) rant_node_close(C,1);
        if (P) rant_node_close(P,1);
        rant_allocator_reset(&pa); rant_allocator_reset(&qa);
    }
}

/* (19b4) external interest end to end: a 300 topic publisher ships a sub MTU bootstrap
   and the subscriber pulls the blob by byte range paging (spec/testing.md). */
#define IX_TOPICS 300
static int ix_recv;
static uint32_t ix_epoch_pid, ix_epoch;
static void ix_on_message(const RantMsg *msg){ (void)msg; ix_recv++; }
static void interest_external_checks(void){
    RantAllocator pa = rant_allocator_heap(0);
    RantAllocator sa = rant_allocator_heap(0);
    RantNodeOpts po, so; RantNode *P, *S; RantTopic *pub=NULL, *sub=NULL;
    RantTopicOpts co; RantAddr seed; char name[16];
    uint8_t payload[8]; int i, t;
    memset(&co,0,sizeof co); co.qos.reliability=RANT_RELIABLE; co.qos.keep_last=4; co.qos.catch_up=1;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=TEST_DOMAIN+30; po.max_topics=IX_TOPICS+2;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po; so.max_topics=2;
    wire_tap_write(&wire_tap.tx_max_len, 0); wire_tap_write(&wire_tap.tx_interest_req, 0);
    P = rant_node_open(&pa, "ix-pub", NULL, NULL, &po);
    S = rant_node_open(&sa, "ix-sub", ix_on_message, NULL, &so);
    CHECK(P&&S, "interest: nodes open");
    if (!(P&&S)){ if(P)rant_node_close(P,0); if(S)rant_node_close(S,0); return; }
    for (i=0;i<IX_TOPICS;i++){
        snprintf(name,sizeof name,"ix/%03d",i);
        if (!rant_node_create_topic(P, name, RANT_PUB_ONLY, NULL, &co)) break;
    }
    CHECK(i==IX_TOPICS, "interest: %d topics created (%d)", IX_TOPICS, i);
    pub = rant_node_topic(P, 250);
    sub = rant_node_create_topic(S, "ix/250", RANT_SUB_ONLY, NULL, &co);
    CHECK(pub && sub, "interest: endpoints ready");
    ix_recv=0;
    for (t=0;t<2000 && (!pub || rant_topic_match_count(pub)==0);t++){ rant_node_poll(P,2); rant_node_poll(S,2); }
    CHECK(pub && rant_topic_match_count(pub)>0, "interest: match formed through the external fetch");
    memset(payload,0x77,sizeof payload);
    if (pub) rant_topic_send(pub, rant_bytes(payload,sizeof payload), NULL);
    for (t=0;t<800 && ix_recv==0;t++){ rant_node_poll(P,1); rant_node_poll(S,2); }
    CHECK(ix_recv>=1, "interest: delivery across the external match (%d)", ix_recv);
    {   /* reflection reads the assembled interest, not the announce: the entity fold must
           enumerate the external peer's whole set. The interest epoch is the observer cache key. */
        uint16_t cnt, k; int ents = 0; uint32_t pid = 0, epoch = 0;
        const i_RantDiscoveryPeerView *ps = test_peers(S, &cnt);
        for (k=0;k<cnt;k++)
            if (ps && ps[k].name.len==6 && !memcmp(ps[k].name.data,"ix-pub",6)){
                pid = ps[k].id; epoch = i_rant_node_peer_interest_epoch(&ps[k]);
            }
        CHECK(pid!=0, "interest: publisher visible in the peer view");
        CHECK(epoch>0, "interest: interest epoch advanced on assembly (%u)", epoch);
        if (pid){
            RantIter eit; RantEntityInfo ei;
            memset(&eit,0,sizeof eit);
            while (rant_node_entities_next(S, pid, &eit, &ei))
                ents += !test_builtin(&ei);
            CHECK(ents==IX_TOPICS, "interest: reflection enumerates all %d external entities (%d)",
                     IX_TOPICS, ents);
        }
        ix_epoch_pid = pid; ix_epoch = epoch;   /* steady-state stability checked below */
    }
    {   long long max_len = wire_tap_read(&wire_tap.tx_max_len);
        long long reqs = wire_tap_read(&wire_tap.tx_interest_req);
        CHECK(max_len>0 && max_len<=(long long)RANT_DGRAM_MAX,
                 "interest: no datagram exceeded RANT_DGRAM_MAX (max=%lld)", max_len);
        CHECK(reqs>=1, "interest: the blob traveled by paged fetch (%lld reqs)", reqs);
        for (t=0;t<250;t++){ rant_node_poll(P,5); rant_node_poll(S,5); }
        CHECK(wire_tap_read(&wire_tap.tx_interest_req)==reqs,
                 "interest: steady-state announces trigger no re-fetch (+%lld)",
                 wire_tap_read(&wire_tap.tx_interest_req)-reqs); }
    {   /* the epoch is quiet in steady state too: an epoch-keyed observer cache
           (the explorer) re-walks only on real change, never per announce */
        uint16_t cnt, k; uint32_t epoch_now = 0;
        const i_RantDiscoveryPeerView *ps = test_peers(S, &cnt);
        for (k=0;k<cnt;k++)
            if (ps && ps[k].id==ix_epoch_pid) epoch_now = i_rant_node_peer_interest_epoch(&ps[k]);
        CHECK(ix_epoch_pid && epoch_now==ix_epoch,
                 "interest: epoch stable across steady state (%u -> %u)", ix_epoch, epoch_now);
    }
    rant_node_close(P,1); rant_node_close(S,1);
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
        int lr = rant_node_log(A, RANT_LOG_ERROR, "boom %d", i);   /* hoisted out of ST_CHECK */
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
          pf_pump(A,B,2);
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
      for (t=0;t<1500 && !ml_live;t++) pf_pump(A,B,2);
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
                    && nm.bytes.len >= 8 && !memcmp(nm.bytes.data, "@rant/", 8)) hid++;
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

static void stream_checks(void){
    uint8_t payload[32]; unsigned i;
    memset(payload, 0x5A, sizeof payload);

    i_RantTopicDef ch[4]; memset(ch, 0, sizeof ch);
    ch[0].name = "st/gap";
    ch[0].qos.reliability = RANT_RELIABLE; ch[0].qos.keep_last = ST_DEPTH;
    ch[0].qos.max_message_bytes = 64; ch[0].qos.heartbeat_us = 50000;
    ch[1] = ch[0]; ch[1].name = "st/block"; ch[1].qos.backpressure_wait_us = ST_BLOCK_US;
    ch[2] = ch[0]; ch[2].name = "st/dyn";
    ch[2].qos.catch_up = ST_DEPTH;   /* phase 5 asserts ring replay on join */
    ch[3] = ch[0]; ch[3].name = "st/block2";
    ch[3].qos.backpressure_wait_us = ST_BLOCK_US; ch[3].qos.repair_delay_us = ST_NACK_US;

    /* disable_shm: phases 2 to 4b exercise the UDP reliability path. The same host SHM path
       has its own coverage and its chunk recycle semantics differ. */
    RantNodeOpts wo = { .domain = TEST_DOMAIN, .disable_shm = 1 };
    { RantNodeOpts ro = wo; i_RantTopicDef chr[4]; RantNode *w, *r;
      memcpy(chr, ch, sizeof ch);
      ch[0].role = ch[1].role = ch[2].role = ch[3].role = RANT_PUB_ONLY;
      chr[0].role = chr[1].role = chr[3].role = RANT_SUB_ONLY;
      chr[2].role = RANT_INACTIVE;

      w = test_node_open(NULL, NULL, NULL, wo, ch, 4);
      r = test_node_open(NULL, test_on_message, test_on_event, ro, chr, 4);
      CHECK(w && r, "stream: nodes open");
      if (!w || !r){ if (w) rant_node_close(w, 0); if (r) rant_node_close(r, 0); return; }

      /* 1. JOIN: the writer streams while discovery completes. The reader must adopt the
         stream head silently, no gap for a late joiner */
      { uint64_t end = i_rant_plat_now_us() + 5000000u;
        while (test_samples[ST_CH_GAP]==0 && i_rant_plat_now_us() < end){
            test_send(w, ST_CH_GAP, payload, sizeof payload);
            test_pump(w, r, 20);
        } }
      CHECK(test_samples[ST_CH_GAP] > 0, "join: reader receives (got %lu)", test_samples[ST_CH_GAP]);
      CHECK(test_gap_calls[ST_CH_GAP] == 0, "join: no on_gap for late join (calls=%lu)", test_gap_calls[ST_CH_GAP]);

      /* 2. GAP: stage 50 samples with no flush in between. Only the last ST_DEPTH survive,
         the rest must arrive as exactly one gap */
      test_pump(w, r, 200);                           /* settle acks */
      { unsigned long s0 = test_samples[ST_CH_GAP];
        for (i=0;i<50;i++) test_send(w, ST_CH_GAP, payload, sizeof payload);
        test_pump(w, r, 500);
        CHECK(test_gap_calls[ST_CH_GAP] == 1, "gap: one on_gap call (calls=%lu)", test_gap_calls[ST_CH_GAP]);
        CHECK(test_gap_lost[ST_CH_GAP] == 50-ST_DEPTH, "gap: count == evicted span (%lu, want %u)",
                 test_gap_lost[ST_CH_GAP], 50-ST_DEPTH);
        CHECK(test_samples[ST_CH_GAP]-s0 == ST_DEPTH, "gap: surviving tail delivered (%lu, want %u)",
                 test_samples[ST_CH_GAP]-s0, ST_DEPTH);
      }

      /* 3. BLOCKED: fill ST_CH_BLOCK's ring, then keep sending while the
            reader never runs: each eviction-send must wait ~ST_BLOCK_US */
      test_pump(w, r, 200);
      for (i=0;i<ST_DEPTH;i++) test_send(w, ST_CH_BLOCK, payload, sizeof payload);
      test_pump(w, NULL, 50);                         /* flush. The reader is silent, no acks */
      { uint64_t t0 = i_rant_plat_now_us(), dt;
        test_send(w, ST_CH_BLOCK, payload, sizeof payload);     /* evicts un-acked */
        dt = i_rant_plat_now_us() - t0;
        CHECK(dt >= ST_BLOCK_US-10000 && dt < 4*ST_BLOCK_US,
                 "blocked: send waited ~backpressure_wait_us (%.1f ms)", dt/1000.0);
      }

      /* 4. RELEASED: let the reader catch up and ack. Sends are instant */
      test_pump(w, r, 300);
      { uint64_t t0 = i_rant_plat_now_us(), dt;
        test_send(w, ST_CH_BLOCK, payload, sizeof payload);
        dt = i_rant_plat_now_us() - t0;
        CHECK(dt < 20000, "released: acked ring sends instantly (%.1f ms)", dt/1000.0);
        test_pump(w, r, 100);
        /* the blocked phase eviction dropped only history already in flight to the reader's
           socket, so every send must have been delivered and no gap reported */
        CHECK(test_gap_calls[ST_CH_BLOCK] == 0 && test_samples[ST_CH_BLOCK] == ST_DEPTH+2,
                 "blocked: in-flight eviction loses nothing (gaps=%lu, samples=%lu/%u)",
                 test_gap_calls[ST_CH_BLOCK], test_samples[ST_CH_BLOCK], ST_DEPTH+2);
      }

      /* 4b. SWEEP-ACK: ST_CH_BLOCK2 has nack_delay > 0, so the reader's ACKNACK is timer armed
         and flushed only by the sweep once the writer goes quiet (spec/testing.md). */
      test_pump(w, r, 200);                              /* match + settle */
      { unsigned long s0 = test_samples[ST_CH_BLOCK2];
        for (i=0;i<ST_DEPTH;i++) test_send(w, ST_CH_BLOCK2, payload, sizeof payload);
        test_pump(w, r, 200);   /* the reader drains the burst, the sweep must ack */
        CHECK(test_samples[ST_CH_BLOCK2]-s0 == ST_DEPTH, "sweep-ack: ring delivered (%lu, want %u)",
                 test_samples[ST_CH_BLOCK2]-s0, ST_DEPTH);
        { uint64_t t0 = i_rant_plat_now_us(), dt;
          test_send(w, ST_CH_BLOCK2, payload, sizeof payload);     /* would evict slot 0 */
          dt = i_rant_plat_now_us() - t0;
          CHECK(dt < 20000,
                   "sweep-ack: sub-only reader's timer ack releases backpressure (%.1f ms)", dt/1000.0);
        }
      }

      /* 5. DYNAMIC: ST_CH_DYN is inactive on the reader. Subscribe replays the cached ring,
         unsubscribe goes silent at the writer, resubscribe replays again, all gap free. */
      test_pump(w, r, 200);
      for (i=0;i<3;i++) test_send(w, ST_CH_DYN, payload, sizeof payload);
      test_pump(w, r, 300);
      CHECK(test_samples[ST_CH_DYN] == 0, "dynamic: inactive receives nothing (%lu)",
               test_samples[ST_CH_DYN]);
      test_set_role(r, ST_CH_DYN, RANT_SUB_ONLY);
      test_pump(w, r, 400);
      CHECK(test_samples[ST_CH_DYN] == 3, "dynamic: subscribe replays cached history (%lu, want 3)",
               test_samples[ST_CH_DYN]);
      test_send(w, ST_CH_DYN, payload, sizeof payload);
      test_pump(w, r, 300);
      CHECK(test_samples[ST_CH_DYN] == 4, "dynamic: live sample delivered (%lu, want 4)",
               test_samples[ST_CH_DYN]);
      test_set_role(r, ST_CH_DYN, RANT_INACTIVE);
      test_pump(w, r, 300);                  /* let the new list reach the writer */
      for (i=0;i<5;i++) test_send(w, ST_CH_DYN, payload, sizeof payload);
      test_pump(w, r, 300);
      CHECK(test_samples[ST_CH_DYN] == 4, "dynamic: unsubscribed receives nothing (%lu)",
               test_samples[ST_CH_DYN]);
      test_set_role(r, ST_CH_DYN, RANT_SUB_ONLY);
      test_pump(w, r, 400);
      CHECK(test_samples[ST_CH_DYN] == 4+ST_DEPTH, "dynamic: resubscribe replays ring (%lu, want %u)",
               test_samples[ST_CH_DYN], 4+ST_DEPTH);
      CHECK(test_gap_calls[ST_CH_DYN] == 0, "dynamic: joins are silent (gaps=%lu)",
               test_gap_calls[ST_CH_DYN]);

      /* 5b. FLAP and EPOCH GUARD: a one sided flap where the reader rebuilt its state. The
         changed reader epoch in the first ACKNACK must make every writer lane re join. */
      test_pump(w, r, 200);
      { unsigned long s0 = test_samples[ST_CH_DYN];
        uint32_t wid = 0; uint16_t k; uint8_t ib[256]; size_t il;
        for (k=0;k<i_rant_node_core_max_peers(r->core);k++) if (i_rant_node_core_peer_at(r->core,k,&wid,NULL,NULL,NULL)) break;
        i_rant_transport_peer_remove(r->transport, wid);
        i_rant_transport_peer_add(r->transport, wid,RANT_FRAG_SIZE);
        il = i_rant_transport_build_interest(w->transport, ib, sizeof ib);
        i_rant_transport_apply_peer_interest(r->transport, wid, rant_bytes(ib, il));
        /* the apply only nominates, since peer_remove dropped the cached verdicts. Run the sans
           IO detail exchange by hand so the flapped reader re verifies and rematches */
        {   i_RantDetailWant wl[8]; uint8_t rq[256], rp[1024]; uint16_t nw2, verified; size_t rl2, pl2;
            nw2 = i_rant_transport_detail_wants(r->transport, NULL, wid, rant_bytes(ib, il), wl, 8);
            CHECK(nw2 > 0, "flap: re-added peer nominates pending candidates (%u)", nw2);
            rl2 = i_rant_detail_req_build(TEST_DOMAIN, 0, wl, nw2, rq, sizeof rq);
            pl2 = i_rant_transport_detail_respond(w->transport, NULL, 0, rant_bytes(rq, rl2), rp, sizeof rp);
            verified = i_rant_transport_apply_peer_details(r->transport, wid, rant_bytes(rp, pl2));
            CHECK(verified == nw2, "flap: details verify every candidate (%u/%u)", verified, nw2);
            i_rant_transport_apply_peer_interest(r->transport, wid, rant_bytes(ib, il));
        }
        test_pump(w, r, 600);
        CHECK(test_samples[ST_CH_DYN] == s0+ST_DEPTH,
                 "flap: writer re-joins new reader incarnation, replays ring (%lu, want %lu)",
                 test_samples[ST_CH_DYN], s0+ST_DEPTH);
        CHECK(test_gap_calls[ST_CH_DYN] == 0, "flap: recovery is silent (gaps=%lu)",
                 test_gap_calls[ST_CH_DYN]);
      }

      /* 5c. RESUME: a discovery blip drops the peer on both sides without tearing down
         transport state. A same incarnation resume replays the withheld backlog with no gap. */
      test_pump(w, r, 200);
      { unsigned long s0 = test_samples[ST_CH_DYN], g0 = test_gap_calls[ST_CH_DYN];
        uint32_t wid = 0, rid = 0; uint16_t k;
        for (k=0;k<i_rant_node_core_max_peers(r->core);k++) if (i_rant_node_core_peer_at(r->core,k,&wid,NULL,NULL,NULL)) break;
        for (k=0;k<i_rant_node_core_max_peers(w->core);k++) if (i_rant_node_core_peer_at(w->core,k,&rid,NULL,NULL,NULL)) break;
        i_rant_transport_peer_dormant(w->transport, rid);     /* drops the reader from flow control */
        i_rant_transport_peer_dormant(r->transport, wid);     /* reader stops acking the writer */
        for (i=0;i<3;i++) test_send(w, ST_CH_DYN, payload, sizeof payload);
        test_pump(w, r, 300);
        CHECK(test_samples[ST_CH_DYN] == s0, "resume: dormant peer withholds sends (%lu, want %lu)",
                 test_samples[ST_CH_DYN], s0);
        i_rant_transport_peer_resume(w->transport, rid);
        i_rant_transport_peer_resume(r->transport, wid);
        test_pump(w, r, 400);
        CHECK(test_samples[ST_CH_DYN] == s0+3,
                 "resume: backlog replays from preserved position (%lu, want %lu)",
                 test_samples[ST_CH_DYN], s0+3);
        CHECK(test_gap_calls[ST_CH_DYN] == g0, "resume: lossless, no gap (gaps=%lu, want %lu)",
                 test_gap_calls[ST_CH_DYN], g0);
      }

      rant_node_close(r, 1);
      rant_node_close(w, 1);
    }

    /* 6. SCALE: 40 topics. The full interest list rides one announce blob, matched at peer_up */
    {
      static i_RantTopicDef cha[ST_NCH], chb[ST_NCH];
      static char snames[ST_NCH][12];     /* "scale/0".."scale/39" */
      RantNodeOpts ao, bo; RantNode *a, *b; uint16_t k;
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
      ao = (RantNodeOpts){ .domain = TEST_DOMAIN+1 };
      bo = ao;
      a = test_node_open(NULL, NULL, NULL, ao, cha, ST_NCH);
      b = test_node_open(NULL, test_on_message, test_on_event, bo, chb, ST_NCH);
      CHECK(a && b, "scale: %u-topic nodes open", ST_NCH);
      if (a && b){
          test_messages = 0;
          for (k=0;k<ST_NCH;k++) test_send(a, k, payload, 16);
          { uint64_t end = i_rant_plat_now_us() + 5000000u;
            while (test_messages < ST_NCH && i_rant_plat_now_us() < end) test_pump(a, b, 20); }
          CHECK(test_messages == ST_NCH, "scale: all topics delivered (%lu/%u)", test_messages, ST_NCH);
          rant_node_close(b, 1);
          rant_node_close(a, 1);
      }
    }

    /* 7. NAMED: the cross peer identity is the topic name hash, independent of each node's
       local handle. A distinct name never cross wires and clean names raise no collision. */
    {
      i_RantTopicDef nw[1], nr[2];
      RantNodeOpts wo2, ro2; RantNode *w2, *r2;
      memset(nw,0,sizeof nw); memset(nr,0,sizeof nr);
      nw[0].name="robot/lidar"; nw[0].role=RANT_PUB_ONLY;
      nw[0].qos.reliability=RANT_RELIABLE; nw[0].qos.keep_last=1;
      nw[0].qos.catch_up=1; nw[0].qos.max_message_bytes=32; nw[0].qos.heartbeat_us=50000;
      nr[0]=nw[0]; nr[0].role=RANT_SUB_ONLY;     /* same name (index 0), other node */
      nr[1]=nw[0]; nr[1].name="sensors/imu"; nr[1].role=RANT_SUB_ONLY;     /* index 1 */
      wo2 = (RantNodeOpts){ .domain=TEST_DOMAIN+2, .discovery={ .max_peers=4 } };
      ro2=wo2;
      test_samples[0]=test_samples[1]=0; test_collisions=0; test_last_sender[0]='\0';
      w2=test_node_open("lidar-node",NULL,NULL,wo2,nw,1);
      r2=test_node_open("reader-node",test_on_message,test_on_event,ro2,nr,2);
      CHECK(w2 && r2, "named: nodes open");
      if (w2 && r2){
          uint64_t end = i_rant_plat_now_us() + 5000000u;
          while (test_samples[0]==0 && i_rant_plat_now_us()<end){
              test_send(w2, 0, payload, 16); test_pump(w2,r2,20);
          }
          CHECK(test_samples[0] > 0, "named: same name matches across nodes (%lu)", test_samples[0]);
          /* the node name is synced via discovery and surfaces as RantMsg.publisher_name */
          CHECK(strcmp(test_last_sender, "lidar-node")==0,
                   "named: publisher_name carries the publisher's node name (%s)", test_last_sender);
          test_pump(w2,r2,200);
          CHECK(test_samples[1] == 0, "named: distinct name never cross-wires (%lu)", test_samples[1]);
          CHECK(test_collisions == 0, "named: clean names raise no collision (%lu)", test_collisions);
          rant_node_close(r2,1); rant_node_close(w2,1);
      }
    }

    /* 8. COLLISION: two names with the same 64 bit identity, found by Pollard's rho against
       FNV-1a. Rant must fire on_collision and refuse the match, never cross wire. */
    {
      const char *A="iuZA9tcJzAG", *B="5wVGxhTCmOC";   /* both hash to 23f58aa8628b1cce */
      i_RantTopicDef cw, cr; RantNodeOpts wo3, ro3; RantNode *w3, *r3;
      CHECK(i_rant_topic_id(A)==i_rant_topic_id(B) && strcmp(A,B)!=0,
               "collision: test pair still shares one identity (else regen via collide)");
      memset(&cw,0,sizeof cw);
      cw.name=A; cw.role=RANT_PUB_ONLY;
      cw.qos.reliability=RANT_RELIABLE; cw.qos.keep_last=1; cw.qos.catch_up=1;
      cw.qos.max_message_bytes=32; cw.qos.heartbeat_us=50000;
      cr=cw; cr.name=B; cr.role=RANT_SUB_ONLY;
      wo3 = (RantNodeOpts){ .domain=TEST_DOMAIN+3, .discovery={ .max_peers=4 } };
      ro3=wo3;
      test_samples[0]=0; test_collisions=0;
      w3=test_node_open(NULL,NULL,NULL,wo3,&cw,1);
      r3=test_node_open(NULL,test_on_message,test_on_event,ro3,&cr,1);
      CHECK(w3 && r3, "collision: nodes open");
      if (w3 && r3){
          for (i=0;i<60;i++){ test_send(w3,0,payload,16); rant_node_poll(w3,0); rant_node_poll(r3,20); }
          CHECK(test_collisions >= 1, "collision: detected (RANT_E_NAME_COLLISION fired %lu)", test_collisions);
          CHECK(test_samples[0] == 0, "collision: match refused, no cross-wire (%lu)", test_samples[0]);
          rant_node_close(r3,1); rant_node_close(w3,1);
      }
    }
}

static const TestCase cases[] = {
    { "stream", stream_checks },
#ifdef RANT_SHM
    { "shm_node", shm_node_checks },
#endif
    { "open_fail", open_fail_checks },
    { "event_user", event_user_checks },
    { "dynamic_grow", dynamic_grow_checks },
    { "schema_advert", schema_advert_checks },
    { "schema_bind", schema_bind_checks },
    { "schema_bigenum", schema_bigenum_checks },
    { "schema_root", schema_root_checks },
    { "stdtypes", stdtypes_checks },
    { "interest_external", interest_external_checks },
    { "detail_live", detail_live_checks },
    { "queue", queue_checks },
    { "pull", pull_checks },
    { "pattern_schema", pattern_schema_checks },
    { "callback_queue", callback_queue_checks },
    { "callback_pattern", callback_pattern_checks },
    { "patterns", patterns_checks },
    { "metalog", metalog_checks },
    { "dup_authority", dup_authority_checks },
    { "retire", retire_checks },
    { "reflect_dropped", reflect_dropped_checks },
    { "reflect_reuse", reflect_reuse_checks },
    { "varwait", varwait_checks },
    { "churn", churn_checks },
    { "task", task_checks },
    { "taskx", taskx_checks },
    { "loud", loud_checks },
    { "matchwait", matchwait_checks },
    { "relay", relay_checks },
    { "nat", nat_checks },
    { "selfip", selfip_checks },
    { "launchname", launchname_checks },
    { "launchdomain", launchdomain_checks },
    { "prefix", prefix_checks },
    { "ts", ts_checks },
#ifdef RANT_THREADS
    { "threaded", threaded_checks },
#endif
};

TEST_MAIN(cases)
