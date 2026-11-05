/* The shared memory path below the node: the mapping module with its seqlock guards, and
 * loss, repair and skip on the transport core. Built only with RANT_SHM.
 * spec/testing.md says what each case pins. */
#include "support/test.h"

/* The mapping module: create and attach by name, write, stamp and read, the generation
   recycle guard and the seqlock verify tail. */
static void shm_module_checks(void){
    i_RantShmConfig cfg; void *pw, *pr; i_RantShmPool *w, *r;
    uint32_t cap=0, rlen=0; void *cp; const void *rp;
    i_RantShmDesc d, d2; uint8_t wire[RANT_SHM_DESC_WIRE], a[16], b[16];
    const char msg[] = "hello shared memory";
    i_rant_plat_startup();
    memset(&cfg,0,sizeof cfg);
    /* segment names are OS global: the domain base keeps concurrent runs apart */
#ifdef _WIN32
    snprintf(cfg.name,sizeof cfg.name,"rant-shm-stmod-%u",(unsigned)test_domain_base);
#else
    snprintf(cfg.name,sizeof cfg.name,"/rant-shm-stmod-%u",(unsigned)test_domain_base);
#endif
    cfg.segment_id=0x1234; cfg.chunk_bytes=4096; cfg.n_chunks=4;
    pw=malloc(i_rant_shm_state_bytes()); pr=malloc(i_rant_shm_state_bytes());
    w=i_rant_shm_create(pw,&cfg); r=i_rant_shm_attach(pr,&cfg);
    CHECK(w && r, "shm-mod: create + attach by name");
    if (w && r){
        i_rant_plat_host_uuid(a); i_rant_plat_host_uuid(b);
        CHECK(i_rant_shm_host_match(a,b)==1, "shm-mod: host_uuid stable + matches");
        cp=i_rant_shm_chunk(w,0,&cap); memcpy(cp,msg,sizeof msg);
        i_rant_shm_stamp(w,0,(uint32_t)sizeof msg,&d);
        CHECK(cap==4096 && d.generation==1, "shm-mod: loan + stamp (gen=%llu)", (unsigned long long)d.generation);
        i_rant_shm_desc_encode(&d,wire);
        CHECK(i_rant_shm_desc_decode(&d2,wire,sizeof wire) &&
                 d2.chunk==d.chunk && d2.length==d.length && d2.generation==d.generation,
                 "shm-mod: descriptor wire round-trip");
        rp=i_rant_shm_read(r,&d2,&rlen);
        CHECK(rp && rlen==sizeof msg && memcmp(rp,msg,sizeof msg)==0, "shm-mod: read sees writer bytes");
        CHECK(i_rant_shm_verify(r,&d2)==1, "shm-mod: verify current gen ok");
        { void *cp2=i_rant_shm_chunk(w,0,NULL); i_RantShmDesc dn; uint32_t l;
          memcpy(cp2,"new",4); i_rant_shm_stamp(w,0,4,&dn);
          CHECK(i_rant_shm_read(r,&d2,&l)==NULL, "shm-mod: recycled chunk -> old descriptor refused");
          CHECK(i_rant_shm_verify(r,&d2)==0, "shm-mod: verify recycled gen fails (torn guard)"); }
        i_rant_shm_detach(r); i_rant_shm_detach(w);
    }
    free(pw); free(pr);
    i_rant_plat_cleanup();
}

/* Loss and repair over shared memory on the transport core: a mock on_shm with controllable
   success and a pump that can drop SHM DATA. */
static int shml_ok, shml_recv, shml_lost, shml_drop;
static uint64_t shml_now;
static i_RantTransportState *shml_W, *shml_R;
static int shml_on_shm(void *u, uint16_t ch, uint32_t from, const uint8_t *desc){
    (void)u;(void)ch;(void)from;(void)desc; if (shml_ok){ shml_recv++; return 1; } return 0;
}
static void shml_on_event(const RantEvent *ev){ if (ev->kind==RANT_MSG_LOST) shml_lost++; }
static void shml_pump(int n){
    uint8_t buf[RANT_DGRAM_MAX]; uint32_t to; size_t ol; int i;
    for (i=0;i<n;i++){
        while (i_rant_transport_poll_send(shml_W,&to,buf,sizeof buf,&ol,shml_now)){
            if (shml_drop>0 && (buf[0]&RANT_F_SHM)){ shml_drop--; continue; }
            i_rant_transport_on_datagram(shml_R, 1u, rant_bytes(buf, ol), shml_now);
        }
        while (i_rant_transport_poll_send(shml_R,&to,buf,sizeof buf,&ol,shml_now))
            i_rant_transport_on_datagram(shml_W, 2u, rant_bytes(buf, ol), shml_now);
        shml_now += 30000;   /* 30 ms: past repair_delay (20ms), lets heartbeats fire */
    }
}
static void shml_send(void){
    static unsigned char chunk[2048]; unsigned char desc[RANT_SHM_DESC_WIRE];
    memset(desc,0,sizeof desc); i_rant_transport_send_shm(shml_W, 0, rant_bytes(chunk, 1000), desc, shml_now);
}
static void shm_loss_checks(void){
    TestPair pair; i_RantTransportConfig cb;
    RantQos q; memset(&q,0,sizeof q); q.reliability=RANT_RELIABLE; q.keep_last=8;
    q.heartbeat_us=50000; q.repair_delay_us=20000;
    memset(&cb,0,sizeof cb); cb.on_shm=shml_on_shm; cb.on_event=shml_on_event;
    if (!test_pair_open(&pair, "shmloss", q, q, &cb)){ CHECK(0, "shm-loss: transport pair opens"); return; }
    shml_W = pair.w; shml_R = pair.r;
    shml_now=1000000;
    test_apply_verified(shml_R, 1u, shml_W);
    test_apply_verified(shml_W, 2u, shml_R);
    i_rant_transport_peer_set_shm(shml_W,2u,1);
    CHECK(i_rant_transport_publisher_match_count(shml_W,0)>0, "shm-loss: writer matched reader");
    shml_ok=1; shml_recv=0; shml_lost=0; shml_drop=0; shml_send(); shml_pump(5);
    CHECK(shml_recv==1 && shml_lost==0, "shm-loss: [a] normal delivered");
    shml_recv=0; shml_lost=0; shml_drop=1; shml_send(); shml_pump(10);
    CHECK(shml_recv==1 && shml_lost==0, "shm-loss: [b] dropped descriptor repaired");
    shml_recv=0; shml_lost=0; shml_drop=0; shml_ok=0; shml_send(); shml_pump(4);
    CHECK(shml_recv==0, "shm-loss: [c] unresolvable not delivered");
    shml_ok=1; shml_pump(6);
    CHECK(shml_recv==1 && shml_lost==0, "shm-loss: [c] delivered after transient clears");
    shml_recv=0; shml_lost=0; shml_ok=0; shml_send(); shml_pump(20);
    CHECK(shml_recv==0 && shml_lost>=1, "shm-loss: [d] persistent -> skip + MSG_LOST");
    shml_ok=1; shml_recv=0; shml_send(); shml_pump(6);
    CHECK(shml_recv==1, "shm-loss: [d] not wedged, next delivered");
    test_pair_close(&pair);
}

static const TestCase cases[] = {
    { "module", shm_module_checks },
    { "loss", shm_loss_checks },
};

TEST_MAIN(cases)
