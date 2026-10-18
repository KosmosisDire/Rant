/* A diagnostic: the interfaces discovery joins and announces out of, plus what the OS
 * route table would have picked. spec/platform.md says why discovery ignores the latter. */
#define RANT_IMPLEMENTATION
#define RANT_NO_SHM
#include "rant.h"
#include <stdio.h>

static void show(const char *label, uint32_t naddr){
    uint8_t ip[4];
    i_rant_plat_naddr_to_ip4(naddr, ip);
    printf("%-28s %u.%u.%u.%u\n", label, ip[0], ip[1], ip[2], ip[3]);
}

int main(void){
    uint32_t base = RANT_DISCOVERY_GROUP_BASE;     /* domain 0 */
    uint32_t grp = i_rant_plat_ipv4((uint8_t)(base >> 24), (uint8_t)(base >> 16),
                                    (uint8_t)(base >> 8), (uint8_t)base);
    i_RantIface ifs[16];
    int n, i;
    i_rant_plat_startup();
    show("route_src(group):",     i_rant_plat_route_src(grp, RANT_DISCOVERY_PORT));
    show("route_src(192.0.2.1):", i_rant_plat_route_src(i_rant_plat_ipv4(192,0,2,1), RANT_DISCOVERY_PORT));
    n = i_rant_plat_local_ifaces(ifs, 16);
    printf("discovery uses all %d interface(s):\n", n);
    for (i = 0; i < n; i++){
        uint8_t ip[4], mask[4];
        i_rant_plat_naddr_to_ip4(ifs[i].addr, ip);
        i_rant_plat_naddr_to_ip4(ifs[i].mask, mask);
        printf("  %u.%u.%u.%u mask %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3],
               mask[0], mask[1], mask[2], mask[3]);
    }
    i_rant_plat_cleanup();
    return 0;
}
