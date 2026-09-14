/* The linkage of every public entry point. spec/build.md explains the three builds. */
#ifndef RAMBLE_API_H
#define RAMBLE_API_H

/* Default: the single header build, where the caller compiles Ramble into its own binary
 * and needs no decoration. RAMBLE_BUILD_SHARED builds the shared library, RAMBLE_LINK_SHARED
 * consumes one. */
#if defined(RAMBLE_BUILD_SHARED)
  #if defined(_WIN32)
    #define RAMBLE_API __declspec(dllexport)
  #else
    #define RAMBLE_API __attribute__((visibility("default")))
  #endif
#elif defined(RAMBLE_LINK_SHARED) && defined(_WIN32)
  #define RAMBLE_API __declspec(dllimport)
#else
  #define RAMBLE_API
#endif

#endif /* RAMBLE_API_H */
