/* The sweep spawns N node children per rate with stdout to a temp file, waits for their exit
 * and aggregates the SUMMARY lines into a table. serve is its worker on a second machine: it
 * takes commands over a Rant control domain and publishes its children's lines back. */
#include "bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
  #include <signal.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

#define SW_MAX_NODES   64
#define CTL_DOMAIN     9     /* keep the node --domain away from this */
#define CTL_CMD        0     /* control topic indices, in creation order */
#define CTL_RES        1
#define CTL_RES_MAX    1400
#define SW_MAX_RESULTS 128

typedef struct {
#ifdef _WIN32
    HANDLE h;
#else
    pid_t  pid;
#endif
    char   out[512];
    int    reaped;
} sw_child;

typedef struct { char keys[64][24]; char vals[64][40]; int n; } sw_kv;

/* parse "10000", "10k" or "1m" into an integer */
static long sw_num(const char *p){
    char *end; double v = strtod(p, &end);
    if (*end=='k'||*end=='K') v *= 1e3;
    else if (*end=='m'||*end=='M') v *= 1e6;
    return (long)v;
}

static void sw_kv_parse(const char *line, sw_kv *o){
    const char *p = line; o->n = 0;
    while (*p && o->n < 64){
        const char *start, *eq;
        while (*p==' '||*p=='\t'||*p=='\n'||*p=='\r') p++;
        if (!*p) break;
        start = p;
        while (*p && *p!=' ' && *p!='\t' && *p!='\n' && *p!='\r') p++;
        eq = start; while (eq < p && *eq != '=') eq++;
        if (eq < p){
            int kl=(int)(eq-start), vl=(int)(p-eq-1);
            if (kl>23) kl=23;
            if (vl>39) vl=39;
            memcpy(o->keys[o->n], start, (size_t)kl); o->keys[o->n][kl]=0;
            memcpy(o->vals[o->n], eq+1,  (size_t)vl); o->vals[o->n][vl]=0;
            o->n++;
        }
    }
}
static double sw_kv_get(const sw_kv *o, const char *key, double dflt){
    int i; for (i=0;i<o->n;i++) if (strcmp(o->keys[i],key)==0) return atof(o->vals[i]);
    return dflt;
}
static const char *sw_kv_gets(const sw_kv *o, const char *key){
    int i; for (i=0;i<o->n;i++) if (strcmp(o->keys[i],key)==0) return o->vals[i];
    return NULL;
}

/* read the last SUMMARY and SUMMARY2 lines out of a child's output file */
static int sw_read_summary(const char *path, sw_kv *sum, sw_kv *sum2){
    FILE *f = fopen(path, "rb"); char line[4096]; int got = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)){
        if (strncmp(line, "SUMMARY2 ", 9)==0) sw_kv_parse(line+9, sum2);
        else if (strncmp(line, "SUMMARY ", 8)==0){ sw_kv_parse(line+8, sum); got=1; }
    }
    fclose(f);
    return got;
}

static int sw_spawn(sw_child *c, const char *self, const char *name,
                    int domain, long rate, int dur, int mcast, int rel, int blk,
                    int xch, int spread, const char *ifip, const char *peerip){
    if (!ifip   || !*ifip)   ifip   = "0";
    if (!peerip || !*peerip) peerip = "0";
#ifdef _WIN32
    {
    SECURITY_ATTRIBUTES sa; STARTUPINFOA si; PROCESS_INFORMATION pi;
    HANDLE hout; char cmd[1024];
    memset(&sa, 0, sizeof sa); sa.nLength = sizeof sa; sa.bInheritHandle = TRUE;
    hout = CreateFileA(c->out, GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE,
                       &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hout == INVALID_HANDLE_VALUE) return -1;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hout; si.hStdError = hout;
    snprintf(cmd, sizeof cmd, "\"%s\" node %s %d %ld %d %d %d %d %d %d %s %s",
             self, name, domain, rate, dur, mcast, rel, blk, xch, spread, ifip, peerip);
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessA(self, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)){
        CloseHandle(hout); return -1;
    }
    CloseHandle(hout); CloseHandle(pi.hThread);
    c->h = pi.hProcess; c->reaped = 0;
    return 0;
    }
#else
    {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0){
        char sd[16], sr[24], su[16], sm[8], se[8], sb[16], sx[16], sp[8];
        snprintf(sd,sizeof sd,"%d",domain);  snprintf(sr,sizeof sr,"%ld",rate);
        snprintf(su,sizeof su,"%d",dur);     snprintf(sm,sizeof sm,"%d",mcast);
        snprintf(se,sizeof se,"%d",rel);     snprintf(sb,sizeof sb,"%d",blk);
        snprintf(sx,sizeof sx,"%d",xch);     snprintf(sp,sizeof sp,"%d",spread);
        if (!freopen(c->out, "w", stdout)) _exit(126);
        { char *av[] = { (char*)self, "node", (char*)name, sd, sr, su, sm, se, sb, sx, sp,
                         (char*)ifip, (char*)peerip, NULL };
          execvp(self, av); }
        _exit(127);
    }
    c->pid = pid; c->reaped = 0;
    return 0;
    }
#endif
}

/* The control plane for two machine sweeps, over Rant itself (spec/testing.md). */
static RantTopic *g_ctl_topics[2];
static char g_ctl_cmd[512];
static int  g_ctl_cmd_new = 0;
static char g_ctl_res[SW_MAX_RESULTS][CTL_RES_MAX];
static int  g_ctl_res_n = 0, g_ctl_done = 0, g_ctl_res_drop = 0;
static int  g_ctl_dom_filter = -1;   /* accept RESULTs for this domain only */
static char g_ctl_workers[16][24];
static int  g_ctl_nworkers = 0;

static void ctl_send(int topic, const char *text){
    rant_topic_send(g_ctl_topics[topic], rant_bytes(text, strlen(text)), NULL);
}

static void ctl_on_message(const RantMsg *msg){
    uint16_t ch = msg->topic_index; const void *d = msg->data.data; size_t len = msg->data.len;
    if (ch==CTL_CMD && len < sizeof g_ctl_cmd){
        memcpy(g_ctl_cmd, d, len); g_ctl_cmd[len]=0; g_ctl_cmd_new=1;
    } else if (ch==CTL_RES){
        /* every control peer flap replays the worker's whole RES history by design. Old rate
           replays must not eat inbox slots, so filter by the rate's domain up front */
        if (g_ctl_dom_filter >= 0 && len > 7 && !memcmp(d, "RESULT ", 7)){
            char head[48]; int dd = -1;
            size_t hl = len < sizeof head-1 ? len : sizeof head-1;
            memcpy(head, d, hl); head[hl] = 0;
            sscanf(head, "RESULT domain=%d", &dd);
            if (dd != g_ctl_dom_filter) return;
        }
        if (g_ctl_res_n < SW_MAX_RESULTS && len < CTL_RES_MAX){
            memcpy(g_ctl_res[g_ctl_res_n], d, len); g_ctl_res[g_ctl_res_n][len]=0;
            g_ctl_res_n++;
        } else g_ctl_res_drop++;
    }
}

static RantNode *ctl_open(uint16_t domain, int coordinator,
                          const char *if_ip, const char *peer_ip){
    static RantAddr seed;
    RantAllocator alloc = rant_allocator_heap(0);
    RantNodeOpts opts;
    RantTopicOpts co;
    RantNode *n;
    memset(&opts, 0, sizeof opts);
    opts.domain = domain;
    opts.max_topics = 2;
    opts.discovery.announce_interval_us = 500000;     /* ride through data floods */
    opts.discovery.peer_timeout_us = 10000000;
    bench_net(&opts, if_ip, peer_ip, &seed);
    n = rant_node_open(&alloc, NULL, ctl_on_message, NULL, &opts);
    if (!n) return NULL;
    /* CMD keeps catch_up 0 so stale commands never replay. RES keeps catch_up at depth so
       results survive a control peer flap */
    memset(&co, 0, sizeof co);
    co.qos.reliability       = RANT_RELIABLE;
    co.qos.keep_last         = 8;
    co.qos.max_message_bytes = sizeof g_ctl_cmd;
    g_ctl_topics[CTL_CMD] = rant_node_create_topic(n, "ctl/cmd",
                                coordinator ? RANT_PUB_ONLY : RANT_SUB_ONLY, NULL, &co);
    co.qos.keep_last         = SW_MAX_RESULTS;
    co.qos.catch_up          = SW_MAX_RESULTS;
    co.qos.max_message_bytes = CTL_RES_MAX;
    g_ctl_topics[CTL_RES] = rant_node_create_topic(n, "ctl/res",
                                coordinator ? RANT_SUB_ONLY : RANT_PUB_ONLY, NULL, &co);
    if (!g_ctl_topics[CTL_CMD] || !g_ctl_topics[CTL_RES]
        || rant_topic_index(g_ctl_topics[CTL_RES]) != CTL_RES){
        rant_node_close(n, 0);
        return NULL;
    }
    return n;
}

/* the IP of the first live control peer, for seeding the node children. NULL if none yet */
static const char *ctl_peer_ip(RantNode *ctl, char out[20]){
    RantIter it; RantPeerInfo p;
    memset(&it, 0, sizeof it);
    while (rant_node_peers_next(ctl, &it, &p)){
        size_t k = 0;
        if (p.liveness != RANT_PEER_ACTIVE) continue;
        while (k < p.address.len && k < 19 && p.address.data[k] != ':'){ out[k] = p.address.data[k]; k++; }
        out[k] = 0;
        return out;
    }
    return NULL;
}

/* log control peer transitions: which side lost whom, and when, is the first question in
   any two machine debugging session */
static void ctl_watch(const char *who, RantNode *ctl){
    static int had = 0;
    static uint64_t t0 = 0;
    char pb[20];
    int has = ctl_peer_ip(ctl, pb) != NULL;
    if (!t0) t0 = bench_now_ns();
    if (has != had){
        printf("%s: [t=%.1fs] control peer %s%s%s\n", who,
               (bench_now_ns()-t0)/1e9, has?"up (":"DOWN", has?pb:"", has?")":"");
        had = has;
    }
}

/* wait for the children to exit on their own, pumping the control node so the control plane
   stays live, and kill stragglers at the deadline. cmd is published again every 2 s */
static void sw_wait_pump(sw_child *cs, int n, int timeout_ms, RantNode *ctl, const char *cmd){
    uint64_t deadline = bench_now_ns() + (uint64_t)timeout_ms*1000000ull;
    uint64_t next_cmd = 0;
    int left = 0, i;
    for (i=0;i<n;i++) if (!cs[i].reaped) left++;
    while (left > 0 && bench_now_ns() < deadline){
        if (ctl) rant_node_poll(ctl, 20); else bench_sleep_ms(5);
        if (ctl && cmd && bench_now_ns() >= next_cmd){
            ctl_send(CTL_CMD, cmd);
            next_cmd = bench_now_ns() + 2000000000ull;
        }
        for (i=0;i<n;i++){
            if (cs[i].reaped) continue;
#ifdef _WIN32
            if (WaitForSingleObject(cs[i].h, 0) == WAIT_OBJECT_0){
                CloseHandle(cs[i].h); cs[i].reaped=1; left--;
            }
#else
            { int st;
              if (waitpid(cs[i].pid, &st, WNOHANG) == cs[i].pid){ cs[i].reaped=1; left--; } }
#endif
        }
    }
    for (i=0;i<n;i++){
        if (cs[i].reaped) continue;
#ifdef _WIN32
        TerminateProcess(cs[i].h, 1); CloseHandle(cs[i].h);
#else
        kill(cs[i].pid, SIGKILL); waitpid(cs[i].pid, NULL, 0);
#endif
        cs[i].reaped = 1;
    }
}

/* consume new inbox entries: HELLOs grow the worker set, RESULTs matching (domain, run)
   parse into sum and sum2 at *count. Returns the results consumed */
static int ctl_drain(int domain, unsigned long run, sw_kv *sum, sw_kv *sum2, int *count){
    int got = 0;
    for (; g_ctl_done < g_ctl_res_n; g_ctl_done++){
        const char *e = g_ctl_res[g_ctl_done];
        if (!strncmp(e, "HELLO ", 6)){
            int k, known = 0;
            for (k=0;k<g_ctl_nworkers;k++) if (!strcmp(g_ctl_workers[k], e+6)) known=1;
            if (!known && g_ctl_nworkers < 16){
                snprintf(g_ctl_workers[g_ctl_nworkers], sizeof g_ctl_workers[0], "%s", e+6);
                g_ctl_nworkers++;
                printf("sweep: worker %s joined\n", e+6);
            }
        } else if (!strncmp(e, "RESULT ", 7) && sum && count && *count < SW_MAX_RESULTS){
            int d = -1; unsigned long r = 0;
            sscanf(e, "RESULT domain=%d run=%lu", &d, &r);
            if (d==domain && r==run){
                const char *p1 = strstr(e, "\nSUMMARY "), *p2 = strstr(e, "\nSUMMARY2 ");
                char line[2048]; size_t L;
                if (!p1) continue;
                L = strcspn(p1+1, "\n"); if (L >= sizeof line) L = sizeof line-1;
                memcpy(line, p1+1, L); line[L]=0;
                sum[*count].n = 0; sum2[*count].n = 0;
                sw_kv_parse(line+8, &sum[*count]);
                /* a flap replays this rate's earlier results too: the same node reporting
                   twice must not skew the aggregates */
                { const char *nm = sw_kv_gets(&sum[*count], "name");
                  int k, dup = 0;
                  for (k=0;k<*count;k++){
                      const char *nm2 = sw_kv_gets(&sum[k], "name");
                      if (nm && nm2 && !strcmp(nm, nm2)){ dup=1; break; }
                  }
                  if (dup) continue;
                }
                if (p2){
                    L = strcspn(p2+1, "\n"); if (L >= sizeof line) L = sizeof line-1;
                    memcpy(line, p2+1, L); line[L]=0;
                    sw_kv_parse(line+9, &sum2[*count]);
                }
                (*count)++; got++;
            }
        }
    }
    return got;
}

/* the last SUMMARY and SUMMARY2 lines of a child's output, wrapped for the RES topic */
static int sw_read_result(const char *path, char *out, size_t cap, int domain, unsigned long run){
    FILE *f = fopen(path, "rb"); char line[1024], s1[1024]="", s2[1024]="";
    if (!f) return 0;
    while (fgets(line, sizeof line, f)){
        if (!strncmp(line, "SUMMARY2 ", 9)) snprintf(s2, sizeof s2, "%s", line);
        else if (!strncmp(line, "SUMMARY ", 8)) snprintf(s1, sizeof s1, "%s", line);
    }
    fclose(f);
    if (!s1[0]) return 0;
    snprintf(out, cap, "RESULT domain=%d run=%lu\n%s%s", domain, run, s1, s2);
    return 1;
}

/* this executable, the temp folder with a trailing separator and the process id */
static void sw_self(char **argv, const char **self, char *tmpdir, size_t cap, unsigned long *pid){
#ifdef _WIN32
    static char selfbuf[1024];
    (void)argv;
    GetModuleFileNameA(NULL, selfbuf, sizeof selfbuf);
    *self = selfbuf;
    GetTempPathA((DWORD)cap, tmpdir);
    *pid = (unsigned long)GetCurrentProcessId();
#else
    const char *td = getenv("TMPDIR");
    *self = argv[0];
    snprintf(tmpdir, cap, "%s/", td && *td ? td : "/tmp");
    *pid = (unsigned long)getpid();
#endif
}

int serve_main(int argc, char **argv){
    uint16_t dom = CTL_DOMAIN;
    char prefix[8], hello[32];
    const char *self; char tmpdir[260]; unsigned long mypid;
    const char *if_ip = NULL, *peer_arg = NULL;
    static sw_child cs[SW_MAX_NODES];
    RantNode *ctl;
    unsigned long last_run = (unsigned long)-1;
    int i;

    for (i=2;i<argc;i++){
        if (!strcmp(argv[i],"--domain") && i+1<argc) dom=(uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i],"--if")   && i+1<argc) if_ip   = argv[++i];
        else if (!strcmp(argv[i],"--peer") && i+1<argc) peer_arg = argv[++i];
    }
    sw_self(argv, &self, tmpdir, sizeof tmpdir, &mypid);
    snprintf(prefix, sizeof prefix, "w%02lu", mypid%100);

    ctl = ctl_open(dom, 0, if_ip, peer_arg);
    if (!ctl){ fprintf(stderr, "serve: control node open failed\n"); return 1; }
    snprintf(hello, sizeof hello, "HELLO %s", prefix);
    ctl_send(CTL_RES, hello);
    printf("serve: worker %s on control domain %u, waiting for sweeps\n", prefix, dom);

    for (;;){
        sw_kv c; unsigned long run;
        int domain, dur, mcast, rel, blk, xch, spread, nodes; long rate;
        rant_node_poll(ctl, 100);
        ctl_watch("serve", ctl);
        if (!g_ctl_cmd_new) continue;
        g_ctl_cmd_new = 0;
        c.n = 0; sw_kv_parse(g_ctl_cmd, &c);
        domain=(int)sw_kv_get(&c,"domain",-1);  rate=(long)sw_kv_get(&c,"rate",0);
        dur   =(int)sw_kv_get(&c,"dur",5);      mcast=(int)sw_kv_get(&c,"mcast",0);
        rel   =(int)sw_kv_get(&c,"rel",0);      blk  =(int)sw_kv_get(&c,"blk",0);
        xch   =(int)sw_kv_get(&c,"xch",0);      spread=(int)sw_kv_get(&c,"spread",0);
        nodes =(int)sw_kv_get(&c,"nodes",0);    run  =(unsigned long)sw_kv_get(&c,"run",0);
        if (domain < 0 || nodes <= 0 || nodes > SW_MAX_NODES) continue;
        if (run == last_run) continue;          /* the coordinator publishes again */
        last_run = run;
        printf("serve: run %lu: %d nodes, domain %d, %ld Hz, %ds\n", run, nodes, domain, rate, dur);
        /* seed the children with the coordinator's address so the test domain also
           bootstraps without multicast */
        { char pbuf[20];
          const char *cpeer = ctl_peer_ip(ctl, pbuf);
          if (!cpeer) cpeer = peer_arg;
          for (i=0;i<nodes;i++){
              char name[16];
              snprintf(cs[i].out, sizeof cs[i].out, "%srant_serve_%lu_n%d.txt", tmpdir, mypid, i);
              snprintf(name, sizeof name, "%s.%d", prefix, i);
              if (sw_spawn(&cs[i], self, name, domain, rate, dur, mcast, rel, blk, xch, spread,
                           if_ip, cpeer) != 0){
                  fprintf(stderr, "serve: spawn %d failed\n", i);
                  cs[i].reaped = 1;
              }
          } }
        sw_wait_pump(cs, nodes, (dur+10)*1000, ctl, NULL);
        for (i=0;i<nodes;i++){
            char res[CTL_RES_MAX];
            if (sw_read_result(cs[i].out, res, sizeof res, domain, run)) ctl_send(CTL_RES, res);
            remove(cs[i].out);
        }
        { uint64_t end = bench_now_ns()+1500000000ull;     /* flush and repair window */
          while (bench_now_ns() < end) rant_node_poll(ctl, 20); }
        printf("serve: run %lu done\n", run);
    }
}

/* aggregates over an array of parsed SUMMARY lines */
static double sw_avg(const sw_kv *a, int n, const char *k){
    double s=0; int i,c=0; for (i=0;i<n;i++){ s+=sw_kv_get(&a[i],k,0); c++; } return c?s/c:0; }
static double sw_sum(const sw_kv *a, int n, const char *k){
    double s=0; int i; for (i=0;i<n;i++) s+=sw_kv_get(&a[i],k,0); return s; }
static double sw_max(const sw_kv *a, int n, const char *k){
    double m=0; int i; for (i=0;i<n;i++){ double v=sw_kv_get(&a[i],k,0); if(v>m)m=v; } return m; }

int sweep_main(int argc, char **argv){
    int nodes=10, base_domain=20, dur=8, mcast=0, rel=0, blk=0, diag=0, xch=0, spread=0;
    int remote=0; uint16_t ctl_dom=CTL_DOMAIN; RantNode *ctl=NULL;
    const char *if_ip=NULL, *peer_arg=NULL;
    long rates[64]; int nrates=0, i, ri;
    const char *self;
    static sw_child  cs[SW_MAX_NODES];
    static sw_kv     sum[SW_MAX_RESULTS], sum2[SW_MAX_RESULTS];
    char tmpdir[260]; unsigned long mypid;

    for (i=2;i<argc;i++){
        if (!strcmp(argv[i],"--nodes")    && i+1<argc) nodes=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--domain")  && i+1<argc) base_domain=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--duration")&& i+1<argc) dur=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--mcast")   && i+1<argc) mcast=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--block-ms")&& i+1<argc) blk=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--extra-ch")&& i+1<argc) xch=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--spread"))   spread=1;
        else if (!strcmp(argv[i],"--void"))     spread=2;
        else if (!strcmp(argv[i],"--reliable")) rel=1;
        else if (!strcmp(argv[i],"--diag"))     diag=1;
        else if (!strcmp(argv[i],"--remote"))   remote=1;
        else if (!strcmp(argv[i],"--ctl-domain")&& i+1<argc) ctl_dom=(uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i],"--if")   && i+1<argc) if_ip=argv[++i];
        else if (!strcmp(argv[i],"--peer") && i+1<argc) peer_arg=argv[++i];
        else if (!strcmp(argv[i],"--rates")   && i+1<argc){
            const char *p = argv[++i];
            nrates = 0;
            while (*p && nrates<64){
                rates[nrates++]=sw_num(p);
                while (*p && *p!=',') p++;
                if (*p==',') p++;
            }
        }
        else fprintf(stderr, "sweep: ignoring unknown option '%s'\n", argv[i]);
    }
    if (nrates==0){ long d[]={0,1000,10000,50000,100000}; for (i=0;i<5;i++) rates[i]=d[i]; nrates=5; }
    if (nodes>SW_MAX_NODES){ nodes=SW_MAX_NODES; fprintf(stderr,"sweep: capping nodes at %d\n",SW_MAX_NODES); }
    sw_self(argv, &self, tmpdir, sizeof tmpdir, &mypid);

    if (remote){
        ctl = ctl_open(ctl_dom, 1, if_ip, peer_arg);
        if (!ctl){ fprintf(stderr, "sweep: control node open failed\n"); return 1; }
        printf("sweep: control domain %u, discovering workers...\n", ctl_dom);
        { uint64_t end = bench_now_ns()+3000000000ull;
          while (bench_now_ns() < end){ rant_node_poll(ctl, 50); ctl_drain(-1, 0, NULL, NULL, NULL); } }
        printf("sweep: %d worker(s), %d remote nodes per rate\n",
               g_ctl_nworkers, g_ctl_nworkers*nodes);
    }

    printf("sweep: %d nodes, %ds each, rates", nodes, dur);
    for (ri=0;ri<nrates;ri++) printf(" %ld", rates[ri]);
    printf("%s%s", rel?", reliable load":"", blk>0?" (block)":"");
    if (xch) printf(", +%d %s ch", xch, spread==2?"void":spread?"spread":"idle");
    printf("\n\n");
    printf("%10s %12s %10s %10s %10s %8s %9s %9s %6s\n",
           "TargetHz","Achieved/n","RTTavg ms","RTTmin ms","Jitter ms",
           "Drop%","Block ms","Stall ms","Nodes");

    for (ri=0; ri<nrates; ri++){
        long rate = rates[ri];
        int domain = base_domain + ri;     /* a distinct domain per rate */
        int count = 0, peers_n = 0, expect = nodes;
        unsigned long run = mypid*100u + (unsigned long)ri;
        double achieved, rttAvg=0, rttJit=0, rttMin=1e18, recv, drops, stall, blkMs, dpct;
        char cmd[256];

        if (remote){
            /* a fresh inbox per rate: flap replays of past rates are filtered at insert by
               domain, so slots stay free for this rate's results */
            g_ctl_dom_filter = domain;
            g_ctl_res_n = 0; g_ctl_done = 0; g_ctl_res_drop = 0;
            snprintf(cmd, sizeof cmd,
                "domain=%d rate=%ld dur=%d mcast=%d rel=%d blk=%d xch=%d spread=%d nodes=%d run=%lu",
                domain, rate, dur, mcast, rel, blk, xch, spread, nodes, run);
            ctl_send(CTL_CMD, cmd);
            for (i=0;i<10;i++) rant_node_poll(ctl, 1);     /* push it out now */
        }

        { char pbuf[20];
          const char *cpeer = remote ? ctl_peer_ip(ctl, pbuf) : NULL;
          if (!cpeer && remote) cpeer = peer_arg;
          for (i=0;i<nodes;i++){
              char name[16];
              snprintf(cs[i].out, sizeof cs[i].out, "%srant_sweep_%lu_r%ld_n%d.txt",
                       tmpdir, mypid, rate, i);
              snprintf(name, sizeof name, "n%d", i);
              if (sw_spawn(&cs[i], self, name, domain, rate, dur, mcast, rel, blk, xch, spread,
                           if_ip, cpeer) != 0){
                  fprintf(stderr, "sweep: failed to spawn node %d\n", i);
                  cs[i].reaped = 1;
              }
          } }
        sw_wait_pump(cs, nodes, (dur+10)*1000, ctl, remote ? cmd : NULL);

        /* collect: compact the parsed SUMMARYs into [0, count) of sum and sum2 */
        { sw_kv s, s2;
          for (i=0;i<nodes;i++){
              s.n = 0; s2.n = 0;
              if (sw_read_summary(cs[i].out, &s, &s2)){
                  sum[count] = s; sum2[count] = s2; count++;
              }
              remove(cs[i].out);
          }
        }
        if (remote){
            /* remote results: collected until every known worker reported its share or the
               window closes */
            uint64_t t0 = bench_now_ns(), next_cmd = 0; int rgot = 0;
            for (;;){
                uint64_t lim = (uint64_t)(g_ctl_nworkers ? 15 : 5)*1000000000ull;
                rant_node_poll(ctl, 50);
                ctl_watch("sweep", ctl);
                if (bench_now_ns() >= next_cmd){       /* keep recovered workers in sync */
                    ctl_send(CTL_CMD, cmd);
                    next_cmd = bench_now_ns() + 2000000000ull;
                }
                rgot += ctl_drain(domain, run, sum, sum2, &count);
                if (g_ctl_nworkers && rgot >= g_ctl_nworkers*nodes) break;
                if (bench_now_ns() - t0 > lim) break;
            }
            expect = nodes*(1+g_ctl_nworkers);
            if (g_ctl_nworkers && rgot < g_ctl_nworkers*nodes)
                printf("    note: %d/%d remote results (inbox %d, dropped %d)\n",
                       rgot, g_ctl_nworkers*nodes, g_ctl_res_n, g_ctl_res_drop);
        }
        if (count==0){ printf("%10ld   (no SUMMARY captured)\n", rate); continue; }

        for (i=0;i<count;i++){
            if (sw_kv_get(&sum[i],"peers",0) > 0){
                rttAvg += sw_kv_get(&sum[i],"rtt_avg_ms",0);
                rttJit += sw_kv_get(&sum[i],"rtt_jit_ms",0);
                peers_n++;
            }
            { double rm = sw_kv_get(&sum[i],"rtt_min_ms",0); if (rm>0 && rm<rttMin) rttMin=rm; }
        }

        achieved = sw_avg(sum, count, "sent_hz");
        recv     = sw_sum(sum, count, "recv");
        drops    = sw_sum(sum, count, "drops");
        stall    = sw_max(sum, count, "stall_ms");
        blkMs    = sw_avg(sum, count, "blk_wait_ms");
        dpct     = (recv+drops)>0 ? 100.0*drops/(recv+drops) : 0.0;
        if (peers_n){ rttAvg/=peers_n; rttJit/=peers_n; } else { rttAvg=rttJit=0; }
        if (rttMin>1e17) rttMin=0;

        printf("%10ld %12d %10.3f %10.3f %10.3f %8.2f %9.0f %9.0f %4d/%d\n",
               rate, (int)(achieved+0.5), rttAvg, rttMin, rttJit, dpct, blkMs, stall,
               count, expect);

        if (diag && count){
            double txDATA=sw_sum(sum2,count,"txDATA"), rxDATA=sw_sum(sum2,count,"rxDATA");
            double wire = txDATA>0 ? 100.0*(txDATA-rxDATA)/txDATA : 0.0;
            printf("    LOOP    iters/s %.0f  stage_ms %.0f  poll0_ms %.0f  poll1_ms %.0f  burst_capped %.1f  max_deficit %.0f\n",
                   sw_avg(sum2,count,"iters")/(dur>0?dur:1), sw_avg(sum2,count,"stage_ms"),
                   sw_avg(sum2,count,"poll0_ms"), sw_avg(sum2,count,"poll1_ms"),
                   sw_avg(sum2,count,"burst_capped"), sw_avg(sum2,count,"max_deficit"));
            printf("    SYSCALL tx %.0f/n (%.0f ms) wblock %.1f reset %.1f err %.1f   rx %.0f/n (%.0f ms) reset %.1f err %.1f\n",
                   sw_avg(sum2,count,"tx_calls"), sw_avg(sum2,count,"tx_ms"),
                   sw_avg(sum2,count,"tx_wouldblock"), sw_avg(sum2,count,"tx_reset"), sw_avg(sum2,count,"tx_err"),
                   sw_avg(sum2,count,"rx_calls"), sw_avg(sum2,count,"rx_ms"),
                   sw_avg(sum2,count,"rx_reset"), sw_avg(sum2,count,"rx_err"));
            printf("    TYPES   txDATA %.0f (probe %.0f / load %.0f) txNACK %.0f   rxDATA %.0f (load %.0f) rxNACK %.0f   wire loss %.2f%%\n",
                   txDATA, sw_sum(sum2,count,"txD_probe"), sw_sum(sum2,count,"txD_load"),
                   sw_sum(sum2,count,"txNACK"), rxDATA, sw_sum(sum2,count,"rxD_load"),
                   sw_sum(sum2,count,"rxNACK"), wire);
        }
    }
    printf("\nDone.\n");
    return 0;
}
