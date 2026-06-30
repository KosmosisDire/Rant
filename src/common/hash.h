/* Shared FNV-1a 64 for a stable 64-bit identity from bytes (topic ids, schema ids).
 * static inline: no link symbol and no unused-function warning in a layer that doesn't use
 * it. The amalgamator emits it once per implementation TU; the local #include is for
 * standalone compilation of a layer. The basis/prime match DART's existing topic ids (so
 * on-wire identities are unchanged), not the textbook offset basis. */
#ifndef DART_HASH_H
#define DART_HASH_H

#include <stddef.h>
#include <stdint.h>

static inline uint64_t dart_fnv1a64(const void *data, size_t n){
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ull; size_t i;
    for (i = 0; i < n; i++){ h ^= (uint64_t)p[i]; h *= 1099511628211ull; }
    return h;
}

/* Over a NUL-terminated string (the topic-name identity form); NULL => 0. */
static inline uint64_t dart_fnv1a64_str(const char *s){
    uint64_t h = 1469598103934665603ull;
    const unsigned char *p = (const unsigned char *)s;
    if (!s) return 0;
    for (; *p; p++){ h ^= (uint64_t)*p; h *= 1099511628211ull; }
    return h;
}

#endif /* DART_HASH_H */
