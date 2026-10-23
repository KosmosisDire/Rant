/* memscale: one reliable publisher and N subscribers on loopback over a grid of payload size,
 * keep_last and subscriber count, and again with a bulk pool on every node. A steady state
 * send must not allocate (spec/testing.md), so the command exits 1 when one did. */
#include "bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_ms_rx;
static void ms_on_message(const RantMsg *m){ (void)m; g_ms_rx++; }

static unsigned ms_domain = 200;

/* The bulk pools' page source. mem_stats counts the main pool only, so the bulk's heap takes
   are counted here. */
static uint64_t ms_bulk_takes; static long long ms_bulk_live;
static void *ms_bulk_page(void *ptr, size_t size){
    uint8_t *h;
    if (ptr){ h = (uint8_t*)ptr - 16; ms_bulk_live -= (long long)*(size_t*)h; free(h); return NULL; }
    h = (uint8_t*)malloc(size + 16);
    if (!h) return NULL;
    *(size_t*)h = size; ms_bulk_takes++; ms_bulk_live += (long long)size;
    return h + 16;
}

/* heap allocations so far across the publisher and every subscriber, both pools */
static uint64_t ms_allocs(RantNode *P, RantNode **S, int nsubs){
    uint64_t a, sum=0; int i; rant_node_mem_stats(P,NULL,NULL,&a); sum=a;
    for (i=0;i<nsubs;i++){ rant_node_mem_stats(S[i],NULL,NULL,&a); sum+=a; }
    return sum + ms_bulk_takes;
}

/* one row of the grid. Returns the allocations the steady state sends made */
static uint64_t ms_run(size_t plen, uint16_t keep, int nsubs, int disable_shm, int bulk){
    RantAllocator pa, sa[16], pb, sb[16]; RantNodeOpts po, so; RantNode *P=NULL, *S[16];
    RantTopic *pc=NULL; RantTopicOpts co; RantAddr seed;
    uint8_t *payload; int i, j, k, t, nwarm, nsteady; unsigned dom = ms_domain++;
    uint64_t a0=0, a1=0, a2=0; size_t ppeak=0, speak=0;
    double cold_sum=0, warm_sum=0, t0, t1, msg_s; int coldc=0;
    if (nsubs>16) nsubs=16;
    payload=(uint8_t*)malloc(plen?plen:1); if(!payload) return 0; memset(payload,0x5A,plen?plen:1);
    nsteady = plen<=1024 ? 300 : plen<=65536 ? 100 : 20;
    nwarm   = keep + 4;
    memset(&co,0,sizeof co); co.qos.reliability=RANT_RELIABLE; co.qos.keep_last=keep; co.qos.heartbeat_us=50000;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=(uint16_t)dom; po.max_topics=4;
    po.discovery.max_peers=(uint16_t)(nsubs+2); po.disable_shm=(uint8_t)disable_shm;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po;
    ms_bulk_takes=0; ms_bulk_live=0;
    pa=rant_allocator_heap(0); pb=rant_allocator_dynamic(ms_bulk_page,0);
    po.bulk = bulk ? &pb : NULL;
    P=rant_node_open(&pa,"ms-pub",NULL,NULL,&po);
    for (i=0;i<nsubs;i++){
        sa[i]=rant_allocator_heap(0); sb[i]=rant_allocator_dynamic(ms_bulk_page,0);
        so.bulk = bulk ? &sb[i] : NULL;
        S[i]=rant_node_open(&sa[i],"ms-sub",ms_on_message,NULL,&so);
    }
    if (!P){ free(payload); return 0; }
    pc=rant_node_create_topic(P,"ms/ch",RANT_PUB_ONLY,NULL,&co);
    for (i=0;i<nsubs;i++) rant_node_create_topic(S[i],"ms/ch",RANT_SUB_ONLY,NULL,&co);
    for (t=0;t<4000 && rant_topic_match_count(pc)<nsubs;t++){ rant_node_poll(P,1); for(j=0;j<nsubs;j++) rant_node_poll(S[j],1); }

    a0=ms_allocs(P,S,nsubs);                              /* before any traffic */
    g_ms_rx=0;                                            /* warmup: fill and size the buffers */
    for (i=0;i<nwarm;i++){
        double s0=(double)bench_now_ns()/1e3, s1;
        rant_topic_send(pc,rant_bytes(payload,plen), NULL);
        s1=(double)bench_now_ns()/1e3;
        if (i<keep){ cold_sum += s1-s0; coldc++; }
        for (k=0;k<400000 && g_ms_rx < (i+1)*nsubs;k++){ rant_node_poll(P,0); for(j=0;j<nsubs;j++) rant_node_poll(S[j],0); }
    }
    a1=ms_allocs(P,S,nsubs);                              /* after warmup */
    g_ms_rx=0; t0=(double)bench_now_ns()/1e3;             /* steady: the buffers are sized */
    for (i=0;i<nsteady;i++){
        double s0=(double)bench_now_ns()/1e3, s1;
        rant_topic_send(pc,rant_bytes(payload,plen), NULL);
        s1=(double)bench_now_ns()/1e3;
        warm_sum += s1-s0;
        for (k=0;k<400000 && g_ms_rx < (i+1)*nsubs;k++){ rant_node_poll(P,0); for(j=0;j<nsubs;j++) rant_node_poll(S[j],0); }
    }
    t1=(double)bench_now_ns()/1e3;
    a2=ms_allocs(P,S,nsubs);
    rant_node_mem_stats(P,NULL,&ppeak,NULL); rant_node_mem_stats(S[0],NULL,&speak,NULL);
    msg_s = (t1>t0) ? nsteady / ((t1-t0)/1e6) : 0;
    printf("%-9lu %-5u %-5d %11.1f %11.1f %9.1f %11llu %13llu %10.0f %9.2f %9.2f\n",
        (unsigned long)plen, keep, nsubs, ppeak/1024.0, speak/1024.0, ms_bulk_live/1024.0,
        (unsigned long long)(a1-a0), (unsigned long long)(a2-a1),
        msg_s, coldc?cold_sum/coldc:0.0, nsteady?warm_sum/nsteady:0.0);
    rant_node_close(P,0); for(i=0;i<nsubs;i++) rant_node_close(S[i],0);
    free(payload);
    return a2 - a1;
}

static void ms_head(const char *path){
    printf("\n--- %s ---\n", path);
    printf("%-9s %-5s %-5s %11s %11s %9s %11s %13s %10s %9s %9s\n",
        "payload","keep","subs","pub_peak_kB","sub_peak_kB","bulk_kB","warm_alloc","steady_alloc","msg/s","cold_us","warm_us");
}

static uint64_t ms_grid(int disable_shm){
    uint64_t steady = 0;
    ms_head(disable_shm ? "UDP path (cross host, writer and reader buffers on the heap)"
                        : "SHM path (same host, payload in mapped segments)");
    steady += ms_run(64,8,1,disable_shm,0);     steady += ms_run(1024,8,1,disable_shm,0);
    steady += ms_run(65536,8,1,disable_shm,0);  steady += ms_run(1048576,8,1,disable_shm,0);
    steady += ms_run(1024,1,1,disable_shm,0);   steady += ms_run(1024,16,1,disable_shm,0);
    steady += ms_run(1024,256,1,disable_shm,0);
    steady += ms_run(1024,8,4,disable_shm,0);   steady += ms_run(1024,8,16,disable_shm,0);
    return steady;
}

/* The UDP path with a bulk pool on every node: the peaks are the main pools', which the
   message sized buffers leave, and bulk_kB is what the bulk pools hold at the end. */
static uint64_t ms_bulk_grid(void){
    uint64_t steady = 0;
    ms_head("UDP path with a bulk pool");
    steady += ms_run(64,8,1,1,1);     steady += ms_run(65536,8,1,1,1);
    steady += ms_run(1048576,8,1,1,1);  steady += ms_run(1024,8,4,1,1);
    return steady;
}

int memscale_main(void){
    uint64_t steady;
    printf("Rant allocator scale test (in process, reliable, loopback)\n");
    printf("peak = live message buffer bytes, warm and steady_alloc = heap allocations (pub and subs), cold and warm_us = send call time\n");
    steady = ms_grid(1);   /* the allocation relevant path */
    steady += ms_grid(0);  /* the same host path, for comparison */
    steady += ms_bulk_grid();
    if (steady){
        printf("\nFAIL: steady state sends allocated %llu times\n", (unsigned long long)steady);
        return 1;
    }
    printf("\nPASS: steady state sends never allocate\n");
    return 0;
}
