/* The detail exchange on live sockets: stateless replies to the source, and an interest
 * too big to inline fetched by paging. spec/testing.md says what each case pins. */
#include "support/test.h"

/* Live uDTL routing: a DETAIL_REQ at the data socket is answered to its source even
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
    PUMP_UNTIL(P, S, 6400, rant_topic_match_count(pc)!=0);
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
    if (q != RANT_SOCK_BAD){ if (!i_rant_plat_bind(q, i_rant_plat_ipv4(127,0,0,1), 0, 0)){ i_rant_plat_close(q); q = RANT_SOCK_BAD; } }
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

/* External interest end to end: a 300 topic publisher ships a sub MTU bootstrap
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
    PUMP_UNTIL(P, S, 16000, pub && rant_topic_match_count(pub)!=0);
    CHECK(pub && rant_topic_match_count(pub)>0, "interest: match formed through the external fetch");
    memset(payload,0x77,sizeof payload);
    if (pub) rant_topic_send(pub, rant_bytes(payload,sizeof payload), NULL);
    PUMP_UNTIL(P, S, 4800, ix_recv!=0);
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
        test_pump(P, S, 2500);
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

static const TestCase cases[] = {
    { "detail_live", detail_live_checks },
    { "interest_external", interest_external_checks },
};

TEST_MAIN(cases)
