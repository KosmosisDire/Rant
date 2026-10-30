/* The one include of a C test program: the whole implementation with the wire tap, then the
 * harness. A program is one translation unit, so its cases reach every internal. */
#ifndef RANT_TEST_H
#define RANT_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rant_impl.h"
#include "harness.h"
#include "atomic.h"

/* the helpers that need the implementation's own declarations */

/* compiles text in a throwaway registry on a, so the schema lives until a is reset */
static inline RantSchema *test_compile(RantAllocator *a, const char *text, i_RantSchemaErr *err){
    i_RantRegistry r;
    i_rant_registry_init(&r, rant_allocator_alloc, a);
    return i_rant_registry_compile(&r, text, err);
}

#ifndef RANT_NO_STDTYPES
/* a standard type as a schema file spells it, its description lines above Name and its body.
   The length written, 0 when it does not fit. */
static inline size_t test_std_def(RantStdType t, char *out, size_t cap){
    const char *txt = rant_std_text(t), *body = txt, *name = rant_std_name(t);
    int n;
    if (!txt || !name) return 0;
    while (body[0] == '-' && body[1] == '-'){ body = strchr(body, '\n'); body = body ? body + 1 : ""; }
    n = snprintf(out, cap, "%.*s%s%s%s\n", (int)(body - txt), txt, name, body[0] == '{' ? " " : " = ", body);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}
#endif

/* the slotted peers behind a node with the blobs they hold, for raw announce checks. Every
   known peer is the public walk rant_node_peers_next */
static inline const i_RantDiscoveryPeerView *test_peers(RantNode *n, uint16_t *count){
    return i_rant_discovery_peers(n->discovery, count);
}

#endif /* RANT_TEST_H */
