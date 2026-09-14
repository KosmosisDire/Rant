/* Links against the built library with no RAMBLE_IMPLEMENTATION and no anchor of its own,
 * which is the whole point of ramble::ramble_host. It opens no sockets. */
#include "ramble.h"

int main(void){
    RambleAllocator a = ramble_allocator_heap(0);
    return a.page_realloc ? 0 : 1;
}
