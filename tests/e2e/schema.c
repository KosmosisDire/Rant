/* Schemas between nodes: the announce, subset binding, a large enum, bare type roots, the
 * standard types end to end and the schemas of pattern handles.
 * spec/testing.md says what each case pins. */
#include "support/test.h"

/* The index n's first peer advertises a publisher of hash h at, 0xFFFF when it does not */
static uint16_t advertised_pub_index(RantNode *n, uint32_t h){
    uint16_t n_peers = 0, index = 0xFFFF;
    const i_RantDiscoveryPeerView *peers = test_peers(n, &n_peers);
    i_RantInterestIter it; i_RantTopicEntry tp;
    if (!peers || !n_peers) return index;
    memset(&it, 0, sizeof it);
    while (i_rant_node_peer_interest_next(&peers[0], &it, &tp))
        if (tp.is_pub && tp.hash == h) index = tp.index;
    return index;
}

/* Announce interest: one positional hash and flags entry per topic slot. An inactive topic
   is not yielded but holds its position. */
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
        CHECK(le.error == RANT_E_BAD_SCHEMA && le.detail
                 && strstr(le.detail, "another shape near: RantLog") != NULL,
                 "announce: the refusal says why and where (%s)", le.detail ? le.detail : "null");
        CHECK(rant_node_schema(P, "Open {") == NULL && rant_last_error(P).detail
                 && strstr(rant_last_error(P).detail, "expected } at the end") != NULL,
                 "announce: a truncated text is refused at the end (%s)",
                 rant_last_error(P).detail ? rant_last_error(P).detail : "null");
        CHECK(rant_node_schema(P, "Reading { p: Nope }") == NULL && rant_last_error(P).detail
                 && strstr(rant_last_error(P).detail, "unknown type near: Nope") != NULL,
                 "announce: an unknown type is named (%s)",
                 rant_last_error(P).detail ? rant_last_error(P).detail : "null");
        CHECK(rant_node_schema(P, "Reading { x: f64 }") != NULL && rant_node_schema(P, "Reading") != NULL,
                 "announce: a definition made on the node names the next compile");
    }
    i_rant_schema_free(sch, rant_allocator_alloc, &ma);       /* the node registered its own: freed NOW */

    /* a typed-typed match through hash nomination + detail verification */
    PUMP_UNTIL(P, S, 6400, rant_topic_match_count(pc)!=0);
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
            test_pump(P, S, 800);
            {   i_RantInterestIter it2; i_RantTopicEntry tp2; int seen=0;
                peers = test_peers(S, &n_peers);
                memset(&it2,0,sizeof it2);
                while (i_rant_node_peer_interest_next(&peers[0], &it2, &tp2))
                    if (tp2.hash==late_h) seen=1;
                CHECK(!seen, "announce: inactive topic not advertised");
            }
            rant_topic_set_role(lc, RANT_PUB_ONLY);
            PUMP_UNTIL(P, S, 5000, advertised_pub_index(S, late_h) != 0xFFFF);
            CHECK(advertised_pub_index(S, late_h)==2,
                  "announce: role flip advertises the held position (index %u)",
                  advertised_pub_index(S, late_h));
        }
    }
    rant_node_close(P,1); rant_node_close(S,1);
    rant_allocator_reset(&ma);
}

/* Reader side subset binding: a subset subscriber matches with a rebased schema, an
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

    PUMP_UNTIL(P, S, 6400, rant_topic_match_count(pc)!=0);
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
        PUMP_UNTIL(P, S, 2400, sb_recv!=0);
        CHECK(sb_recv==1, "schema-bind: subset message delivered");
        CHECK(sb_schema_ok, "schema-bind: RantMsg.schema is the rebased view (writer size, reader fields)");
        CHECK(sb_y==-2.25 && sb_stamp==0x1122334455667788ULL,
                 "schema-bind: reader indices read the writer's offsets (y=%.2f)", sb_y);
    }
    {   /* a message that does not fit the sender's schema is refused at the sender */
        unsigned long before = sb_mismatch_n;
        uint8_t junk[3] = {1,2,3};
        int rc = rant_topic_send(pc, rant_bytes(junk, sizeof junk), NULL);
        test_pump(P, S, 150);
        CHECK(rc==RANT_ERR_SCHEMA && sb_recv==1 && sb_mismatch_n==before,
                 "schema-bind: wrong-size message refused at the sender (rc=%d recv=%d)", rc, sb_recv);
    }
    rant_node_close(P,1); rant_node_close(S,1);
    rant_allocator_reset(&ma);
}

/* A large procedurally built enum, 1024 options with a u16 backing. Both nodes
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

    PUMP_UNTIL(P, S, 6400, rant_topic_match_count(pc)!=0);
    CHECK(rant_topic_match_count(pc)>0, "bigenum: typed match reached the peer");

    {   uint8_t buf[16];
        rant_schema_message_default(es, buf, sizeof buf);
        rant_set_uint(buf, sizeof buf, es, "id", 42);
        CHECK(rant_set_enum(buf, sizeof buf, es, "job", "job_1000"),
                 "bigenum: set the value by its human name");
        rant_topic_send(pc, rant_bytes(buf, rant_schema_msg_len(es, buf, sizeof buf)), NULL);
        PUMP_UNTIL(P, S, 2400, be_recv!=0);
        CHECK(be_recv==1 && be_val==1000 && strcmp(be_label,"Job1000")==0,
                 "bigenum: value delivered + resolved to its name (val=%lld, label=%s)",
                 (long long)be_val, be_label);
    }
    rant_node_close(P,1); rant_node_close(S,1);
    i_rant_schema_free(es, rant_allocator_alloc, &ma);
    rant_allocator_reset(&ma);
}

/* Primitive rooted schemas: one bare type, anonymous, addressed by the empty path,
   the same wire in any language (spec/testing.md). Every root kind is covered. */

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
    CHECK(rant_schema_field_count(s) == (kind == RANT_ARR || kind == RANT_VARR ? 2 : 1)
             && rant_schema_field_at(s, 0, &fi)
             && fi.kind == kind && fi.name.len == 0 && fi.name.data != NULL
             && fi.depth == 0 && fi.offset == 0,
             "schema-root: '%s' is one anonymous field and its element (kind=%u)", text, fi.kind);
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
    CHECK(rant_schema_hash(sb) == test_golden_hash("bool"),
             "schema-root: `bool` canonical hash %016llx", (unsigned long long)rant_schema_hash(sb));
    CHECK(rant_schema_hash(sarr) == test_golden_hash("f32[]"),
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
                 && rant_schema_field_count(rt) == 2,
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
        CHECK(rb && rant_schema_field_count(rb) == 2 && rant_schema_msg_min(rb) == 4,
                 "schema-root: rebase of a bare root stays valid");
        if (rb) i_rant_schema_free(rb, rant_allocator_alloc, &ma);
    }

    /* end to end: one topic per root kind, plus the two refusals */
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

/* The standard type library: names always in scope, golden bytes and hash, shape
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
    /* GOLDEN: every line of tests/golden/schemas.txt, the canonical wire and hash every
       binding mirrors. Change them only with a deliberate wire bump */
    {   TestGolden g; int i;
        for (i = 0; test_golden(i, &g); i++){
            RantSchema *gs = test_compile(&ma, g.text, NULL);
            RantBytes w = gs ? rant_schema_wire(gs) : rant_bytes(NULL, 0);
            CHECK(gs && rant_schema_hash(gs) == g.hash, "golden: %s hash %016llx", g.text,
                  gs ? (unsigned long long)rant_schema_hash(gs) : 0ull);
            if (g.wire_len)
                CHECK(gs && w.len == g.wire_len && memcmp(w.data, g.wire, g.wire_len) == 0,
                      "golden: %s wire (%u bytes)", g.text, (unsigned)w.len);
            if (gs) i_rant_schema_free(gs, rant_allocator_alloc, &ma);
        }
        CHECK(i >= 6, "golden: tests/golden/schemas.txt read (%d lines)", i);
    }
    {   RantSchema *s = test_compile(&ma, "Float3", NULL);
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
            CHECK(i_rant_std_recognize_field(&reg, good, (uint16_t)rant_schema_field_index(good, "pts[0]"))
                        == RANT_STD_FLOAT3,
                     "stdtypes: an array's element row recognizes");
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
        CHECK(e && rant_schema_msg_min(e) == 0 && rant_schema_message_default(e, NULL, 0)
                 && rant_schema_msg_len(e, NULL, 0) == 0 && rant_schema_validate(e, rant_bytes(NULL, 0)),
                 "stdtypes: an Empty message needs no buffer and its length 0 is valid");
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

    /* end to end: Transform and Image over two nodes, plus a shape impostor refused */
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
        PUMP_UNTIL(P, S, 7200, sd_pose_recv!=0 && sd_img_recv!=0 && sd_empty_recv!=0);
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

/* The pull phase: a pull topic never calls back, take pops the oldest, take_latest the
 * newest after dropping the rest, and a topic created without pull refuses take. */
/* Pattern handle schemas: what a definition declared, a function's missing progress, and
   what a reflect_from_mesh remote adopted once the mesh typed it. */
static void ps_request(RantRequest *req, void *user){ (void)user; rant_request_reply(req, rant_bytes(NULL, 0)); }
/* 1 once a reflect remote adopted both schemas from the mesh */
static int ps_adopted(RantFunction *remote){
    rant_function_refresh(remote);
    return rant_function_request_schema(remote) && rant_function_response_schema(remote);
}
static void pattern_schema_checks(void){
    RantAllocator aa = rant_allocator_heap(0), ba = rant_allocator_heap(0);
    RantNodeOpts o;
    RantNode *a, *b;
    const RantSchema *rq, *rs, *pg;
    RantFunction *fn = NULL, *task = NULL, *remote = NULL;
    RantVariable *var = NULL;
    int typed = 0;
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
    if (remote) PUMP_UNTIL(a, b, 10000, (typed = ps_adopted(remote)));
    CHECK(typed && rant_schema_hash(rant_function_request_schema(remote)) == rant_schema_hash(rq)
             && rant_schema_hash(rant_function_response_schema(remote)) == rant_schema_hash(rs),
             "pattern schema: a reflect remote reports the schemas it adopted");
done:
    if (a) rant_node_close(a, 0);
    if (b) rant_node_close(b, 0);
}

/* Nested variable data end to end: the receiver validates every level and a subset reader
   in another order reads elements through the rebased schema. */
static int sn_recv, sn_valid, sn_points; static double sn_y; static char sn_name[16];
static void sn_on_message(const RantMsg *msg){
    RantString nm;
    if (!msg->schema) return;
    sn_recv++;
    sn_valid = rant_schema_validate(msg->schema, msg->data);
    sn_points = (int)rant_get_array_count(msg->data, msg->schema, "areas[1].points");
    sn_y = rant_get_f64(msg->data, msg->schema, "areas[1].points[2].y");
    nm = rant_get_string(msg->data, msg->schema, "names[1]");
    if (nm.data && nm.len < sizeof sn_name){ memcpy(sn_name, nm.data, nm.len); sn_name[nm.len] = '\0'; }
}

static void schema_nested_checks(void){
    RantAllocator pa = rant_allocator_heap(0), sa = rant_allocator_heap(0);
    RantNodeOpts po, so; RantNode *P, *S; RantTopicOpts co; RantAddr seed;
    const RantSchema *ps, *ss; RantTopic *pub = NULL; int t;
    static uint8_t m[1024];
    memset(&co,0,sizeof co); co.qos.keep_last=4; co.qos.reliability=RANT_RELIABLE;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=TEST_DOMAIN+12; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po;
    sn_recv = sn_valid = sn_points = 0; sn_y = 0; sn_name[0] = '\0';
    P = rant_node_open(&pa, "sn-pub", NULL, NULL, &po);
    S = rant_node_open(&sa, "sn-sub", sn_on_message, NULL, &so);
    CHECK(P && S, "schema-nested: nodes open");
    if (!(P && S)){ if (P) rant_node_close(P,0); if (S) rant_node_close(S,0); return; }
    ps = rant_node_schema(P, "Zones { pad: u64, areas: Polygon2D[], names: string[], t: string }");
    ss = rant_node_schema(S, "Zones { names: string[], areas: Polygon2D[] }");
    if (ps) pub = rant_node_create_topic(P, "sn/zones", RANT_PUB_ONLY, ps, &co);
    if (ss) rant_node_create_topic(S, "sn/zones", RANT_SUB_ONLY, ss, &co);
    for (t=0;t<1200 && pub && rant_topic_match_count(pub)==0;t++){ rant_node_poll(P,2); rant_node_poll(S,2); }
    CHECK(pub && rant_topic_match_count(pub)==1, "schema-nested: a subset reader in another order matches");
    if (pub && rant_topic_match_count(pub)==1){
        RantDouble2 pts[4] = { { 0, 0 }, { 2, 0 }, { 2, 1 }, { 0, 1 } };
        int ok = rant_schema_message_default(ps, m, sizeof m);
        ok &= rant_set_array_count(m, sizeof m, ps, "areas", 2);
        ok &= rant_set_array(m, sizeof m, ps, "areas[1].points", rant_bytes(pts, sizeof pts));
        ok &= rant_set_array_count(m, sizeof m, ps, "names", 2);
        ok &= rant_set_string(m, sizeof m, ps, "names[1]", rant_cstr("dock"));
        ok &= rant_set_string(m, sizeof m, ps, "t", rant_cstr("tail"));
        CHECK(ok, "schema-nested: the nested message builds");
        rant_topic_send(pub, rant_bytes(m, rant_schema_msg_len(ps, m, sizeof m)), NULL);
        for (t=0;t<800 && sn_recv==0;t++){ rant_node_poll(P,1); rant_node_poll(S,2); }
    }
    CHECK(sn_recv==1 && sn_valid && sn_points==4 && sn_y==1.0 && strcmp(sn_name, "dock")==0,
             "schema-nested: delivered and read at every level (recv=%d points=%d name=%s)",
             sn_recv, sn_points, sn_name);
    rant_node_close(P,1); rant_node_close(S,1);
    rant_allocator_reset(&pa); rant_allocator_reset(&sa);
}

static const TestCase cases[] = {
    { "advert", schema_advert_checks },
    { "bind", schema_bind_checks },
    { "big_enum", schema_bigenum_checks },
    { "root", schema_root_checks },
    { "stdtypes", stdtypes_checks },
    { "pattern_schema", pattern_schema_checks },
    { "nested", schema_nested_checks },
};

TEST_MAIN(cases)
