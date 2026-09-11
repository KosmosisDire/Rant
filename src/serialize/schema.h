/* The schema and serialization layer. Both ends share the schema, so the wire carries no
 * tags or names. Depends only on common/. The rules are in spec/schema.md. */
#ifndef DART_SCHEMA_H
#define DART_SCHEMA_H

#include <stddef.h>
#include <stdint.h>
#include "../common/api.h"
#include "../common/string.h"
#include "../common/alloc.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DART_SCHEMA_WIRE_VERSION
#define DART_SCHEMA_WIRE_VERSION 8u    /* bumped on any schema wire change */
#endif
#ifndef DART_SCHEMA_MAX_DEPTH
#define DART_SCHEMA_MAX_DEPTH 8u        /* struct nesting the builder accepts */
#endif

/* The kind byte on the wire. Fixed kinds pack first at static offsets, the variable kinds
 * ride tail frames. The encodings are in spec/schema.md. */
typedef enum {
    DART_U8 = 0, DART_U16 = 1, DART_U32 = 2, DART_U64 = 3,
    DART_I8 = 4, DART_I16 = 5, DART_I32 = 6, DART_I64 = 7,
    DART_F32 = 8, DART_F64 = 9, DART_BOOL = 10,   /* 0 to 10: fixed scalars */
    DART_ARR    = 11,   /* fixed array, the element must be fixed */
    DART_STRUCT = 12,
    DART_STR    = 13,   /* capped string, a slot of [u16 len][cap bytes] */
    DART_VSTR   = 14,   /* variable string, a tail frame */
    DART_VARR   = 15,   /* variable array, a tail frame of packed elements */
    DART_MAP    = 16,   /* self describing map, a tail frame */
    DART_ENUM   = 17,   /* a named integer, on the wire just its backing scalar */
    DART_NAMED  = 18    /* a name tag on another type, zero message bytes, never a root */
} DartSchemaTypeKind;

/* Bytes of a fixed scalar kind, 0 otherwise. */
DART_API uint32_t dart_schema_scalar_size(DartSchemaTypeKind kind);

/* The compiled schema: wire bytes, hash, size and a flat field table in one block from
 * the hook. Free it with dart_schema_free. */
typedef struct DartSchema DartSchema;

/* One field of the flat depth first table. A struct array flattens its element once as
 * an element 0 template under the array field. See spec/schema.md. */
typedef struct {
    DartString name;      /* the field's own name, a view into the wire */
    DartString type_name; /* the field type's name, {NULL,0} when anonymous */
    DartString elem_name; /* an array element type's name, {NULL,0} when anonymous */
    uint8_t    kind;      /* DartSchemaTypeKind, a NAMED wrapper is unwrapped into type_name */
    uint8_t    elem;      /* the array element kind, or the enum backing kind, else 0 */
    uint16_t   count;     /* ARR element count, or ENUM variant count, else 0 */
    uint16_t   depth;     /* 0 = top level, n = a member of the struct n levels up */
    uint16_t   str_cap;   /* string capacity for STR fields and STR elements, else 0 */
    uint16_t   arr_parent;/* flat index of the enclosing struct array, or 0xFFFF */
    uint32_t   offset;    /* absolute byte offset, 0 for variable kinds */
    uint32_t   size;      /* byte size, 0 for variable kinds */
    uint32_t   elem_size; /* ARR and VARR: bytes of one element, else 0 */
} DartSchemaFieldInfo;

/* One enum option. */
typedef struct {
    int64_t     value;
    const char *name;     /* NUL terminated builder input, at most 255 bytes */
} DartEnumVariant;

/* The builder grows its wire buffer through the hook as fields are added. finish returns
 * the compiled schema, or NULL after any latched error. Always finish a begun builder. */
typedef struct {
    DartAllocFn alloc; void *user;
    uint8_t *buf;
    size_t   cap;
    size_t   len;                              /* wire bytes written so far */
    int      err;                              /* 0 ok, nonzero latches failure */
    uint8_t  value_root;                       /* a bare root: 1 awaits its type, 2 written */
    uint8_t  raw_type;                         /* internal: type bytes only, no header */
    uint16_t base_depth;                       /* the struct depth the root sits at */
    uint16_t arr_depth;                        /* the open array element depth, 0xFFFF = none */
    uint16_t depth;                            /* open structs, 1 = a struct root only */
    size_t   count_pos[DART_SCHEMA_MAX_DEPTH]; /* wire offset of each open struct's nfields byte */
    uint16_t field_count[DART_SCHEMA_MAX_DEPTH];
} DartSchemaBuilder;

/* Compiles DSL text, pasted verbatim on the writer and every reader. The DSL is in
 * docs/stdtypes.md and spec/schema.md. NULL on error, with *err at the offending character. */
DART_API DartSchema *dart_schema_compile(DartAllocFn alloc, void *user, const char *text, const char **err);
/* compile with an environment: each env schema is referenceable by its root name. An
 * anonymous env root is ignored. */
DART_API DartSchema *dart_schema_compile_env(DartAllocFn alloc, void *user, const char *text,
                                             const DartSchema *const *env, size_t n_env,
                                             const char **err);

DART_API DartSchemaBuilder dart_schema_begin(DartAllocFn alloc, void *user, const char *root_name);
/* A bare type root: add exactly one field with an empty name, then finish. */
DART_API DartSchemaBuilder dart_schema_begin_value(DartAllocFn alloc, void *user);
/* A named alias root, like Uuid = u8[16]: one empty named field, then finish. */
DART_API DartSchemaBuilder dart_schema_begin_alias(DartAllocFn alloc, void *user, const char *name);
/* A fixed scalar field. */
DART_API void        dart_schema_field(DartSchemaBuilder *b, const char *name, DartSchemaTypeKind kind);
/* A fixed array of count scalars. */
DART_API void        dart_schema_field_array(DartSchemaBuilder *b, const char *name,
                                             DartSchemaTypeKind elem_scalar, uint16_t count);
/* A capped string, cap at least 1. */
DART_API void        dart_schema_field_string(DartSchemaBuilder *b, const char *name, uint16_t cap);
/* A fixed array of count capped strings. */
DART_API void        dart_schema_field_string_array(DartSchemaBuilder *b, const char *name,
                                                    uint16_t cap, uint16_t count);
/* A variable string. Any depth outside an array element. */
DART_API void        dart_schema_field_var_string(DartSchemaBuilder *b, const char *name);
/* A variable array of scalars. */
DART_API void        dart_schema_field_var_array(DartSchemaBuilder *b, const char *name,
                                                 DartSchemaTypeKind elem_scalar);
/* A variable array of capped strings. */
DART_API void        dart_schema_field_var_string_array(DartSchemaBuilder *b, const char *name,
                                                        uint16_t cap);
/* A self describing map. Write it with DartMapWriter, read it with dart_map_get. */
DART_API void        dart_schema_field_map(DartSchemaBuilder *b, const char *name);
/* A named integer with an integer backing and n options, n at most 65535. Latches an
 * error on a non integer backing or a value that does not fit it. */
DART_API void        dart_schema_field_enum(DartSchemaBuilder *b, const char *name,
                                            DartSchemaTypeKind backing,
                                            const DartEnumVariant *variants, uint16_t n);
/* A field of a compiled named type. type need not outlive the call. */
DART_API void        dart_schema_field_named(DartSchemaBuilder *b, const char *name,
                                             const DartSchema *type);
/* An array of a named type, variable when count is 0. The element must be fixed. */
DART_API void        dart_schema_field_named_array(DartSchemaBuilder *b, const char *name,
                                                   const DartSchema *type, uint16_t count);
/* Opens a nested struct. Add its fields, then end_struct. */
DART_API void        dart_schema_begin_struct(DartSchemaBuilder *b, const char *name);
/* An array of anonymous structs, variable when count is 0. Add the element's fields,
 * then end_struct. The element must be fixed. */
DART_API void        dart_schema_begin_struct_array(DartSchemaBuilder *b, const char *name,
                                                    uint16_t count);
DART_API void        dart_schema_end_struct(DartSchemaBuilder *b);
/* Closes the root, compiles, and returns the schema. NULL on any latched error. */
DART_API DartSchema *dart_schema_finish(DartSchemaBuilder *b);

/* Compiles received wire, bounds checked. NULL on an overrun or a version mismatch. The
 * bytes are copied in. */
DART_API DartSchema *dart_schema_parse(const void *wire, size_t wire_len, DartAllocFn alloc, void *user);
/* The same hook the schema was made with. */
DART_API void        dart_schema_free(DartSchema *s, DartAllocFn alloc, void *user);
/* An owned copy of any schema, safe to keep and to hand to create. */
DART_API DartSchema *dart_schema_copy(const DartSchema *s, DartAllocFn alloc, void *user);

DART_API DartBytes   dart_schema_wire(const DartSchema *s);        /* the canonical bytes to advertise */
DART_API uint64_t    dart_schema_hash(const DartSchema *s);        /* FNV-1a over the wire */
DART_API DartString  dart_schema_name(const DartSchema *s);   /* the root name, "" if anonymous */
/* Spells the schema back as DSL text, the inverse of compile. Returns the full length
 * excluding the NUL, so a NULL buffer measures. */
DART_API uint32_t    dart_schema_print(const DartSchema *s, char *buf, size_t cap);
/* The fixed section size, where the variable tail starts. */
DART_API uint32_t    dart_schema_size(const DartSchema *s);
/* The smallest valid message: the fixed section plus one empty frame per variable field. */
DART_API uint32_t    dart_schema_msg_min(const DartSchema *s);
/* The live length to send. 0 on a malformed or uninitialized buffer. */
DART_API uint32_t    dart_schema_msg_len(const DartSchema *s, const void *buf, size_t cap);
/* The flat depth first table. A bare root is one field named "". */
DART_API uint16_t    dart_schema_field_count(const DartSchema *s);
/* 1 and fills out, else 0 */
DART_API int         dart_schema_field_at(const DartSchema *s, uint16_t i, DartSchemaFieldInfo *out);
/* Resolves a dotted or indexed path ("velocity.dx", "corners[2].x") to the flat index, or -1. */
DART_API int         dart_schema_field_index(const DartSchema *s, const char *path);
/* One field's type encoding, a view into the wire. Two fields have the same type exactly
 * when these agree. {NULL,0} for an unknown index. */
DART_API DartBytes   dart_schema_field_type_wire(const DartSchema *s, uint16_t field);

/* The options of an enum field by flat index. count is 0 for a non enum field. */
DART_API uint16_t    dart_schema_enum_count  (const DartSchema *s, uint16_t field);
DART_API int         dart_schema_enum_variant(const DartSchema *s, uint16_t field, uint16_t i,
                                              int64_t *value, DartString *name);
/* Resolution over the schema alone. name_of gives {NULL,0} for an unknown number, which
 * is safe, not an error. value_of returns 1 and *out for a known name. */
DART_API DartString  dart_enum_name_of (const DartSchema *s, uint16_t field, int64_t value);
DART_API int         dart_enum_value_of(const DartSchema *s, uint16_t field, const char *name, int64_t *out);

/* 1 if msg is exactly one message of this schema. */
DART_API int       dart_schema_validate(const DartSchema *s, DartBytes msg);

/* 1 if a reader declaring sub can read messages written with pub. The rules are in
 * spec/schema.md. */
DART_API int dart_schema_subset(const DartSchema *sub, const DartSchema *pub);
/* The same, with the first incompatibility written to buf as one line on refusal. */
DART_API int dart_schema_subset_why(const DartSchema *sub, const DartSchema *pub,
                                    char *buf, size_t cap);
/* The reader's fields with the writer's offsets and size. Requires subset, NULL
 * otherwise or on OOM. Free with dart_schema_free. */
DART_API DartSchema *dart_schema_rebase(const DartSchema *sub, const DartSchema *pub,
                                        DartAllocFn alloc, void *user);

/* Getters by name, nested members by dotted path and array members by indexed path,
 * widened. A mismatch or unknown field yields 0. */
DART_API uint64_t  dart_get_uint(DartBytes msg, const DartSchema *s, const char *field);  /* and BOOL */
DART_API int64_t   dart_get_int (DartBytes msg, const DartSchema *s, const char *field);  /* I8 to I64 */
DART_API double    dart_get_f64 (DartBytes msg, const DartSchema *s, const char *field);  /* F64 or F32 */
DART_API float     dart_get_f32 (DartBytes msg, const DartSchema *s, const char *field);  /* F32 */
/* ARR: the whole array. VARR: the live elements, a whole multiple of the element size. */
DART_API DartBytes dart_get_array(DartBytes msg, const DartSchema *s, const char *field);
/* STR or VSTR: the live bytes. A hostile length prefix is clamped to the cap. */
DART_API DartString dart_get_string(DartBytes msg, const DartSchema *s, const char *field);
/* One element of a string array, {NULL,0} past the live count. */
DART_API DartString dart_get_string_at(DartBytes msg, const DartSchema *s, const char *field,
                                       uint16_t index);
/* The map body, to feed to dart_map_get. An empty body is an empty map. */
DART_API DartBytes dart_get_map(DartBytes msg, const DartSchema *s, const char *field);
/* The current option name, {NULL,0} if the stored number has no option. */
DART_API DartString dart_get_enum(DartBytes msg, const DartSchema *s, const char *field);
/* The live element count of a struct array, 0 if the field is not an array. */
DART_API uint32_t  dart_get_array_count(DartBytes msg, const DartSchema *s, const char *field);

/* The canonical default message: zeros and one empty frame per variable field. Always
 * start from this, since the variable setters rely on every frame existing. */
DART_API int dart_schema_message_default(const DartSchema *s, void *buf, size_t cap);

/* Setters by name. Values narrow like a C cast. A variable setter resizes its frame in
 * place and returns 0 when the message would exceed cap. 1 ok, 0 on a mismatch. */
/* set_uint also takes BOOL */
DART_API int dart_set_uint(void *buf, size_t cap, const DartSchema *s, const char *field, uint64_t v);
DART_API int dart_set_int (void *buf, size_t cap, const DartSchema *s, const char *field, int64_t v);
/* set_f64 also takes an F32 field */
DART_API int dart_set_f64 (void *buf, size_t cap, const DartSchema *s, const char *field, double v);
DART_API int dart_set_f32 (void *buf, size_t cap, const DartSchema *s, const char *field, float v);
/* ARR: elems over the front, the rest zeroed. VARR: the frame becomes elems. Whole
 * elements only, never silently truncated. */
DART_API int dart_set_array(void *buf, size_t cap, const DartSchema *s, const char *field, DartBytes elems);
/* Grows or shrinks a variable array, zero filling new elements. */
DART_API int dart_set_array_count(void *buf, size_t cap, const DartSchema *s, const char *field,
                                  uint32_t count);
/* STR: the live bytes, the slot tail zeroed, v.len must fit the cap. VSTR: the frame becomes v. */
DART_API int dart_set_string(void *buf, size_t cap, const DartSchema *s, const char *field, DartString v);
/* One element of a string array, under the live count for a variable one. */
DART_API int dart_set_string_at(void *buf, size_t cap, const DartSchema *s, const char *field,
                                uint16_t index, DartString v);
/* The frame becomes the map body, validated first. */
DART_API int dart_set_map(void *buf, size_t cap, const DartSchema *s, const char *field, DartBytes map);
/* Writes the option by name, 0 for an unknown one. */
DART_API int dart_set_enum(void *buf, size_t cap, const DartSchema *s, const char *field, const char *name);

/* One tagged value for reflection by flat index. */
typedef struct {
    uint8_t  kind;        /* DartSchemaTypeKind */
    uint8_t  elem;        /* the array element kind, or the enum backing kind */
    uint16_t count;       /* ARR count, VARR live count, MAP entries (saturated), ENUM variants */
    uint16_t str_cap;     /* string capacity for STR fields and STR elements */
    union { uint64_t u; int64_t i; double f; } v;   /* the scalar value, BOOL in u, ENUM in i */
    DartBytes bytes;      /* the raw bytes, the live string, the live elements or the map body */
} DartValue;
DART_API int dart_get_value(DartBytes msg, const DartSchema *s, uint16_t field, DartValue *out);
DART_API int dart_set_value(void *buf, size_t cap, const DartSchema *s, uint16_t field, const DartValue *val);
/* The same under a struct array, elem selects the element. Ignored for other fields. */
DART_API int dart_get_value_at(DartBytes msg, const DartSchema *s, uint16_t field, uint32_t elem,
                               DartValue *out);
DART_API int dart_set_value_at(void *buf, size_t cap, const DartSchema *s, uint16_t field, uint32_t elem,
                               const DartValue *val);
/* The live count of the struct array at flat index field, 0 if it is not an array. */
DART_API uint32_t dart_array_count_at(DartBytes msg, const DartSchema *s, uint16_t field);

/* The map is a self describing tagged value tree, the escape from the schema. Its body
 * grammar is in spec/schema.md. The writer fills a caller buffer with no allocation. */
typedef struct {
    uint8_t *buf; size_t cap, len;             /* the caller's buffer and write position */
    int      err;                              /* 0 ok, nonzero latches failure */
    uint16_t depth;                            /* open bodies, 1 = the root map only */
    size_t   count_pos[DART_SCHEMA_MAX_DEPTH]; /* offset of each open body's count */
    uint16_t count[DART_SCHEMA_MAX_DEPTH];
    uint8_t  is_arr[DART_SCHEMA_MAX_DEPTH];
} DartMapWriter;

DART_API DartMapWriter dart_map_begin(void *buf, size_t cap);   /* opens the root map */
DART_API int dart_map_put_uint  (DartMapWriter *w, const char *key, uint64_t v);
DART_API int dart_map_put_int   (DartMapWriter *w, const char *key, int64_t v);
DART_API int dart_map_put_f64   (DartMapWriter *w, const char *key, double v);
DART_API int dart_map_put_f32   (DartMapWriter *w, const char *key, float v);
DART_API int dart_map_put_bool  (DartMapWriter *w, const char *key, int v);
DART_API int dart_map_put_string(DartMapWriter *w, const char *key, DartString v);
DART_API int dart_map_open_map  (DartMapWriter *w, const char *key);   /* a nested map, close it */
DART_API int dart_map_open_array(DartMapWriter *w, const char *key);   /* elements pass key NULL */
DART_API int dart_map_close     (DartMapWriter *w);                    /* closes the innermost open body */
DART_API uint32_t dart_map_finish(DartMapWriter *w);                   /* the body length, 0 on any error */

/* Readers over a possibly hostile body: every walk is bounds checked and a nested value
 * comes back in DartValue.bytes ready for these same functions. */
DART_API uint16_t dart_map_count(DartBytes map);                       /* 0 if empty or short */
DART_API int dart_map_at (DartBytes map, uint16_t index, DartString *key, DartValue *out);
DART_API int dart_map_get(DartBytes map, const char *key, DartValue *out);   /* 1 and fills out, else 0 */
DART_API uint16_t dart_map_array_count(DartBytes arr);
DART_API int dart_map_array_at(DartBytes arr, uint16_t index, DartValue *out);
DART_API int dart_map_valid(DartBytes map);   /* the full structural check, dart_set_map runs it */

#ifdef __cplusplus
}
#endif
#endif /* DART_SCHEMA_H */
