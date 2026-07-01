/* throwaway: prove dart_discovery_mcast_if_for picks the LAN interface, not loopback */
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
    uint32_t ifs[16];
    int n, i;
    i_dart_plat_startup();
    show("raw route_src(group):",   i_dart_plat_route_src(grp, 7400));
    show("route_src(192.0.2.1):",   i_dart_plat_route_src(i_dart_plat_ipv4(192,0,2,1), 7400));
    show("mcast_if_for(group) =>",  dart_discovery_mcast_if_for(grp, 7400));
    n = i_dart_plat_local_ipv4s(ifs, 16);
    printf("enumerated %d interface(s):\n", n);
    for (i = 0; i < n; i++) show("  iface:", ifs[i]);
    i_dart_plat_cleanup();
    return 0;
}
