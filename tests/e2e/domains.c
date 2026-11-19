/* The census watch: it counts the nodes of each domain without being one, drops a node that
 * says BYE at once, and refuses what it cannot open. spec/discovery.md, Census. */
#include "support/test.h"

static RantNode *dm_node(uint16_t domain, const char *name){
    RantAllocator a = rant_allocator_heap(0);
    RantNode *n = rant_node_open(&a, name, NULL, NULL, &(RantNodeOpts){ .domain = domain, .disable_shm = 1,
        .net = { .multicast_interface = "127.0.0.1" } });
    if (n) rant_node_start(n);
    return n;
}

static RantDomains *dm_watch(void){
    RantAllocator a = rant_allocator_heap(0);
    return rant_domains_open(&a, &(RantDomainsOpts){ .multicast_interface = "127.0.0.1" });
}

/* The nodes the watch counts on domain, -1 when it lists none. */
static int dm_nodes(RantDomains *w, uint16_t domain){
    RantIter it; RantDomainInfo d; int nodes = -1, last = -1, sorted = 1;
    memset(&it, 0, sizeof it);
    while (rant_domains_next(w, &it, &d)){
        if ((int)d.domain <= last) sorted = 0;
        last = d.domain;
        if (d.domain == domain) nodes = d.nodes;
    }
    return sorted ? nodes : -2;
}

static void counts_checks(void){
    RantDomains *w = dm_watch();
    RantNode *a, *b, *c;
    uint16_t one = (uint16_t)(TEST_DOMAIN + 1), two = (uint16_t)(TEST_DOMAIN + 2);
    CHECK(w != NULL, "counts: a watch opens on loopback");
    if (!w) return;
    CHECK(dm_nodes(w, one) == -1, "counts: an empty domain is not listed");
    a = dm_node(one, "dm-a"); b = dm_node(one, "dm-b"); c = dm_node(two, "dm-c");
    CHECK(a && b && c, "counts: three nodes open");
    WAIT_UNTIL(2000, dm_nodes(w, one) == 2 && dm_nodes(w, two) == 1);
    CHECK(dm_nodes(w, one) == 2 && dm_nodes(w, two) == 1,
          "counts: two nodes on one domain and one on another, heard from their openings (%d, %d)",
          dm_nodes(w, one), dm_nodes(w, two));
    if (b) rant_node_close(b, 1);
    WAIT_UNTIL(1000, dm_nodes(w, one) == 1);
    CHECK(dm_nodes(w, one) == 1, "counts: a node that says BYE leaves the count at once (%d)", dm_nodes(w, one));
    if (c) rant_node_close(c, 1);
    WAIT_UNTIL(1000, dm_nodes(w, two) == -1);
    CHECK(dm_nodes(w, two) == -1, "counts: a domain whose last node left is not listed");
    if (a) rant_node_close(a, 1);
    rant_domains_close(w);
}

/* Two watches on one host both hear the census, as an explorer and a bridge do. */
static void shared_checks(void){
    RantDomains *w1 = dm_watch(), *w2 = dm_watch();
    uint16_t domain = (uint16_t)(TEST_DOMAIN + 3);
    RantNode *n;
    CHECK(w1 && w2, "shared: two watches open on one port");
    n = dm_node(domain, "dm-shared");
    WAIT_UNTIL(2000, w1 && w2 && dm_nodes(w1, domain) == 1 && dm_nodes(w2, domain) == 1);
    CHECK(w1 && w2 && dm_nodes(w1, domain) == 1 && dm_nodes(w2, domain) == 1, "shared: both hear the node");
    if (n) rant_node_close(n, 1);
    rant_domains_close(w1); rant_domains_close(w2);
}

static void refused_checks(void){
    RantAllocator a = rant_allocator_heap(0);
    CHECK(rant_domains_open(NULL, NULL) == NULL && rant_last_error(NULL).error == RANT_E_OOM,
          "refused: no allocator");
    CHECK(rant_domains_open(&a, &(RantDomainsOpts){ .discovery_port = 65534 }) == NULL
          && rant_last_error(NULL).error == RANT_E_BAD_ADDRESS, "refused: a census port past 65535");
    rant_domains_close(NULL);
}

static const TestCase cases[] = {
    { "counts",  counts_checks },
    { "shared",  shared_checks },
    { "refused", refused_checks },
};

TEST_MAIN(cases)
