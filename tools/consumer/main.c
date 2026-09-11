/* Links against the built library with no DART_IMPLEMENTATION and no anchor of its own,
 * which is the whole point of dart::dart_host. It opens no sockets. */
#include "dart.h"

int main(void){
    DartAllocator a = dart_allocator_heap(0);
    return a.page_realloc ? 0 : 1;
}
