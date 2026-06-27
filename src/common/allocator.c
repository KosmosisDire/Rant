/* DartAllocator constructors. See common/allocator.h. A runtime copies what it
 * needs at open, so the DartAllocator value itself need not outlive the call (a
 * static buffer must). Shared by the node and discovery runtimes. */
#include "allocator.h"
#include <string.h>

DartAllocator dart_allocator_static(void *buffer, size_t size){
    DartAllocator a; memset(&a, 0, sizeof a);
    a.buffer = buffer; a.size = size; a.dynamic = 0;
    return a;
}
DartAllocator dart_allocator_dynamic(size_t size_hint){
    DartAllocator a; memset(&a, 0, sizeof a);
    a.size = size_hint; a.dynamic = 1;
    return a;
}
