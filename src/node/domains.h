/* The census watch: which domains have nodes on this network, without being a node.
 * docs/discovery.md, Domains in use. spec/discovery.md, Census. */
#ifndef RANT_DOMAINS_H
#define RANT_DOMAINS_H

#include "../common/api.h"
#include "../common/alloc.h"
#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RANT_DOMAINS_MAX_NODES
#define RANT_DOMAINS_MAX_NODES 4096     /* nodes counted at once, past it a newcomer waits */
#endif

/* The watch options, passed as a compound literal. NULL means all defaults. */
typedef struct {
    uint16_t    discovery_port;       /* the nodes' discovery port, RANT_DISCOVERY_PORT */
    const char *multicast_interface;  /* hear on this interface IP only. NULL = every interface */
} RantDomainsOpts;

/* One domain with nodes on it. */
typedef struct {
    uint16_t domain;
    uint16_t nodes;      /* nodes heard on it within the peer timeout */
} RantDomainInfo;

typedef struct RantDomains RantDomains;

/* Opens a watch on alloc, which it copies and resets on close, so use one per watch. NULL on
 * failure, and rant_last_error(NULL) says why. */
RANT_API RantDomains *rant_domains_open(RantAllocator *alloc, const RantDomainsOpts *opts);
/* One domain per call in ascending order over a zero initialized RantIter, until 0. The first
 * call reads what arrived since the last walk, so walk at least once per announce interval. */
RANT_API int          rant_domains_next(RantDomains *w, RantIter *it, RantDomainInfo *out);
RANT_API void         rant_domains_close(RantDomains *w);

#ifdef __cplusplus
}
#endif
#endif /* RANT_DOMAINS_H */
