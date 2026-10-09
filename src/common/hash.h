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

/* Entity names match without regard to ASCII case: the identity hashes the lower case and
 * i_rant_name_eq compares folded. The spelling a name was made with is kept for display. */
static inline uint8_t i_rant_name_fold(uint8_t c){ return (uint8_t)(c >= 'A' && c <= 'Z' ? c + 32 : c); }

static inline uint64_t i_rant_name_hash(const void *data, size_t n){
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ull; size_t i;
    for (i = 0; i < n; i++){ h ^= (uint64_t)i_rant_name_fold(p[i]); h *= 1099511628211ull; }
    return h;
}

static inline int i_rant_name_eq(const void *a, size_t a_len, const void *b, size_t b_len){
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b; size_t i;
    if (a_len != b_len) return 0;
    for (i = 0; i < a_len; i++) if (i_rant_name_fold(x[i]) != i_rant_name_fold(y[i])) return 0;
    return 1;
}

#endif /* RANT_HASH_H */
