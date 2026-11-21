/* Same host shared memory between full nodes: size classes byte exact, and the inline
 * fallback for a peer without it. Built only with RANT_SHM. spec/testing.md says more. */
#include "support/test.h"

/* Full nodes on loopback: SHM across size classes (byte-exact + shm_tx/rx), and
   the inline fallback for a non-SHM-capable subscriber. */
static int shmn_recv; static size_t shmn_len; static unsigned long shmn_sum;
static void shmn_on_message(const RantMsg *msg){
    const unsigned char *p=(const unsigned char*)msg->data.data; size_t i, len=msg->data.len; unsigned long s=0;
    for(i=0;i<len;i++) s+=p[i];
    shmn_recv++; shmn_len=len; shmn_sum=s;
}
static RantNode *shmn_open(int is_pub, int shm_capable, uint16_t domain, uint32_t hint){
    static i_RantTopicDef ch[2]; static int slot;
    i_RantTopicDef *d=&ch[slot++ & 1]; RantAddr seed; RantNodeOpts opts;
    RantQos q; memset(&q,0,sizeof q);
    q.reliability=RANT_RELIABLE; q.keep_last=4; q.catch_up=1; q.shm_max_bytes=hint;
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
    P=shmn_open(1,1,(uint16_t)(TEST_DOMAIN+40),0); S=shmn_open(0,1,(uint16_t)(TEST_DOMAIN+40),0);
    CHECK(P&&S, "shm-node: SHM-capable pub + sub open");
    if (P&&S){
        PUMP_UNTIL(P, S, 6400, test_match_count(P,0)!=0);
        CHECK(test_match_count(P,0)>0, "shm-node: matched");
        /* sizes[0] fits one datagram so it ships inline, not SHM. The two larger ones would
           fragment, so they take the SHM path. */
        sizes[0]=200; sizes[1]=300*1024; sizes[2]=4*1024*1024;
        for (i=0;i<3;i++){
            unsigned long want=0; size_t j; int before=shmn_recv, t;
            for (j=0;j<sizes[i];j++){ buf[j]=(unsigned char)((j*31u+(unsigned)i+1)&0xFF); want+=buf[j]; }
            test_send(P,0,buf,sizes[i]);
            PUMP_UNTIL(P, S, 4000, shmn_recv!=before);
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
    P=shmn_open(1,1,(uint16_t)(TEST_DOMAIN+42),0); S=shmn_open(0,0,(uint16_t)(TEST_DOMAIN+42),0);
    if (P&&S){
        unsigned long want=0; size_t j; int before, t;
        PUMP_UNTIL(P, S, 6400, test_match_count(P,0)!=0);
        for (j=0;j<50*1024;j++){ buf[j]=(unsigned char)((j*31u+9)&0xFF); want+=buf[j]; }
        before=shmn_recv; test_send(P,0,buf,50*1024);
        PUMP_UNTIL(P, S, 4000, shmn_recv!=before);
        CHECK(shmn_recv==before+1 && shmn_sum==want, "shm-node: non-SHM sub -> inline byte-exact");
        tx=0; rant_node_shm_stats(P,&tx,NULL);
        CHECK(tx==0, "shm-node: no SHM used for the non-SHM reader (tx=%u)", tx);
        rant_node_close(P,1); rant_node_close(S,1);
    }
}

/* A reader lets go of a publisher's segment when the publisher goes, so one that restarts
   leaves nothing mapped behind it. */
static void shm_gone_checks(void){
    static unsigned char buf[300*1024];
    uint16_t domain=(uint16_t)(TEST_DOMAIN+41);
    RantNode *S=shmn_open(0,1,domain,0);
    int round, t;
    CHECK(S!=NULL, "shm-gone: the reader opens");
    for (round=0; S && round<3; round++){
        RantNode *P=shmn_open(1,1,domain,0);
        int before=shmn_recv;
        CHECK(P!=NULL, "shm-gone: the publisher opens");
        if (!P) break;
        PUMP_UNTIL(P, S, 6400, test_match_count(P,0)!=0);
        test_send(P,0,buf,sizeof buf);
        PUMP_UNTIL(P, S, 4000, shmn_recv!=before);
        CHECK(shmn_recv==before+1, "shm-gone: delivered");
        CHECK(S->shm_reader_count==1, "shm-gone: one segment attached (%u)", (unsigned)S->shm_reader_count);
        rant_node_close(P,1);
        for (t=0; t<400 && S->shm_reader_count!=0; t++) rant_node_poll(S,10);
        CHECK(S->shm_reader_count==0, "shm-gone: none attached once it went (%u)", (unsigned)S->shm_reader_count);
    }
    if (S) rant_node_close(S,1);
}

/* A hint counts the payload, so a payload as big as its class still fits beside the stamp and
   header that ride the same chunk. */
static void shm_hint_checks(void){
    static unsigned char buf[1024*1024];
    static const uint32_t sizes[] = { 64*1024, 256*1024, 1024*1024 };
    int i;
    for (i=0;i<3;i++){
        uint16_t domain=(uint16_t)(TEST_DOMAIN+43+i);
        RantNode *P=shmn_open(1,1,domain,sizes[i]), *S=shmn_open(0,1,domain,sizes[i]);
        CHECK(P&&S, "shm-hint: open");
        if (P&&S){
            unsigned long want=0; size_t j; int before=shmn_recv, t; uint32_t tx=0;
            PUMP_UNTIL(P, S, 6400, test_match_count(P,0)!=0);
            for (j=0;j<sizes[i];j++){ buf[j]=(unsigned char)((j*7u+(unsigned)i)&0xFF); want+=buf[j]; }
            test_send(P,0,buf,sizes[i]);
            PUMP_UNTIL(P, S, 4000, shmn_recv!=before);
            rant_node_shm_stats(P,&tx,NULL);
            CHECK(shmn_recv==before+1 && shmn_len==sizes[i] && shmn_sum==want,
                  "shm-hint: byte exact %lu bytes", (unsigned long)sizes[i]);
            CHECK(tx==1, "shm-hint: a %lu byte payload pinned at its own size went over SHM (tx=%u)",
                  (unsigned long)sizes[i], tx);
        }
        if (P) rant_node_close(P,1);
        if (S) rant_node_close(S,1);
    }
}

static const TestCase cases[] = {
    { "shm_node", shm_node_checks },
    { "shm_gone", shm_gone_checks },
    { "shm_hint", shm_hint_checks },
};

TEST_MAIN(cases)
