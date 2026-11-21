/* sendbench: the raw winsock send cost on loopback, unconnected, connected and multicast
 * against the number of local joiners. Windows charges a multicast sender per local delivery,
 * which is why Rant data is unicast. */
#include "bench.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32

#define SB_N 200000

static double sb_end(LARGE_INTEGER a, int n){
    LARGE_INTEGER b, f;
    QueryPerformanceCounter(&b); QueryPerformanceFrequency(&f);
    return (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)f.QuadPart / n;
}

int sendbench_main(void){
    WSADATA wsa;
    char buf[1024];
    LARGE_INTEGER t0;
    int i, rb = 8<<20;
    SOCKET sink, tx;
    struct sockaddr_in sa;

    WSAStartup(MAKEWORD(2,2), &wsa);
    memset(buf, 0xAB, sizeof buf);

    /* a sink socket on a known port with a big receive buffer, never read */
    sink = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(47123);
    setsockopt(sink, SOL_SOCKET, SO_RCVBUF, (const char*)&rb, sizeof rb);
    if (bind(sink, (struct sockaddr*)&sa, sizeof sa) != 0){ printf("bind fail\n"); return 1; }

    tx = socket(AF_INET, SOCK_DGRAM, 0);

    QueryPerformanceCounter(&t0);
    for (i=0;i<SB_N;i++) sendto(tx, buf, 70, 0, (struct sockaddr*)&sa, sizeof sa);
    printf("unconnected sendto 70B  : %.2f us/call\n", sb_end(t0, SB_N));

    { SOCKET txc = socket(AF_INET, SOCK_DGRAM, 0);
      connect(txc, (struct sockaddr*)&sa, sizeof sa);
      QueryPerformanceCounter(&t0);
      for (i=0;i<SB_N;i++) send(txc, buf, 70, 0);
      printf("connected   send   70B  : %.2f us/call\n", sb_end(t0, SB_N));
      closesocket(txc); }

    QueryPerformanceCounter(&t0);
    for (i=0;i<SB_N;i++) sendto(tx, buf, 1024, 0, (struct sockaddr*)&sa, sizeof sa);
    printf("unconnected sendto 1024B: %.2f us/call\n", sb_end(t0, SB_N));

    { int joiners[] = {0, 1, 5, 10};
      SOCKET js[10]; int nj = 0, k, ji;
      unsigned long lo = inet_addr("127.0.0.1");
      SOCKET txm = socket(AF_INET, SOCK_DGRAM, 0);
      struct sockaddr_in ma;
      unsigned char ttl = 1, loop = 1;
      int onx = 1, nm = SB_N/10;
      memset(&ma, 0, sizeof ma);
      ma.sin_family = AF_INET; ma.sin_addr.s_addr = inet_addr("239.255.0.99");
      ma.sin_port = htons(47124);
      setsockopt(txm, IPPROTO_IP, IP_MULTICAST_IF,   (const char*)&lo,   sizeof lo);
      setsockopt(txm, IPPROTO_IP, IP_MULTICAST_TTL,  (const char*)&ttl,  sizeof ttl);
      setsockopt(txm, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof loop);
      for (k=0;k<4;k++){
          while (nj < joiners[k]){
              struct sockaddr_in ja; struct ip_mreq mr;
              SOCKET j = socket(AF_INET, SOCK_DGRAM, 0);
              setsockopt(j, SOL_SOCKET, SO_REUSEADDR, (const char*)&onx, sizeof onx);
              setsockopt(j, SOL_SOCKET, SO_RCVBUF, (const char*)&rb, sizeof rb);
              memset(&ja,0,sizeof ja);
              ja.sin_family=AF_INET; ja.sin_addr.s_addr=htonl(INADDR_ANY);
              ja.sin_port=htons(47124);
              bind(j,(struct sockaddr*)&ja,sizeof ja);
              memset(&mr,0,sizeof mr);
              mr.imr_multiaddr.s_addr=inet_addr("239.255.0.99");
              mr.imr_interface.s_addr=lo;
              setsockopt(j,IPPROTO_IP,IP_ADD_MEMBERSHIP,(const char*)&mr,sizeof mr);
              js[nj++]=j;
          }
          QueryPerformanceCounter(&t0);
          for (i=0;i<nm;i++) sendto(txm, buf, 70, 0, (struct sockaddr*)&ma, sizeof ma);
          printf("multicast   sendto 70B  : %.2f us/call  (%d local joiners)\n",
                 sb_end(t0, nm), joiners[k]);
      }
      for (ji=0;ji<nj;ji++) closesocket(js[ji]);
      closesocket(txm); }

    closesocket(tx); closesocket(sink);
    WSACleanup();
    return 0;
}

#else

int sendbench_main(void){
    fprintf(stderr, "sendbench is Windows only: it measures winsock loopback costs\n");
    return 1;
}

#endif
