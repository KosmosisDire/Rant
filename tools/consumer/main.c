/* Links against the built library with no RANT_IMPLEMENTATION and no anchor of its own,
 * which is the whole point of rant::rant_host. It opens no sockets. */
#include "rant.h"

int main(void){
    RantAllocator a = rant_allocator_heap(0);
    return a.page_realloc ? 0 : 1;
}
