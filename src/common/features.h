/* Build feature detection, shared by every layer. A NO flag always wins. */
#ifndef RANT_FEATURES_H
#define RANT_FEATURES_H

/* RANT_SHM is auto detected where the bundled platform layer has it. */
#if !defined(RANT_SHM) && !defined(RANT_NO_SHM)
  #if defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || \
      defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
      defined(__DragonFly__)
    #define RANT_SHM
  #endif
#endif
#if defined(RANT_SHM) && defined(RANT_NO_SHM)
  #undef RANT_SHM                /* both set, the opt out wins */
#endif

/* RANT_THREADS follows the same shape. A new platform layer that implements the thread
 * contract of platform/core.h defines it itself. */
#if !defined(RANT_THREADS) && !defined(RANT_NO_THREADS)
  #if defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || \
      defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
      defined(__DragonFly__) || defined(ESP_PLATFORM)
    #define RANT_THREADS
  #elif defined(__has_include)
    #if __has_include(<pthread.h>)
      #define RANT_THREADS     /* unknown POSIX with pthreads */
    #endif
  #endif
#endif
#if defined(RANT_THREADS) && defined(RANT_NO_THREADS)
  #undef RANT_THREADS          /* both set, the opt out wins */
#endif

/* RANT_PROC_STATS too. When off the platform's two stats functions are absent and every
 * consumer compiles out with them, so a layer that cannot measure implements nothing. */
#if !defined(RANT_PROC_STATS) && !defined(RANT_NO_PROC_STATS)
  #if defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || \
      defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
      defined(__DragonFly__) || defined(ESP_PLATFORM)
    #define RANT_PROC_STATS
  #endif
#endif
#if defined(RANT_PROC_STATS) && defined(RANT_NO_PROC_STATS)
  #undef RANT_PROC_STATS         /* both set, the opt out wins */
#endif

#endif /* RANT_FEATURES_H */
