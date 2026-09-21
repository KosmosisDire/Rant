/* Shared internals of the serialize layer. Not a public header. */
#ifndef RANT_SERIALIZE_INTERNAL_H
#define RANT_SERIALIZE_INTERNAL_H

#include "schema.h"
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

/* schema.c */
int i_rant_enum_backing_ok(uint8_t backing);
int64_t i_rant_enum_read_val(uint8_t backing, const uint8_t *p);
void i_rant_enum_write_val(uint8_t backing, uint8_t *p, int64_t v);
int i_rant_enum_val_fits(uint8_t backing, int64_t v);
int i_rant_schema_wire_fields(const void *wire, size_t wire_len,
                              uint32_t *n, uint32_t *nvar);
RantSchema *i_rant_schema_compile(uint8_t *buf, size_t wire_len, size_t cap);
const i_Field *i_rant_schema_field_by_path(const RantSchema *s, const char *path,
                                           uint32_t *index);

/* text.c */
const char *i_rant_why_kind(uint8_t k);

/* message.c */
uint64_t i_rant_schema_read_uint(uint8_t kind, const uint8_t *p);
int64_t i_rant_schema_read_int(uint8_t kind, const uint8_t *p);
double i_rant_schema_read_f64(uint8_t kind, const uint8_t *p);

#endif /* RANT_SERIALIZE_INTERNAL_H */
