/* The contract of a fuzz target. Each tests/fuzz/<name>.c defines both functions, and builds
 * as fuzz_<name> under libFuzzer and as fuzz_<name>_replay everywhere. */
#ifndef RANT_FUZZ_H
#define RANT_FUZZ_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
/* Hands emit valid inputs that the live code builds, the start of a corpus */
typedef void (*FuzzEmit)(const uint8_t *data, size_t size);
void fuzz_seeds(FuzzEmit emit);

/* A property beyond not crashing. A failure aborts, which libFuzzer reports as a crash */
#define FUZZ_REQUIRE(cond) do { if (!(cond)) { \
        fprintf(stderr, "fuzz property failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        abort(); } } while (0)

/* An input is a run of chunks, each a little endian u16 length then that many bytes. A
   length past the end takes what is left */
typedef struct { const uint8_t *p; size_t left; } FuzzIn;
static inline int fuzz_chunk(FuzzIn *in, const uint8_t **data, size_t *len){
    size_t n;
    if (in->left < 2) return 0;
    n = (size_t)in->p[0] | ((size_t)in->p[1] << 8);
    in->p += 2; in->left -= 2;
    if (n > in->left) n = in->left;
    *data = in->p; *len = n;
    in->p += n; in->left -= n;
    return 1;
}

/* builds a seed one chunk at a time */
typedef struct { uint8_t buf[1 << 16]; size_t len; } FuzzSeed;
static inline void fuzz_seed_chunk(FuzzSeed *s, const void *data, size_t len){
    if (len > 0xFFFFu || s->len + 2 + len > sizeof s->buf) return;
    s->buf[s->len] = (uint8_t)len; s->buf[s->len + 1] = (uint8_t)(len >> 8);
    memcpy(s->buf + s->len + 2, data, len);
    s->len += 2 + len;
}

#endif /* RANT_FUZZ_H */
