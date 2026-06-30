/* DartBytes and DartString: the two basic (pointer + length) views DART passes
 * bytes and strings around with. Neither owns or copies; both are valid only
 * while the memory they reference lives, and neither implies a NUL terminator.
 * Shared by discovery, transport, node, and the serializer, so it sits in common/
 * and the amalgamator emits it once near the top of every distributable. */
#ifndef DART_STRING_H
#define DART_STRING_H

#include <stddef.h>
#include <stdint.h>

/* A run of bytes: message payloads, wire blobs (meta / interest), datagrams. */
typedef struct {
    const uint8_t *data;
    size_t         len;
} DartBytes;

/* A run of string bytes that is NOT NUL-terminated: use for any string carried
 * on the wire (a topic name, the peer name inside an announce). Where a string
 * IS 0-delimited (a config input, a human label) keep a plain const char*. */
typedef struct {
    const char *data;
    size_t      len;
} DartString;

/* Constructors and a couple of helpers. static inline: no link symbol and no
 * unused-function warning; an FFI caller just fills the struct directly. */
static inline DartBytes dart_bytes(const void *data, size_t len){
    DartBytes b; b.data = (const uint8_t *)data; b.len = len; return b;
}
static inline DartString dart_string(const char *data, size_t len){
    DartString s; s.data = data; s.len = len; return s;
}
/* From a NUL-terminated C string (NULL => empty). */
static inline DartString dart_cstr(const char *s){
    DartString r; size_t n = 0;
    if (s) while (s[n]) n++;
    r.data = s; r.len = n; return r;
}
static inline int dart_string_eq(DartString a, DartString b){
    size_t i;
    if (a.len != b.len) return 0;
    for (i = 0; i < a.len; i++) if (a.data[i] != b.data[i]) return 0;
    return 1;
}

#endif /* DART_STRING_H */
