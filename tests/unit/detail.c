/* The pairwise detail exchange and the interest codecs, sans IO: the stateless responder,
 * schema wire inlining, paging that fits one datagram, and the external interest flag.
 * spec/testing.md says what each case pins. */
#include "support/test.h"

/* The pairwise detail codec, sans IO: request build, the stateless responder, schema
   wire only on a hash mismatch, entry boundary truncation, malformed input rejected whole. */
static void detail_codec_checks(void){
    static uint8_t tmem[1<<18];
    RantAllocator ma = rant_allocator_heap(0);
    i_RantTransportConfig tc; i_RantTransportState *tr; i_RantTopicDef ch[3];
    i_RantMetaSchema schemas[3]; RantSchema *S;
    i_RantDetailWant wants[4];
    uint8_t req[256], resp[1024], out2[1024];
    size_t rl, need, len;

    S = test_compile(&ma, "Reading { stamp: u64, x: f64 }", NULL);
    memset(ch,0,sizeof ch);
    ch[0].name="dt/typed"; ch[1].name="dt/raw";
    ch[2].name="dt/off"; ch[2].role=RANT_INACTIVE;
    memset(&tc,0,sizeof tc); tc.topics=ch; tc.n_topics=3; tc.max_peers=2;
    tc.allocator=rant_allocator_alloc; tc.user=&ma;
    tr = i_rant_transport_init(tmem, sizeof tmem, &tc);
    CHECK(tr!=NULL && S!=NULL, "detail: transport + schema ready");
    if (!tr || !S){ rant_allocator_reset(&ma); return; }
    memset(schemas,0,sizeof schemas);
    schemas[0].hash = rant_schema_hash(S); schemas[0].wire = rant_schema_wire(S);

    /* request four indices: typed (requester untyped: hash 0), raw, INACTIVE, unknown */
    wants[0].index=0; wants[0].schema_hash=0;
    wants[1].index=1; wants[1].schema_hash=0;
    wants[2].index=2; wants[2].schema_hash=0;
    wants[3].index=9; wants[3].schema_hash=0;
    rl = i_rant_detail_req_build(77, 5, wants, 4, req, sizeof req);
    CHECK(rl == 14u+4u*10u, "detail: req builds (%u bytes)", (unsigned)rl);
    CHECK(i_rant_detail_kind(rant_bytes(req,rl))==RANT_DETAIL_REQ
          && i_rant_detail_domain(rant_bytes(req,rl))==77
          && i_rant_detail_meta_version(rant_bytes(req,rl))==5,
             "detail: req header round-trips (kind/domain/version)");
    CHECK(i_rant_detail_req_build(77,5,wants,4,req,20)==0, "detail: req refuses a short buffer");

    need = i_rant_transport_detail_resp_size(tr, schemas, rant_bytes(req,rl));
    len  = i_rant_transport_detail_respond(tr, schemas, 9, rant_bytes(req,rl), resp, sizeof resp);
    CHECK(need==len && len>14u, "detail: resp_size == respond, byte for byte (%u)", (unsigned)len);
    CHECK(i_rant_detail_kind(rant_bytes(resp,len))==RANT_DETAIL_RESP
          && i_rant_detail_domain(rant_bytes(resp,len))==77
          && i_rant_detail_meta_version(rant_bytes(resp,len))==9,
             "detail: resp header carries the responder version");
    {   i_RantDetailIter it; i_RantDetail d; int n=0, ok_typed=0, ok_raw=0;
        RantBytes wire = rant_bytes(NULL, 0);
        memset(&it,0,sizeof it);
        while (i_rant_detail_next(rant_bytes(resp,len), &it, &d)){
            n++;
            if (d.index==0){
                ok_typed = d.name.len==8 && memcmp(d.name.data,"dt/typed",8)==0
                        && d.schema_hash==rant_schema_hash(S)
                        && d.schema_wire.len==rant_schema_wire(S).len;
                wire = d.schema_wire;
            }
            if (d.index==1)
                ok_raw = d.name.len==6 && memcmp(d.name.data,"dt/raw",6)==0
                      && d.schema_hash==0 && d.schema_wire.len==0;
        }
        CHECK(n==2, "detail: advertised indices answered, INACTIVE + unknown skipped (n=%d)", n);
        CHECK(ok_typed, "detail: typed entry carries name + hash + inlined wire");
        CHECK(ok_raw, "detail: raw entry carries name, no schema");
        {   RantSchema *P2 = wire.len ? i_rant_schema_parse(wire.data, wire.len,
                                                          rant_allocator_alloc, &ma) : NULL;
            CHECK(P2 && rant_schema_hash(P2)==rant_schema_hash(S),
                     "detail: inlined wire parses back to the same identity");
        }
    }

    /* identical hash: hash-only entry, zero wire bytes (identical wire is implied) */
    wants[0].schema_hash = rant_schema_hash(S);
    rl  = i_rant_detail_req_build(77, 5, wants, 2, req, sizeof req);
    len = i_rant_transport_detail_respond(tr, schemas, 9, rant_bytes(req,rl), resp, sizeof resp);
    {   i_RantDetailIter it; i_RantDetail d; int hash_only=0; memset(&it,0,sizeof it);
        while (i_rant_detail_next(rant_bytes(resp,len), &it, &d))
            if (d.index==0) hash_only = d.schema_wire.len==0 && d.schema_hash==rant_schema_hash(S);
        CHECK(hash_only, "detail: identical hash rides hash-only (no wire)");
    }

    /* a cap one byte short of full truncates at an entry boundary: still parseable,
       holding exactly the leading entries that fit (the requester re-asks for the rest) */
    wants[0].schema_hash = 0;
    rl = i_rant_detail_req_build(77, 5, wants, 2, req, sizeof req);
    {   size_t full = i_rant_transport_detail_resp_size(tr, schemas, rant_bytes(req,rl));
        size_t cut  = i_rant_transport_detail_respond(tr, schemas, 9, rant_bytes(req,rl), out2, full-1);
        i_RantDetailIter it; i_RantDetail d; int n=0; uint16_t first=0xFFFF;
        memset(&it,0,sizeof it);
        while (i_rant_detail_next(rant_bytes(out2,cut), &it, &d)){ if (!n) first=d.index; n++; }
        CHECK(cut>0 && cut<full && n==1 && first==0,
                 "detail: truncation stops at an entry boundary (paging: %d/%d entries)", n, 2);
    }

    /* malformed input is rejected wholesale, never partially trusted */
    CHECK(i_rant_transport_detail_respond(tr, schemas, 9, rant_bytes(req, rl-1), out2, sizeof out2)==0,
             "detail: truncated req rejected");
    CHECK(i_rant_transport_detail_respond(tr, schemas, 9, rant_bytes(resp, len), out2, sizeof out2)==0,
             "detail: a RESP fed to the responder is refused (kind gate)");
    {   uint8_t junk[32]; memset(junk, 0x5A, sizeof junk);
        CHECK(i_rant_detail_kind(rant_bytes(junk, sizeof junk))==0
              && i_rant_detail_kind(rant_bytes(req, 4))==0,
                 "detail: non-detail bytes yield kind 0");
    }
    rant_allocator_reset(&ma);
}

/* Detail paging never IP fragments and never wedges: a response fits one datagram
   and the requester pages the rest (spec/testing.md). */
static void detail_paging_checks(void){
#define DP_N 40
    RantAllocator ma = rant_allocator_heap(0);
    i_RantTransportConfig tc; i_RantTransportState *tr; i_RantMetaSchema schemas[DP_N]; RantSchema *S;
    i_RantDetailWant wants[DP_N]; uint8_t req[512], buf[RANT_DGRAM_MAX + 8];
    char names[DP_N][RANT_TOPIC_NAME_MAX + 1];
    int done[DP_N], i, rounds, resolved, max_page = 0;

    S = test_compile(&ma, "Reading { stamp: u64, x: f64, y: f64 }", NULL);
    memset(&tc, 0, sizeof tc);
    tc.topics = NULL; tc.n_topics = DP_N; tc.max_peers = 2; tc.allocator = rant_allocator_alloc; tc.user = &ma;
    { size_t need = i_rant_transport_required_memory(&tc);         /* dynamic reserve mode */
      void *mem = rant_allocator_alloc(&ma, NULL, need);
      tr = mem ? i_rant_transport_init(mem, need, &tc) : NULL; }
    CHECK(tr != NULL && S != NULL, "detail-paging: transport + schema ready");
    if (!tr || !S){ rant_allocator_reset(&ma); return; }

    memset(schemas, 0, sizeof schemas);
    for (i = 0; i < DP_N; i++){                          /* long names so entries are fat */
        i_RantTopicDef d; memset(&d, 0, sizeof d);
        snprintf(names[i], sizeof names[i],
                 "paging.detail.regression.topic.with.a.long.name.%02d", i);
        d.name = names[i]; d.role = RANT_PUB_ONLY;
        i_rant_transport_topic_define(tr, (uint16_t)i, &d);
        schemas[i].hash = rant_schema_hash(S); schemas[i].wire = rant_schema_wire(S);
    }

    /* the requester's paging loop: ask for the unresolved indices, take one page, mark what
       it carried, repeat. Exactly what i_rant_transport_detail_wants drives live. */
    memset(done, 0, sizeof done);
    for (rounds = 0, resolved = 0; resolved < DP_N && rounds < DP_N; rounds++){
        uint16_t nw = 0; size_t rl, page, len; i_RantDetailIter it; i_RantDetail dd; int got = 0;
        for (i = 0; i < DP_N; i++) if (!done[i]){ wants[nw].index = (uint16_t)i; wants[nw].schema_hash = 0; nw++; }
        rl = i_rant_detail_req_build(3, 1, wants, nw, req, sizeof req);
        if (!rl) break;
        page = i_rant_transport_detail_resp_size(tr, schemas, rant_bytes(req, rl));
        if (page > (size_t)max_page) max_page = (int)page;
        if (page > RANT_DGRAM_MAX){ CHECK(0, "detail-paging: a page exceeded one datagram (%u)", (unsigned)page); break; }
        len = i_rant_transport_detail_respond(tr, schemas, 1, rant_bytes(req, rl), buf, page);
        memset(&it, 0, sizeof it);
        while (i_rant_detail_next(rant_bytes(buf, len), &it, &dd))
            if (dd.index < DP_N && !done[dd.index]){ done[dd.index] = 1; resolved++; got++; }
        if (!got) break;                                 /* no forward progress: wedged */
    }
    CHECK(resolved == DP_N, "detail-paging: all %d entries resolved (%d)", DP_N, resolved);
    CHECK(rounds > 1, "detail-paging: it actually paged (%d single-datagram rounds)", rounds);
    CHECK(max_page > 0 && max_page <= RANT_DGRAM_MAX,
             "detail-paging: every page stayed within one datagram (max=%u)", (unsigned)max_page);
    rant_allocator_reset(&ma);

    /* (b) force first: one topic whose schema wire alone exceeds a datagram. respond must
       emit exactly that whole entry, never a header only reply that re asks forever. */
    {   RantAllocator mb = rant_allocator_heap(0);
        static char bigtext[16 + 200 * 16];
        i_RantTransportState *t2; i_RantTransportConfig c2; i_RantMetaSchema sc; i_RantDetailWant w; RantSchema *B;
        i_RantTopicDef d; uint8_t rq[64], *big; size_t need2, rl2, page2, len2; int k;
        len2 = (size_t)sprintf(bigtext, "Big {");
        for (k = 0; k < 200; k++) len2 += (size_t)sprintf(bigtext + len2, " field%03d: f64,", k);
        sprintf(bigtext + len2, " }");
        B = test_compile(&mb, bigtext, NULL);
        CHECK(B && rant_schema_wire(B).len > RANT_DGRAM_MAX,
                 "detail-paging: built a schema wire over one datagram (%u)",
                 (unsigned)(B ? rant_schema_wire(B).len : 0));
        memset(&c2, 0, sizeof c2);
        c2.topics = NULL; c2.n_topics = 1; c2.max_peers = 1; c2.allocator = rant_allocator_alloc; c2.user = &mb;
        need2 = i_rant_transport_required_memory(&c2);
        t2 = B ? i_rant_transport_init(rant_allocator_alloc(&mb, NULL, need2), need2, &c2) : NULL;
        if (t2){
            memset(&d, 0, sizeof d); d.name = "big/schema"; d.role = RANT_PUB_ONLY;
            i_rant_transport_topic_define(t2, 0, &d);
            memset(&sc, 0, sizeof sc); sc.hash = rant_schema_hash(B); sc.wire = rant_schema_wire(B);
            w.index = 0; w.schema_hash = 0;   /* requester untyped: forces the wire inline */
            rl2 = i_rant_detail_req_build(3, 1, &w, 1, rq, sizeof rq);
            page2 = i_rant_transport_detail_resp_size(t2, &sc, rant_bytes(rq, rl2));
            CHECK(page2 > RANT_DGRAM_MAX, "detail-paging: the oversize entry is measured whole (%u)", (unsigned)page2);
            big = (uint8_t*)rant_allocator_alloc(&mb, NULL, page2 + 8);
            len2 = big ? i_rant_transport_detail_respond(t2, &sc, 1, rant_bytes(rq, rl2), big, page2) : 0;
            {   i_RantDetailIter it; i_RantDetail dd; int n = 0; uint64_t h = 0;
                memset(&it, 0, sizeof it);
                while (big && i_rant_detail_next(rant_bytes(big, len2), &it, &dd)){ n++; h = dd.schema_hash; }
                CHECK(n == 1 && h == rant_schema_hash(B),
                         "detail-paging: the lone oversize entry rides its own page (n=%d)", n);
            }
        } else CHECK(0, "detail-paging: force-first transport ready");
        rant_allocator_reset(&mb);
    }
#undef DP_N
}

/* The interest paging codec and the external overlay flag: build both forms, page
   a blob by byte range, reject malformed pages. Sans IO, codec only. */
static void interest_codec_checks(void){
    static uint8_t tmem[1<<16];
    RantAllocator ma = rant_allocator_heap(0);
    i_RantTransportConfig tc; i_RantTransportState *tr; i_RantTopicDef ch[3];
    uint8_t meta[256], req[64], page[128];
    uint16_t ml; uint32_t total, off; RantBytes chunk;

    memset(ch,0,sizeof ch);
    ch[0].name="ic/a";
    ch[1].name="ic/b"; ch[1].role=RANT_SUB_ONLY; ch[1].qos.max_rate_hz=50;    /* rate section too */
    ch[2].name="ic/c";
    memset(&tc,0,sizeof tc); tc.topics=ch; tc.n_topics=3; tc.max_peers=2;
    tc.allocator=rant_allocator_alloc; tc.user=&ma;
    tr = i_rant_transport_init(tmem, sizeof tmem, &tc);
    CHECK(tr!=NULL, "icodec: transport init");
    if (!tr){ rant_allocator_reset(&ma); return; }

    /* inline overlay: flag clear, interest present, all three size views agree */
    ml = i_rant_transport_meta_build(tr, meta, sizeof meta, 1200, 0, NULL, 0);
    CHECK(ml == i_rant_transport_meta_size(tr), "icodec: inline build == meta_size (%u)", ml);
    CHECK(i_rant_meta_frag(rant_bytes(meta,ml))==1200, "icodec: frag survives");
    CHECK(!i_rant_meta_interest_external(rant_bytes(meta,ml)), "icodec: inline -> external flag clear");
    {   RantBytes in = i_rant_meta_interest(rant_bytes(meta,ml));
        CHECK(in.data && in.len == i_rant_transport_interest_size(tr),
                 "icodec: interest slice == interest_size (%u)", (unsigned)in.len); }

    /* bootstrap overlay: fixed size, flag set, interest NULL (never read as empty) */
    ml = i_rant_transport_meta_build(tr, meta, sizeof meta, 1200, 0, NULL, 1);
    CHECK(ml == i_rant_transport_meta_bootstrap_size(), "icodec: bootstrap size (%u)", ml);
    CHECK(i_rant_meta_interest_external(rant_bytes(meta,ml)), "icodec: bootstrap -> external flag set");
    CHECK(i_rant_meta_interest(rant_bytes(meta,ml)).data == NULL, "icodec: bootstrap interest is NULL");
    CHECK(i_rant_meta_frag(rant_bytes(meta,ml))==1200, "icodec: bootstrap still carries frag");

    /* REQ roundtrip + header gating */
    {   size_t rl = i_rant_interest_req_build(7, 42u, 123u, req, sizeof req);
        CHECK(rl==18u, "icodec: req builds (%u bytes)", (unsigned)rl);
        CHECK(i_rant_detail_kind(rant_bytes(req,rl))==RANT_INTEREST_REQ
              && i_rant_detail_domain(rant_bytes(req,rl))==7
              && i_rant_detail_meta_version(rant_bytes(req,rl))==42u, "icodec: req header decodes");
        CHECK(i_rant_interest_req_offset(rant_bytes(req,rl), &off) && off==123u, "icodec: req offset rides");
        CHECK(!i_rant_interest_req_offset(rant_bytes(req,rl-1u), &off), "icodec: short req rejected");
        CHECK(i_rant_interest_req_build(7,42u,0u,req,17u)==0u, "icodec: tiny cap refused"); }

    /* RESP page roundtrip + bounds */
    {   uint8_t blob[40]; size_t hl; unsigned i;
        for (i=0;i<sizeof blob;i++) blob[i]=(uint8_t)i;
        hl = i_rant_interest_resp_head(7, 42u, 40u, 10u, 20u, page, sizeof page);
        CHECK(hl==RANT_INTEREST_RESP_HEAD, "icodec: resp head builds");
        memcpy(page+hl, blob+10, 20);
        CHECK(i_rant_interest_resp_parse(rant_bytes(page, hl+20u), &total, &off, &chunk)
              && total==40u && off==10u && chunk.len==20u && chunk.data[0]==10u,
                 "icodec: resp page parses (total=%u off=%u len=%u)", total, off, (unsigned)chunk.len);
        CHECK(!i_rant_interest_resp_parse(rant_bytes(page, hl+19u), &total, &off, &chunk),
                 "icodec: truncated page rejected");
        i_rant_interest_resp_head(7, 42u, 40u, 30u, 20u, page, sizeof page);     /* 30+20 > 40 */
        memcpy(page+RANT_INTEREST_RESP_HEAD, blob, 20);
        CHECK(!i_rant_interest_resp_parse(rant_bytes(page, RANT_INTEREST_RESP_HEAD+20u), &total, &off, &chunk),
                 "icodec: out-of-blob range rejected"); }
    rant_allocator_reset(&ma);
}

static const TestCase cases[] = {
    { "detail_codec", detail_codec_checks },
    { "detail_paging", detail_paging_checks },
    { "interest_codec", interest_codec_checks },
};

TEST_MAIN(cases)
