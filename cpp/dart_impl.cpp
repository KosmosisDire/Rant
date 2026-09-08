/* The implementation anchor: DART_IMPLEMENTATION makes dart.hpp emit the C library at
 * global scope. Exactly one translation unit does this (docs/cpp.md). */
#define DART_IMPLEMENTATION
#include "dart.hpp"
