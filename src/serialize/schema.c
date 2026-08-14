#include "schema.h"
#include "../common/bytes.h"   /* dart_le_* */
#include "../common/hash.h"    /* i_dart_fnv1a64 (the schema id) */
#include <string.h>            /* memcpy (float bit reinterpret) */

/* The schema wire format (what the builder emits and dart_schema_parse reads):
 *   schema := [u8 version][u8 root_namelen][root_name...][type]
 *             A named root_name with a STRUCT type is an ordinary named struct; with any
 *             other type it is a named ALIAS (`Uuid = u8[16]`). root_namelen 0 leaves the
 *             root anonymous (purely structural). The root type is never NAMED: a root's
 *             name is spelled by the header, so there is exactly one encoding for it.
 *   type   := [u8 kind] payload
 *     scalar (U8..BOOL)      : (none; size implied by kind)
 *     ARR                    : [u16 count][type elem]   ; elem must be FIXED, never an array
 *     STRUCT                 : [u8 nfields] ( [u8 namelen][name][type] )*nfields
 *     STR                    : [u16 cap]                ; in a message: [u16 len][cap bytes]
 *     VSTR                   : (none)                   ; not inside an array element
 *     VARR                   : [type elem]              ; elem must be FIXED, never an array
 *     MAP                    : (none)                   ; not inside an array element
 *     ENUM                   : [u8 backing][u16 n]( [value:backing][u8 nl][name] )*
 *     NAMED                  : [u8 tnamelen >= 1][tname][type inner]   ; inner never NAMED
 * Fixed types pack first: a fixed field's byte offset is the sum of the preceding fixed
 * sizes. Each variable field (VSTR/VARR/MAP) is one [u32 len][payload] frame in the
 * message tail, frames in depth-first declaration order; its ordinal locates it by a
 * length-hop walk. Declaration nests, storage does not: a variable member declared inside
 * a nested struct still claims a flat top-level frame, and its struct's size covers only
 * its fixed part. */

/* The per-field record, one for EVERY field at every depth (flattened depth-first: a
 * struct's members directly follow it). Offsets are message-absolute, except for the
 * members of a VARIABLE struct array, which are relative to their element. */
typedef struct {
    DartString name;      /* field's own name, a view into the wire bytes */
    DartString type_name; /* the field type's NAMED tag, or {NULL,0} */
    DartString elem_name; /* an ARR/VARR element type's NAMED tag, or {NULL,0} */
    uint32_t   offset;    /* byte offset in a message (element 0 under an array); 0 for variable kinds */
    uint32_t   size;      /* byte size; 0 for variable kinds (live, per message) */
    uint32_t   elem_size; /* ARR/VARR: bytes of one element, else 0 */
    uint32_t   type_off;  /* wire offset of the field's type encoding (subset compare) */
    uint32_t   type_len;  /* wire length of the type encoding */
    uint16_t   count;     /* ARR element count, or ENUM variant count, else 0 */
    uint16_t   depth;     /* 0 = top level */
    uint16_t   parent;    /* flat index of the enclosing struct/array field; 0xFFFF = root */
    uint16_t   arr_parent;/* flat index of the enclosing struct ARRAY; 0xFFFF = none */
    uint16_t   str_cap;   /* string capacity (STR fields and STR-element arrays), else 0 */
    uint16_t   var_ord;   /* variable kinds: ordinal of this field's tail frame */
    uint8_t    kind;
    uint8_t    elem;      /* ARR/VARR element kind, else 0 */
} i_Field;

struct DartSchema {
    DartBytes   wire;       /* the canonical bytes (a view into the caller's buffer) */
    uint64_t    hash;
    uint32_t    size;       /* fixed-section size (the message size when n_var == 0) */
    DartString  name;       /* root type name, a view into the wire bytes ("" if anonymous) */
    uint16_t    nfields;
    uint16_t    n_var;      /* variable (tail-frame) fields */
    uint8_t     value_root; /* 1 = the root is a bare/alias type, not a struct */
    i_Field     fields[1];   /* nfields entries, laid out in the caller's buffer */
};

#define I_DART_NO_PARENT 0xFFFFu

static int i_dart_kind_var(uint8_t k){
    return k == DART_VSTR || k == DART_VARR || k == DART_MAP;
}

uint32_t dart_schema_scalar_size(DartSchemaTypeKind kind){
    switch (kind){
        case DART_U8: case DART_I8: case DART_BOOL: return 1;
        case DART_U16: case DART_I16:               return 2;
        case DART_U32: case DART_I32: case DART_F32: return 4;
        case DART_U64: case DART_I64: case DART_F64: return 8;
        default: return 0;
    }
}

/* ---- enum backing values (an integer kind U8..I64; read/written like the scalar) ------ */
static int i_dart_enum_backing_ok(uint8_t backing){ return backing <= (uint8_t)DART_I64; }
/* read a backing-sized value from p, widened to int64 (sign-extended for a signed kind) */
static int64_t i_dart_enum_read_val(uint8_t backing, const uint8_t *p){
    switch (backing){
        case DART_U8:  return (int64_t)(uint64_t)p[0];
        case DART_U16: return (int64_t)(uint64_t)i_dart_le_r16(p);
        case DART_U32: return (int64_t)(uint64_t)i_dart_le_r32(p);
        case DART_U64: return (int64_t)i_dart_le_r64(p);
        case DART_I8:  return (int64_t)(int8_t)p[0];
        case DART_I16: return (int64_t)(int16_t)i_dart_le_r16(p);
        case DART_I32: return (int64_t)(int32_t)i_dart_le_r32(p);
        case DART_I64: return (int64_t)i_dart_le_r64(p);
        default:       return 0;
    }
}
/* write v into p as backing-sized little-endian bytes (truncated to the backing) */
static void i_dart_enum_write_val(uint8_t backing, uint8_t *p, int64_t v){
    uint64_t u = (uint64_t)v; uint32_t bs = dart_schema_scalar_size((DartSchemaTypeKind)backing), i;
    for (i = 0; i < bs; i++) p[i] = (uint8_t)(u >> (8 * i));
}
/* does v fit the backing's representable range? (u64 above INT64_MAX is not expressible) */
static int i_dart_enum_val_fits(uint8_t backing, int64_t v){
    switch (backing){
        case DART_U8:  return v >= 0 && v <= 0xFF;
        case DART_U16: return v >= 0 && v <= 0xFFFF;
        case DART_U32: return v >= 0 && v <= (int64_t)0xFFFFFFFF;
        case DART_U64: return v >= 0;
        case DART_I8:  return v >= -128 && v <= 127;
        case DART_I16: return v >= -32768 && v <= 32767;
        case DART_I32: return v >= (int64_t)(-2147483647 - 1) && v <= 2147483647;
        case DART_I64: return 1;
        default:       return 0;
    }
}

/* ---- bounds-checked reader over (possibly hostile) wire bytes ---------------------- */
typedef struct { const uint8_t *w; size_t n, pos; int fail; } i_Rd;
static uint8_t  i_dart_rd_u8 (i_Rd *r){ if (r->pos + 1 > r->n){ r->fail = 1; return 0; } return r->w[r->pos++]; }
static uint16_t i_dart_rd_u16(i_Rd *r){ uint16_t v; if (r->pos + 2 > r->n){ r->fail = 1; return 0; } v = i_dart_le_r16(r->w + r->pos); r->pos += 2; return v; }
static void     i_dart_rd_skip(i_Rd *r, size_t k){ if (r->pos + k > r->n){ r->fail = 1; r->pos = r->n; return; } r->pos += k; }

/* The type at r->pos with any NAMED wrappers peeled off; 0xFF if the wire runs out. */
static uint8_t i_dart_peek_kind(const i_Rd *r){
    size_t p = r->pos;
    for (;;){
        if (p >= r->n) return 0xFFu;
        if (r->w[p] != DART_NAMED) return r->w[p];
        if (p + 2u > r->n) return 0xFFu;
        p += 2u + r->w[p + 1];
    }
}

/* Where a type sits relative to an array: DIRECT = it IS an element type (so it may not
 * itself be an array), INSIDE = it is somewhere within one (so no variable kinds, whose
 * size is not static, and no further struct arrays, which would need a second index). */
#define I_T_ELEM_DIRECT 1u
#define I_T_ELEM_INSIDE 2u

/* FIXED byte size of the type at r->pos (variable kinds contribute 0); advances r past
 * it. Counts every field it walks (all depths, including a struct array's element-0
 * template) into *fields and every variable field into *nvar when given. depth is the
 * field-nesting depth of the type (the root struct is 0, so a top-level field's type sits
 * at depth 1). Fails on an unknown kind, a misplaced kind (see the flags above), a
 * doubly-wrapped NAMED, or nesting past DART_SCHEMA_MAX_DEPTH (the read-side twin of the
 * builder's cap: hostile wire must not recurse unboundedly). */
static uint32_t i_dart_rd_type_size(i_Rd *r, uint32_t *fields, uint32_t *nvar,
                                    uint16_t depth, uint32_t fl){
    uint8_t k = i_dart_rd_u8(r);
    if (r->fail) return 0;
    switch (k){
        case DART_U8: case DART_I8: case DART_BOOL: return 1;
        case DART_U16: case DART_I16:               return 2;
        case DART_U32: case DART_I32: case DART_F32: return 4;
        case DART_U64: case DART_I64: case DART_F64: return 8;
        case DART_STR:
            return 2u + (uint32_t)i_dart_rd_u16(r);              /* [u16 len][cap bytes] */
        case DART_ARR: {
            uint16_t count; uint8_t ek; uint64_t es;
            if (fl & I_T_ELEM_DIRECT){ r->fail = 1; return 0; }  /* no array of arrays */
            count = i_dart_rd_u16(r);
            if (r->fail) return 0;
            ek = i_dart_peek_kind(r);
            if (ek == 0xFFu){ r->fail = 1; return 0; }
            if ((fl & I_T_ELEM_INSIDE) && ek == DART_STRUCT){ r->fail = 1; return 0; }
            es = i_dart_rd_type_size(r, fields, nvar, (uint16_t)(depth + 1),
                                     I_T_ELEM_DIRECT | I_T_ELEM_INSIDE);
            if (r->fail || es == 0){ r->fail = 1; return 0; }    /* elements must be FIXED */
            if ((uint64_t)count * es > 0xFFFFFFFFu){ r->fail = 1; return 0; }
            return (uint32_t)((uint64_t)count * es);
        }
        case DART_ENUM: {                                        /* [u8 backing][u16 n]( [value][u8 nl][name] )* */
            uint8_t backing; uint16_t n; uint32_t bs, i;
            if (fl & I_T_ELEM_DIRECT){ r->fail = 1; return 0; }  /* the element kind would be ambiguous */
            backing = i_dart_rd_u8(r);
            n = i_dart_rd_u16(r);
            bs = dart_schema_scalar_size((DartSchemaTypeKind)backing);
            if (r->fail || bs == 0 || !i_dart_enum_backing_ok(backing)){ r->fail = 1; return 0; }
            for (i = 0; i < n && !r->fail; i++){                  /* skip the option table */
                i_dart_rd_skip(r, bs);                           /* the value */
                i_dart_rd_skip(r, i_dart_rd_u8(r));              /* the name */
            }
            if (r->fail) return 0;
            return bs;                                           /* on the wire: just the backing scalar */
        }
        case DART_VSTR: case DART_MAP:
            if (fl){ r->fail = 1; return 0; }                    /* never inside an array element */
            if (nvar) (*nvar)++;
            return 0;
        case DART_VARR: {
            uint64_t es;
            if (fl){ r->fail = 1; return 0; }
            if (i_dart_peek_kind(r) == 0xFFu){ r->fail = 1; return 0; }
            es = i_dart_rd_type_size(r, fields, nvar, (uint16_t)(depth + 1),
                                     I_T_ELEM_DIRECT | I_T_ELEM_INSIDE);
            if (r->fail || es == 0){ r->fail = 1; return 0; }
            if (nvar) (*nvar)++;
            return 0;
        }
        case DART_STRUCT: {
            uint8_t nf; uint64_t sum = 0; uint16_t i;
            if (depth >= DART_SCHEMA_MAX_DEPTH){ r->fail = 1; return 0; }
            nf = i_dart_rd_u8(r);
            for (i = 0; i < nf && !r->fail; i++){
                uint8_t fnl = i_dart_rd_u8(r);
                i_dart_rd_skip(r, fnl);
                if (fields) (*fields)++;
                sum += i_dart_rd_type_size(r, fields, nvar, (uint16_t)(depth + 1),
                                           fl & I_T_ELEM_INSIDE);
            }
            if (sum > 0xFFFFFFFFu){ r->fail = 1; return 0; }
            return (uint32_t)sum;
        }
        case DART_NAMED: {
            uint8_t nl = i_dart_rd_u8(r);
            if (r->fail || nl == 0){ r->fail = 1; return 0; }    /* a name, or do not wrap */
            i_dart_rd_skip(r, nl);
            if (r->fail) return 0;
            if (r->pos < r->n && r->w[r->pos] == DART_NAMED){ r->fail = 1; return 0; }
            return i_dart_rd_type_size(r, fields, nvar, depth, fl);
        }
        default: r->fail = 1; return 0;                          /* unknown kind: reject */
    }
}

/* Offset of the compiled handle in buf: past the wire bytes, 8-aligned for the handle. */
static size_t i_dart_schema_handle_off(const uint8_t *buf, size_t wire_len){
    uintptr_t addr = (uintptr_t)buf + wire_len;
    size_t pad = (size_t)((8u - (addr & 7u)) & 7u);
    return wire_len + pad;
}

/* Count every field (all depths) of the schema wire's root into *n and its variable
 * fields into *nvar; 1, or 0 on malformed wire (an empty-but-valid struct schema is 1
 * with *n == 0; a bare/alias root is 1 plus whatever its type flattens to). */
static int i_dart_schema_wire_fields(const void *wire, size_t wire_len,
                                     uint32_t *n, uint32_t *nvar){
    i_Rd r; uint8_t ver, rl;
    *n = 0; *nvar = 0;
    r.w = (const uint8_t *)wire; r.n = wire_len; r.pos = 0; r.fail = 0;
    ver = i_dart_rd_u8(&r); if (r.fail || ver != DART_SCHEMA_WIRE_VERSION) return 0;
    rl = i_dart_rd_u8(&r); i_dart_rd_skip(&r, rl);
    if (r.fail || (size_t)r.pos >= r.n) return 0;
    if (r.w[r.pos] == DART_NAMED) return 0;          /* a root's name rides the header */
    if (r.w[r.pos] == DART_STRUCT){
        i_dart_rd_type_size(&r, n, nvar, 0, 0);
    } else {
        uint32_t sub = 0;
        i_dart_rd_type_size(&r, &sub, nvar, 0, 0);   /* depth 0: a variable root is allowed */
        *n = 1u + sub;                               /* the root IS the first field */
    }
    return r.fail ? 0 : 1;
}

/* ---- flattening a wire type into the field table -------------------------------------- */
static void i_dart_field_init(i_Field *f, DartString name, uint16_t depth, uint16_t parent,
                              uint32_t offset){
    memset(f, 0, sizeof *f);
    f->name = name;
    f->type_name = dart_string(NULL, 0);
    f->elem_name = dart_string(NULL, 0);
    f->depth = depth; f->parent = parent; f->arr_parent = I_DART_NO_PARENT;
    f->offset = offset;
}

static uint32_t i_dart_emit_type(uint8_t *buf, size_t wl, i_Rd *r, DartSchema *s,
                                 uint16_t *emitted, uint32_t total,
                                 uint16_t idx, uint16_t depth, uint32_t base);

/* Emit the members of the struct body at r->pos (its nfields byte) into the flat table,
 * depth-first. Returns the struct's fixed size; sets r->fail on malformed wire. */
static uint32_t i_dart_emit_struct(uint8_t *buf, size_t wl, i_Rd *r, DartSchema *s,
                                   uint16_t *emitted, uint32_t total,
                                   uint16_t depth, uint16_t parent, uint32_t base){
    uint8_t nf = i_dart_rd_u8(r); uint16_t i;
    uint32_t running = base;
    if (depth >= DART_SCHEMA_MAX_DEPTH){ r->fail = 1; return 0; }
    for (i = 0; i < nf && !r->fail; i++){
        uint8_t fl = i_dart_rd_u8(r);
        const char *fn = (const char *)(buf + r->pos);
        uint16_t idx; uint32_t sz;
        i_dart_rd_skip(r, fl);
        if (r->fail) return 0;
        if (*emitted >= total){ r->fail = 1; return 0; }
        idx = (*emitted)++;
        i_dart_field_init(&s->fields[idx], dart_string(fn, fl), depth, parent, running);
        sz = i_dart_emit_type(buf, wl, r, s, emitted, total, idx, depth, running);
        if (r->fail) return 0;
        running += sz;
    }
    return running - base;
}

/* Fill field `idx` from the type at r->pos (NAMED wrappers unwrapped into type_name) and
 * emit any child fields it flattens to. `base` is where this field's storage starts. */
static uint32_t i_dart_emit_type(uint8_t *buf, size_t wl, i_Rd *r, DartSchema *s,
                                 uint16_t *emitted, uint32_t total,
                                 uint16_t idx, uint16_t depth, uint32_t base){
    size_t kpos = r->pos;
    uint8_t k;
    uint32_t sz = 0;
    i_Field *f = &s->fields[idx];
    f->type_off = (uint32_t)kpos;
    k = i_dart_rd_u8(r);
    if (k == DART_NAMED){
        uint8_t nl = i_dart_rd_u8(r);
        const char *tn = (const char *)(buf + r->pos);
        i_dart_rd_skip(r, nl);
        if (r->fail || nl == 0){ r->fail = 1; return 0; }
        f->type_name = dart_string(tn, nl);
        k = i_dart_rd_u8(r);
        if (k == DART_NAMED){ r->fail = 1; return 0; }
    }
    if (r->fail) return 0;
    f->kind = k;
    switch (k){
        case DART_STRUCT:
            sz = i_dart_emit_struct(buf, wl, r, s, emitted, total,
                                    (uint16_t)(depth + 1), idx, base);
            break;
        case DART_ARR: case DART_VARR: {
            uint8_t ek; uint32_t esz = 0;
            if (k == DART_ARR){
                f->count = i_dart_rd_u16(r);
                if (r->fail) return 0;
            }
            ek = i_dart_rd_u8(r);
            if (ek == DART_NAMED){
                uint8_t nl = i_dart_rd_u8(r);
                const char *tn = (const char *)(buf + r->pos);
                i_dart_rd_skip(r, nl);
                if (r->fail || nl == 0){ r->fail = 1; return 0; }
                f->elem_name = dart_string(tn, nl);
                ek = i_dart_rd_u8(r);
                if (ek == DART_NAMED){ r->fail = 1; return 0; }
            }
            if (r->fail) return 0;
            f->elem = ek;
            if (ek == DART_STRUCT){                       /* one element-0 template */
                esz = i_dart_emit_struct(buf, wl, r, s, emitted, total,
                                         (uint16_t)(depth + 1), idx,
                                         k == DART_ARR ? base : 0u);
            } else if (ek == DART_STR){
                f->str_cap = i_dart_rd_u16(r);
                esz = 2u + (uint32_t)f->str_cap;
            } else {
                esz = dart_schema_scalar_size((DartSchemaTypeKind)ek);
            }
            if (r->fail || esz == 0){ r->fail = 1; return 0; }
            f->elem_size = esz;
            if (k == DART_ARR){
                if ((uint64_t)f->count * esz > 0xFFFFFFFFu){ r->fail = 1; return 0; }
                sz = (uint32_t)((uint64_t)f->count * esz);
            }
            break;
        }
        case DART_STR:
            f->str_cap = i_dart_rd_u16(r);
            sz = 2u + (uint32_t)f->str_cap;
            break;
        case DART_ENUM: {
            uint16_t i;
            f->elem = i_dart_rd_u8(r);
            f->count = i_dart_rd_u16(r);
            sz = dart_schema_scalar_size((DartSchemaTypeKind)f->elem);
            if (r->fail || sz == 0 || !i_dart_enum_backing_ok(f->elem)){ r->fail = 1; return 0; }
            for (i = 0; i < f->count && !r->fail; i++){
                i_dart_rd_skip(r, sz);
                i_dart_rd_skip(r, i_dart_rd_u8(r));
            }
            break;
        }
        case DART_VSTR: case DART_MAP:
            sz = 0;
            break;
        default:
            sz = dart_schema_scalar_size((DartSchemaTypeKind)k);
            if (sz == 0){ r->fail = 1; return 0; }
            break;
    }
    if (r->fail) return 0;
    f = &s->fields[idx];                      /* (unchanged pointer; recursion never moves it) */
    f->type_len = (uint32_t)(r->pos - kpos);
    f->size = sz;
    return sz;
}

/* Compile wire bytes already sitting at buf[0..wire_len] into a DartSchema placed after
 * them in buf. Returns NULL on a malformed blob or if buf[cap] is too small. */
static DartSchema *i_dart_schema_compile(uint8_t *buf, size_t wire_len, size_t cap){
    i_Rd r; uint8_t ver, root_kind, root_namelen;
    const char *root_name; size_t hoff, need; DartSchema *s;
    uint32_t total, nvar; uint16_t emitted = 0;

    if (!i_dart_schema_wire_fields(buf, wire_len, &total, &nvar) || total > 0xFFFFu) return NULL;

    r.w = buf; r.n = wire_len; r.pos = 0; r.fail = 0;
    ver = i_dart_rd_u8(&r);
    if (r.fail || ver != DART_SCHEMA_WIRE_VERSION) return NULL;
    root_namelen = i_dart_rd_u8(&r);
    root_name = (const char *)(buf + r.pos);
    i_dart_rd_skip(&r, root_namelen);
    if (r.fail || r.pos >= r.n) return NULL;
    root_kind = buf[r.pos];

    hoff = i_dart_schema_handle_off(buf, wire_len);
    need = hoff + sizeof(DartSchema) + (size_t)(total ? total - 1u : 0u) * sizeof(i_Field);
    if (need > cap) return NULL;

    s = (DartSchema *)(buf + hoff);
    s->wire = dart_bytes(buf, wire_len);
    s->name = dart_string(root_name, root_namelen);
    s->nfields = (uint16_t)total;
    s->n_var = (uint16_t)nvar;
    s->value_root = (uint8_t)(root_kind != DART_STRUCT);
    s->hash = i_dart_fnv1a64(buf, wire_len);
    if (!s->value_root){
        r.pos++;                                 /* past the root's STRUCT kind byte */
        s->size = i_dart_emit_struct(buf, wire_len, &r, s, &emitted, total,
                                     0, I_DART_NO_PARENT, 0);
    } else {                                     /* the bare/alias root IS the first field */
        if (total == 0) return NULL;
        i_dart_field_init(&s->fields[0], dart_string((const char *)(buf + r.pos), 0),
                          0, I_DART_NO_PARENT, 0);
        emitted = 1;
        s->size = i_dart_emit_type(buf, wire_len, &r, s, &emitted, total, 0, 0, 0);
    }
    if (r.fail || emitted != (uint16_t)total) return NULL;
    {   /* tail-frame ordinals in depth-first declaration order, and array ancestry */
        uint16_t i, ord = 0;
        for (i = 0; i < s->nfields; i++){
            i_Field *f = &s->fields[i];
            if (f->parent != I_DART_NO_PARENT){
                const i_Field *p = &s->fields[f->parent];
                f->arr_parent = (p->kind == DART_ARR || p->kind == DART_VARR)
                              ? f->parent : p->arr_parent;
            }
            if (i_dart_kind_var(f->kind)){
                f->offset = 0;                   /* no static offset: it is a frame */
                f->var_ord = ord++;
            }
        }
    }
    return s;
}
/* ---- builder ----------------------------------------------------------------------- */
/* ensure room for `extra` more bytes, growing the wire buffer through the hook */
static int i_dart_schema_builder_reserve(DartSchemaBuilder *b, size_t extra){
    size_t newcap; uint8_t *nb;
    if (b->err) return 0;
    if (b->len + extra <= b->cap) return 1;
    newcap = b->cap ? b->cap : 64u;
    while (newcap < b->len + extra){
        if (newcap > ((size_t)-1) / 2u){ newcap = b->len + extra; break; }
        newcap *= 2u;
    }
    nb = (uint8_t *)b->alloc(b->user, b->buf, newcap);
    if (!nb){ b->err = -1; return 0; }              /* realloc failure leaves b->buf intact */
    b->buf = nb; b->cap = newcap;
    return 1;
}
static void i_dart_schema_builder_put(DartSchemaBuilder *b, uint8_t v){
    if (!i_dart_schema_builder_reserve(b, 1)) return;
    b->buf[b->len++] = v;
}
static void i_dart_schema_builder_put_u16(DartSchemaBuilder *b, uint16_t v){
    if (!i_dart_schema_builder_reserve(b, 2)) return;
    i_dart_le_w16(b->buf + b->len, v); b->len += 2;
}
/* append raw bytes (a compiled type encoding from elsewhere) */
static void i_dart_schema_builder_put_raw(DartSchemaBuilder *b, const void *src, size_t len){
    if (!len) return;
    if (!src){ b->err = -6; return; }
    if (!i_dart_schema_builder_reserve(b, len)) return;
    memcpy(b->buf + b->len, src, len);
    b->len += len;
}
/* append `bytes` little-endian bytes of v (an enum option's backing-sized value) */
static void i_dart_schema_builder_put_le(DartSchemaBuilder *b, uint64_t v, uint32_t bytes){
    uint32_t i;
    if (!i_dart_schema_builder_reserve(b, bytes)) return;
    for (i = 0; i < bytes; i++) b->buf[b->len++] = (uint8_t)(v >> (8 * i));
}
static void i_dart_schema_builder_put_name(DartSchemaBuilder *b, const char *name){
    size_t n = 0, i; if (name) while (name[n]) n++;
    if (b->depth == 0 && (b->raw_type || b->value_root)){   /* the root type carries no field name */
        if (n) b->err = -4;
        return;
    }
    if (n > 255){ b->err = -4; return; }
    if (!i_dart_schema_builder_reserve(b, 1 + n)) return;
    b->buf[b->len++] = (uint8_t)n;
    for (i = 0; i < n; i++) b->buf[b->len++] = (uint8_t)name[i];
}
/* count a field on the innermost open struct (a bare/alias root takes exactly one type) */
static void i_dart_schema_builder_count(DartSchemaBuilder *b){
    uint16_t *c;
    if (b->err) return;
    if (b->depth > 0){
        c = &b->field_count[b->depth - 1];
        if (*c >= 255){ b->err = -5; return; }   /* nfields is a u8 */
        (*c)++;
        return;
    }
    if (b->value_root){
        if (b->value_root == 2){ b->err = -3; return; }   /* a bare root holds one type */
        b->value_root = 2;
        return;
    }
    if (b->raw_type) return;                     /* a standalone type body */
    b->err = -3;                                 /* no open struct */
}
/* open a struct type: [u8 STRUCT][u8 nfields placeholder], push a nesting level */
static void i_dart_schema_builder_open_struct(DartSchemaBuilder *b){
    if (b->err) return;
    if (b->depth >= DART_SCHEMA_MAX_DEPTH){ b->err = -2; return; }
    i_dart_schema_builder_put(b, (uint8_t)DART_STRUCT);
    b->count_pos[b->depth] = b->len;
    i_dart_schema_builder_put(b, 0);
    b->field_count[b->depth] = 0;
    b->depth++;
}

DartSchemaBuilder dart_schema_begin(DartAllocFn alloc, void *user, const char *root_name){
    DartSchemaBuilder b;
    memset(&b, 0, sizeof b);
    b.alloc = alloc; b.user = user; b.arr_depth = 0xFFFFu; b.base_depth = 1;
    if (!alloc){ b.err = -1; return b; }
    b.cap = 64u;
    b.buf = (uint8_t *)alloc(user, NULL, b.cap);
    if (!b.buf){ b.err = -1; b.cap = 0; return b; }
    i_dart_schema_builder_put(&b, (uint8_t)DART_SCHEMA_WIRE_VERSION);
    i_dart_schema_builder_put_name(&b, root_name);
    i_dart_schema_builder_open_struct(&b);              /* the root is a struct; depth -> 1 */
    return b;
}

/* the shared head of a bare-type root: [version][root_namelen][root_name] */
static DartSchemaBuilder i_dart_schema_begin_bare(DartAllocFn alloc, void *user,
                                                  const char *name){
    DartSchemaBuilder b; size_t n = 0, i;
    memset(&b, 0, sizeof b);
    b.alloc = alloc; b.user = user; b.arr_depth = 0xFFFFu;
    if (name) while (name[n]) n++;
    if (!alloc || n > 255){ b.err = -1; return b; }
    b.cap = 64u;
    b.buf = (uint8_t *)alloc(user, NULL, b.cap);
    if (!b.buf){ b.err = -1; b.cap = 0; return b; }
    b.value_root = 1;                                  /* depth stays 0: no struct is open */
    i_dart_schema_builder_put(&b, (uint8_t)DART_SCHEMA_WIRE_VERSION);
    i_dart_schema_builder_put(&b, (uint8_t)n);
    for (i = 0; i < n; i++) i_dart_schema_builder_put(&b, (uint8_t)name[i]);
    return b;
}

DartSchemaBuilder dart_schema_begin_value(DartAllocFn alloc, void *user){
    return i_dart_schema_begin_bare(alloc, user, NULL);
}

DartSchemaBuilder dart_schema_begin_alias(DartAllocFn alloc, void *user, const char *name){
    DartSchemaBuilder b = i_dart_schema_begin_bare(alloc, user, name);
    if (!b.err && (!name || !name[0])) b.err = -4;     /* an alias needs a name */
    return b;
}

/* a standalone type encoding (no schema header, no field name): the DSL's definition arena */
static DartSchemaBuilder i_dart_schema_begin_raw(DartAllocFn alloc, void *user){
    DartSchemaBuilder b;
    memset(&b, 0, sizeof b);
    b.alloc = alloc; b.user = user; b.arr_depth = 0xFFFFu; b.raw_type = 1;
    if (!alloc){ b.err = -1; return b; }
    b.cap = 64u;
    b.buf = (uint8_t *)alloc(user, NULL, b.cap);
    if (!b.buf){ b.err = -1; b.cap = 0; }
    return b;
}

void dart_schema_field(DartSchemaBuilder *b, const char *name, DartSchemaTypeKind kind){
    if (!b || b->err) return;
    if (dart_schema_scalar_size(kind) == 0){ b->err = -6; return; }  /* fixed scalars only */
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name); i_dart_schema_builder_put(b, (uint8_t)kind);
}

void dart_schema_field_array(DartSchemaBuilder *b, const char *name,
                             DartSchemaTypeKind elem_scalar, uint16_t count){
    if (!b || b->err) return;
    if (dart_schema_scalar_size(elem_scalar) == 0){ b->err = -6; return; }  /* scalar elements only */
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    i_dart_schema_builder_put(b, (uint8_t)DART_ARR); i_dart_schema_builder_put_u16(b, count);
    i_dart_schema_builder_put(b, (uint8_t)elem_scalar);
}

void dart_schema_field_string(DartSchemaBuilder *b, const char *name, uint16_t cap){
    if (!b || b->err) return;
    if (cap == 0){ b->err = -6; return; }
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    i_dart_schema_builder_put(b, (uint8_t)DART_STR); i_dart_schema_builder_put_u16(b, cap);
}

void dart_schema_field_string_array(DartSchemaBuilder *b, const char *name,
                                    uint16_t cap, uint16_t count){
    if (!b || b->err) return;
    if (cap == 0){ b->err = -6; return; }
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    i_dart_schema_builder_put(b, (uint8_t)DART_ARR); i_dart_schema_builder_put_u16(b, count);
    i_dart_schema_builder_put(b, (uint8_t)DART_STR); i_dart_schema_builder_put_u16(b, cap);
}

/* variable kinds have no static size, so they may sit at any struct depth but never
 * inside an ARRAY ELEMENT (whose stride must be known) */
static int i_dart_schema_builder_var_ok(DartSchemaBuilder *b){
    if (b->err) return 0;
    if (b->arr_depth != 0xFFFFu){ b->err = -8; return 0; }
    return 1;
}

void dart_schema_field_var_string(DartSchemaBuilder *b, const char *name){
    if (!b || !i_dart_schema_builder_var_ok(b)) return;
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    i_dart_schema_builder_put(b, (uint8_t)DART_VSTR);
}

void dart_schema_field_var_array(DartSchemaBuilder *b, const char *name,
                                 DartSchemaTypeKind elem_scalar){
    if (!b || !i_dart_schema_builder_var_ok(b)) return;
    if (dart_schema_scalar_size(elem_scalar) == 0){ b->err = -6; return; }  /* scalar elements only */
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    i_dart_schema_builder_put(b, (uint8_t)DART_VARR); i_dart_schema_builder_put(b, (uint8_t)elem_scalar);
}

void dart_schema_field_var_string_array(DartSchemaBuilder *b, const char *name, uint16_t cap){
    if (!b || !i_dart_schema_builder_var_ok(b)) return;
    if (cap == 0){ b->err = -6; return; }
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    i_dart_schema_builder_put(b, (uint8_t)DART_VARR);
    i_dart_schema_builder_put(b, (uint8_t)DART_STR); i_dart_schema_builder_put_u16(b, cap);
}

void dart_schema_field_map(DartSchemaBuilder *b, const char *name){
    if (!b || !i_dart_schema_builder_var_ok(b)) return;
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    i_dart_schema_builder_put(b, (uint8_t)DART_MAP);
}

/* the root type bytes of a compiled schema (past [version][namelen][name]) */
static int i_dart_schema_root_type(const DartSchema *t, const uint8_t **bytes, size_t *len){
    size_t off;
    if (!t || !t->wire.data) return 0;
    off = 2u + t->name.len;
    if (off >= t->wire.len) return 0;
    *bytes = t->wire.data + off;
    *len   = t->wire.len - off;
    return 1;
}

/* emit [NAMED][len][name] + the referenced schema's root type */
static void i_dart_schema_put_named(DartSchemaBuilder *b, const DartSchema *type){
    const uint8_t *tb; size_t tl;
    if (b->err) return;
    if (!type || !type->name.len || type->name.len > 255 ||
        !i_dart_schema_root_type(type, &tb, &tl)){ b->err = -6; return; }
    i_dart_schema_builder_put(b, (uint8_t)DART_NAMED);
    i_dart_schema_builder_put(b, (uint8_t)type->name.len);
    i_dart_schema_builder_put_raw(b, type->name.data, type->name.len);
    i_dart_schema_builder_put_raw(b, tb, tl);
}

void dart_schema_field_named(DartSchemaBuilder *b, const char *name, const DartSchema *type){
    if (!b || b->err) return;
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    i_dart_schema_put_named(b, type);
}

void dart_schema_field_named_array(DartSchemaBuilder *b, const char *name,
                                   const DartSchema *type, uint16_t count){
    if (!b || b->err) return;
    if (count == 0 && !i_dart_schema_builder_var_ok(b)) return;
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    if (count){
        i_dart_schema_builder_put(b, (uint8_t)DART_ARR);
        i_dart_schema_builder_put_u16(b, count);
    } else {
        i_dart_schema_builder_put(b, (uint8_t)DART_VARR);
    }
    i_dart_schema_put_named(b, type);
}

/* enum construction, streaming (the DSL parser also uses these so it never buffers the
 * whole option list): open writes name + [ENUM][backing][u16 n placeholder] and returns the
 * buffer offset of the placeholder; add appends one option; finish backpatches the count. */
static size_t i_dart_schema_field_enum_open(DartSchemaBuilder *b, const char *name,
                                            DartSchemaTypeKind backing){
    size_t count_pos;
    i_dart_schema_builder_count(b);
    i_dart_schema_builder_put_name(b, name);
    i_dart_schema_builder_put(b, (uint8_t)DART_ENUM);
    i_dart_schema_builder_put(b, (uint8_t)backing);
    count_pos = b->len;
    i_dart_schema_builder_put_u16(b, 0);               /* n: backpatched by finish */
    return count_pos;
}
static void i_dart_schema_field_enum_add(DartSchemaBuilder *b, DartSchemaTypeKind backing,
                                         int64_t value, const char *name, size_t name_len){
    size_t i;
    if (!b || b->err) return;
    if (name_len > 255){ b->err = -4; return; }
    i_dart_schema_builder_put_le(b, (uint64_t)value, dart_schema_scalar_size(backing));
    if (!i_dart_schema_builder_reserve(b, 1 + name_len)) return;
    b->buf[b->len++] = (uint8_t)name_len;
    for (i = 0; i < name_len; i++) b->buf[b->len++] = (uint8_t)name[i];
}
static void i_dart_schema_field_enum_finish(DartSchemaBuilder *b, size_t count_pos, uint16_t count){
    if (!b || b->err) return;
    i_dart_le_w16(b->buf + count_pos, count);          /* u16: up to 65535 options */
}

void dart_schema_field_enum(DartSchemaBuilder *b, const char *name, DartSchemaTypeKind backing,
                            const DartEnumVariant *variants, uint16_t n){
    size_t count_pos; uint16_t i;
    if (!b || b->err) return;
    if (dart_schema_scalar_size(backing) == 0 || !i_dart_enum_backing_ok((uint8_t)backing)){
        b->err = -6; return;                           /* integer backings only */
    }
    count_pos = i_dart_schema_field_enum_open(b, name, backing);
    for (i = 0; i < n; i++){
        int64_t v = variants ? variants[i].value : 0;
        const char *vn = variants ? variants[i].name : NULL;
        size_t vl = 0; if (vn) while (vn[vl]) vl++;
        if (!i_dart_enum_val_fits((uint8_t)backing, v)){ b->err = -6; return; }
        i_dart_schema_field_enum_add(b, backing, v, vn, vl);
    }
    i_dart_schema_field_enum_finish(b, count_pos, n);
}

void dart_schema_begin_struct(DartSchemaBuilder *b, const char *name){
    if (!b || b->err) return;
    if (b->value_root && b->depth == 0){ b->err = -3; return; }  /* a struct root comes from dart_schema_begin */
    i_dart_schema_builder_count(b);                 /* a field of the parent */
    i_dart_schema_builder_put_name(b, name);        /* the field's name */
    i_dart_schema_builder_open_struct(b);           /* the field's type: a struct */
}

void dart_schema_begin_struct_array(DartSchemaBuilder *b, const char *name, uint16_t count){
    if (!b || b->err) return;
    if (count == 0 && !i_dart_schema_builder_var_ok(b)) return;
    if (b->arr_depth != 0xFFFFu){ b->err = -8; return; }   /* one array level: elements stay fixed */
    i_dart_schema_builder_count(b);
    i_dart_schema_builder_put_name(b, name);
    if (count){
        i_dart_schema_builder_put(b, (uint8_t)DART_ARR);
        i_dart_schema_builder_put_u16(b, count);
    } else {
        i_dart_schema_builder_put(b, (uint8_t)DART_VARR);
    }
    i_dart_schema_builder_open_struct(b);
    if (!b->err) b->arr_depth = b->depth;           /* this level is an array element */
}

void dart_schema_end_struct(DartSchemaBuilder *b){
    if (!b || b->err) return;
    if (b->depth <= b->base_depth){ b->err = -3; return; }   /* the root closes in finish */
    if (b->arr_depth == b->depth) b->arr_depth = 0xFFFFu;
    b->depth--;
    b->buf[b->count_pos[b->depth]] = (uint8_t)b->field_count[b->depth];
}

DartSchema *dart_schema_finish(DartSchemaBuilder *b){
    DartSchema *s = NULL;
    int closed = b && !b->err &&
                 (b->value_root ? (b->depth == 0 && b->value_root == 2)   /* the one bare type */
                                : b->depth == 1);            /* depth != 1 = unbalanced begin/end */
    if (closed){
        size_t need; uint8_t *nb; uint32_t total = 0, nvar = 0;
        if (!b->value_root)
            b->buf[b->count_pos[0]] = (uint8_t)b->field_count[0]; /* backpatch the root field count */
        i_dart_schema_wire_fields(b->buf, b->len, &total, &nvar);   /* every field, all depths */
        need = b->len + 7u + sizeof(DartSchema)
             + (size_t)(total ? total - 1u : 0u) * sizeof(i_Field);
        nb = (uint8_t *)b->alloc(b->user, b->buf, need);      /* resize the block to hold the handle */
        if (nb){ b->buf = nb; b->cap = need; s = i_dart_schema_compile(b->buf, b->len, b->cap); }
    }
    if (!s && b && b->buf) b->alloc(b->user, b->buf, 0);      /* free on any failure */
    if (b) b->buf = NULL;                                     /* owned by s now, or freed */
    return s;
}

/* ---- parse a received schema ------------------------------------------------------- */
/* bytes a compiled schema needs for `wire`: copy + alignment pad + handle + the full
   flat field table. 0 if the wire is malformed. */
static size_t i_dart_schema_compiled_size(const void *wire, size_t wire_len){
    uint32_t total, nvar;
    if (!i_dart_schema_wire_fields(wire, wire_len, &total, &nvar) || total > 0xFFFFu) return 0;
    return wire_len + 7u + sizeof(DartSchema) + (size_t)(total ? total - 1u : 0u) * sizeof(i_Field);
}

DartSchema *dart_schema_parse(const void *wire, size_t wire_len, DartAllocFn alloc, void *user){
    size_t need, i; uint8_t *buf; DartSchema *s;
    if (!wire || !alloc || wire_len == 0) return NULL;
    need = i_dart_schema_compiled_size(wire, wire_len);
    if (need == 0) return NULL;                              /* malformed header */
    buf = (uint8_t *)alloc(user, NULL, need);
    if (!buf) return NULL;
    for (i = 0; i < wire_len; i++) buf[i] = ((const uint8_t *)wire)[i];   /* persist the bytes */
    s = i_dart_schema_compile(buf, wire_len, need);
    if (!s) alloc(user, buf, 0);                             /* malformed body: free, no leak */
    return s;
}

void dart_schema_free(DartSchema *s, DartAllocFn alloc, void *user){
    if (s && alloc) alloc(user, (void *)s->wire.data, 0);    /* wire.data is the block base */
}

/* ---- queries ----------------------------------------------------------------------- */
DartBytes dart_schema_wire(const DartSchema *s){
    DartBytes b; if (s) return s->wire;
    b.data = NULL; b.len = 0; return b;
}
uint64_t   dart_schema_hash(const DartSchema *s){ return s ? s->hash : 0; }
DartString dart_schema_name(const DartSchema *s){
    DartString n; if (s) return s->name;
    n.data = NULL; n.len = 0; return n;
}
uint32_t dart_schema_size(const DartSchema *s){ return s ? s->size : 0; }
uint16_t dart_schema_field_count(const DartSchema *s){ return s ? s->nfields : 0; }

int dart_schema_field_at(const DartSchema *s, uint16_t i, DartSchemaFieldInfo *out){
    const i_Field *f;
    if (!s || i >= s->nfields) return 0;
    f = &s->fields[i];
    if (out){
        out->name = f->name;
        out->type_name = f->type_name; out->elem_name = f->elem_name;
        out->kind = f->kind; out->elem = f->elem;
        out->count = f->count; out->depth = f->depth;
        out->str_cap = f->str_cap; out->arr_parent = f->arr_parent;
        out->offset = f->offset; out->size = f->size;
        out->elem_size = f->elem_size;
    }
    return 1;
}

/* Match a dotted path against a field: the last segment is its own name, the ones before
 * it its ancestors, and the first segment must sit at the root. A segment may carry an
 * element index ("corners[2].x"), which must land on an array field; *index gets it. */
static int i_dart_schema_path_match(const DartSchema *s, const i_Field *f, const char *path,
                                    size_t path_len, uint32_t *index){
    const char *end = path + path_len;
    uint32_t found = 0;
    for (;;){
        const char *seg = end, *nend;
        while (seg > path && seg[-1] != '.') seg--;
        nend = end;
        if (nend - seg >= 3 && nend[-1] == ']'){          /* strip a trailing [N] */
            const char *br = nend - 1;
            while (br > seg && br[-1] != '[') br--;
            if (br > seg){
                const char *q = br; uint32_t idx = 0; int ok = 1;
                while (q < nend - 1){
                    if (*q < '0' || *q > '9'){ ok = 0; break; }
                    idx = idx * 10u + (uint32_t)(*q - '0');
                    q++;
                }
                if (ok && q > br){
                    if (f->kind != DART_ARR && f->kind != DART_VARR) return 0;
                    found = idx;
                    nend = br - 1;
                }
            }
        }
        if (f->name.len != (size_t)(nend - seg) ||
            (f->name.len && memcmp(f->name.data, seg, f->name.len) != 0)) return 0;
        if (f->parent == I_DART_NO_PARENT){                /* root: all segments consumed */
            if (seg != path) return 0;
            if (index) *index = found;
            return 1;
        }
        if (seg == path) return 0;                         /* segments ran out early */
        end = seg - 1;                                     /* past the '.' */
        f = &s->fields[f->parent];
    }
}

static const i_Field *i_dart_schema_field_by_path(const DartSchema *s, const char *path,
                                                  uint32_t *index){
    uint16_t i; size_t n;
    if (index) *index = 0;
    if (!s || !path) return NULL;
    n = strlen(path);
    for (i = 0; i < s->nfields; i++)
        if (i_dart_schema_path_match(s, &s->fields[i], path, n, index)) return &s->fields[i];
    return NULL;
}

int dart_schema_field_index(const DartSchema *s, const char *path){
    const i_Field *f = i_dart_schema_field_by_path(s, path, NULL);
    return f ? (int)(f - s->fields) : -1;
}

DartBytes dart_schema_field_type_wire(const DartSchema *s, uint16_t field){
    const i_Field *f;
    if (!s || field >= s->nfields) return dart_bytes(NULL, 0);
    f = &s->fields[field];
    if ((size_t)f->type_off + f->type_len > s->wire.len) return dart_bytes(NULL, 0);
    return dart_bytes(s->wire.data + f->type_off, f->type_len);
}

uint16_t dart_schema_enum_count(const DartSchema *s, uint16_t field){
    if (!s || field >= s->nfields) return 0;
    return s->fields[field].kind == DART_ENUM ? s->fields[field].count : 0;
}

int dart_schema_enum_variant(const DartSchema *s, uint16_t field, uint16_t i,
                             int64_t *value, DartString *name){
    const i_Field *f; const uint8_t *w; uint32_t pos, end; uint16_t k; uint8_t bs;
    if (!s || field >= s->nfields) return 0;
    f = &s->fields[field];
    if (f->kind != DART_ENUM || i >= f->count) return 0;
    bs  = (uint8_t)dart_schema_scalar_size((DartSchemaTypeKind)f->elem);
    w   = s->wire.data;
    /* past [NAMED tag]?[ENUM][backing][u16 n] to the first option */
    pos = f->type_off;
    if (w[pos] == DART_NAMED) pos += 2u + w[pos + 1];
    pos += 4u;
    end = f->type_off + f->type_len;
    for (k = 0; k < i; k++){                 /* hop over earlier options: [value][u8 nl][name] */
        if ((size_t)pos + bs + 1u > end) return 0;
        pos += bs + 1u + w[pos + bs];
    }
    if ((size_t)pos + bs + 1u > end) return 0;
    { uint8_t nl = w[pos + bs];
      if ((size_t)pos + bs + 1u + nl > end) return 0;
      if (value) *value = i_dart_enum_read_val(f->elem, w + pos);
      if (name)  *name  = dart_string((const char *)(w + pos + bs + 1u), nl); }
    return 1;
}

DartString dart_enum_name_of(const DartSchema *s, uint16_t field, int64_t value){
    uint16_t i, n = dart_schema_enum_count(s, field);
    for (i = 0; i < n; i++){
        int64_t v; DartString nm;
        if (dart_schema_enum_variant(s, field, i, &v, &nm) && v == value) return nm;
    }
    return dart_string(NULL, 0);
}

int dart_enum_value_of(const DartSchema *s, uint16_t field, const char *name, int64_t *out){
    uint16_t i, n = dart_schema_enum_count(s, field);
    size_t want = 0;
    if (name) while (name[want]) want++;
    for (i = 0; i < n; i++){
        int64_t v; DartString nm;
        if (dart_schema_enum_variant(s, field, i, &v, &nm) && nm.len == want &&
            (want == 0 || memcmp(nm.data, name, want) == 0)){ if (out) *out = v; return 1; }
    }
    return 0;
}
/* ---- spelling types back as DSL text ------------------------------------------------- */
static const char *i_dart_why_kind(uint8_t k){
    switch (k){
        case DART_U8:  return "u8";  case DART_U16: return "u16";
        case DART_U32: return "u32"; case DART_U64: return "u64";
        case DART_I8:  return "i8";  case DART_I16: return "i16";
        case DART_I32: return "i32"; case DART_I64: return "i64";
        case DART_F32: return "f32"; case DART_F64: return "f64";
        case DART_BOOL: return "bool"; case DART_STRUCT: return "struct";
        default: return "?";
    }
}

/* A counting text sink: appends into [p,end) but always tallies the full length in n, so a
   NULL/short buffer still measures (snprintf semantics). */
typedef struct { char *p, *end; uint32_t n; } i_DartTextOut;
static void i_dart_out_raw(i_DartTextOut *o, const char *s, size_t len){
    size_t i;
    o->n += (uint32_t)len;
    for (i = 0; i < len && o->p < o->end; i++) *o->p++ = s[i];
}
static void i_dart_out_str(i_DartTextOut *o, const char *s){ i_dart_out_raw(o, s, strlen(s)); }
static void i_dart_out_view(i_DartTextOut *o, DartString v){ if (v.data) i_dart_out_raw(o, v.data, v.len); }
static void i_dart_out_indent(i_DartTextOut *o, int levels){ while (levels-- > 0) i_dart_out_raw(o, "  ", 2); }
static void i_dart_out_i64(i_DartTextOut *o, int64_t v){
    char tmp[20]; int k = 0; uint64_t u = v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    if (v < 0) i_dart_out_raw(o, "-", 1);
    do { tmp[k++] = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
    while (k) { char c = tmp[--k]; i_dart_out_raw(o, &c, 1); }
}

/* Step over the type at pos in already-validated wire; returns the position past it. */
static size_t i_dart_skip_type(const uint8_t *w, size_t n, size_t pos){
    uint8_t k;
    if (pos >= n) return n;
    k = w[pos++];
    switch (k){
        case DART_NAMED:
            if (pos >= n) return n;
            pos += 1u + w[pos];
            return i_dart_skip_type(w, n, pos);
        case DART_STR: return pos + 2u <= n ? pos + 2u : n;
        case DART_ARR: return i_dart_skip_type(w, n, pos + 2u <= n ? pos + 2u : n);
        case DART_VARR: return i_dart_skip_type(w, n, pos);
        case DART_VSTR: case DART_MAP: return pos;
        case DART_ENUM: {
            uint8_t bs; uint16_t cnt, i;
            if (pos + 3u > n) return n;
            bs = (uint8_t)dart_schema_scalar_size((DartSchemaTypeKind)w[pos]);
            cnt = i_dart_le_r16(w + pos + 1);
            pos += 3u;
            for (i = 0; i < cnt; i++){
                if (pos + bs + 1u > n) return n;
                pos += bs + 1u + w[pos + bs];
            }
            return pos <= n ? pos : n;
        }
        case DART_STRUCT: {
            uint8_t nf; uint16_t i;
            if (pos >= n) return n;
            nf = w[pos++];
            for (i = 0; i < nf; i++){
                if (pos >= n) return n;
                pos += 1u + w[pos];
                if (pos > n) return n;
                pos = i_dart_skip_type(w, n, pos);
            }
            return pos;
        }
        default: return pos;
    }
}

/* Spell the type at `pos` as DSL text; a NAMED type spells as its bare name (the caller
 * hoists its definition). Returns the position past the type. */
static size_t i_dart_spell_type(i_DartTextOut *o, const uint8_t *w, size_t n, size_t pos,
                                int indent){
    uint8_t k;
    if (pos >= n) return n;
    k = w[pos++];
    switch (k){
        case DART_NAMED: {
            uint8_t nl;
            if (pos >= n) return n;
            nl = w[pos++];
            i_dart_out_raw(o, (const char *)(w + pos), nl);
            return i_dart_skip_type(w, n, pos + nl);
        }
        case DART_STR:
            i_dart_out_str(o, "string<");
            i_dart_out_i64(o, (int64_t)i_dart_le_r16(w + pos));
            i_dart_out_str(o, ">");
            return pos + 2u;
        case DART_VSTR: i_dart_out_str(o, "string"); return pos;
        case DART_MAP:  i_dart_out_str(o, "map");    return pos;
        case DART_ARR: {
            uint16_t cnt = i_dart_le_r16(w + pos);
            size_t after = i_dart_spell_type(o, w, n, pos + 2u, indent);
            i_dart_out_str(o, "[");
            i_dart_out_i64(o, (int64_t)cnt);
            i_dart_out_str(o, "]");
            return after;
        }
        case DART_VARR: {
            size_t after = i_dart_spell_type(o, w, n, pos, indent);
            i_dart_out_str(o, "[]");
            return after;
        }
        case DART_ENUM: {                       /* enum<uN> { Name = value, ... } */
            uint8_t backing; uint16_t cnt, i; uint32_t bs;
            if (pos + 3u > n) return n;
            backing = w[pos]; cnt = i_dart_le_r16(w + pos + 1); pos += 3u;
            bs = dart_schema_scalar_size((DartSchemaTypeKind)backing);
            i_dart_out_str(o, "enum<");
            i_dart_out_str(o, i_dart_why_kind(backing));
            i_dart_out_str(o, "> { ");
            for (i = 0; i < cnt; i++){
                uint8_t nl;
                if (pos + bs + 1u > n) return n;
                if (i) i_dart_out_str(o, ", ");
                nl = w[pos + bs];
                i_dart_out_raw(o, (const char *)(w + pos + bs + 1u), nl);
                i_dart_out_str(o, " = ");
                i_dart_out_i64(o, i_dart_enum_read_val(backing, w + pos));
                pos += bs + 1u + nl;
            }
            i_dart_out_str(o, " }");
            return pos;
        }
        case DART_STRUCT: {
            uint8_t nf; uint16_t i;
            if (pos >= n) return n;
            nf = w[pos++];
            i_dart_out_str(o, "{\n");
            for (i = 0; i < nf; i++){
                uint8_t nl;
                if (pos >= n) return n;
                nl = w[pos++];
                i_dart_out_indent(o, indent + 1);
                i_dart_out_raw(o, (const char *)(w + pos), nl);
                pos += nl;
                i_dart_out_str(o, ": ");
                pos = i_dart_spell_type(o, w, n, pos, indent + 1);
                i_dart_out_str(o, ",\n");
            }
            i_dart_out_indent(o, indent);
            i_dart_out_str(o, "}");
            return pos;
        }
        default:
            i_dart_out_str(o, i_dart_why_kind(k));
            return pos;
    }
}

/* Named types used by a schema, in DEPENDENCY order (a type is recorded only after the
 * types it references), deduped by name so each prints one leading definition. */
#define I_DART_MAX_DEFS 48u
typedef struct {
    const uint8_t *w; size_t n;
    struct { uint32_t noff, toff; uint8_t nlen; } d[I_DART_MAX_DEFS];
    uint16_t count;
} i_DartDefScan;

static void i_dart_defscan_add(i_DartDefScan *L, uint32_t noff, uint8_t nlen, uint32_t toff){
    uint16_t i;
    for (i = 0; i < L->count; i++)
        if (L->d[i].nlen == nlen && memcmp(L->w + L->d[i].noff, L->w + noff, nlen) == 0) return;
    if (L->count >= I_DART_MAX_DEFS) return;   /* deep enough: the tail spells by reference */
    L->d[L->count].noff = noff; L->d[L->count].nlen = nlen; L->d[L->count].toff = toff;
    L->count++;
}

static size_t i_dart_defscan_type(i_DartDefScan *L, size_t pos){
    uint8_t k;
    if (pos >= L->n) return L->n;
    k = L->w[pos];
    if (k == DART_NAMED){
        uint8_t nl; size_t ipos, after;
        if (pos + 2u > L->n) return L->n;
        nl = L->w[pos + 1];
        ipos = pos + 2u + nl;
        if (ipos > L->n) return L->n;
        after = i_dart_defscan_type(L, ipos);        /* dependencies first */
        i_dart_defscan_add(L, (uint32_t)(pos + 2u), nl, (uint32_t)ipos);
        return after;
    }
    pos++;
    switch (k){
        case DART_ARR:  return i_dart_defscan_type(L, pos + 2u <= L->n ? pos + 2u : L->n);
        case DART_VARR: return i_dart_defscan_type(L, pos);
        case DART_STRUCT: {
            uint8_t nf; uint16_t i;
            if (pos >= L->n) return L->n;
            nf = L->w[pos++];
            for (i = 0; i < nf; i++){
                if (pos >= L->n) return L->n;
                pos += 1u + L->w[pos];
                if (pos > L->n) return L->n;
                pos = i_dart_defscan_type(L, pos);
            }
            return pos;
        }
        default: return i_dart_skip_type(L->w, L->n, pos - 1u);
    }
}

uint32_t dart_schema_print(const DartSchema *s, char *buf, size_t cap){
    i_DartTextOut o;
    i_DartDefScan scan;
    size_t root_type;
    uint16_t i;
    o.p   = (buf && cap) ? buf : NULL;
    o.end = o.p ? buf + cap - 1 : NULL;   /* reserve one byte for the NUL */
    o.n   = 0;
    if (!s){ if (buf && cap) buf[0] = '\0'; return 0; }
    root_type = 2u + s->name.len;
    scan.w = s->wire.data; scan.n = s->wire.len; scan.count = 0;
    i_dart_defscan_type(&scan, root_type);
    for (i = 0; i < scan.count; i++){                    /* each named type, once, in order */
        i_dart_out_raw(&o, (const char *)(scan.w + scan.d[i].noff), scan.d[i].nlen);
        i_dart_out_str(&o, " = ");
        i_dart_spell_type(&o, scan.w, scan.n, scan.d[i].toff, 0);
        i_dart_out_str(&o, "\n");
    }
    if (s->value_root){                    /* a bare type, or `Name = type` for an alias */
        if (s->name.len){
            i_dart_out_view(&o, s->name);
            i_dart_out_str(&o, " = ");
        }
        i_dart_spell_type(&o, scan.w, scan.n, root_type, 0);
        i_dart_out_str(&o, "\n");
    } else {
        i_dart_out_view(&o, s->name);
        i_dart_out_str(&o, " ");
        i_dart_spell_type(&o, scan.w, scan.n, root_type, 0);
        i_dart_out_str(&o, "\n");
    }
    if (o.p) *o.p = '\0';                   /* o.p <= end, the reserved byte */
    else if (buf && cap) buf[0] = '\0';
    return o.n;
}

/* ---- reader/writer compatibility ----------------------------------------------------- */
/* find a TOP-LEVEL field by name (the match predicate works on top-level fields; a
 * nested type is compared as one exact unit) */
static const i_Field *i_dart_schema_find(const DartSchema *s, DartString name){
    uint16_t i;
    for (i = 0; i < s->nfields; i++){
        const i_Field *f = &s->fields[i];
        if (f->depth == 0 && f->name.len == name.len &&
            (name.len == 0 || memcmp(f->name.data, name.data, name.len) == 0)) return f;
    }
    return NULL;
}

/* consume a NAMED wrapper if one is present, returning its name ({NULL,0} if not) */
static DartString i_dart_rd_type_name(i_Rd *r){
    DartString nm = dart_string(NULL, 0);
    if (r->pos < r->n && r->w[r->pos] == DART_NAMED){
        uint8_t nl;
        r->pos++;
        nl = i_dart_rd_u8(r);
        nm = dart_string((const char *)(r->w + r->pos), nl);
        i_dart_rd_skip(r, nl);
        if (r->fail) return dart_string(NULL, 0);
    }
    return nm;
}

static void i_dart_rd_skip_enum(i_Rd *r, uint8_t backing){
    uint16_t n = i_dart_rd_u16(r), i;
    uint32_t bs = dart_schema_scalar_size((DartSchemaTypeKind)backing);
    for (i = 0; i < n && !r->fail; i++){
        i_dart_rd_skip(r, bs);
        i_dart_rd_skip(r, i_dart_rd_u8(r));
    }
}

/* Can a reader declaring the type at ra read a writer's type at rb? Both readers advance
 * past their type. TYPE NAMES NARROW: an anonymous reader type reads any writer type of
 * the same shape, a named one demands the identical writer name. Everything else compares
 * exactly (a struct member for member, an array element for element). */
static int i_dart_type_cmp(i_Rd *ra, i_Rd *rb, uint16_t depth){
    DartString na, nb; uint8_t ka, kb;
    if (depth > DART_SCHEMA_MAX_DEPTH + 1u) return 0;
    na = i_dart_rd_type_name(ra);
    nb = i_dart_rd_type_name(rb);
    if (ra->fail || rb->fail) return 0;
    if (na.len && !dart_string_eq(na, nb)) return 0;
    ka = i_dart_rd_u8(ra); kb = i_dart_rd_u8(rb);
    if (ra->fail || rb->fail || ka != kb) return 0;
    switch (ka){
        case DART_STR: {
            uint16_t ca = i_dart_rd_u16(ra), cb = i_dart_rd_u16(rb);
            return !ra->fail && !rb->fail && ca == cb;
        }
        case DART_ARR: {
            uint16_t ca = i_dart_rd_u16(ra), cb = i_dart_rd_u16(rb);
            if (ra->fail || rb->fail || ca != cb) return 0;
            return i_dart_type_cmp(ra, rb, (uint16_t)(depth + 1));
        }
        case DART_VARR:
            return i_dart_type_cmp(ra, rb, (uint16_t)(depth + 1));
        case DART_VSTR: case DART_MAP:
            return 1;
        case DART_ENUM: {                        /* backing width only: names are advisory */
            uint8_t ba = i_dart_rd_u8(ra), bb = i_dart_rd_u8(rb);
            i_dart_rd_skip_enum(ra, ba);
            i_dart_rd_skip_enum(rb, bb);
            return !ra->fail && !rb->fail && ba == bb;
        }
        case DART_STRUCT: {
            uint8_t nfa = i_dart_rd_u8(ra), nfb = i_dart_rd_u8(rb); uint16_t i;
            if (ra->fail || rb->fail || nfa != nfb) return 0;
            for (i = 0; i < nfa; i++){
                uint8_t la = i_dart_rd_u8(ra), lb = i_dart_rd_u8(rb);
                const char *pa = (const char *)(ra->w + ra->pos);
                const char *pb = (const char *)(rb->w + rb->pos);
                i_dart_rd_skip(ra, la); i_dart_rd_skip(rb, lb);
                if (ra->fail || rb->fail || la != lb) return 0;
                if (la && memcmp(pa, pb, la) != 0) return 0;
                if (!i_dart_type_cmp(ra, rb, (uint16_t)(depth + 1))) return 0;
            }
            return 1;
        }
        default:
            return dart_schema_scalar_size((DartSchemaTypeKind)ka) != 0;
    }
}

/* the two fields' types, compared from the wire */
static int i_dart_field_cmp(const DartSchema *sa, const i_Field *a,
                            const DartSchema *sb, const i_Field *b){
    i_Rd ra, rb;
    ra.w = sa->wire.data; ra.n = sa->wire.len; ra.pos = a->type_off; ra.fail = 0;
    rb.w = sb->wire.data; rb.n = sb->wire.len; rb.pos = b->type_off; rb.fail = 0;
    return i_dart_type_cmp(&ra, &rb, 0);
}

/* bounded appenders for the subset-why text (no stdio; a NULL buffer skips all text) */
static char *i_dart_why_str(char *p, char *end, const char *s){
    if (!p) return NULL;
    while (*s && p < end) *p++ = *s++;
    return p;
}
static char *i_dart_why_view(char *p, char *end, DartString s){
    size_t i;
    if (!p) return NULL;
    for (i = 0; i < s.len && p < end; i++) *p++ = s.data[i];
    return p;
}
static char *i_dart_why_u(char *p, char *end, uint32_t v){
    char tmp[10]; int n = 0;
    if (!p) return NULL;
    do { tmp[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n && p < end) *p++ = tmp[--n];
    return p;
}
/* a field's type in compact DSL form: "Float3[4]", "f32[8]", "string<33>", "map" */
static char *i_dart_why_type(char *p, char *end, const i_Field *f){
    uint8_t is_arr = (uint8_t)(f->kind == DART_ARR || f->kind == DART_VARR);
    if (f->type_name.len) return i_dart_why_view(p, end, f->type_name);
    if (f->kind == DART_MAP)  return i_dart_why_str(p, end, "map");
    if (f->kind == DART_VSTR) return i_dart_why_str(p, end, "string");
    if (f->kind == DART_ENUM){                            /* enum<backing> (the width is what matters) */
        p = i_dart_why_str(p, end, "enum<");
        p = i_dart_why_str(p, end, i_dart_why_kind(f->elem));
        return i_dart_why_str(p, end, ">");
    }
    if (is_arr && f->elem_name.len){
        p = i_dart_why_view(p, end, f->elem_name);
    } else {
        uint8_t elem = is_arr ? f->elem : f->kind;
        if (elem == DART_STR){
            p = i_dart_why_str(p, end, "string<");
            p = i_dart_why_u(p, end, f->str_cap);
            p = i_dart_why_str(p, end, ">");
        } else {
            p = i_dart_why_str(p, end, i_dart_why_kind(elem));
        }
    }
    if (f->kind == DART_ARR){
        p = i_dart_why_str(p, end, "[");
        p = i_dart_why_u(p, end, f->count);
        p = i_dart_why_str(p, end, "]");
    } else if (f->kind == DART_VARR){
        p = i_dart_why_str(p, end, "[]");
    }
    return p;
}
static int i_dart_why_done(char *buf, char *p){   /* NUL-terminate the reason, refuse */
    if (buf) *p = '\0';
    return 0;
}
/* a schema's root in DSL words: a bare type's spelling, or `struct 'Name'` */
static char *i_dart_why_root(char *p, char *end, const DartSchema *s){
    if (s->value_root){
        if (s->name.len){
            p = i_dart_why_view(p, end, s->name);
            p = i_dart_why_str(p, end, " = ");
        }
        return i_dart_why_type(p, end, &s->fields[0]);
    }
    p = i_dart_why_str(p, end, "struct '");
    p = i_dart_why_view(p, end, s->name);
    return i_dart_why_str(p, end, "'");
}

int dart_schema_subset_why(const DartSchema *sub, const DartSchema *pub,
                           char *buf, size_t cap){
    uint16_t i;
    char *p = (buf && cap) ? buf : NULL, *end = p ? buf + cap - 1 : NULL;
    if (p) *p = '\0';
    if (!sub || !pub)
        return i_dart_why_done(p, i_dart_why_str(p, end, "schema missing"));
    if (sub->value_root || pub->value_root){        /* a bare root: the two roots ARE the types */
        int named_ok = !sub->name.len || dart_string_eq(sub->name, pub->name);
        if (sub->value_root == pub->value_root && named_ok &&
            i_dart_field_cmp(sub, &sub->fields[0], pub, &pub->fields[0])) return 1;
        p = i_dart_why_str(p, end, "root: reader ");
        p = i_dart_why_root(p, end, sub);
        p = i_dart_why_str(p, end, ", writer ");
        return i_dart_why_done(buf, i_dart_why_root(p, end, pub));
    }
    if (sub->name.len != pub->name.len ||
        (sub->name.len && memcmp(sub->name.data, pub->name.data, sub->name.len) != 0)){
        p = i_dart_why_str(p, end, "reader type '"); p = i_dart_why_view(p, end, sub->name);
        p = i_dart_why_str(p, end, "' != writer type '"); p = i_dart_why_view(p, end, pub->name);
        return i_dart_why_done(buf, i_dart_why_str(p, end, "'"));
    }
    for (i = 0; i < sub->nfields; i++){
        const i_Field *a = &sub->fields[i], *b;
        if (a->depth != 0) continue;                          /* members ride their struct */
        b = i_dart_schema_find(pub, a->name);
        if (!b){
            p = i_dart_why_str(p, end, "field '"); p = i_dart_why_view(p, end, a->name);
            return i_dart_why_done(buf, i_dart_why_str(p, end, "' missing from writer"));
        }
        if (!i_dart_field_cmp(sub, a, pub, b)){
            p = i_dart_why_str(p, end, "field '"); p = i_dart_why_view(p, end, a->name);
            p = i_dart_why_str(p, end, "': reader "); p = i_dart_why_type(p, end, a);
            p = i_dart_why_str(p, end, ", writer ");
            return i_dart_why_done(buf, i_dart_why_type(p, end, b));
        }
    }
    return 1;
}

int dart_schema_subset(const DartSchema *sub, const DartSchema *pub){
    return dart_schema_subset_why(sub, pub, NULL, 0);
}

/* the flat index just past a field's subtree (its members, and their members) */
static uint16_t i_dart_subtree_end(const DartSchema *s, uint16_t i){
    uint16_t d = s->fields[i].depth, k = (uint16_t)(i + 1u);
    while (k < s->nfields && s->fields[k].depth > d) k++;
    return k;
}

DartSchema *dart_schema_rebase(const DartSchema *sub, const DartSchema *pub,
                               DartAllocFn alloc, void *user){
    DartSchema *r; uint16_t i = 0;
    if (!alloc || !dart_schema_subset(sub, pub)) return NULL;
    r = dart_schema_parse(sub->wire.data, sub->wire.len, alloc, user);
    if (!r) return NULL;
    while (i < r->nfields){                                   /* the writer's layout... */
        const i_Field *p = i_dart_schema_find(pub, r->fields[i].name);
        uint16_t re = i_dart_subtree_end(r, i), j, k;
        if (!p || r->fields[i].depth != 0){ dart_schema_free(r, alloc, user); return NULL; }
        j = (uint16_t)(p - pub->fields);
        for (k = 0; (uint16_t)(i + k) < re; k++){             /* the subtrees match exactly */
            i_Field *rf = &r->fields[i + k];
            const i_Field *pf;
            if ((uint16_t)(j + k) >= pub->nfields){ dart_schema_free(r, alloc, user); return NULL; }
            pf = &pub->fields[j + k];
            rf->offset = pf->offset;
            rf->var_ord = pf->var_ord;
        }
        i = re;
    }
    r->size = pub->size;                                      /* ...and the writer's layout bounds: */
    r->n_var = pub->n_var;                                    /* validate/walk see pub's messages  */
    return r;
}
/* ---- the variable tail --------------------------------------------------------------- */
uint32_t dart_schema_msg_min(const DartSchema *s){
    return s ? s->size + 4u * s->n_var : 0;
}

uint32_t dart_schema_msg_len(const DartSchema *s, const void *buf, size_t cap){
    const uint8_t *p = (const uint8_t *)buf;
    uint64_t total; uint16_t i;
    if (!s || !p) return 0;
    total = s->size;
    for (i = 0; i < s->n_var; i++){                     /* hop the frames, bounds-checked */
        if (total + 4u > cap) return 0;
        total += 4u + (uint64_t)i_dart_le_r32(p + total);
        if (total > cap) return 0;
    }
    return total <= 0xFFFFFFFFu ? (uint32_t)total : 0;
}

/* payload view of variable field ordinal `ord`; {NULL,0} on any bound break */
static DartBytes i_dart_schema_frame(const DartSchema *s, DartBytes msg, uint16_t ord){
    uint64_t pos = s->size; uint32_t flen; uint16_t i;
    for (i = 0; i <= ord && i < s->n_var; i++){
        if (pos + 4u > msg.len) break;
        flen = i_dart_le_r32(msg.data + pos);
        if (pos + 4u + flen > msg.len) break;
        if (i == ord) return dart_bytes(msg.data + pos + 4u, flen);
        pos += 4u + (uint64_t)flen;
    }
    return dart_bytes(NULL, 0);
}

/* Resize variable field `ord`'s frame to new_len, memmoving the rest of the tail. src
 * (may be NULL to keep the existing prefix and zero any growth) must not alias buf. */
static int i_dart_schema_frame_write(const DartSchema *s, uint8_t *buf, size_t cap,
                                     uint16_t ord, const void *src, size_t new_len){
    uint64_t pos = s->size, total; uint32_t old; uint16_t i;
    if (new_len && !src && src != NULL) return 0;
    if (new_len > 0xFFFFFFFFu - 4u) return 0;
    total = dart_schema_msg_len(s, buf, cap);
    if (total == 0) return 0;                           /* malformed/uninitialized buffer */
    for (i = 0; i < ord; i++) pos += 4u + (uint64_t)i_dart_le_r32(buf + pos);
    old = i_dart_le_r32(buf + pos);                     /* bounds proven by msg_len's walk */
    if (total - old + new_len > cap) return 0;          /* never silently truncate */
    memmove(buf + pos + 4u + new_len, buf + pos + 4u + old,
            (size_t)(total - (pos + 4u + old)));
    i_dart_le_w32(buf + pos, (uint32_t)new_len);
    if (src){ if (new_len) memcpy(buf + pos + 4u, src, new_len); }
    else if (new_len > old) memset(buf + pos + 4u + old, 0, new_len - old);
    return 1;
}
static int i_dart_schema_set_frame(const DartSchema *s, uint8_t *buf, size_t cap,
                                   uint16_t ord, const void *src, size_t src_len){
    if (src_len && !src) return 0;
    return i_dart_schema_frame_write(s, buf, cap, ord, src_len ? src : (const void *)"", src_len);
}

/* ---- read a message ---------------------------------------------------------------- */
int dart_schema_validate(const DartSchema *s, DartBytes msg){
    uint32_t total;
    if (!s) return 0;
    if (s->n_var == 0) return msg.len == s->size;
    total = dart_schema_msg_len(s, msg.data, msg.len);  /* frames must consume it exactly */
    return total != 0 && total == msg.len;
}

/* Where field f's bytes sit in msg for array element `index`, bounds-checked. A field
 * under a FIXED struct array is at its element-0 offset plus index * stride; one under a
 * VARIABLE struct array is at that stride inside the array's frame. */
static int i_dart_field_addr(const DartSchema *s, DartBytes msg, const i_Field *f,
                             uint32_t index, size_t *out_off){
    size_t off;
    if (i_dart_kind_var(f->kind)){ *out_off = 0; return 1; }   /* frames locate themselves */
    if (f->arr_parent == I_DART_NO_PARENT){
        off = f->offset;
    } else {
        const i_Field *a = &s->fields[f->arr_parent];
        if (a->elem_size == 0) return 0;
        if (a->kind == DART_ARR){
            if (index >= a->count) return 0;
            off = (size_t)f->offset + (size_t)index * a->elem_size;
        } else {
            DartBytes fr = i_dart_schema_frame(s, msg, a->var_ord);
            if (!fr.data || (uint64_t)index * a->elem_size + a->elem_size > fr.len) return 0;
            off = (size_t)(fr.data - msg.data) + (size_t)index * a->elem_size + f->offset;
        }
    }
    if (off + f->size > msg.len) return 0;
    *out_off = off;
    return 1;
}

/* Resolve a (possibly indexed) dotted path against a message; NULL if unknown, out of
 * range, or the message is too short. */
static const i_Field *i_dart_schema_read_lookup(const DartSchema *s, DartBytes msg,
                                                const char *field, size_t *off){
    uint32_t index = 0;
    const i_Field *f = i_dart_schema_field_by_path(s, field, &index);
    if (!f || !i_dart_field_addr(s, msg, f, index, off)) return NULL;
    return f;
}

static uint64_t i_dart_schema_read_uint(uint8_t kind, const uint8_t *p){
    switch (kind){
        case DART_U8: case DART_BOOL: return p[0];
        case DART_U16: return i_dart_le_r16(p);
        case DART_U32: return i_dart_le_r32(p);
        case DART_U64: return i_dart_le_r64(p);
        default: return 0;
    }
}
static int64_t i_dart_schema_read_int(uint8_t kind, const uint8_t *p){
    switch (kind){
        case DART_I8:  return (int8_t)p[0];
        case DART_I16: return (int16_t)i_dart_le_r16(p);
        case DART_I32: return (int32_t)i_dart_le_r32(p);
        case DART_I64: return (int64_t)i_dart_le_r64(p);
        default: return 0;
    }
}
static double i_dart_schema_read_f64(uint8_t kind, const uint8_t *p){
    if (kind == DART_F64){ uint64_t b = i_dart_le_r64(p); double d; memcpy(&d, &b, 8); return d; }
    if (kind == DART_F32){ uint32_t b = i_dart_le_r32(p); float  x; memcpy(&x, &b, 4); return (double)x; }
    return 0.0;
}

uint64_t dart_get_uint(DartBytes msg, const DartSchema *s, const char *field){
    size_t off; const i_Field *f = i_dart_schema_read_lookup(s, msg, field, &off);
    if (!f) return 0;
    if (f->kind == DART_ENUM) return (uint64_t)i_dart_enum_read_val(f->elem, msg.data + off);
    return i_dart_schema_read_uint(f->kind, msg.data + off);
}

int64_t dart_get_int(DartBytes msg, const DartSchema *s, const char *field){
    size_t off; const i_Field *f = i_dart_schema_read_lookup(s, msg, field, &off);
    if (!f) return 0;
    if (f->kind == DART_ENUM) return i_dart_enum_read_val(f->elem, msg.data + off);
    return i_dart_schema_read_int(f->kind, msg.data + off);
}

double dart_get_f64(DartBytes msg, const DartSchema *s, const char *field){
    size_t off; const i_Field *f = i_dart_schema_read_lookup(s, msg, field, &off);
    return f ? i_dart_schema_read_f64(f->kind, msg.data + off) : 0.0;
}

float dart_get_f32(DartBytes msg, const DartSchema *s, const char *field){
    size_t off; const i_Field *f = i_dart_schema_read_lookup(s, msg, field, &off);
    if (!f || f->kind != DART_F32) return 0.0f;
    { uint32_t b = i_dart_le_r32(msg.data + off); float x; memcpy(&x, &b, 4); return x; }
}

DartBytes dart_get_array(DartBytes msg, const DartSchema *s, const char *field){
    DartBytes out; size_t off; const i_Field *f = i_dart_schema_read_lookup(s, msg, field, &off);
    out.data = NULL; out.len = 0;
    if (!f) return out;
    if (f->kind == DART_VARR){
        DartBytes fr = i_dart_schema_frame(s, msg, f->var_ord);
        if (!fr.data || !f->elem_size) return out;
        out.data = fr.data; out.len = fr.len - fr.len % f->elem_size;   /* whole elements only */
        return out;
    }
    if (f->kind != DART_ARR) return out;
    out.data = msg.data + off; out.len = f->size;
    return out;
}

/* live element count of an array field (declared for a fixed one, per message otherwise) */
static uint32_t i_dart_array_count(const DartSchema *s, DartBytes msg, const i_Field *f){
    if (!f || !f->elem_size) return 0;
    if (f->kind == DART_ARR) return f->count;
    if (f->kind == DART_VARR){
        DartBytes fr = i_dart_schema_frame(s, msg, f->var_ord);
        return fr.data ? (uint32_t)(fr.len / f->elem_size) : 0;
    }
    return 0;
}

uint32_t dart_get_array_count(DartBytes msg, const DartSchema *s, const char *field){
    size_t off; const i_Field *f = i_dart_schema_read_lookup(s, msg, field, &off);
    return i_dart_array_count(s, msg, f);
}

uint32_t dart_array_count_at(DartBytes msg, const DartSchema *s, uint16_t field){
    if (!s || field >= s->nfields) return 0;
    return i_dart_array_count(s, msg, &s->fields[field]);
}

/* live-string view of one slot ([u16 len][cap bytes]); the length is clamped to the cap
   so a hostile message can never over-read */
static DartString i_dart_schema_str_view(const uint8_t *slot, uint16_t cap){
    uint16_t len = i_dart_le_r16(slot);
    if (len > cap) len = cap;
    return dart_string((const char *)(slot + 2), len);
}

DartString dart_get_string(DartBytes msg, const DartSchema *s, const char *field){
    size_t off; const i_Field *f = i_dart_schema_read_lookup(s, msg, field, &off);
    if (!f) return dart_string(NULL, 0);
    if (f->kind == DART_VSTR){
        DartBytes fr = i_dart_schema_frame(s, msg, f->var_ord);   /* the frame IS the string */
        return dart_string((const char *)fr.data, fr.data ? fr.len : 0);
    }
    if (f->kind != DART_STR) return dart_string(NULL, 0);
    return i_dart_schema_str_view(msg.data + off, f->str_cap);
}

DartString dart_get_string_at(DartBytes msg, const DartSchema *s, const char *field,
                              uint16_t index){
    size_t off; const i_Field *f = i_dart_schema_read_lookup(s, msg, field, &off);
    if (!f || f->elem != DART_STR) return dart_string(NULL, 0);
    if (f->kind == DART_VARR){
        DartBytes fr = i_dart_schema_frame(s, msg, f->var_ord);
        uint32_t esz = f->elem_size;
        if (!fr.data || !esz || ((uint64_t)index + 1u) * esz > fr.len) return dart_string(NULL, 0);
        return i_dart_schema_str_view(fr.data + (size_t)index * esz, f->str_cap);
    }
    if (f->kind != DART_ARR || index >= f->count) return dart_string(NULL, 0);
    return i_dart_schema_str_view(msg.data + off + (size_t)index * f->elem_size, f->str_cap);
}

DartBytes dart_get_map(DartBytes msg, const DartSchema *s, const char *field){
    size_t off; const i_Field *f = i_dart_schema_read_lookup(s, msg, field, &off);
    if (!f || f->kind != DART_MAP) return dart_bytes(NULL, 0);
    return i_dart_schema_frame(s, msg, f->var_ord);
}

DartString dart_get_enum(DartBytes msg, const DartSchema *s, const char *field){
    size_t off; const i_Field *f = i_dart_schema_read_lookup(s, msg, field, &off);
    if (!f || f->kind != DART_ENUM) return dart_string(NULL, 0);
    return dart_enum_name_of(s, (uint16_t)(f - s->fields),
                             i_dart_enum_read_val(f->elem, msg.data + off));
}

int dart_get_value_at(DartBytes msg, const DartSchema *s, uint16_t field, uint32_t elem,
                      DartValue *out){
    const i_Field *f; const uint8_t *p; size_t off;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!s || field >= s->nfields) return 0;
    f = &s->fields[field];
    if (!i_dart_field_addr(s, msg, f, elem, &off)) return 0;
    p = msg.data + off;
    out->kind = f->kind; out->elem = f->elem; out->count = f->count; out->str_cap = f->str_cap;
    switch (f->kind){
        case DART_U8: case DART_U16: case DART_U32: case DART_U64: case DART_BOOL:
            out->v.u = i_dart_schema_read_uint(f->kind, p); break;
        case DART_I8: case DART_I16: case DART_I32: case DART_I64:
            out->v.i = i_dart_schema_read_int(f->kind, p); break;
        case DART_F32: case DART_F64:
            out->v.f = i_dart_schema_read_f64(f->kind, p); break;
        case DART_ARR: case DART_STRUCT:
            out->bytes = dart_bytes(p, f->size); break;
        case DART_STR: {
            DartString sv = i_dart_schema_str_view(p, f->str_cap);
            out->bytes = dart_bytes(sv.data, sv.len); break;
        }
        case DART_VSTR: {
            DartBytes fr = i_dart_schema_frame(s, msg, f->var_ord);
            if (!fr.data) return 0;
            out->bytes = fr; break;
        }
        case DART_VARR: {
            DartBytes fr = i_dart_schema_frame(s, msg, f->var_ord);
            uint64_t n;
            if (!fr.data || !f->elem_size) return 0;
            n = fr.len / f->elem_size;
            out->bytes = dart_bytes(fr.data, (size_t)(n * f->elem_size));
            out->count = n > 0xFFFFu ? 0xFFFFu : (uint16_t)n;   /* saturated; bytes.len rules */
            break;
        }
        case DART_MAP: {
            DartBytes fr = i_dart_schema_frame(s, msg, f->var_ord);
            if (!fr.data) return 0;
            out->bytes = fr;
            out->count = dart_map_count(fr); break;
        }
        case DART_ENUM:                    /* value in v.i (also v.u); elem/count carry the backing + options */
            out->v.i = i_dart_enum_read_val(f->elem, p); break;
        default: return 0;
    }
    return 1;
}

int dart_get_value(DartBytes msg, const DartSchema *s, uint16_t field, DartValue *out){
    return dart_get_value_at(msg, s, field, 0, out);
}

/* ---- write a message ----------------------------------------------------------------- */
int dart_schema_message_default(const DartSchema *s, void *buf, size_t cap){
    size_t min;
    if (!s || !buf) return 0;
    min = (size_t)s->size + 4u * s->n_var;
    if (cap < min) return 0;
    memset(buf, 0, min);            /* zeroed fixed fields + one empty frame per variable */
    return 1;
}

static const i_Field *i_dart_schema_set_lookup(const DartSchema *s, const void *buf, size_t cap,
                                               const char *field, size_t *off){
    if (!buf) return NULL;
    return i_dart_schema_read_lookup(s, dart_bytes(buf, cap), field, off);
}

static int i_dart_schema_write_uint(const i_Field *f, uint8_t *p, uint64_t v){
    switch (f->kind){
        case DART_U8:   p[0] = (uint8_t)v;  return 1;
        case DART_BOOL: p[0] = v ? 1u : 0u; return 1;
        case DART_U16: i_dart_le_w16(p, (uint16_t)v); return 1;
        case DART_U32: i_dart_le_w32(p, (uint32_t)v); return 1;
        case DART_U64: i_dart_le_w64(p, v); return 1;
        default: return 0;
    }
}
static int i_dart_schema_write_int(const i_Field *f, uint8_t *p, int64_t v){
    switch (f->kind){
        case DART_I8:  p[0] = (uint8_t)v; return 1;
        case DART_I16: i_dart_le_w16(p, (uint16_t)v); return 1;
        case DART_I32: i_dart_le_w32(p, (uint32_t)v); return 1;
        case DART_I64: i_dart_le_w64(p, (uint64_t)v); return 1;
        default: return 0;
    }
}
static int i_dart_schema_write_f64(const i_Field *f, uint8_t *p, double v){
    if (f->kind == DART_F64){ uint64_t b; memcpy(&b, &v, 8); i_dart_le_w64(p, b); return 1; }
    if (f->kind == DART_F32){ float x = (float)v; uint32_t b; memcpy(&b, &x, 4); i_dart_le_w32(p, b); return 1; }
    return 0;
}
/* write one string slot: [u16 len][bytes][zeroed tail]; refuses v.len > cap */
static int i_dart_schema_write_str_slot(uint8_t *p, uint16_t cap, DartString v){
    if (v.len > cap || (v.len && !v.data)) return 0;             /* never silently truncate */
    i_dart_le_w16(p, (uint16_t)v.len);
    if (v.len) memcpy(p + 2, v.data, v.len);
    memset(p + 2 + v.len, 0, (size_t)cap - v.len);
    return 1;
}
/* element payload sanity shared by fixed and variable arrays: whole elements, and for
 * string elements every slot's length prefix within its cap; refuses misfits */
static int i_dart_schema_check_elems(const i_Field *f, DartBytes elems, uint32_t esz){
    if (!esz || elems.len % esz != 0) return 0;     /* never silently truncate */
    if (elems.len && !elems.data) return 0;
    if (f->elem == DART_STR){                       /* whole slots: every length must fit its cap */
        size_t off;
        for (off = 0; off + esz <= elems.len; off += esz)
            if (i_dart_le_r16(elems.data + off) > f->str_cap) return 0;
    }
    return 1;
}
/* copy elems over the front of a fixed array, zero the rest */
static int i_dart_schema_write_array(const i_Field *f, uint8_t *p, DartBytes elems){
    if (f->kind != DART_ARR) return 0;
    if (elems.len > f->size || !i_dart_schema_check_elems(f, elems, f->elem_size)) return 0;
    if (elems.len) memcpy(p, elems.data, elems.len);
    memset(p + elems.len, 0, f->size - elems.len);
    return 1;
}
/* a variable array's frame becomes exactly elems */
static int i_dart_schema_write_var_array(const DartSchema *s, uint8_t *buf, size_t cap,
                                         const i_Field *f, DartBytes elems){
    if (!i_dart_schema_check_elems(f, elems, f->elem_size)) return 0;
    return i_dart_schema_set_frame(s, buf, cap, f->var_ord, elems.data, elems.len);
}

int dart_set_uint(void *buf, size_t cap, const DartSchema *s, const char *field, uint64_t v){
    size_t off; const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field, &off);
    if (!f) return 0;
    if (f->kind == DART_ENUM){ i_dart_enum_write_val(f->elem, (uint8_t *)buf + off, (int64_t)v); return 1; }
    return i_dart_schema_write_uint(f, (uint8_t *)buf + off, v);
}

int dart_set_int(void *buf, size_t cap, const DartSchema *s, const char *field, int64_t v){
    size_t off; const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field, &off);
    if (!f) return 0;
    if (f->kind == DART_ENUM){ i_dart_enum_write_val(f->elem, (uint8_t *)buf + off, v); return 1; }
    return i_dart_schema_write_int(f, (uint8_t *)buf + off, v);
}

int dart_set_f64(void *buf, size_t cap, const DartSchema *s, const char *field, double v){
    size_t off; const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field, &off);
    return f ? i_dart_schema_write_f64(f, (uint8_t *)buf + off, v) : 0;
}

int dart_set_f32(void *buf, size_t cap, const DartSchema *s, const char *field, float v){
    size_t off; const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field, &off); uint32_t b;
    if (!f || f->kind != DART_F32) return 0;
    memcpy(&b, &v, 4); i_dart_le_w32((uint8_t *)buf + off, b);
    return 1;
}

int dart_set_array(void *buf, size_t cap, const DartSchema *s, const char *field, DartBytes elems){
    size_t off; const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field, &off);
    if (!f) return 0;
    if (f->kind == DART_VARR)
        return i_dart_schema_write_var_array(s, (uint8_t *)buf, cap, f, elems);
    return i_dart_schema_write_array(f, (uint8_t *)buf + off, elems);
}

int dart_set_array_count(void *buf, size_t cap, const DartSchema *s, const char *field,
                         uint32_t count){
    size_t off; const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field, &off);
    if (!f || f->kind != DART_VARR || !f->elem_size) return 0;
    if ((uint64_t)count * f->elem_size > 0xFFFFFFFFu) return 0;
    return i_dart_schema_frame_write(s, (uint8_t *)buf, cap, f->var_ord, NULL,
                                     (size_t)count * f->elem_size);
}

int dart_set_string(void *buf, size_t cap, const DartSchema *s, const char *field, DartString v){
    size_t off; const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field, &off);
    if (!f) return 0;
    if (f->kind == DART_VSTR){
        if (v.len && !v.data) return 0;
        return i_dart_schema_set_frame(s, (uint8_t *)buf, cap, f->var_ord, v.data, v.len);
    }
    if (f->kind != DART_STR) return 0;
    return i_dart_schema_write_str_slot((uint8_t *)buf + off, f->str_cap, v);
}

int dart_set_string_at(void *buf, size_t cap, const DartSchema *s, const char *field,
                       uint16_t index, DartString v){
    size_t off; const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field, &off);
    if (!f || f->elem != DART_STR) return 0;
    if (f->kind == DART_VARR){                      /* in place, under the LIVE count */
        DartBytes fr = i_dart_schema_frame(s, dart_bytes(buf, cap), f->var_ord);
        uint32_t esz = f->elem_size;
        if (!fr.data || !esz || ((uint64_t)index + 1u) * esz > fr.len) return 0;
        return i_dart_schema_write_str_slot((uint8_t *)fr.data + (size_t)index * esz,
                                            f->str_cap, v);
    }
    if (f->kind != DART_ARR || index >= f->count) return 0;
    return i_dart_schema_write_str_slot((uint8_t *)buf + off + (size_t)index * f->elem_size,
                                        f->str_cap, v);
}

int dart_set_map(void *buf, size_t cap, const DartSchema *s, const char *field, DartBytes map){
    size_t off; const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field, &off);
    if (!f || f->kind != DART_MAP) return 0;
    if (!dart_map_valid(map)) return 0;             /* malformed bytes never enter a message */
    return i_dart_schema_set_frame(s, (uint8_t *)buf, cap, f->var_ord, map.data, map.len);
}

int dart_set_enum(void *buf, size_t cap, const DartSchema *s, const char *field, const char *name){
    size_t off; const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field, &off);
    int64_t v;
    if (!f || f->kind != DART_ENUM) return 0;
    if (!dart_enum_value_of(s, (uint16_t)(f - s->fields), name, &v)) return 0;   /* unknown option */
    i_dart_enum_write_val(f->elem, (uint8_t *)buf + off, v);
    return 1;
}

int dart_set_value_at(void *buf, size_t cap, const DartSchema *s, uint16_t field, uint32_t elem,
                      const DartValue *val){
    const i_Field *f; uint8_t *p; size_t off;
    if (!buf || !val || !s || field >= s->nfields) return 0;
    f = &s->fields[field];
    if (!i_dart_field_addr(s, dart_bytes(buf, cap), f, elem, &off)) return 0;
    p = (uint8_t *)buf + off;
    switch (f->kind){
        case DART_U8: case DART_U16: case DART_U32: case DART_U64: case DART_BOOL:
            return i_dart_schema_write_uint(f, p, val->v.u);
        case DART_I8: case DART_I16: case DART_I32: case DART_I64:
            return i_dart_schema_write_int(f, p, val->v.i);
        case DART_F32: case DART_F64:
            return i_dart_schema_write_f64(f, p, val->v.f);
        case DART_ARR:
            return i_dart_schema_write_array(f, p, val->bytes);
        case DART_STR:
            return i_dart_schema_write_str_slot(p, f->str_cap,
                       dart_string((const char *)val->bytes.data, val->bytes.len));
        case DART_STRUCT:                                  /* raw bytes, short = zero-filled tail */
            if (val->bytes.len > f->size || (val->bytes.len && !val->bytes.data)) return 0;
            if (val->bytes.len) memcpy(p, val->bytes.data, val->bytes.len);
            memset(p + val->bytes.len, 0, f->size - val->bytes.len);
            return 1;
        case DART_VSTR:
            if (val->bytes.len && !val->bytes.data) return 0;
            return i_dart_schema_set_frame(s, (uint8_t *)buf, cap, f->var_ord,
                                           val->bytes.data, val->bytes.len);
        case DART_VARR:
            return i_dart_schema_write_var_array(s, (uint8_t *)buf, cap, f, val->bytes);
        case DART_MAP:
            if (!dart_map_valid(val->bytes)) return 0;
            return i_dart_schema_set_frame(s, (uint8_t *)buf, cap, f->var_ord,
                                           val->bytes.data, val->bytes.len);
        case DART_ENUM:
            i_dart_enum_write_val(f->elem, p, val->v.i);
            return 1;
        default: return 0;
    }
}

int dart_set_value(void *buf, size_t cap, const DartSchema *s, uint16_t field, const DartValue *val){
    return dart_set_value_at(buf, cap, s, field, 0, val);
}
/* ---- the map: a self-describing tagged value tree ------------------------------------ */
/* body := [u16 n] ( [u8 keylen][key] value )*n ; value := [u8 kind] payload (schema.h).
 * Readers walk hostile bytes: every read is bounds-checked (i_Rd), the kind vocabulary
 * is closed (scalars, VSTR, VARR, MAP only), and nesting is depth-capped. */

/* Read (out != NULL) or skip (out == NULL) one value at r->pos; 1, or 0 + r->fail. */
static int i_dart_map_value(i_Rd *r, uint16_t depth, DartValue *out){
    uint8_t k = i_dart_rd_u8(r);
    uint32_t sz;
    if (r->fail) return 0;
    if (out){ memset(out, 0, sizeof *out); out->kind = k; }
    sz = dart_schema_scalar_size((DartSchemaTypeKind)k);
    if (sz){
        const uint8_t *p = r->w + r->pos;
        i_dart_rd_skip(r, sz);
        if (r->fail) return 0;
        if (out) switch (k){
            case DART_U8: case DART_U16: case DART_U32: case DART_U64: case DART_BOOL:
                out->v.u = i_dart_schema_read_uint(k, p); break;
            case DART_I8: case DART_I16: case DART_I32: case DART_I64:
                out->v.i = i_dart_schema_read_int(k, p); break;
            default:
                out->v.f = i_dart_schema_read_f64(k, p); break;
        }
        return 1;
    }
    if (k == DART_VSTR){
        uint16_t len = i_dart_rd_u16(r);
        const uint8_t *p = r->w + r->pos;
        i_dart_rd_skip(r, len);
        if (r->fail) return 0;
        if (out) out->bytes = dart_bytes(p, len);
        return 1;
    }
    if (k == DART_MAP || k == DART_VARR){
        size_t start = r->pos; uint16_t n, i;
        if (depth + 1u >= DART_SCHEMA_MAX_DEPTH){ r->fail = 1; return 0; }
        n = i_dart_rd_u16(r);
        for (i = 0; i < n && !r->fail; i++){
            if (k == DART_MAP){
                uint8_t kl = i_dart_rd_u8(r);
                i_dart_rd_skip(r, kl);
            }
            if (!i_dart_map_value(r, (uint16_t)(depth + 1), NULL)) return 0;
        }
        if (r->fail) return 0;
        if (out){ out->bytes = dart_bytes(r->w + start, r->pos - start); out->count = n; }
        return 1;
    }
    r->fail = 1;                                        /* unknown kind: reject */
    return 0;
}

uint16_t dart_map_count(DartBytes map){
    return (map.data && map.len >= 2) ? i_dart_le_r16(map.data) : 0;
}
uint16_t dart_map_array_count(DartBytes arr){ return dart_map_count(arr); }

int dart_map_at(DartBytes map, uint16_t index, DartString *key, DartValue *out){
    i_Rd r; uint16_t n, i;
    if (!map.data || map.len < 2) return 0;
    r.w = map.data; r.n = map.len; r.pos = 0; r.fail = 0;
    n = i_dart_rd_u16(&r);
    if (index >= n) return 0;
    for (i = 0; i <= index; i++){
        uint8_t kl = i_dart_rd_u8(&r);
        const char *kp = (const char *)(r.w + r.pos);
        i_dart_rd_skip(&r, kl);
        if (r.fail) return 0;
        if (i == index){
            if (!i_dart_map_value(&r, 0, out)) return 0;
            if (key) *key = dart_string(kp, kl);
            return 1;
        }
        if (!i_dart_map_value(&r, 0, NULL)) return 0;
    }
    return 0;
}

int dart_map_get(DartBytes map, const char *key, DartValue *out){
    i_Rd r; uint16_t n, i; size_t want;
    if (!map.data || map.len < 2 || !key) return 0;
    want = strlen(key);
    r.w = map.data; r.n = map.len; r.pos = 0; r.fail = 0;
    n = i_dart_rd_u16(&r);
    for (i = 0; i < n; i++){
        uint8_t kl = i_dart_rd_u8(&r);
        const char *kp = (const char *)(r.w + r.pos);
        i_dart_rd_skip(&r, kl);
        if (r.fail) return 0;
        if (kl == want && (want == 0 || memcmp(kp, key, want) == 0))
            return i_dart_map_value(&r, 0, out);
        if (!i_dart_map_value(&r, 0, NULL)) return 0;
    }
    return 0;
}

int dart_map_array_at(DartBytes arr, uint16_t index, DartValue *out){
    i_Rd r; uint16_t n, i;
    if (!arr.data || arr.len < 2) return 0;
    r.w = arr.data; r.n = arr.len; r.pos = 0; r.fail = 0;
    n = i_dart_rd_u16(&r);
    if (index >= n) return 0;
    for (i = 0; i < index; i++)
        if (!i_dart_map_value(&r, 0, NULL)) return 0;
    return i_dart_map_value(&r, 0, out);
}

int dart_map_valid(DartBytes map){
    i_Rd r; uint16_t n, i;
    if (map.len == 0) return 1;                         /* an empty body is an empty map */
    if (!map.data || map.len < 2) return 0;
    r.w = map.data; r.n = map.len; r.pos = 0; r.fail = 0;
    n = i_dart_rd_u16(&r);
    for (i = 0; i < n; i++){
        uint8_t kl = i_dart_rd_u8(&r);
        i_dart_rd_skip(&r, kl);
        if (r.fail || !i_dart_map_value(&r, 0, NULL)) return 0;
    }
    return r.pos == r.n;                                /* no trailing garbage */
}

/* ---- the map writer ------------------------------------------------------------------ */
DartMapWriter dart_map_begin(void *buf, size_t cap){
    DartMapWriter w;
    memset(&w, 0, sizeof w);
    w.buf = (uint8_t *)buf; w.cap = cap;
    if (!buf || cap < 2){ w.err = -1; return w; }
    w.buf[0] = 0; w.buf[1] = 0;                         /* the root map's count */
    w.count_pos[0] = 0; w.len = 2;
    w.depth = 1;
    return w;
}

/* room for `extra` more bytes (the buffer is the caller's: no growth) */
static int i_dart_map_room(DartMapWriter *w, size_t extra){
    if (w->err) return 0;
    if (w->len + extra > w->cap){ w->err = -1; return 0; }
    return 1;
}
/* start an entry: key in a map (required), none in an array; bumps the open count */
static int i_dart_map_entry(DartMapWriter *w, const char *key){
    size_t kl = 0;
    if (w->err) return 0;
    if (w->depth == 0){ w->err = -3; return 0; }        /* finished writer */
    if (w->is_arr[w->depth - 1]){
        if (key){ w->err = -3; return 0; }              /* array elements carry no key */
    } else {
        if (!key){ w->err = -3; return 0; }
        kl = strlen(key);
        if (kl > 255){ w->err = -4; return 0; }
    }
    if (w->count[w->depth - 1] == 0xFFFFu){ w->err = -5; return 0; }
    if (key){
        if (!i_dart_map_room(w, 1 + kl)) return 0;
        w->buf[w->len++] = (uint8_t)kl;
        if (kl) memcpy(w->buf + w->len, key, kl);
        w->len += kl;
    }
    w->count[w->depth - 1]++;
    return 1;
}
static int i_dart_map_put_scalar(DartMapWriter *w, const char *key, uint8_t kind,
                                 const uint8_t *le, uint32_t sz){
    if (!w || !i_dart_map_entry(w, key) || !i_dart_map_room(w, 1u + sz)) return 0;
    w->buf[w->len++] = kind;
    memcpy(w->buf + w->len, le, sz);
    w->len += sz;
    return 1;
}

int dart_map_put_uint(DartMapWriter *w, const char *key, uint64_t v){
    uint8_t le[8];                                       /* smallest kind that fits */
    uint8_t k = v <= 0xFFu ? DART_U8 : v <= 0xFFFFu ? DART_U16
              : v <= 0xFFFFFFFFu ? DART_U32 : DART_U64;
    i_dart_le_w64(le, v);
    return i_dart_map_put_scalar(w, key, k, le, dart_schema_scalar_size((DartSchemaTypeKind)k));
}

int dart_map_put_int(DartMapWriter *w, const char *key, int64_t v){
    uint8_t le[8];
    uint8_t k = (v >= -128 && v <= 127) ? DART_I8
              : (v >= -32768 && v <= 32767) ? DART_I16
              : (v >= -2147483647 - 1 && v <= 2147483647) ? DART_I32 : DART_I64;
    i_dart_le_w64(le, (uint64_t)v);                      /* two's-complement LE: low bytes */
    return i_dart_map_put_scalar(w, key, k, le, dart_schema_scalar_size((DartSchemaTypeKind)k));
}

int dart_map_put_f64(DartMapWriter *w, const char *key, double v){
    uint8_t le[8]; uint64_t b;
    memcpy(&b, &v, 8); i_dart_le_w64(le, b);
    return i_dart_map_put_scalar(w, key, (uint8_t)DART_F64, le, 8);
}

int dart_map_put_f32(DartMapWriter *w, const char *key, float v){
    uint8_t le[4]; uint32_t b;
    memcpy(&b, &v, 4); i_dart_le_w32(le, b);
    return i_dart_map_put_scalar(w, key, (uint8_t)DART_F32, le, 4);
}

int dart_map_put_bool(DartMapWriter *w, const char *key, int v){
    uint8_t b = v ? 1u : 0u;
    return i_dart_map_put_scalar(w, key, (uint8_t)DART_BOOL, &b, 1);
}

int dart_map_put_string(DartMapWriter *w, const char *key, DartString v){
    if (!w) return 0;
    if (v.len > 0xFFFFu || (v.len && !v.data)){ w->err = -4; return 0; }
    if (!i_dart_map_entry(w, key) || !i_dart_map_room(w, 3u + v.len)) return 0;
    w->buf[w->len++] = (uint8_t)DART_VSTR;
    i_dart_le_w16(w->buf + w->len, (uint16_t)v.len); w->len += 2;
    if (v.len) memcpy(w->buf + w->len, v.data, v.len);
    w->len += v.len;
    return 1;
}

static int i_dart_map_open(DartMapWriter *w, const char *key, uint8_t kind, uint8_t is_arr){
    if (!w || !i_dart_map_entry(w, key)) return 0;
    if (w->depth >= DART_SCHEMA_MAX_DEPTH){ w->err = -2; return 0; }
    if (!i_dart_map_room(w, 3)) return 0;
    w->buf[w->len++] = kind;
    w->count_pos[w->depth] = w->len;                    /* the nested body's count */
    w->buf[w->len++] = 0; w->buf[w->len++] = 0;
    w->count[w->depth] = 0; w->is_arr[w->depth] = is_arr;
    w->depth++;
    return 1;
}
int dart_map_open_map(DartMapWriter *w, const char *key){
    return i_dart_map_open(w, key, (uint8_t)DART_MAP, 0);
}
int dart_map_open_array(DartMapWriter *w, const char *key){
    return i_dart_map_open(w, key, (uint8_t)DART_VARR, 1);
}
int dart_map_close(DartMapWriter *w){
    if (!w || w->err) return 0;
    if (w->depth <= 1){ w->err = -3; return 0; }        /* the root closes in finish */
    w->depth--;
    i_dart_le_w16(w->buf + w->count_pos[w->depth], w->count[w->depth]);
    return 1;
}

uint32_t dart_map_finish(DartMapWriter *w){
    if (!w || w->err || w->depth != 1) return 0;        /* depth != 1 = unbalanced open/close */
    i_dart_le_w16(w->buf + w->count_pos[0], w->count[0]);
    w->depth = 0;
    return (uint32_t)w->len;
}
/* ---- the schema DSL ------------------------------------------------------------------ */
/* dart_schema_compile input (see schema.h for the full doc):
 *   schema := def* root?                     ; with no root, the LAST def is the root
 *   def    := IDENT '=' type
 *   root   := IDENT '{' fields '}' | type
 *   field  := name ':' type  (',')?          fields self-delimit; commas optional
 *   type   := base | base '[' count ']' | base '[' ']' | '{' fields '}' ( '[' ... ']' )?
 *   base   := scalar | 'string' ('<' cap '>')? | 'map' | enum | NAME
 * A type word that is not a built-in resolves against the text's own definitions, the
 * environment schemas, and the standard type library, and emits as a NAMED type.
 * The parser is a thin front end over the builder, so all structural limits (name
 * lengths, field counts, nesting depth) are the builder's. */
#ifndef DART_NO_STDTYPES
const char *i_dart_std_lookup(const char *name);   /* serialize/stdtypes.c */
#else
#define i_dart_std_lookup(name) ((const char *)0)
#endif

typedef struct { const char *p; const char *err; } i_DartDsl;

/* The definition registry: one growing arena holding each named type's name bytes and
 * compiled type encoding, plus an index. Definitions are compiled into their own scratch
 * builder and only then appended here, so resolving a reference mid-definition (which may
 * pull a standard type in) can never interleave bytes. */
#define I_DART_DSL_MAX_DEFS 64u
typedef struct {
    DartSchemaBuilder arena;
    struct { uint32_t noff, toff, tlen; uint8_t nlen; } e[I_DART_DSL_MAX_DEFS];
    uint16_t n;
    uint16_t rec;                                  /* standard-type expansion depth */
    const DartSchema *const *env; size_t n_env;
} i_DartDefs;

static void i_dart_dsl_ws(i_DartDsl *d){
    for (;;){
        while (*d->p==' ' || *d->p=='\t' || *d->p=='\r' || *d->p=='\n') d->p++;
        if (d->p[0]=='-' && d->p[1]=='-'){ while (*d->p && *d->p!='\n') d->p++; continue; }
        return;
    }
}
static void i_dart_dsl_fail(i_DartDsl *d, const char *at){ if (!d->err) d->err = at; }
/* identifier into out[256] (NUL-terminated); 0 + err on missing/overlong */
static int i_dart_dsl_ident(i_DartDsl *d, char out[256]){
    const char *q = d->p; size_t n;
    if (!((*q>='A'&&*q<='Z') || (*q>='a'&&*q<='z') || *q=='_')){ i_dart_dsl_fail(d, q); return 0; }
    while ((*q>='A'&&*q<='Z') || (*q>='a'&&*q<='z') || (*q>='0'&&*q<='9') || *q=='_') q++;
    n = (size_t)(q - d->p);
    if (n > 255){ i_dart_dsl_fail(d, d->p); return 0; }
    memcpy(out, d->p, n); out[n] = '\0';
    d->p = q;
    return 1;
}
static int i_dart_dsl_expect(i_DartDsl *d, char c){
    if (*d->p == c){ d->p++; return 1; }
    i_dart_dsl_fail(d, d->p);
    return 0;
}
/* decimal array count, 1..65535 */
static int i_dart_dsl_count(i_DartDsl *d, uint16_t *out){
    const char *at = d->p; uint32_t v = 0;
    while (*d->p>='0' && *d->p<='9'){
        v = v*10u + (uint32_t)(*d->p - '0');
        if (v > 0xFFFFu){ i_dart_dsl_fail(d, at); return 0; }
        d->p++;
    }
    if (d->p == at || v == 0){ i_dart_dsl_fail(d, at); return 0; }
    *out = (uint16_t)v;
    return 1;
}
static int i_dart_dsl_kind(const char *s, DartSchemaTypeKind *k){
    static const struct { const char *word; uint8_t kind; } table[] = {
        {"u8",DART_U8},{"u16",DART_U16},{"u32",DART_U32},{"u64",DART_U64},
        {"i8",DART_I8},{"i16",DART_I16},{"i32",DART_I32},{"i64",DART_I64},
        {"f32",DART_F32},{"f64",DART_F64},{"bool",DART_BOOL} };
    size_t i;
    for (i = 0; i < sizeof table / sizeof table[0]; i++)
        if (strcmp(s, table[i].word) == 0){ *k = (DartSchemaTypeKind)table[i].kind; return 1; }
    return 0;
}
/* a signed decimal enum value fitting int64: optional '-' then digits */
static int i_dart_dsl_enum_value(i_DartDsl *d, int64_t *out){
    const char *at = d->p; int neg = 0, any = 0; uint64_t v = 0, lim;
    if (*d->p == '-'){ neg = 1; d->p++; }
    lim = neg ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    while (*d->p >= '0' && *d->p <= '9'){
        v = v * 10u + (uint64_t)(*d->p - '0');
        if (v > lim){ i_dart_dsl_fail(d, at); return 0; }
        d->p++; any = 1;
    }
    if (!any){ i_dart_dsl_fail(d, at); return 0; }
    *out = neg ? -(int64_t)v : (int64_t)v;
    return 1;
}
/* `enum<uN> { Name [= value] , ... }` -- the type after `name:` has already been read as
 * the ident "enum". Streams options into the builder (no whole-list buffering). */
static void i_dart_dsl_enum(i_DartDsl *d, DartSchemaBuilder *b, const char *name){
    char wname[256]; DartSchemaTypeKind backing; size_t count_pos; uint16_t count = 0; int64_t next = 0;
    i_dart_dsl_ws(d);
    if (!i_dart_dsl_expect(d, '<')) return;
    i_dart_dsl_ws(d);
    if (!i_dart_dsl_ident(d, wname)) return;
    if (!i_dart_dsl_kind(wname, &backing) || !i_dart_enum_backing_ok((uint8_t)backing)){
        i_dart_dsl_fail(d, d->p); return;                /* backing must be an integer kind */
    }
    i_dart_dsl_ws(d);
    if (!i_dart_dsl_expect(d, '>')) return;
    i_dart_dsl_ws(d);
    if (!i_dart_dsl_expect(d, '{')) return;
    count_pos = i_dart_schema_field_enum_open(b, name, backing);
    for (;;){
        char vname[256]; int64_t v;
        i_dart_dsl_ws(d);
        if (*d->p == '}' || *d->p == '\0' || d->err || b->err) break;
        if (!i_dart_dsl_ident(d, vname)) return;
        i_dart_dsl_ws(d);
        if (*d->p == '='){                               /* explicit value, else auto-increment */
            d->p++; i_dart_dsl_ws(d);
            if (!i_dart_dsl_enum_value(d, &v)) return;
        } else v = next;
        if (!i_dart_enum_val_fits((uint8_t)backing, v)){ i_dart_dsl_fail(d, d->p); return; }
        i_dart_schema_field_enum_add(b, backing, v, vname, strlen(vname));
        count++; next = v + 1;
        i_dart_dsl_ws(d);
        if (*d->p == ',') d->p++;                        /* optional separator */
    }
    if (!i_dart_dsl_expect(d, '}')) return;
    i_dart_schema_field_enum_finish(b, count_pos, count);
}

static void i_dart_dsl_field_type(i_DartDsl *d, DartSchemaBuilder *b, i_DartDefs *defs,
                                  const char *name);
static void i_dart_dsl_fields(i_DartDsl *d, DartSchemaBuilder *b, i_DartDefs *defs);

/* record a named type: its name bytes then its compiled type encoding, both in the arena */
static int i_dart_defs_add_bytes(i_DartDefs *defs, const char *name, size_t nlen,
                                 const uint8_t *type, size_t tlen){
    uint32_t noff, toff;
    if (defs->n >= I_DART_DSL_MAX_DEFS || nlen == 0 || nlen > 255 || tlen == 0) return 0;
    noff = (uint32_t)defs->arena.len;
    i_dart_schema_builder_put_raw(&defs->arena, name, nlen);
    toff = (uint32_t)defs->arena.len;
    i_dart_schema_builder_put_raw(&defs->arena, type, tlen);
    if (defs->arena.err) return 0;
    defs->e[defs->n].noff = noff; defs->e[defs->n].nlen = (uint8_t)nlen;
    defs->e[defs->n].toff = toff; defs->e[defs->n].tlen = (uint32_t)tlen;
    defs->n++;
    return 1;
}

static int i_dart_defs_find(i_DartDefs *defs, const char *name, uint32_t *toff, uint32_t *tlen){
    size_t nlen = strlen(name); uint16_t i;
    for (i = 0; i < defs->n; i++)
        if (defs->e[i].nlen == nlen &&
            memcmp(defs->arena.buf + defs->e[i].noff, name, nlen) == 0){
            *toff = defs->e[i].toff; *tlen = defs->e[i].tlen;
            return 1;
        }
    return 0;
}

/* compile one type spelling (a standard library entry, or any type text) as a definition */
static int i_dart_defs_add_text(i_DartDefs *defs, const char *name, const char *text){
    DartSchemaBuilder sb; i_DartDsl sd; int ok = 0;
    if (defs->rec >= 8u) return 0;                       /* the roster is acyclic; be sure */
    defs->rec++;
    sb = i_dart_schema_begin_raw(defs->arena.alloc, defs->arena.user);
    sd.p = text; sd.err = NULL;
    i_dart_dsl_field_type(&sd, &sb, defs, "");
    i_dart_dsl_ws(&sd);
    if (*sd.p) i_dart_dsl_fail(&sd, sd.p);
    if (!sd.err && !sb.err && sb.len)
        ok = i_dart_defs_add_bytes(defs, name, strlen(name), sb.buf, sb.len);
    if (sb.buf) sb.alloc(sb.user, sb.buf, 0);
    defs->rec--;
    return ok;
}

/* Resolve a type name to its encoding in the arena: the text's own definitions first,
 * then the environment schemas, then the standard library (which is always in scope). */
static int i_dart_dsl_ref(i_DartDefs *defs, const char *name, uint32_t *toff, uint32_t *tlen){
    size_t nlen = strlen(name), i;
    const char *std;
    if (i_dart_defs_find(defs, name, toff, tlen)) return 1;
    for (i = 0; i < defs->n_env; i++){
        const DartSchema *t = defs->env ? defs->env[i] : NULL;
        const uint8_t *tb; size_t tl;
        if (!t || t->name.len != nlen || memcmp(t->name.data, name, nlen) != 0) continue;
        if (!i_dart_schema_root_type(t, &tb, &tl)) continue;
        if (!i_dart_defs_add_bytes(defs, name, nlen, tb, tl)) return 0;
        return i_dart_defs_find(defs, name, toff, tlen);
    }
    std = i_dart_std_lookup(name);
    if (std && i_dart_defs_add_text(defs, name, std))
        return i_dart_defs_find(defs, name, toff, tlen);
    return 0;
}

/* turn the type just written at b->buf[head..len) into an array of itself by splicing the
 * array head in front of it (the count follows the body in the text) */
static void i_dart_dsl_splice_array(DartSchemaBuilder *b, size_t head, uint16_t count,
                                    int variable){
    size_t extra = variable ? 1u : 3u, tail;
    if (b->err) return;
    if (!i_dart_schema_builder_reserve(b, extra)) return;
    tail = b->len - head;
    memmove(b->buf + head + extra, b->buf + head, tail);
    if (variable){
        b->buf[head] = (uint8_t)DART_VARR;
    } else {
        b->buf[head] = (uint8_t)DART_ARR;
        i_dart_le_w16(b->buf + head + 1u, count);
    }
    b->len += extra;
}

/* an optional `[N]` / `[]` suffix; 1 if one was read (variable = the `[]` form) */
static int i_dart_dsl_suffix(i_DartDsl *d, uint16_t *count, int *variable){
    *count = 0; *variable = 0;
    i_dart_dsl_ws(d);
    if (*d->p != '[') return 0;
    d->p++;
    i_dart_dsl_ws(d);
    if (*d->p == ']'){ d->p++; *variable = 1; return 1; }
    if (!i_dart_dsl_count(d, count)) return 0;
    i_dart_dsl_ws(d);
    if (!i_dart_dsl_expect(d, ']')) return 0;
    return 1;
}

/* `Name`, `Name[N]`, `Name[]`: a reference to an already-resolved named type */
static void i_dart_dsl_emit_ref(i_DartDsl *d, DartSchemaBuilder *b, i_DartDefs *defs,
                                const char *name, const char *tname,
                                uint32_t toff, uint32_t tlen){
    uint16_t cnt; int variable, arr;
    size_t nlen = strlen(tname);
    arr = i_dart_dsl_suffix(d, &cnt, &variable);
    if (d->err) return;
    if (arr && variable && !i_dart_schema_builder_var_ok(b)) return;
    i_dart_schema_builder_count(b);
    i_dart_schema_builder_put_name(b, name);
    if (arr){
        if (variable) i_dart_schema_builder_put(b, (uint8_t)DART_VARR);
        else { i_dart_schema_builder_put(b, (uint8_t)DART_ARR); i_dart_schema_builder_put_u16(b, cnt); }
    }
    i_dart_schema_builder_put(b, (uint8_t)DART_NAMED);
    i_dart_schema_builder_put(b, (uint8_t)nlen);
    i_dart_schema_builder_put_raw(b, tname, nlen);
    i_dart_schema_builder_put_raw(b, defs->arena.buf + toff, tlen);
}

/* One type whose leading word is already read into tname (`at` points at it, for error
 * reporting): whatever follows (`<cap>`, `[N]`, `[]`, an enum body) plus the field it
 * defines. Serves a struct's fields, a definition body, and a bare root (then name is ""). */
static void i_dart_dsl_word_type(i_DartDsl *d, DartSchemaBuilder *b, i_DartDefs *defs,
                                 const char *name, const char *tname, const char *at){
    DartSchemaTypeKind k = DART_U8;
    int is_str = 0, has_cap = 0; uint16_t str_cap = 0;
    uint16_t cnt; int variable;
    if (strcmp(tname, "map") == 0){                      /* the self-describing escape */
        dart_schema_field_map(b, name);
        return;
    }
    if (strcmp(tname, "enum") == 0){                     /* enum<uN> { Name = value, ... } */
        i_dart_dsl_enum(d, b, name);
        return;
    }
    if (strcmp(tname, "string") == 0){                   /* string / string<cap> */
        is_str = 1;
        i_dart_dsl_ws(d);
        if (*d->p == '<'){
            d->p++; has_cap = 1;
            i_dart_dsl_ws(d);
            if (!i_dart_dsl_count(d, &str_cap)) return;
            i_dart_dsl_ws(d);
            if (!i_dart_dsl_expect(d, '>')) return;
        }
    } else if (!i_dart_dsl_kind(tname, &k)){             /* a named type */
        uint32_t toff, tlen;
        if (!i_dart_dsl_ref(defs, tname, &toff, &tlen)){ i_dart_dsl_fail(d, at); return; }
        i_dart_dsl_emit_ref(d, b, defs, name, tname, toff, tlen);
        return;
    }
    if (i_dart_dsl_suffix(d, &cnt, &variable)){
        if (d->err) return;
        if (is_str && !has_cap){ i_dart_dsl_fail(d, at); return; }  /* string[]: ragged; use a map */
        if (variable){
            if (is_str) dart_schema_field_var_string_array(b, name, str_cap);
            else        dart_schema_field_var_array(b, name, k);
        } else {
            if (is_str) dart_schema_field_string_array(b, name, str_cap, cnt);
            else        dart_schema_field_array(b, name, k, cnt);
        }
    } else if (d->err){
        return;
    } else if (is_str){
        if (has_cap) dart_schema_field_string(b, name, str_cap);
        else         dart_schema_field_var_string(b, name);
    } else {
        dart_schema_field(b, name, k);
    }
}

static void i_dart_dsl_field_type(i_DartDsl *d, DartSchemaBuilder *b, i_DartDefs *defs,
                                  const char *name){
    char tname[256]; const char *at;
    i_dart_dsl_ws(d);
    if (*d->p == '{'){                                   /* a struct, or an array of them */
        size_t head; uint16_t cnt; int variable;
        d->p++;
        i_dart_schema_builder_count(b);
        i_dart_schema_builder_put_name(b, name);
        head = b->len;
        i_dart_schema_builder_open_struct(b);
        i_dart_dsl_fields(d, b, defs);
        if (!i_dart_dsl_expect(d, '}')) return;
        dart_schema_end_struct(b);
        if (i_dart_dsl_suffix(d, &cnt, &variable) && !d->err)
            i_dart_dsl_splice_array(b, head, cnt, variable);
        return;
    }
    at = d->p;
    if (!i_dart_dsl_ident(d, tname)) return;
    i_dart_dsl_word_type(d, b, defs, name, tname, at);
}

/* fields of one struct body, up to (not consuming) the closing '}' */
static void i_dart_dsl_fields(i_DartDsl *d, DartSchemaBuilder *b, i_DartDefs *defs){
    char name[256];
    for (;;){
        i_dart_dsl_ws(d);
        if (*d->p == '}' || *d->p == '\0' || d->err || b->err) return;
        if (!i_dart_dsl_ident(d, name)) return;
        i_dart_dsl_ws(d);
        if (!i_dart_dsl_expect(d, ':')) return;
        i_dart_dsl_field_type(d, b, defs, name);
        if (d->err) return;
        i_dart_dsl_ws(d);
        if (*d->p == ',') d->p++;                        /* optional separator */
    }
}

/* `Name = type`: compile the body on its own, then keep it (or verify it matches an
 * existing / reserved definition of the same name exactly). */
static void i_dart_dsl_def(i_DartDsl *d, i_DartDefs *defs, const char *name){
    DartSchemaBuilder sb; uint32_t toff = 0, tlen = 0; int ok = 0, exists;
    exists = i_dart_dsl_ref(defs, name, &toff, &tlen);
    sb = i_dart_schema_begin_raw(defs->arena.alloc, defs->arena.user);
    i_dart_dsl_field_type(d, &sb, defs, "");
    if (!d->err && !sb.err && sb.len){
        if (exists)                                      /* redefining is fine if identical */
            ok = (tlen == sb.len && memcmp(defs->arena.buf + toff, sb.buf, sb.len) == 0);
        else
            ok = i_dart_defs_add_bytes(defs, name, strlen(name), sb.buf, sb.len);
    }
    if (sb.buf) sb.alloc(sb.user, sb.buf, 0);
    if (!ok) i_dart_dsl_fail(d, d->p);
}

DartSchema *dart_schema_compile_env(DartAllocFn alloc, void *user, const char *text,
                                    const DartSchema *const *env, size_t n_env,
                                    const char **err){
    i_DartDsl d; i_DartDefs defs; DartSchemaBuilder root;
    DartSchema *s = NULL;
    char rootbuf[256];
    const char *rname = NULL; size_t rnlen = 0;
    const uint8_t *rtype = NULL; size_t rtlen = 0;
    int have_root = 0, is_struct = 0;

    if (err) *err = NULL;
    if (!alloc || !text) return NULL;
    memset(&defs, 0, sizeof defs);
    defs.env = env; defs.n_env = n_env;
    defs.arena = i_dart_schema_begin_raw(alloc, user);
    root = i_dart_schema_begin_raw(alloc, user);
    d.p = text; d.err = NULL;
    if (defs.arena.err || root.err) i_dart_dsl_fail(&d, text);

    while (!d.err){
        const char *at;
        i_dart_dsl_ws(&d);
        if (!*d.p) break;
        at = d.p;
        if (!i_dart_dsl_ident(&d, rootbuf)) break;
        i_dart_dsl_ws(&d);
        if (*d.p == '='){                                /* a definition */
            d.p++;
            i_dart_dsl_def(&d, &defs, rootbuf);
            continue;
        }
        if (*d.p == '{'){                                /* `Name { fields }`: a struct root */
            d.p++;
            i_dart_schema_builder_open_struct(&root);
            i_dart_dsl_fields(&d, &root, &defs);
            if (!i_dart_dsl_expect(&d, '}')) break;
            dart_schema_end_struct(&root);
            rnlen = strlen(rootbuf); rname = rootbuf;
            is_struct = 1;
        } else {                                         /* a bare type (maybe a reference) */
            d.p = at;
            i_dart_dsl_field_type(&d, &root, &defs, "");
        }
        have_root = 1;
        i_dart_dsl_ws(&d);
        if (*d.p) i_dart_dsl_fail(&d, d.p);              /* the root ends the text */
        break;
    }
    if (!d.err && root.err) i_dart_dsl_fail(&d, d.p);
    if (!d.err && defs.arena.err) i_dart_dsl_fail(&d, d.p);

    if (!d.err){
        if (have_root){
            rtype = root.buf; rtlen = root.len;
            if (!is_struct && rtlen >= 2 && rtype[0] == DART_NAMED){
                size_t nl = rtype[1];                    /* a bare reference IS an alias root */
                if (2u + nl <= rtlen){
                    rname = (const char *)(rtype + 2); rnlen = nl;
                    rtype += 2u + nl; rtlen -= 2u + nl;
                }
            }
        } else if (defs.n){                              /* no root: the last definition is it */
            uint16_t last = (uint16_t)(defs.n - 1u);
            rname = (const char *)(defs.arena.buf + defs.e[last].noff);
            rnlen = defs.e[last].nlen;
            rtype = defs.arena.buf + defs.e[last].toff;
            rtlen = defs.e[last].tlen;
        }
        if (!rtype || !rtlen) i_dart_dsl_fail(&d, d.p);
    }
    /* NB the reservation covers DEFINITIONS and references (i_dart_dsl_def), not a plain
     * `Name { ... }` root: nothing can reference a root, so a schema generated by
     * reflection from a class called Color or Image still compiles, and a shape clash with
     * the standard type of that name simply refuses to match, loudly, like any other. */
    if (!d.err){
        size_t wlen = 2u + rnlen + rtlen;
        uint8_t *w = (uint8_t *)alloc(user, NULL, wlen);
        if (w){
            w[0] = (uint8_t)DART_SCHEMA_WIRE_VERSION;
            w[1] = (uint8_t)rnlen;
            if (rnlen) memcpy(w + 2, rname, rnlen);
            memcpy(w + 2 + rnlen, rtype, rtlen);
            s = dart_schema_parse(w, wlen, alloc, user);
            alloc(user, w, 0);
        }
        if (!s) i_dart_dsl_fail(&d, d.p);
    }
    if (root.buf) alloc(user, root.buf, 0);
    if (defs.arena.buf) alloc(user, defs.arena.buf, 0);
    if (!s && err) *err = d.err ? d.err : d.p;
    return s;
}

DartSchema *dart_schema_compile(DartAllocFn alloc, void *user, const char *text, const char **err){
    return dart_schema_compile_env(alloc, user, text, NULL, 0, err);
}
