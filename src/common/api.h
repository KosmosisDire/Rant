/* The linkage of every public entry point. spec/build.md explains the three builds. */
#ifndef RANT_API_H
#define RANT_API_H

/* Default: the single header build, where the caller compiles Rant into its own binary
 * and needs no decoration. RANT_BUILD_SHARED builds the shared library, RANT_LINK_SHARED
 * consumes one. */
#if defined(RANT_BUILD_SHARED)
  #if defined(_WIN32)
    #define RANT_API __declspec(dllexport)
  #else
    #define RANT_API __attribute__((visibility("default")))
  #endif
#elif defined(RANT_LINK_SHARED) && defined(_WIN32)
  #define RANT_API __declspec(dllimport)
#else
  #define RANT_API
#endif

#endif /* RANT_API_H */
