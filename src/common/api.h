/* The linkage of every public entry point. spec/build.md explains the three builds. */
#ifndef DART_API_H
#define DART_API_H

/* Default: the single header build, where the caller compiles DART into its own binary
 * and needs no decoration. DART_BUILD_SHARED builds the shared library, DART_LINK_SHARED
 * consumes one. */
#if defined(DART_BUILD_SHARED)
  #if defined(_WIN32)
    #define DART_API __declspec(dllexport)
  #else
    #define DART_API __attribute__((visibility("default")))
  #endif
#elif defined(DART_LINK_SHARED) && defined(_WIN32)
  #define DART_API __declspec(dllimport)
#else
  #define DART_API
#endif

#endif /* DART_API_H */
