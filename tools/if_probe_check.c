/* A diagnostic: the interfaces discovery joins and announces out of, plus what the OS
 * route table would have picked. spec/platform.md says why discovery ignores the latter. */
#define DART_TRANSPORT_IMPLEMENTATION
#define DART_NO_SHM
#include "dart_transport.h"
#include <stdio.h>

static void show(const char *label, uint32_t naddr){
    uint8_t ip[4];
    i_dart_plat_naddr_to_ip4(naddr, ip);
    printf("%-28s %u.%u.%u.%u\n", label, ip[0], ip[1], ip[2], ip[3]);
}

int main(void){
    uint32_t grp = i_dart_plat_parse_ip("239.255.0.7");
    i_DartIface ifs[16];
    int n, i;
    i_dart_plat_startup();
    show("route_src(group):",     i_dart_plat_route_src(grp, 7400));
    show("route_src(192.0.2.1):", i_dart_plat_route_src(i_dart_plat_ipv4(192,0,2,1), 7400));
    n = i_dart_plat_local_ifaces(ifs, 16);
    printf("discovery uses all %d interface(s):\n", n);
    for (i = 0; i < n; i++){
        uint8_t ip[4], mask[4];
        i_dart_plat_naddr_to_ip4(ifs[i].addr, ip);
        i_dart_plat_naddr_to_ip4(ifs[i].mask, mask);
        printf("  %u.%u.%u.%u mask %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3],
               mask[0], mask[1], mask[2], mask[3]);
    }
    i_dart_plat_cleanup();
    return 0;
}
