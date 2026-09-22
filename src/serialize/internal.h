/* Shared internals of the serialize layer. Not a public header. */
#ifndef RANT_SERIALIZE_INTERNAL_H
#define RANT_SERIALIZE_INTERNAL_H

#include "schema.h"
#ifndef RANT_NO_STDTYPES
#include "stdtypes.h"
#endif
#include "../common/bytes.h"
#include "../common/hash.h"
#include <string.h>

/* One field of the flat table. Offsets are message absolute, except members of a
 * variable struct array, which are relative to their element. */
typedef struct {
    RantString name;        /* a view into the wire bytes */
    RantString type_name; /* the field type's NAMED tag, or {NULL,0} */
    RantString elem_name; /* an array element type's NAMED tag, or {NULL,0} */
    uint32_t     offset;    /* element 0 under an array, 0 for variable kinds */
    uint32_t     size;      /* 0 for variable kinds */
    uint32_t     elem_size; /* ARR and VARR: bytes of one element, else 0 */
    uint32_t     type_off;  /* wire offset of the type encoding, for the subset compare */
    uint32_t     type_len;
    uint16_t     count;     /* ARR element count, or ENUM variant count, else 0 */
    uint16_t     depth;     /* 0 = top level */
    uint16_t     parent;    /* flat index of the enclosing struct or array, 0xFFFF = root */
    uint16_t     arr_parent;/* flat index of the enclosing struct array, 0xFFFF = none */
    uint16_t     str_cap;   /* STR fields and STR elements, else 0 */
    uint16_t     var_ord;   /* variable kinds: the ordinal of this field's tail frame */
    uint8_t      kind;
    uint8_t      elem;      /* ARR and VARR element kind, else 0 */
} i_Field;

struct RantSchema {
    RantBytes     wire;       /* the canonical bytes, a view into the block */
    uint64_t      hash;
    uint32_t      size;       /* the fixed section size */
    RantString    name;       /* the root name, a view into the wire, "" if anonymous */
    uint16_t      nfields;
    uint16_t      n_var;      /* variable fields */
    uint8_t       value_root; /* 1 = a bare or alias root, not a struct */
    i_Field       fields[1];   /* nfields entries in the block */
};

#define I_RANT_NO_PARENT 0xFFFFu

static inline int i_rant_kind_var(uint8_t k){
    return k == RANT_VSTR || k == RANT_VARR || k == RANT_MAP;
}

/* the bounds checked reader over possibly hostile wire bytes */
typedef struct { const uint8_t *w; size_t n, pos; int fail; } i_Rd;
static inline uint8_t  i_rant_rd_u8 (i_Rd *r){ if (r->pos + 1 > r->n){ r->fail = 1; return 0; } return r->w[r->pos++]; }
static inline uint16_t i_rant_rd_u16(i_Rd *r){ uint16_t v; if (r->pos + 2 > r->n){ r->fail = 1; return 0; } v = i_rant_le_r16(r->w + r->pos); r->pos += 2; return v; }
static inline void     i_rant_rd_skip(i_Rd *r, size_t k){ if (r->pos + k > r->n){ r->fail = 1; r->pos = r->n; return; } r->pos += k; }

/* The type writer: the growing buffer of wire type bytes that the parser fills. It holds a
 * type on its own, with no schema header, and a failure latches in err. */
typedef struct {
    RantAllocFn alloc; void *user;
    uint8_t *buf;
    size_t   cap;
    size_t   len;                                /* wire bytes written so far */
    int      err;                                /* 0 ok, nonzero latches failure */
    uint16_t depth;                              /* open structs */
    size_t   count_pos[RANT_SCHEMA_MAX_DEPTH]; /* wire offset of each open struct's nfields byte */
    uint16_t field_count[RANT_SCHEMA_MAX_DEPTH];
} i_RantSchemaBuilder;

/* The schema registry a node owns: every name defined so far, each with its compiled type
 * in one arena, and every schema handed out, one per shape. All of it lives in alloc. */
typedef struct { uint32_t noff, toff, tlen; uint8_t nlen; } i_RantDef;
typedef struct {
    i_RantSchemaBuilder arena;                   /* the names and their type bytes */
    i_RantDef   *defs; uint32_t n_defs, cap_defs;
    RantSchema **owned; uint32_t n_owned, cap_owned;
    uint16_t     rec;                            /* standard type expansion depth */
} i_RantRegistry;

/* schema.c */
/* Compiles received wire, bounds checked. NULL on an overrun or a version mismatch. The
 * bytes are copied in. Free it with the same hook. */
RantSchema *i_rant_schema_parse(const void *wire, size_t wire_len, RantAllocFn alloc, void *user);
void i_rant_schema_free(RantSchema *s, RantAllocFn alloc, void *user);
int i_rant_enum_backing_ok(uint8_t backing);
int64_t i_rant_enum_read_val(uint8_t backing, const uint8_t *p);
void i_rant_enum_write_val(uint8_t backing, uint8_t *p, int64_t v);
int i_rant_enum_val_fits(uint8_t backing, int64_t v);
const i_Field *i_rant_schema_field_by_path(const RantSchema *s, const char *path,
                                           uint32_t *index);

/* text.c */
const char *i_rant_why_kind(uint8_t k);
void i_rant_registry_init(i_RantRegistry *r, RantAllocFn alloc, void *user);
/* Compiles text against the registry's definitions and keeps the new ones. NULL with *err at
 * the offending character, or at the end, and the registry as it was. */
RantSchema *i_rant_registry_compile(i_RantRegistry *r, const char *text, const char **err);
/* The schema the registry holds for these wire bytes, parsed on first sight. */
RantSchema *i_rant_registry_parse(i_RantRegistry *r, const void *wire, size_t wire_len);
/* A name's compiled type in the arena, a standard name expanded on first use. The view
 * lasts until the next compile. */
int i_rant_registry_ref(i_RantRegistry *r, const char *name, const uint8_t **type, size_t *tlen);

/* match.c */
/* The reader's fields with the writer's offsets and size. Requires subset, NULL otherwise
 * or on OOM. Free with i_rant_schema_free. */
RantSchema *i_rant_schema_rebase(const RantSchema *sub, const RantSchema *pub,
                                 RantAllocFn alloc, void *user);

#ifndef RANT_NO_STDTYPES
/* stdtypes.c: the name and the shape must both match the registry's. */
RantStdType i_rant_std_recognize(i_RantRegistry *r, const RantSchema *s);
RantStdType i_rant_std_recognize_field(i_RantRegistry *r, const RantSchema *s, uint16_t field);
RantStdType i_rant_std_recognize_elem(i_RantRegistry *r, const RantSchema *s, uint16_t field);
#endif

/* message.c */
uint64_t i_rant_schema_read_uint(uint8_t kind, const uint8_t *p);
int64_t i_rant_schema_read_int(uint8_t kind, const uint8_t *p);
double i_rant_schema_read_f64(uint8_t kind, const uint8_t *p);

#endif /* RANT_SERIALIZE_INTERNAL_H */
