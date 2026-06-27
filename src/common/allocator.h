/* The runtime memory contract shared by the node and the standalone discovery
 * runtime: construct a DartAllocator and hand it to dart_node_open /
 * dart_discovery_open. Hoisted out of the node so discovery takes the same one.
 * A low-level header (no socket or clock); the IO-owning runtimes consume it. */
#ifndef DART_ALLOCATOR_H
#define DART_ALLOCATOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dynamic-mode growth ceiling when DartAllocator.max_bytes is 0: a runaway guard,
 * not a reservation. Define before the include to override. */
#ifndef DART_MEM_DEFAULT_MAX
#define DART_MEM_DEFAULT_MAX ((size_t)1 << 30)   /* 1 GiB */
#endif

/* The memory contract: construct one and hand it to a runtime open. One allocator
 * backs exactly one handle (claimed on open). Two modes, set by the constructor,
 * never by hand:
 *   static  - all memory is carved from your fixed buffer; no heap, no growth.
 *             For embedded (ESP32/Arduino). A bigger buffer admits more/larger
 *             work; exhaustion refuses the work rather than growing.
 *   dynamic - memory comes from the platform heap and buffers grow to fit, so a
 *             desktop caller need not pre-size anything.
 */
typedef struct {
    void   *buffer;     /* static: your block. dynamic: NULL (heap-backed) */
    size_t  size;       /* static: its size (hard budget). dynamic: initial size hint */
    size_t  max_bytes;  /* dynamic: growth ceiling (0 = DART_MEM_DEFAULT_MAX). static: ignored */
    uint8_t dynamic;    /* set by the constructor: 0 = static, 1 = dynamic */
    uint8_t claimed;    /* set when a handle takes ownership; reuse is then refused */
} DartAllocator;

/* Static: no heap, no growth; everything lives in buffer[0..size). */
DartAllocator dart_allocator_static(void *buffer, size_t size);
/* Dynamic: heap-backed, buffers grow to fit. size_hint pre-sizes the initial block
 * (advisory). Set .max_bytes on the result to override the ceiling. */
DartAllocator dart_allocator_dynamic(size_t size_hint);

#ifdef __cplusplus
}
#endif
#endif /* DART_ALLOCATOR_H */
