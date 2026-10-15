/* Compiles the whole Rant implementation into this translation unit with its socket calls
 * routed through the wire tap. A test or bench program includes it exactly once, which also
 * puts every internal in reach. */
#ifndef RANT_IMPL_H
#define RANT_IMPL_H

#include "wire_tap.h"

#define bind     wire_tap_bind
#define sendto   wire_tap_sendto
#define recvfrom wire_tap_recvfrom
#define RANT_IMPLEMENTATION
#include "rant.h"
#undef RANT_IMPLEMENTATION
#undef bind
#undef sendto
#undef recvfrom

#endif /* RANT_IMPL_H */
