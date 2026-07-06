/* The DART implementation anchor. The DART library compiles cleanly as C++, so
 * this is a plain C++ TU: DART_IMPLEMENTATION makes dart.hpp emit the library
 * implementation (at global scope, with C linkage) that the wrapper links
 * against. Exactly one TU in the program defines DART_IMPLEMENTATION; it can be
 * this file or your main.cpp. No C compiler is involved. */
#define DART_IMPLEMENTATION
#include "dart.hpp"
