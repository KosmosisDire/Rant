/* FNV-1a 64 for topic and schema identities. */
#ifndef RAMBLE_HASH_H
#define RAMBLE_HASH_H

#include <stddef.h>
#include <stdint.h>

/* The basis is not the textbook one. On wire identities depend on it, so it never changes. */
static inline uint64_t i_ramble_fnv1a64(const void *data, size_t n){
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ull; size_t i;
    for (i = 0; i < n; i++){ h ^= (uint64_t)p[i]; h *= 1099511628211ull; }
    return h;
}

/* Over a C string. NULL gives 0. */
static inline uint64_t i_ramble_fnv1a64_str(const char *s){
    uint64_t h = 1469598103934665603ull;
    const unsigned char *p = (const unsigned char *)s;
    if (!s) return 0;
    for (; *p; p++){ h ^= (uint64_t)*p; h *= 1099511628211ull; }
    return h;
}

#endif /* RAMBLE_HASH_H */
