/* FNV-1a 64 for topic and schema identities. */
#ifndef RANT_HASH_H
#define RANT_HASH_H

#include <stddef.h>
#include <stdint.h>

/* The basis is not the textbook one. On wire identities depend on it, so it never changes. */
static inline uint64_t i_rant_fnv1a64(const void *data, size_t n){
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ull; size_t i;
    for (i = 0; i < n; i++){ h ^= (uint64_t)p[i]; h *= 1099511628211ull; }
    return h;
}

/* A value below n from a hash. The high half is the well mixed one: the low bits of FNV
 * follow the low bits of each byte alone. */
static inline uint32_t i_rant_hash_below(uint64_t h, uint32_t n){
    return (uint32_t)(((h >> 32) * (uint64_t)n) >> 32);
}

/* The splitmix64 finalizer: every input bit reaches every output bit. */
static inline uint64_t i_rant_hash_mix64(uint64_t z){
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Over a C string. NULL gives 0. */
static inline uint64_t i_rant_fnv1a64_str(const char *s){
    uint64_t h = 1469598103934665603ull;
    const unsigned char *p = (const unsigned char *)s;
    if (!s) return 0;
    for (; *p; p++){ h ^= (uint64_t)*p; h *= 1099511628211ull; }
    return h;
}

#endif /* RANT_HASH_H */
