/* The two view types. Neither owns, copies or implies a NUL terminator. */
#ifndef RANT_STRING_H
#define RANT_STRING_H

#include <stddef.h>
#include <stdint.h>

/* A read only run of bytes. */
typedef struct {
    const uint8_t *data;
    size_t         len;
} RantBytes;

/* A run of string bytes with no NUL terminator. Never use %s or strlen on it. */
typedef struct {
    const char *data;
    size_t      len;
} RantString;

static inline RantBytes rant_bytes(const void *data, size_t len){
    RantBytes b; b.data = (const uint8_t *)data; b.len = len; return b;
}
static inline RantString rant_string(const char *data, size_t len){
    RantString s; s.data = data; s.len = len; return s;
}
/* From a C string. NULL gives an empty string. */
static inline RantString rant_cstr(const char *s){
    RantString r; size_t n = 0;
    if (s) while (s[n]) n++;
    r.data = s; r.len = n; return r;
}
static inline int rant_string_eq(RantString a, RantString b){
    size_t i;
    if (a.len != b.len) return 0;
    for (i = 0; i < a.len; i++) if (a.data[i] != b.data[i]) return 0;
    return 1;
}

#endif /* RANT_STRING_H */
