/* The allocation hook: a realloc-style callback a memory-taking core calls for growable
 * buffers (ptr NULL = alloc, size 0 = free, else resize). A runtime supplies one derived
 * from its DartAllocator (static bump or dynamic heap), so the core stays memory-policy
 * agnostic. Shared by the transport core and the serializer. */
#ifndef DART_ALLOC_H
#define DART_ALLOC_H

#include <stddef.h>

typedef void *(*DartAllocFn)(void *user, void *ptr, size_t size);

#endif /* DART_ALLOC_H */
