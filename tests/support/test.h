/* The one include of a C test program: the whole implementation with the wire tap, then the
 * harness. A program is one translation unit, so its cases reach every internal. */
#ifndef RANT_TEST_H
#define RANT_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rant_impl.h"
#include "harness.h"

/* the helpers that need the implementation's own declarations */

/* compiles text in a throwaway registry on a, so the schema lives until a is reset */
static inline RantSchema *test_compile(RantAllocator *a, const char *text, i_RantSchemaErr *err){
    i_RantRegistry r;
    i_rant_registry_init(&r, rant_allocator_alloc, a);
    return i_rant_registry_compile(&r, text, err);
}

/* the discovery peer table behind a node, for raw announce checks. The public walk is
   rant_node_peers_next */
static inline const i_RantDiscoveryPeerView *test_peers(RantNode *n, uint16_t *count){
    return i_rant_discovery_peers(n->discovery, count);
}

#endif /* RANT_TEST_H */
