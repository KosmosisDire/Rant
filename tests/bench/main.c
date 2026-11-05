/* rant_bench: dispatches one command. The only translation unit with the implementation. */
#include "../support/rant_impl.h"
#include "bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
  #include <timeapi.h>
#else
  #include <arpa/inet.h>
  #include <time.h>
#endif

uint64_t bench_now_ns(void){
    return i_rant_plat_now_us() * 1000u;
}

void bench_sleep_ms(int ms){
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

void bench_net(RantNodeOpts *opts, const char *if_ip, const char *peer_ip, RantAddr *seed){
    if (if_ip) opts->net.multicast_interface = if_ip;
    if (peer_ip){
        uint32_t a4 = inet_addr(peer_ip);
        if (a4 != INADDR_NONE){
            memset(seed, 0, sizeof *seed);
            memcpy(seed->ip, &a4, 4);
            seed->ip_len = 4;       /* port 0 is the discovery port */
            opts->net.seed_peers = seed;
            opts->net.n_seed_peers = 1;
        }
    }
}

static const char USAGE[] =
    "usage: rant_bench <command> [args]\n"
    "  node <name> [domain] [load_hz] [duration_s] [if_mode] [reliable] [block_ms]\n"
    "       [extra_ch] [spread] [if_ip] [peer_ip]\n"
    "        one latency and throughput node, SUMMARY and SUMMARY2 lines on a timed exit.\n"
    "        if_mode 1 pins discovery to loopback, 2 uses the real interfaces. reliable 1\n"
    "        makes the load topic reliable and block_ms is its backpressure window. extra_ch\n"
    "        declares N more topics, spread 1 round robins the load across them, 0 leaves\n"
    "        them idle, 2 makes them pub only everywhere. if_ip pins discovery to one\n"
    "        interface, peer_ip seeds discovery without multicast, \"0\" leaves either unset.\n"
    "        env: RANT_DIAG_RCVBUF and RANT_DIAG_SNDBUF (bytes), RANT_DIAG_TRACE\n"
    "  sweep [--nodes N] [--domain D] [--duration S] [--rates a,b,c] [--mcast 0|1|2]\n"
    "        [--reliable] [--block-ms N] [--extra-ch N] [--spread] [--void] [--remote]\n"
    "        [--ctl-domain D] [--if IP] [--peer IP] [--diag]\n"
    "        spawns N node children per rate and prints an RTT against throughput table.\n"
    "        --remote also commands every serve worker on the LAN to run N nodes per rate\n"
    "  serve [--domain D] [--if IP] [--peer IP]\n"
    "        the two machine sweep worker on the control domain (default 9). Machines must\n"
    "        share a subnet\n"
    "  sendbench    UDP send cost on loopback (Windows)\n"
    "  memscale     heap use and send cost over payload, depth and subscribers, exit 1 when\n"
    "               a steady state send allocates\n"
    "  threadbench  the threaded send path at paced and flat out rates\n"
    "  queuebench   callback queue dispatch against inline callbacks\n";

int main(int argc, char **argv){
    const char *cmd = argc >= 2 ? argv[1] : "";
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
    timeBeginPeriod(1);   /* millisecond sleeps in the pacing loops */
#endif
    if (!strcmp(cmd, "node"))      return node_main(argc - 1, argv + 1);
    if (!strcmp(cmd, "sweep"))     return sweep_main(argc, argv);
    if (!strcmp(cmd, "serve"))     return serve_main(argc, argv);
    if (!strcmp(cmd, "sendbench")) return sendbench_main();
    if (!strcmp(cmd, "memscale"))  return memscale_main();
    if (!strcmp(cmd, "threadbench")) return threadbench_main();
    if (!strcmp(cmd, "queuebench"))  return queuebench_main();
    fputs(USAGE, stderr);
    return 2;
}
