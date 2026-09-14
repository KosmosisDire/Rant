#include "schema.h"
#include "../common/bytes.h"
#include "../common/hash.h"
#include <string.h>

/* The wire format and the layout rules are in spec/schema.md. */

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

static int i_rant_kind_var(uint8_t k){
    return k == RANT_VSTR || k == RANT_VARR || k == RANT_MAP;
}

uint32_t rant_schema_scalar_size(RantSchemaTypeKind kind){
    switch (kind){
        case RANT_U8: case RANT_I8: case RANT_BOOL: return 1;
        case RANT_U16: case RANT_I16:                   return 2;
        case RANT_U32: case RANT_I32: case RANT_F32: return 4;
        case RANT_U64: case RANT_I64: case RANT_F64: return 8;
        default: return 0;
    }
}

/* enum backing values: an integer kind, read and written like the scalar */
static int i_rant_enum_backing_ok(uint8_t backing){ return backing <= (uint8_t)RANT_I64; }
/* widened to int64, sign extended for a signed kind */
static int64_t i_rant_enum_read_val(uint8_t backing, const uint8_t *p){
    switch (backing){
        case RANT_U8:    return (int64_t)(uint64_t)p[0];
        case RANT_U16: return (int64_t)(uint64_t)i_rant_le_r16(p);
        case RANT_U32: return (int64_t)(uint64_t)i_rant_le_r32(p);
        case RANT_U64: return (int64_t)i_rant_le_r64(p);
        case RANT_I8:    return (int64_t)(int8_t)p[0];
        case RANT_I16: return (int64_t)(int16_t)i_rant_le_r16(p);
        case RANT_I32: return (int64_t)(int32_t)i_rant_le_r32(p);
        case RANT_I64: return (int64_t)i_rant_le_r64(p);
        default:       return 0;
    }
}
/* truncated to the backing */
static void i_rant_enum_write_val(uint8_t backing, uint8_t *p, int64_t v){
    uint64_t u = (uint64_t)v; uint32_t bs = rant_schema_scalar_size((RantSchemaTypeKind)backing), i;
    for (i = 0; i < bs; i++) p[i] = (uint8_t)(u >> (8 * i));
}
/* a u64 above INT64_MAX is not expressible */
static int i_rant_enum_val_fits(uint8_t backing, int64_t v){
    switch (backing){
        case RANT_U8:    return v >= 0 && v <= 0xFF;
        case RANT_U16: return v >= 0 && v <= 0xFFFF;
        case RANT_U32: return v >= 0 && v <= (int64_t)0xFFFFFFFF;
        case RANT_U64: return v >= 0;
        case RANT_I8:    return v >= -128 && v <= 127;
        case RANT_I16: return v >= -32768 && v <= 32767;
        case RANT_I32: return v >= (int64_t)(-2147483647 - 1) && v <= 2147483647;
        case RANT_I64: return 1;
        default:       return 0;
    }
}

/* the bounds checked reader over possibly hostile wire bytes */
typedef struct { const uint8_t *w; size_t n, pos; int fail; } i_Rd;
static uint8_t  i_rant_rd_u8 (i_Rd *r){ if (r->pos + 1 > r->n){ r->fail = 1; return 0; } return r->w[r->pos++]; }
static uint16_t i_rant_rd_u16(i_Rd *r){ uint16_t v; if (r->pos + 2 > r->n){ r->fail = 1; return 0; } v = i_rant_le_r16(r->w + r->pos); r->pos += 2; return v; }
static void     i_rant_rd_skip(i_Rd *r, size_t k){ if (r->pos + k > r->n){ r->fail = 1; r->pos = r->n; return; } r->pos += k; }

/* The kind at r->pos with any NAMED wrappers peeled off, 0xFF if the wire runs out. */
static uint8_t i_rant_peek_kind(const i_Rd *r){
    size_t p = r->pos;
    for (;;){
        if (p >= r->n) return 0xFFu;
        if (r->w[p] != RANT_NAMED) return r->w[p];
        if (p + 2u > r->n) return 0xFFu;
        p += 2u + r->w[p + 1];
    }
}

/* Where a type sits relative to an array. DIRECT is an element type, so not itself an
 * array. INSIDE is anywhere within one, so no variable kinds and no further struct arrays. */
#define I_T_ELEM_DIRECT 1u
#define I_T_ELEM_INSIDE 2u

/* The fixed byte size of the type at r->pos, advancing past it. Counts every field walked
 * into *fields and every variable field into *nvar. Fails on any rule break. */
static uint32_t i_rant_rd_type_size(i_Rd *r, uint32_t *fields, uint32_t *nvar,
                                    uint16_t depth, uint32_t fl){
    uint8_t k = i_rant_rd_u8(r);
    if (r->fail) return 0;
    switch (k){
        case RANT_U8: case RANT_I8: case RANT_BOOL: return 1;
        case RANT_U16: case RANT_I16:                   return 2;
        case RANT_U32: case RANT_I32: case RANT_F32: return 4;
        case RANT_U64: case RANT_I64: case RANT_F64: return 8;
        case RANT_STR:
            return 2u + (uint32_t)i_rant_rd_u16(r);                /* [u16 len][cap bytes] */
        case RANT_ARR: {
            uint16_t count; uint8_t ek; uint64_t es;
            if (fl & I_T_ELEM_DIRECT){ r->fail = 1; return 0; }  /* no array of arrays */
            count = i_rant_rd_u16(r);
            if (r->fail) return 0;
            ek = i_rant_peek_kind(r);
            if (ek == 0xFFu){ r->fail = 1; return 0; }
            if ((fl & I_T_ELEM_INSIDE) && ek == RANT_STRUCT){ r->fail = 1; return 0; }
            es = i_rant_rd_type_size(r, fields, nvar, (uint16_t)(depth + 1),
                                     I_T_ELEM_DIRECT | I_T_ELEM_INSIDE);
            if (r->fail || es == 0){ r->fail = 1; return 0; }    /* elements must be fixed */
            if ((uint64_t)count * es > 0xFFFFFFFFu){ r->fail = 1; return 0; }
            return (uint32_t)((uint64_t)count * es);
        }
        case RANT_ENUM: {
            uint8_t backing; uint16_t n; uint32_t bs, i;
            if (fl & I_T_ELEM_DIRECT){ r->fail = 1; return 0; }  /* the element kind is ambiguous */
            backing = i_rant_rd_u8(r);
            n = i_rant_rd_u16(r);
            bs = rant_schema_scalar_size((RantSchemaTypeKind)backing);
            if (r->fail || bs == 0 || !i_rant_enum_backing_ok(backing)){ r->fail = 1; return 0; }
            for (i = 0; i < n && !r->fail; i++){                  /* skip the option table */
                i_rant_rd_skip(r, bs);
                i_rant_rd_skip(r, i_rant_rd_u8(r));
            }
            if (r->fail) return 0;
            return bs;   /* on the wire: just the backing scalar */
        }
        case RANT_VSTR: case RANT_MAP:
            if (fl){ r->fail = 1; return 0; }                    /* never inside an array element */
            if (nvar) (*nvar)++;
            return 0;
        case RANT_VARR: {
            uint64_t es;
            if (fl){ r->fail = 1; return 0; }
            if (i_rant_peek_kind(r) == 0xFFu){ r->fail = 1; return 0; }
            es = i_rant_rd_type_size(r, fields, nvar, (uint16_t)(depth + 1),
                                     I_T_ELEM_DIRECT | I_T_ELEM_INSIDE);
            if (r->fail || es == 0){ r->fail = 1; return 0; }
            if (nvar) (*nvar)++;
            return 0;
        }
        case RANT_STRUCT: {
            uint8_t nf; uint64_t sum = 0; uint16_t i;
            if (depth >= RANT_SCHEMA_MAX_DEPTH){ r->fail = 1; return 0; }
            nf = i_rant_rd_u8(r);
            for (i = 0; i < nf && !r->fail; i++){
                uint8_t fnl = i_rant_rd_u8(r);
                i_rant_rd_skip(r, fnl);
                if (fields) (*fields)++;
                sum += i_rant_rd_type_size(r, fields, nvar, (uint16_t)(depth + 1),
                                           fl & I_T_ELEM_INSIDE);
            }
            if (sum > 0xFFFFFFFFu){ r->fail = 1; return 0; }
            return (uint32_t)sum;
        }
        case RANT_NAMED: {
            uint8_t nl = i_rant_rd_u8(r);
            if (r->fail || nl == 0){ r->fail = 1; return 0; }    /* a name is required */
            i_rant_rd_skip(r, nl);
            if (r->fail) return 0;
            if (r->pos < r->n && r->w[r->pos] == RANT_NAMED){ r->fail = 1; return 0; }
            return i_rant_rd_type_size(r, fields, nvar, depth, fl);
        }
        default: r->fail = 1; return 0;                          /* unknown kind: reject */
    }
}

/* Offset of the compiled handle in buf: past the wire bytes, 8 aligned. */
static size_t i_rant_schema_handle_off(const uint8_t *buf, size_t wire_len){
    uintptr_t addr = (uintptr_t)buf + wire_len;
    size_t pad = (size_t)((8u - (addr & 7u)) & 7u);
    return wire_len + pad;
}

/* Counts every field of the root into *n and its variable fields into *nvar. 0 on
 * malformed wire. A bare or alias root is 1 plus whatever its type flattens to. */
static int i_rant_schema_wire_fields(const void *wire, size_t wire_len,
                                     uint32_t *n, uint32_t *nvar){
    i_Rd r; uint8_t ver, rl;
    *n = 0; *nvar = 0;
    r.w = (const uint8_t *)wire; r.n = wire_len; r.pos = 0; r.fail = 0;
    ver = i_rant_rd_u8(&r); if (r.fail || ver != RANT_SCHEMA_WIRE_VERSION) return 0;
    rl = i_rant_rd_u8(&r); i_rant_rd_skip(&r, rl);
    if (r.fail || (size_t)r.pos >= r.n) return 0;
    if (r.w[r.pos] == RANT_NAMED) return 0;            /* a root's name rides the header */
    if (r.w[r.pos] == RANT_STRUCT){
        i_rant_rd_type_size(&r, n, nvar, 0, 0);
    } else {
        uint32_t sub = 0;
        i_rant_rd_type_size(&r, &sub, nvar, 0, 0);     /* depth 0: a variable root is allowed */
        *n = 1u + sub;                                 /* the root is the first field */
    }
    return r.fail ? 0 : 1;
}

/* flattening a wire type into the field table */
static void i_rant_field_init(i_Field *f, RantString name, uint16_t depth, uint16_t parent,
                              uint32_t offset){
    memset(f, 0, sizeof *f);
    f->name = name;
    f->type_name = rant_string(NULL, 0);
    f->elem_name = rant_string(NULL, 0);
    f->depth = depth; f->parent = parent; f->arr_parent = I_RANT_NO_PARENT;
    f->offset = offset;
}

static uint32_t i_rant_emit_type(uint8_t *buf, size_t wl, i_Rd *r, RantSchema *s,
                                 uint16_t *emitted, uint32_t total,
                                 uint16_t idx, uint16_t depth, uint32_t base);

/* Emits the members of the struct body at r->pos into the table, depth first. Returns
 * the struct's fixed size and sets r->fail on malformed wire. */
static uint32_t i_rant_emit_struct(uint8_t *buf, size_t wl, i_Rd *r, RantSchema *s,
                                   uint16_t *emitted, uint32_t total,
                                   uint16_t depth, uint16_t parent, uint32_t base){
    uint8_t nf = i_rant_rd_u8(r); uint16_t i;
    uint32_t running = base;
    if (depth >= RANT_SCHEMA_MAX_DEPTH){ r->fail = 1; return 0; }
    for (i = 0; i < nf && !r->fail; i++){
        uint8_t fl = i_rant_rd_u8(r);
        const char *fn = (const char *)(buf + r->pos);
        uint16_t idx; uint32_t sz;
        i_rant_rd_skip(r, fl);
        if (r->fail) return 0;
        if (*emitted >= total){ r->fail = 1; return 0; }
        idx = (*emitted)++;
        i_rant_field_init(&s->fields[idx], rant_string(fn, fl), depth, parent, running);
        sz = i_rant_emit_type(buf, wl, r, s, emitted, total, idx, depth, running);
        if (r->fail) return 0;
        running += sz;
    }
    return running - base;
}

/* Fills field idx from the type at r->pos, NAMED wrappers unwrapped into type_name, and
 * emits any child fields it flattens to. base is where the field's storage starts. */
static uint32_t i_rant_emit_type(uint8_t *buf, size_t wl, i_Rd *r, RantSchema *s,
                                 uint16_t *emitted, uint32_t total,
                                 uint16_t idx, uint16_t depth, uint32_t base){
    size_t kpos = r->pos;
    uint8_t k;
    uint32_t sz = 0;
    i_Field *f = &s->fields[idx];
    f->type_off = (uint32_t)kpos;
    k = i_rant_rd_u8(r);
    if (k == RANT_NAMED){
        uint8_t nl = i_rant_rd_u8(r);
        const char *tn = (const char *)(buf + r->pos);
        i_rant_rd_skip(r, nl);
        if (r->fail || nl == 0){ r->fail = 1; return 0; }
        f->type_name = rant_string(tn, nl);
        k = i_rant_rd_u8(r);
        if (k == RANT_NAMED){ r->fail = 1; return 0; }
    }
    if (r->fail) return 0;
    f->kind = k;
    switch (k){
        case RANT_STRUCT:
            sz = i_rant_emit_struct(buf, wl, r, s, emitted, total,
                                    (uint16_t)(depth + 1), idx, base);
            break;
        case RANT_ARR: case RANT_VARR: {
            uint8_t ek; uint32_t esz = 0;
            if (k == RANT_ARR){
                f->count = i_rant_rd_u16(r);
                if (r->fail) return 0;
            }
            ek = i_rant_rd_u8(r);
            if (ek == RANT_NAMED){
                uint8_t nl = i_rant_rd_u8(r);
                const char *tn = (const char *)(buf + r->pos);
                i_rant_rd_skip(r, nl);
                if (r->fail || nl == 0){ r->fail = 1; return 0; }
                f->elem_name = rant_string(tn, nl);
                ek = i_rant_rd_u8(r);
                if (ek == RANT_NAMED){ r->fail = 1; return 0; }
            }
            if (r->fail) return 0;
            f->elem = ek;
            if (ek == RANT_STRUCT){                         /* one element 0 template */
                esz = i_rant_emit_struct(buf, wl, r, s, emitted, total,
                                         (uint16_t)(depth + 1), idx,
                                         k == RANT_ARR ? base : 0u);
            } else if (ek == RANT_STR){
                f->str_cap = i_rant_rd_u16(r);
                esz = 2u + (uint32_t)f->str_cap;
            } else {
                esz = rant_schema_scalar_size((RantSchemaTypeKind)ek);
            }
            if (r->fail || esz == 0){ r->fail = 1; return 0; }
            f->elem_size = esz;
            if (k == RANT_ARR){
                if ((uint64_t)f->count * esz > 0xFFFFFFFFu){ r->fail = 1; return 0; }
                sz = (uint32_t)((uint64_t)f->count * esz);
            }
            break;
        }
        case RANT_STR:
            f->str_cap = i_rant_rd_u16(r);
            sz = 2u + (uint32_t)f->str_cap;
            break;
        case RANT_ENUM: {
            uint16_t i;
            f->elem = i_rant_rd_u8(r);
            f->count = i_rant_rd_u16(r);
            sz = rant_schema_scalar_size((RantSchemaTypeKind)f->elem);
            if (r->fail || sz == 0 || !i_rant_enum_backing_ok(f->elem)){ r->fail = 1; return 0; }
            for (i = 0; i < f->count && !r->fail; i++){
                i_rant_rd_skip(r, sz);
                i_rant_rd_skip(r, i_rant_rd_u8(r));
            }
            break;
        }
        case RANT_VSTR: case RANT_MAP:
            sz = 0;
            break;
        default:
            sz = rant_schema_scalar_size((RantSchemaTypeKind)k);
            if (sz == 0){ r->fail = 1; return 0; }
            break;
    }
    if (r->fail) return 0;
    f = &s->fields[idx];                      /* unchanged, recursion never moves it */
    f->type_len = (uint32_t)(r->pos - kpos);
    f->size = sz;
    return sz;
}

/* Compiles the wire bytes at buf[0..wire_len] into a RantSchema placed after them in
 * buf. NULL on a malformed blob or when cap is too small. */
static RantSchema *i_rant_schema_compile(uint8_t *buf, size_t wire_len, size_t cap){
    i_Rd r; uint8_t ver, root_kind, root_namelen;
    const char *root_name; size_t hoff, need; RantSchema *s;
    uint32_t total, nvar; uint16_t emitted = 0;

    if (!i_rant_schema_wire_fields(buf, wire_len, &total, &nvar) || total > 0xFFFFu) return NULL;

    r.w = buf; r.n = wire_len; r.pos = 0; r.fail = 0;
    ver = i_rant_rd_u8(&r);
    if (r.fail || ver != RANT_SCHEMA_WIRE_VERSION) return NULL;
    root_namelen = i_rant_rd_u8(&r);
    root_name = (const char *)(buf + r.pos);
    i_rant_rd_skip(&r, root_namelen);
    if (r.fail || r.pos >= r.n) return NULL;
    root_kind = buf[r.pos];

    hoff = i_rant_schema_handle_off(buf, wire_len);
    need = hoff + sizeof(RantSchema) + (size_t)(total ? total - 1u : 0u) * sizeof(i_Field);
    if (need > cap) return NULL;

    s = (RantSchema *)(buf + hoff);
    s->wire = rant_bytes(buf, wire_len);
    s->name = rant_string(root_name, root_namelen);
    s->nfields = (uint16_t)total;
    s->n_var = (uint16_t)nvar;
    s->value_root = (uint8_t)(root_kind != RANT_STRUCT);
    s->hash = i_rant_fnv1a64(buf, wire_len);
    if (!s->value_root){
        r.pos++;                                 /* past the root's STRUCT kind byte */
        s->size = i_rant_emit_struct(buf, wire_len, &r, s, &emitted, total,
                                     0, I_RANT_NO_PARENT, 0);
    } else {                                     /* the bare or alias root is the first field */
        if (total == 0) return NULL;
        i_rant_field_init(&s->fields[0], rant_string((const char *)(buf + r.pos), 0),
                          0, I_RANT_NO_PARENT, 0);
        emitted = 1;
        s->size = i_rant_emit_type(buf, wire_len, &r, s, &emitted, total, 0, 0, 0);
    }
    if (r.fail || emitted != (uint16_t)total) return NULL;
    {   /* tail frame ordinals in depth first order, and the array ancestry */
        uint16_t i, ord = 0;
        for (i = 0; i < s->nfields; i++){
            i_Field *f = &s->fields[i];
            if (f->parent != I_RANT_NO_PARENT){
                const i_Field *p = &s->fields[f->parent];
                f->arr_parent = (p->kind == RANT_ARR || p->kind == RANT_VARR)
                              ? f->parent : p->arr_parent;
            }
            if (i_rant_kind_var(f->kind)){
                f->offset = 0;                   /* a frame has no static offset */
                f->var_ord = ord++;
            }
        }
    }
    return s;
}
/* the builder */
/* room for extra more bytes, growing the wire buffer through the hook */
static int i_rant_schema_builder_reserve(RantSchemaBuilder *b, size_t extra){
    size_t newcap; uint8_t *nb;
    if (b->err) return 0;
    if (b->len + extra <= b->cap) return 1;
    newcap = b->cap ? b->cap : 64u;
    while (newcap < b->len + extra){
        if (newcap > ((size_t)-1) / 2u){ newcap = b->len + extra; break; }
        newcap *= 2u;
    }
    nb = (uint8_t *)b->alloc(b->user, b->buf, newcap);
    if (!nb){ b->err = -1; return 0; }              /* a failed realloc leaves b->buf intact */
    b->buf = nb; b->cap = newcap;
    return 1;
}
static void i_rant_schema_builder_put(RantSchemaBuilder *b, uint8_t v){
    if (!i_rant_schema_builder_reserve(b, 1)) return;
    b->buf[b->len++] = v;
}
static void i_rant_schema_builder_put_u16(RantSchemaBuilder *b, uint16_t v){
    if (!i_rant_schema_builder_reserve(b, 2)) return;
    i_rant_le_w16(b->buf + b->len, v); b->len += 2;
}
/* raw bytes, a compiled type encoding from elsewhere */
static void i_rant_schema_builder_put_raw(RantSchemaBuilder *b, const void *src, size_t len){
    if (!len) return;
    if (!src){ b->err = -6; return; }
    if (!i_rant_schema_builder_reserve(b, len)) return;
    memcpy(b->buf + b->len, src, len);
    b->len += len;
}
/* bytes little endian bytes of v, an enum option's backing sized value */
static void i_rant_schema_builder_put_le(RantSchemaBuilder *b, uint64_t v, uint32_t bytes){
    uint32_t i;
    if (!i_rant_schema_builder_reserve(b, bytes)) return;
    for (i = 0; i < bytes; i++) b->buf[b->len++] = (uint8_t)(v >> (8 * i));
}
static void i_rant_schema_builder_put_name(RantSchemaBuilder *b, const char *name){
    size_t n = 0, i; if (name) while (name[n]) n++;
    if (b->depth == 0 && (b->raw_type || b->value_root)){   /* the root carries no field name */
        if (n) b->err = -4;
        return;
    }
    if (n > 255){ b->err = -4; return; }
    if (!i_rant_schema_builder_reserve(b, 1 + n)) return;
    b->buf[b->len++] = (uint8_t)n;
    for (i = 0; i < n; i++) b->buf[b->len++] = (uint8_t)name[i];
}
/* count a field on the innermost open struct. A bare or alias root takes exactly one type */
static void i_rant_schema_builder_count(RantSchemaBuilder *b){
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
/* open a struct type: [STRUCT][nfields placeholder], push a nesting level */
static void i_rant_schema_builder_open_struct(RantSchemaBuilder *b){
    if (b->err) return;
    if (b->depth >= RANT_SCHEMA_MAX_DEPTH){ b->err = -2; return; }
    i_rant_schema_builder_put(b, (uint8_t)RANT_STRUCT);
    b->count_pos[b->depth] = b->len;
    i_rant_schema_builder_put(b, 0);
    b->field_count[b->depth] = 0;
    b->depth++;
}

RantSchemaBuilder rant_schema_begin(RantAllocFn alloc, void *user, const char *root_name){
    RantSchemaBuilder b;
    memset(&b, 0, sizeof b);
    b.alloc = alloc; b.user = user; b.arr_depth = 0xFFFFu; b.base_depth = 1;
    if (!alloc){ b.err = -1; return b; }
    b.cap = 64u;
    b.buf = (uint8_t *)alloc(user, NULL, b.cap);
    if (!b.buf){ b.err = -1; b.cap = 0; return b; }
    i_rant_schema_builder_put(&b, (uint8_t)RANT_SCHEMA_WIRE_VERSION);
    i_rant_schema_builder_put_name(&b, root_name);
    i_rant_schema_builder_open_struct(&b);                /* the root is a struct, depth 1 */
    return b;
}

/* the shared head of a bare type root: [version][root_namelen][root_name] */
static RantSchemaBuilder i_rant_schema_begin_bare(RantAllocFn alloc, void *user,
                                                  const char *name){
    RantSchemaBuilder b; size_t n = 0, i;
    memset(&b, 0, sizeof b);
    b.alloc = alloc; b.user = user; b.arr_depth = 0xFFFFu;
    if (name) while (name[n]) n++;
    if (!alloc || n > 255){ b.err = -1; return b; }
    b.cap = 64u;
    b.buf = (uint8_t *)alloc(user, NULL, b.cap);
    if (!b.buf){ b.err = -1; b.cap = 0; return b; }
    b.value_root = 1;                                  /* depth stays 0: no struct is open */
    i_rant_schema_builder_put(&b, (uint8_t)RANT_SCHEMA_WIRE_VERSION);
    i_rant_schema_builder_put(&b, (uint8_t)n);
    for (i = 0; i < n; i++) i_rant_schema_builder_put(&b, (uint8_t)name[i]);
    return b;
}

RantSchemaBuilder rant_schema_begin_value(RantAllocFn alloc, void *user){
    return i_rant_schema_begin_bare(alloc, user, NULL);
}

RantSchemaBuilder rant_schema_begin_alias(RantAllocFn alloc, void *user, const char *name){
    RantSchemaBuilder b = i_rant_schema_begin_bare(alloc, user, name);
    if (!b.err && (!name || !name[0])) b.err = -4;     /* an alias needs a name */
    return b;
}

/* a standalone type encoding with no header and no field name: the DSL's definition arena */
static RantSchemaBuilder i_rant_schema_begin_raw(RantAllocFn alloc, void *user){
    RantSchemaBuilder b;
    memset(&b, 0, sizeof b);
    b.alloc = alloc; b.user = user; b.arr_depth = 0xFFFFu; b.raw_type = 1;
    if (!alloc){ b.err = -1; return b; }
    b.cap = 64u;
    b.buf = (uint8_t *)alloc(user, NULL, b.cap);
    if (!b.buf){ b.err = -1; b.cap = 0; }
    return b;
}

void rant_schema_field(RantSchemaBuilder *b, const char *name, RantSchemaTypeKind kind){
    if (!b || b->err) return;
    if (rant_schema_scalar_size(kind) == 0){ b->err = -6; return; }    /* fixed scalars only */
    i_rant_schema_builder_count(b); i_rant_schema_builder_put_name(b, name); i_rant_schema_builder_put(b, (uint8_t)kind);
}

void rant_schema_field_array(RantSchemaBuilder *b, const char *name,
                             RantSchemaTypeKind elem_scalar, uint16_t count){
    if (!b || b->err) return;
    if (rant_schema_scalar_size(elem_scalar) == 0){ b->err = -6; return; }    /* scalars only */
    i_rant_schema_builder_count(b); i_rant_schema_builder_put_name(b, name);
    i_rant_schema_builder_put(b, (uint8_t)RANT_ARR); i_rant_schema_builder_put_u16(b, count);
    i_rant_schema_builder_put(b, (uint8_t)elem_scalar);
}

void rant_schema_field_string(RantSchemaBuilder *b, const char *name, uint16_t cap){
    if (!b || b->err) return;
    if (cap == 0){ b->err = -6; return; }
    i_rant_schema_builder_count(b); i_rant_schema_builder_put_name(b, name);
    i_rant_schema_builder_put(b, (uint8_t)RANT_STR); i_rant_schema_builder_put_u16(b, cap);
}

void rant_schema_field_string_array(RantSchemaBuilder *b, const char *name,
                                    uint16_t cap, uint16_t count){
    if (!b || b->err) return;
    if (cap == 0){ b->err = -6; return; }
    i_rant_schema_builder_count(b); i_rant_schema_builder_put_name(b, name);
    i_rant_schema_builder_put(b, (uint8_t)RANT_ARR); i_rant_schema_builder_put_u16(b, count);
    i_rant_schema_builder_put(b, (uint8_t)RANT_STR); i_rant_schema_builder_put_u16(b, cap);
}

/* variable kinds may sit at any struct depth but never inside an array element */
static int i_rant_schema_builder_var_ok(RantSchemaBuilder *b){
    if (b->err) return 0;
    if (b->arr_depth != 0xFFFFu){ b->err = -8; return 0; }
    return 1;
}

void rant_schema_field_var_string(RantSchemaBuilder *b, const char *name){
    if (!b || !i_rant_schema_builder_var_ok(b)) return;
    i_rant_schema_builder_count(b); i_rant_schema_builder_put_name(b, name);
    i_rant_schema_builder_put(b, (uint8_t)RANT_VSTR);
}

void rant_schema_field_var_array(RantSchemaBuilder *b, const char *name,
                                 RantSchemaTypeKind elem_scalar){
    if (!b || !i_rant_schema_builder_var_ok(b)) return;
    if (rant_schema_scalar_size(elem_scalar) == 0){ b->err = -6; return; }    /* scalars only */
    i_rant_schema_builder_count(b); i_rant_schema_builder_put_name(b, name);
    i_rant_schema_builder_put(b, (uint8_t)RANT_VARR); i_rant_schema_builder_put(b, (uint8_t)elem_scalar);
}

void rant_schema_field_var_string_array(RantSchemaBuilder *b, const char *name, uint16_t cap){
    if (!b || !i_rant_schema_builder_var_ok(b)) return;
    if (cap == 0){ b->err = -6; return; }
    i_rant_schema_builder_count(b); i_rant_schema_builder_put_name(b, name);
    i_rant_schema_builder_put(b, (uint8_t)RANT_VARR);
    i_rant_schema_builder_put(b, (uint8_t)RANT_STR); i_rant_schema_builder_put_u16(b, cap);
}

void rant_schema_field_map(RantSchemaBuilder *b, const char *name){
    if (!b || !i_rant_schema_builder_var_ok(b)) return;
    i_rant_schema_builder_count(b); i_rant_schema_builder_put_name(b, name);
    i_rant_schema_builder_put(b, (uint8_t)RANT_MAP);
}

/* the root type bytes of a compiled schema, past [version][namelen][name] */
static int i_rant_schema_root_type(const RantSchema *t, const uint8_t **bytes, size_t *len){
    size_t off;
    if (!t || !t->wire.data) return 0;
    off = 2u + t->name.len;
    if (off >= t->wire.len) return 0;
    *bytes = t->wire.data + off;
    *len   = t->wire.len - off;
    return 1;
}

/* emit [NAMED][len][name] plus the referenced schema's root type */
static void i_rant_schema_put_named(RantSchemaBuilder *b, const RantSchema *type){
    const uint8_t *tb; size_t tl;
    if (b->err) return;
    if (!type || !type->name.len || type->name.len > 255 ||
        !i_rant_schema_root_type(type, &tb, &tl)){ b->err = -6; return; }
    i_rant_schema_builder_put(b, (uint8_t)RANT_NAMED);
    i_rant_schema_builder_put(b, (uint8_t)type->name.len);
    i_rant_schema_builder_put_raw(b, type->name.data, type->name.len);
    i_rant_schema_builder_put_raw(b, tb, tl);
}

void rant_schema_field_named(RantSchemaBuilder *b, const char *name, const RantSchema *type){
    if (!b || b->err) return;
    i_rant_schema_builder_count(b); i_rant_schema_builder_put_name(b, name);
    i_rant_schema_put_named(b, type);
}

void rant_schema_field_named_array(RantSchemaBuilder *b, const char *name,
                                   const RantSchema *type, uint16_t count){
    if (!b || b->err) return;
    if (count == 0 && !i_rant_schema_builder_var_ok(b)) return;
    i_rant_schema_builder_count(b); i_rant_schema_builder_put_name(b, name);
    if (count){
        i_rant_schema_builder_put(b, (uint8_t)RANT_ARR);
        i_rant_schema_builder_put_u16(b, count);
    } else {
        i_rant_schema_builder_put(b, (uint8_t)RANT_VARR);
    }
    i_rant_schema_put_named(b, type);
}

/* Streaming enum construction, shared with the DSL so it never buffers the option list:
 * open writes the head and returns the count placeholder, add appends, finish backpatches. */
static size_t i_rant_schema_field_enum_open(RantSchemaBuilder *b, const char *name,
                                            RantSchemaTypeKind backing){
    size_t count_pos;
    i_rant_schema_builder_count(b);
    i_rant_schema_builder_put_name(b, name);
    i_rant_schema_builder_put(b, (uint8_t)RANT_ENUM);
    i_rant_schema_builder_put(b, (uint8_t)backing);
    count_pos = b->len;
    i_rant_schema_builder_put_u16(b, 0);                 /* n, backpatched by finish */
    return count_pos;
}
static void i_rant_schema_field_enum_add(RantSchemaBuilder *b, RantSchemaTypeKind backing,
                                         int64_t value, const char *name, size_t name_len){
    size_t i;
    if (!b || b->err) return;
    if (name_len > 255){ b->err = -4; return; }
    i_rant_schema_builder_put_le(b, (uint64_t)value, rant_schema_scalar_size(backing));
    if (!i_rant_schema_builder_reserve(b, 1 + name_len)) return;
    b->buf[b->len++] = (uint8_t)name_len;
    for (i = 0; i < name_len; i++) b->buf[b->len++] = (uint8_t)name[i];
}
static void i_rant_schema_field_enum_finish(RantSchemaBuilder *b, size_t count_pos, uint16_t count){
    if (!b || b->err) return;
    i_rant_le_w16(b->buf + count_pos, count);
}

void rant_schema_field_enum(RantSchemaBuilder *b, const char *name, RantSchemaTypeKind backing,
                            const RantEnumVariant *variants, uint16_t n){
    size_t count_pos; uint16_t i;
    if (!b || b->err) return;
    if (rant_schema_scalar_size(backing) == 0 || !i_rant_enum_backing_ok((uint8_t)backing)){
        b->err = -6; return;                           /* integer backings only */
    }
    count_pos = i_rant_schema_field_enum_open(b, name, backing);
    for (i = 0; i < n; i++){
        int64_t v = variants ? variants[i].value : 0;
        const char *vn = variants ? variants[i].name : NULL;
        size_t vl = 0; if (vn) while (vn[vl]) vl++;
        if (!i_rant_enum_val_fits((uint8_t)backing, v)){ b->err = -6; return; }
        i_rant_schema_field_enum_add(b, backing, v, vn, vl);
    }
    i_rant_schema_field_enum_finish(b, count_pos, n);
}

void rant_schema_begin_struct(RantSchemaBuilder *b, const char *name){
    if (!b || b->err) return;
    if (b->value_root && b->depth == 0){ b->err = -3; return; }  /* struct roots use begin */
    i_rant_schema_builder_count(b);                   /* a field of the parent */
    i_rant_schema_builder_put_name(b, name);
    i_rant_schema_builder_open_struct(b);
}

void rant_schema_begin_struct_array(RantSchemaBuilder *b, const char *name, uint16_t count){
    if (!b || b->err) return;
    if (count == 0 && !i_rant_schema_builder_var_ok(b)) return;
    if (b->arr_depth != 0xFFFFu){ b->err = -8; return; }   /* one array level only */
    i_rant_schema_builder_count(b);
    i_rant_schema_builder_put_name(b, name);
    if (count){
        i_rant_schema_builder_put(b, (uint8_t)RANT_ARR);
        i_rant_schema_builder_put_u16(b, count);
    } else {
        i_rant_schema_builder_put(b, (uint8_t)RANT_VARR);
    }
    i_rant_schema_builder_open_struct(b);
    if (!b->err) b->arr_depth = b->depth;           /* this level is an array element */
}

void rant_schema_end_struct(RantSchemaBuilder *b){
    if (!b || b->err) return;
    if (b->depth <= b->base_depth){ b->err = -3; return; }   /* the root closes in finish */
    if (b->arr_depth == b->depth) b->arr_depth = 0xFFFFu;
    b->depth--;
    b->buf[b->count_pos[b->depth]] = (uint8_t)b->field_count[b->depth];
}

RantSchema *rant_schema_finish(RantSchemaBuilder *b){
    RantSchema *s = NULL;
    int closed = b && !b->err &&
                 (b->value_root ? (b->depth == 0 && b->value_root == 2)   /* the one bare type */
                                : b->depth == 1);            /* else an unbalanced begin and end */
    if (closed){
        size_t need; uint8_t *nb; uint32_t total = 0, nvar = 0;
        if (!b->value_root)
            b->buf[b->count_pos[0]] = (uint8_t)b->field_count[0]; /* backpatch the count */
        i_rant_schema_wire_fields(b->buf, b->len, &total, &nvar);
        need = b->len + 7u + sizeof(RantSchema)
             + (size_t)(total ? total - 1u : 0u) * sizeof(i_Field);
        nb = (uint8_t *)b->alloc(b->user, b->buf, need);      /* room for the handle */
        if (nb){ b->buf = nb; b->cap = need; s = i_rant_schema_compile(b->buf, b->len, b->cap); }
    }
    if (!s && b && b->buf) b->alloc(b->user, b->buf, 0);      /* free on any failure */
    if (b) b->buf = NULL;                                     /* owned by s now, or freed */
    return s;
}

/* bytes a compiled schema needs for wire: the copy, the alignment pad, the handle and
   the field table. 0 if the wire is malformed. */
static size_t i_rant_schema_compiled_size(const void *wire, size_t wire_len){
    uint32_t total, nvar;
    if (!i_rant_schema_wire_fields(wire, wire_len, &total, &nvar) || total > 0xFFFFu) return 0;
    return wire_len + 7u + sizeof(RantSchema) + (size_t)(total ? total - 1u : 0u) * sizeof(i_Field);
}

RantSchema *rant_schema_parse(const void *wire, size_t wire_len, RantAllocFn alloc, void *user){
    size_t need, i; uint8_t *buf; RantSchema *s;
    if (!wire || !alloc || wire_len == 0) return NULL;
    need = i_rant_schema_compiled_size(wire, wire_len);
    if (need == 0) return NULL;                              /* a malformed header */
    buf = (uint8_t *)alloc(user, NULL, need);
    if (!buf) return NULL;
    for (i = 0; i < wire_len; i++) buf[i] = ((const uint8_t *)wire)[i];   /* persist the bytes */
    s = i_rant_schema_compile(buf, wire_len, need);
    if (!s) alloc(user, buf, 0);                             /* a malformed body: no leak */
    return s;
}

RantSchema *rant_schema_copy(const RantSchema *s, RantAllocFn alloc, void *user){
    RantBytes w;
    if (!s || !alloc) return NULL;
    w = rant_schema_wire(s);
    return rant_schema_parse(w.data, w.len, alloc, user);
}

void rant_schema_free(RantSchema *s, RantAllocFn alloc, void *user){
    if (s && alloc) alloc(user, (void *)s->wire.data, 0);    /* wire.data is the block base */
}

/* queries */
RantBytes rant_schema_wire(const RantSchema *s){
    RantBytes b; if (s) return s->wire;
    b.data = NULL; b.len = 0; return b;
}
uint64_t     rant_schema_hash(const RantSchema *s){ return s ? s->hash : 0; }
RantString rant_schema_name(const RantSchema *s){
    RantString n; if (s) return s->name;
    n.data = NULL; n.len = 0; return n;
}
uint32_t rant_schema_size(const RantSchema *s){ return s ? s->size : 0; }
uint16_t rant_schema_field_count(const RantSchema *s){ return s ? s->nfields : 0; }

int rant_schema_field_at(const RantSchema *s, uint16_t i, RantSchemaFieldInfo *out){
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

/* Matches a dotted path against a field: the last segment is its own name, the earlier
 * ones its ancestors. A segment may carry [N] on an array field, which *index gets. */
static int i_rant_schema_path_match(const RantSchema *s, const i_Field *f, const char *path,
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
                    if (f->kind != RANT_ARR && f->kind != RANT_VARR) return 0;
                    found = idx;
                    nend = br - 1;
                }
            }
        }
        if (f->name.len != (size_t)(nend - seg) ||
            (f->name.len && memcmp(f->name.data, seg, f->name.len) != 0)) return 0;
        if (f->parent == I_RANT_NO_PARENT){                  /* root: all segments consumed */
            if (seg != path) return 0;
            if (index) *index = found;
            return 1;
        }
        if (seg == path) return 0;                         /* segments ran out early */
        end = seg - 1;                                     /* past the dot */
        f = &s->fields[f->parent];
    }
}

static const i_Field *i_rant_schema_field_by_path(const RantSchema *s, const char *path,
                                                  uint32_t *index){
    uint16_t i; size_t n;
    if (index) *index = 0;
    if (!s || !path) return NULL;
    n = strlen(path);
    for (i = 0; i < s->nfields; i++)
        if (i_rant_schema_path_match(s, &s->fields[i], path, n, index)) return &s->fields[i];
    return NULL;
}

int rant_schema_field_index(const RantSchema *s, const char *path){
    const i_Field *f = i_rant_schema_field_by_path(s, path, NULL);
    return f ? (int)(f - s->fields) : -1;
}

RantBytes rant_schema_field_type_wire(const RantSchema *s, uint16_t field){
    const i_Field *f;
    if (!s || field >= s->nfields) return rant_bytes(NULL, 0);
    f = &s->fields[field];
    if ((size_t)f->type_off + f->type_len > s->wire.len) return rant_bytes(NULL, 0);
    return rant_bytes(s->wire.data + f->type_off, f->type_len);
}

uint16_t rant_schema_enum_count(const RantSchema *s, uint16_t field){
    if (!s || field >= s->nfields) return 0;
    return s->fields[field].kind == RANT_ENUM ? s->fields[field].count : 0;
}

int rant_schema_enum_variant(const RantSchema *s, uint16_t field, uint16_t i,
                             int64_t *value, RantString *name){
    const i_Field *f; const uint8_t *w; uint32_t pos, end; uint16_t k; uint8_t bs;
    if (!s || field >= s->nfields) return 0;
    f = &s->fields[field];
    if (f->kind != RANT_ENUM || i >= f->count) return 0;
    bs  = (uint8_t)rant_schema_scalar_size((RantSchemaTypeKind)f->elem);
    w   = s->wire.data;
    /* past an optional NAMED tag, then [ENUM][backing][u16 n], to the first option */
    pos = f->type_off;
    if (w[pos] == RANT_NAMED) pos += 2u + w[pos + 1];
    pos += 4u;
    end = f->type_off + f->type_len;
    for (k = 0; k < i; k++){                 /* hop over earlier options: [value][u8 nl][name] */
        if ((size_t)pos + bs + 1u > end) return 0;
        pos += bs + 1u + w[pos + bs];
    }
    if ((size_t)pos + bs + 1u > end) return 0;
    { uint8_t nl = w[pos + bs];
      if ((size_t)pos + bs + 1u + nl > end) return 0;
      if (value) *value = i_rant_enum_read_val(f->elem, w + pos);
      if (name)  *name  = rant_string((const char *)(w + pos + bs + 1u), nl); }
    return 1;
}

RantString rant_enum_name_of(const RantSchema *s, uint16_t field, int64_t value){
    uint16_t i, n = rant_schema_enum_count(s, field);
    for (i = 0; i < n; i++){
        int64_t v; RantString nm;
        if (rant_schema_enum_variant(s, field, i, &v, &nm) && v == value) return nm;
    }
    return rant_string(NULL, 0);
}

int rant_enum_value_of(const RantSchema *s, uint16_t field, const char *name, int64_t *out){
    uint16_t i, n = rant_schema_enum_count(s, field);
    size_t want = 0;
    if (name) while (name[want]) want++;
    for (i = 0; i < n; i++){
        int64_t v; RantString nm;
        if (rant_schema_enum_variant(s, field, i, &v, &nm) && nm.len == want &&
            (want == 0 || memcmp(nm.data, name, want) == 0)){ if (out) *out = v; return 1; }
    }
    return 0;
}
/* spelling types back as DSL text */
static const char *i_rant_why_kind(uint8_t k){
    switch (k){
        case RANT_U8:    return "u8";  case RANT_U16: return "u16";
        case RANT_U32: return "u32"; case RANT_U64: return "u64";
        case RANT_I8:    return "i8";  case RANT_I16: return "i16";
        case RANT_I32: return "i32"; case RANT_I64: return "i64";
        case RANT_F32: return "f32"; case RANT_F64: return "f64";
        case RANT_BOOL: return "bool"; case RANT_STRUCT: return "struct";
        default: return "?";
    }
}

/* A counting text sink: appends into [p, end) but always tallies the full length in n,
   so a NULL or short buffer still measures. */
typedef struct { char *p, *end; uint32_t n; } i_RantTextOut;
static void i_rant_out_raw(i_RantTextOut *o, const char *s, size_t len){
    size_t i;
    o->n += (uint32_t)len;
    for (i = 0; i < len && o->p < o->end; i++) *o->p++ = s[i];
}
static void i_rant_out_str(i_RantTextOut *o, const char *s){ i_rant_out_raw(o, s, strlen(s)); }
static void i_rant_out_view(i_RantTextOut *o, RantString v){ if (v.data) i_rant_out_raw(o, v.data, v.len); }
static void i_rant_out_indent(i_RantTextOut *o, int levels){ while (levels-- > 0) i_rant_out_raw(o, "  ", 2); }
static void i_rant_out_i64(i_RantTextOut *o, int64_t v){
    char tmp[20]; int k = 0; uint64_t u = v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    if (v < 0) i_rant_out_raw(o, "-", 1);
    do { tmp[k++] = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
    while (k) { char c = tmp[--k]; i_rant_out_raw(o, &c, 1); }
}

/* Steps over the type at pos in already validated wire. Returns the position past it. */
static size_t i_rant_skip_type(const uint8_t *w, size_t n, size_t pos){
    uint8_t k;
    if (pos >= n) return n;
    k = w[pos++];
    switch (k){
        case RANT_NAMED:
            if (pos >= n) return n;
            pos += 1u + w[pos];
            return i_rant_skip_type(w, n, pos);
        case RANT_STR: return pos + 2u <= n ? pos + 2u : n;
        case RANT_ARR: return i_rant_skip_type(w, n, pos + 2u <= n ? pos + 2u : n);
        case RANT_VARR: return i_rant_skip_type(w, n, pos);
        case RANT_VSTR: case RANT_MAP: return pos;
        case RANT_ENUM: {
            uint8_t bs; uint16_t cnt, i;
            if (pos + 3u > n) return n;
            bs = (uint8_t)rant_schema_scalar_size((RantSchemaTypeKind)w[pos]);
            cnt = i_rant_le_r16(w + pos + 1);
            pos += 3u;
            for (i = 0; i < cnt; i++){
                if (pos + bs + 1u > n) return n;
                pos += bs + 1u + w[pos + bs];
            }
            return pos <= n ? pos : n;
        }
        case RANT_STRUCT: {
            uint8_t nf; uint16_t i;
            if (pos >= n) return n;
            nf = w[pos++];
            for (i = 0; i < nf; i++){
                if (pos >= n) return n;
                pos += 1u + w[pos];
                if (pos > n) return n;
                pos = i_rant_skip_type(w, n, pos);
            }
            return pos;
        }
        default: return pos;
    }
}

/* Spells the type at pos as DSL text. A NAMED type spells as its bare name and the
 * caller hoists its definition. Returns the position past the type. */
static size_t i_rant_spell_type(i_RantTextOut *o, const uint8_t *w, size_t n, size_t pos,
                                int indent){
    uint8_t k;
    if (pos >= n) return n;
    k = w[pos++];
    switch (k){
        case RANT_NAMED: {
            uint8_t nl;
            if (pos >= n) return n;
            nl = w[pos++];
            i_rant_out_raw(o, (const char *)(w + pos), nl);
            return i_rant_skip_type(w, n, pos + nl);
        }
        case RANT_STR:
            i_rant_out_str(o, "string<");
            i_rant_out_i64(o, (int64_t)i_rant_le_r16(w + pos));
            i_rant_out_str(o, ">");
            return pos + 2u;
        case RANT_VSTR: i_rant_out_str(o, "string"); return pos;
        case RANT_MAP:    i_rant_out_str(o, "map");    return pos;
        case RANT_ARR: {
            uint16_t cnt = i_rant_le_r16(w + pos);
            size_t after = i_rant_spell_type(o, w, n, pos + 2u, indent);
            i_rant_out_str(o, "[");
            i_rant_out_i64(o, (int64_t)cnt);
            i_rant_out_str(o, "]");
            return after;
        }
        case RANT_VARR: {
            size_t after = i_rant_spell_type(o, w, n, pos, indent);
            i_rant_out_str(o, "[]");
            return after;
        }
        case RANT_ENUM: {
            uint8_t backing; uint16_t cnt, i; uint32_t bs;
            if (pos + 3u > n) return n;
            backing = w[pos]; cnt = i_rant_le_r16(w + pos + 1); pos += 3u;
            bs = rant_schema_scalar_size((RantSchemaTypeKind)backing);
            i_rant_out_str(o, "enum<");
            i_rant_out_str(o, i_rant_why_kind(backing));
            i_rant_out_str(o, "> { ");
            for (i = 0; i < cnt; i++){
                uint8_t nl;
                if (pos + bs + 1u > n) return n;
                if (i) i_rant_out_str(o, ", ");
                nl = w[pos + bs];
                i_rant_out_raw(o, (const char *)(w + pos + bs + 1u), nl);
                i_rant_out_str(o, " = ");
                i_rant_out_i64(o, i_rant_enum_read_val(backing, w + pos));
                pos += bs + 1u + nl;
            }
            i_rant_out_str(o, " }");
            return pos;
        }
        case RANT_STRUCT: {
            uint8_t nf; uint16_t i;
            if (pos >= n) return n;
            nf = w[pos++];
            i_rant_out_str(o, "{\n");
            for (i = 0; i < nf; i++){
                uint8_t nl;
                if (pos >= n) return n;
                nl = w[pos++];
                i_rant_out_indent(o, indent + 1);
                i_rant_out_raw(o, (const char *)(w + pos), nl);
                pos += nl;
                i_rant_out_str(o, ": ");
                pos = i_rant_spell_type(o, w, n, pos, indent + 1);
                i_rant_out_str(o, ",\n");
            }
            i_rant_out_indent(o, indent);
            i_rant_out_str(o, "}");
            return pos;
        }
        default:
            i_rant_out_str(o, i_rant_why_kind(k));
            return pos;
    }
}

/* The named types a schema uses, in dependency order and deduped by name, so each
 * prints one leading definition. */
#define I_RANT_MAX_DEFS 48u
typedef struct {
    const uint8_t *w; size_t n;
    struct { uint32_t noff, toff; uint8_t nlen; } d[I_RANT_MAX_DEFS];
    uint16_t count;
} i_RantDefScan;

static void i_rant_defscan_add(i_RantDefScan *L, uint32_t noff, uint8_t nlen, uint32_t toff){
    uint16_t i;
    for (i = 0; i < L->count; i++)
        if (L->d[i].nlen == nlen && memcmp(L->w + L->d[i].noff, L->w + noff, nlen) == 0) return;
    if (L->count >= I_RANT_MAX_DEFS) return;     /* past the cap the tail spells by reference */
    L->d[L->count].noff = noff; L->d[L->count].nlen = nlen; L->d[L->count].toff = toff;
    L->count++;
}

static size_t i_rant_defscan_type(i_RantDefScan *L, size_t pos){
    uint8_t k;
    if (pos >= L->n) return L->n;
    k = L->w[pos];
    if (k == RANT_NAMED){
        uint8_t nl; size_t ipos, after;
        if (pos + 2u > L->n) return L->n;
        nl = L->w[pos + 1];
        ipos = pos + 2u + nl;
        if (ipos > L->n) return L->n;
        after = i_rant_defscan_type(L, ipos);          /* dependencies first */
        i_rant_defscan_add(L, (uint32_t)(pos + 2u), nl, (uint32_t)ipos);
        return after;
    }
    pos++;
    switch (k){
        case RANT_ARR:    return i_rant_defscan_type(L, pos + 2u <= L->n ? pos + 2u : L->n);
        case RANT_VARR: return i_rant_defscan_type(L, pos);
        case RANT_STRUCT: {
            uint8_t nf; uint16_t i;
            if (pos >= L->n) return L->n;
            nf = L->w[pos++];
            for (i = 0; i < nf; i++){
                if (pos >= L->n) return L->n;
                pos += 1u + L->w[pos];
                if (pos > L->n) return L->n;
                pos = i_rant_defscan_type(L, pos);
            }
            return pos;
        }
        default: return i_rant_skip_type(L->w, L->n, pos - 1u);
    }
}

uint32_t rant_schema_print(const RantSchema *s, char *buf, size_t cap){
    i_RantTextOut o;
    i_RantDefScan scan;
    size_t root_type;
    uint16_t i;
    o.p   = (buf && cap) ? buf : NULL;
    o.end = o.p ? buf + cap - 1 : NULL;   /* one byte reserved for the NUL */
    o.n   = 0;
    if (!s){ if (buf && cap) buf[0] = '\0'; return 0; }
    root_type = 2u + s->name.len;
    scan.w = s->wire.data; scan.n = s->wire.len; scan.count = 0;
    i_rant_defscan_type(&scan, root_type);
    for (i = 0; i < scan.count; i++){                    /* each named type, once, in order */
        i_rant_out_raw(&o, (const char *)(scan.w + scan.d[i].noff), scan.d[i].nlen);
        i_rant_out_str(&o, " = ");
        i_rant_spell_type(&o, scan.w, scan.n, scan.d[i].toff, 0);
        i_rant_out_str(&o, "\n");
    }
    if (s->value_root){                    /* a bare type, or Name = type for an alias */
        if (s->name.len){
            i_rant_out_view(&o, s->name);
            i_rant_out_str(&o, " = ");
        }
        i_rant_spell_type(&o, scan.w, scan.n, root_type, 0);
        i_rant_out_str(&o, "\n");
    } else {
        i_rant_out_view(&o, s->name);
        i_rant_out_str(&o, " ");
        i_rant_spell_type(&o, scan.w, scan.n, root_type, 0);
        i_rant_out_str(&o, "\n");
    }
    if (o.p) *o.p = '\0';                   /* o.p is at most end, the reserved byte */
    else if (buf && cap) buf[0] = '\0';
    return o.n;
}

/* reader and writer compatibility */
/* a top level field by name. A nested type is compared as one exact unit */
static const i_Field *i_rant_schema_find(const RantSchema *s, RantString name){
    uint16_t i;
    for (i = 0; i < s->nfields; i++){
        const i_Field *f = &s->fields[i];
        if (f->depth == 0 && f->name.len == name.len &&
            (name.len == 0 || memcmp(f->name.data, name.data, name.len) == 0)) return f;
    }
    return NULL;
}

/* consumes a NAMED wrapper if present, returning its name, else {NULL,0} */
static RantString i_rant_rd_type_name(i_Rd *r){
    RantString nm = rant_string(NULL, 0);
    if (r->pos < r->n && r->w[r->pos] == RANT_NAMED){
        uint8_t nl;
        r->pos++;
        nl = i_rant_rd_u8(r);
        nm = rant_string((const char *)(r->w + r->pos), nl);
        i_rant_rd_skip(r, nl);
        if (r->fail) return rant_string(NULL, 0);
    }
    return nm;
}

static void i_rant_rd_skip_enum(i_Rd *r, uint8_t backing){
    uint16_t n = i_rant_rd_u16(r), i;
    uint32_t bs = rant_schema_scalar_size((RantSchemaTypeKind)backing);
    for (i = 0; i < n && !r->fail; i++){
        i_rant_rd_skip(r, bs);
        i_rant_rd_skip(r, i_rant_rd_u8(r));
    }
}

/* Can a reader declaring the type at ra read a writer's type at rb. Both advance past
 * their type. Names narrow, everything else compares exactly. See spec/schema.md. */
static int i_rant_type_cmp(i_Rd *ra, i_Rd *rb, uint16_t depth){
    RantString na, nb; uint8_t ka, kb;
    if (depth > RANT_SCHEMA_MAX_DEPTH + 1u) return 0;
    na = i_rant_rd_type_name(ra);
    nb = i_rant_rd_type_name(rb);
    if (ra->fail || rb->fail) return 0;
    if (na.len && !rant_string_eq(na, nb)) return 0;
    ka = i_rant_rd_u8(ra); kb = i_rant_rd_u8(rb);
    if (ra->fail || rb->fail || ka != kb) return 0;
    switch (ka){
        case RANT_STR: {
            uint16_t ca = i_rant_rd_u16(ra), cb = i_rant_rd_u16(rb);
            return !ra->fail && !rb->fail && ca == cb;
        }
        case RANT_ARR: {
            uint16_t ca = i_rant_rd_u16(ra), cb = i_rant_rd_u16(rb);
            if (ra->fail || rb->fail || ca != cb) return 0;
            return i_rant_type_cmp(ra, rb, (uint16_t)(depth + 1));
        }
        case RANT_VARR:
            return i_rant_type_cmp(ra, rb, (uint16_t)(depth + 1));
        case RANT_VSTR: case RANT_MAP:
            return 1;
        case RANT_ENUM: {                          /* the backing width only, names are advisory */
            uint8_t ba = i_rant_rd_u8(ra), bb = i_rant_rd_u8(rb);
            i_rant_rd_skip_enum(ra, ba);
            i_rant_rd_skip_enum(rb, bb);
            return !ra->fail && !rb->fail && ba == bb;
        }
        case RANT_STRUCT: {
            uint8_t nfa = i_rant_rd_u8(ra), nfb = i_rant_rd_u8(rb); uint16_t i;
            if (ra->fail || rb->fail || nfa != nfb) return 0;
            for (i = 0; i < nfa; i++){
                uint8_t la = i_rant_rd_u8(ra), lb = i_rant_rd_u8(rb);
                const char *pa = (const char *)(ra->w + ra->pos);
                const char *pb = (const char *)(rb->w + rb->pos);
                i_rant_rd_skip(ra, la); i_rant_rd_skip(rb, lb);
                if (ra->fail || rb->fail || la != lb) return 0;
                if (la && memcmp(pa, pb, la) != 0) return 0;
                if (!i_rant_type_cmp(ra, rb, (uint16_t)(depth + 1))) return 0;
            }
            return 1;
        }
        default:
            return rant_schema_scalar_size((RantSchemaTypeKind)ka) != 0;
    }
}

/* the two fields' types, compared from the wire */
static int i_rant_field_cmp(const RantSchema *sa, const i_Field *a,
                            const RantSchema *sb, const i_Field *b){
    i_Rd ra, rb;
    ra.w = sa->wire.data; ra.n = sa->wire.len; ra.pos = a->type_off; ra.fail = 0;
    rb.w = sb->wire.data; rb.n = sb->wire.len; rb.pos = b->type_off; rb.fail = 0;
    return i_rant_type_cmp(&ra, &rb, 0);
}

/* bounded appenders for the subset why text. No stdio, and a NULL buffer skips all text */
static char *i_rant_why_str(char *p, char *end, const char *s){
    if (!p) return NULL;
    while (*s && p < end) *p++ = *s++;
    return p;
}
static char *i_rant_why_view(char *p, char *end, RantString s){
    size_t i;
    if (!p) return NULL;
    for (i = 0; i < s.len && p < end; i++) *p++ = s.data[i];
    return p;
}
static char *i_rant_why_u(char *p, char *end, uint32_t v){
    char tmp[10]; int n = 0;
    if (!p) return NULL;
    do { tmp[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n && p < end) *p++ = tmp[--n];
    return p;
}
/* a field's type in compact DSL form: "Float3[4]", "f32[8]", "string<33>", "map" */
static char *i_rant_why_type(char *p, char *end, const i_Field *f){
    uint8_t is_arr = (uint8_t)(f->kind == RANT_ARR || f->kind == RANT_VARR);
    if (f->type_name.len) return i_rant_why_view(p, end, f->type_name);
    if (f->kind == RANT_MAP)    return i_rant_why_str(p, end, "map");
    if (f->kind == RANT_VSTR) return i_rant_why_str(p, end, "string");
    if (f->kind == RANT_ENUM){                              /* the width is what matters */
        p = i_rant_why_str(p, end, "enum<");
        p = i_rant_why_str(p, end, i_rant_why_kind(f->elem));
        return i_rant_why_str(p, end, ">");
    }
    if (is_arr && f->elem_name.len){
        p = i_rant_why_view(p, end, f->elem_name);
    } else {
        uint8_t elem = is_arr ? f->elem : f->kind;
        if (elem == RANT_STR){
            p = i_rant_why_str(p, end, "string<");
            p = i_rant_why_u(p, end, f->str_cap);
            p = i_rant_why_str(p, end, ">");
        } else {
            p = i_rant_why_str(p, end, i_rant_why_kind(elem));
        }
    }
    if (f->kind == RANT_ARR){
        p = i_rant_why_str(p, end, "[");
        p = i_rant_why_u(p, end, f->count);
        p = i_rant_why_str(p, end, "]");
    } else if (f->kind == RANT_VARR){
        p = i_rant_why_str(p, end, "[]");
    }
    return p;
}
static int i_rant_why_done(char *buf, char *p){     /* NUL terminate the reason and refuse */
    if (buf) *p = '\0';
    return 0;
}
/* a schema's root in DSL words: a bare type's spelling, or struct 'Name' */
static char *i_rant_why_root(char *p, char *end, const RantSchema *s){
    if (s->value_root){
        if (s->name.len){
            p = i_rant_why_view(p, end, s->name);
            p = i_rant_why_str(p, end, " = ");
        }
        return i_rant_why_type(p, end, &s->fields[0]);
    }
    p = i_rant_why_str(p, end, "struct '");
    p = i_rant_why_view(p, end, s->name);
    return i_rant_why_str(p, end, "'");
}

int rant_schema_subset_why(const RantSchema *sub, const RantSchema *pub,
                           char *buf, size_t cap){
    uint16_t i;
    char *p = (buf && cap) ? buf : NULL, *end = p ? buf + cap - 1 : NULL;
    if (p) *p = '\0';
    if (!sub || !pub)
        return i_rant_why_done(p, i_rant_why_str(p, end, "schema missing"));
    if (sub->value_root || pub->value_root){        /* a bare root: the two roots are the types */
        int named_ok = !sub->name.len || rant_string_eq(sub->name, pub->name);
        if (sub->value_root == pub->value_root && named_ok &&
            i_rant_field_cmp(sub, &sub->fields[0], pub, &pub->fields[0])) return 1;
        p = i_rant_why_str(p, end, "root: reader ");
        p = i_rant_why_root(p, end, sub);
        p = i_rant_why_str(p, end, ", writer ");
        return i_rant_why_done(buf, i_rant_why_root(p, end, pub));
    }
    if (sub->name.len != pub->name.len ||
        (sub->name.len && memcmp(sub->name.data, pub->name.data, sub->name.len) != 0)){
        p = i_rant_why_str(p, end, "reader type '"); p = i_rant_why_view(p, end, sub->name);
        p = i_rant_why_str(p, end, "' != writer type '"); p = i_rant_why_view(p, end, pub->name);
        return i_rant_why_done(buf, i_rant_why_str(p, end, "'"));
    }
    for (i = 0; i < sub->nfields; i++){
        const i_Field *a = &sub->fields[i], *b;
        if (a->depth != 0) continue;                          /* members ride their struct */
        b = i_rant_schema_find(pub, a->name);
        if (!b){
            p = i_rant_why_str(p, end, "field '"); p = i_rant_why_view(p, end, a->name);
            return i_rant_why_done(buf, i_rant_why_str(p, end, "' missing from writer"));
        }
        if (!i_rant_field_cmp(sub, a, pub, b)){
            p = i_rant_why_str(p, end, "field '"); p = i_rant_why_view(p, end, a->name);
            p = i_rant_why_str(p, end, "': reader "); p = i_rant_why_type(p, end, a);
            p = i_rant_why_str(p, end, ", writer ");
            return i_rant_why_done(buf, i_rant_why_type(p, end, b));
        }
    }
    return 1;
}

int rant_schema_subset(const RantSchema *sub, const RantSchema *pub){
    return rant_schema_subset_why(sub, pub, NULL, 0);
}

/* the flat index just past a field's subtree */
static uint16_t i_rant_subtree_end(const RantSchema *s, uint16_t i){
    uint16_t d = s->fields[i].depth, k = (uint16_t)(i + 1u);
    while (k < s->nfields && s->fields[k].depth > d) k++;
    return k;
}

RantSchema *rant_schema_rebase(const RantSchema *sub, const RantSchema *pub,
                               RantAllocFn alloc, void *user){
    RantSchema *r; uint16_t i = 0;
    if (!alloc || !rant_schema_subset(sub, pub)) return NULL;
    r = rant_schema_parse(sub->wire.data, sub->wire.len, alloc, user);
    if (!r) return NULL;
    while (i < r->nfields){                                   /* the writer's layout */
        const i_Field *p = i_rant_schema_find(pub, r->fields[i].name);
        uint16_t re = i_rant_subtree_end(r, i), j, k;
        if (!p || r->fields[i].depth != 0){ rant_schema_free(r, alloc, user); return NULL; }
        j = (uint16_t)(p - pub->fields);
        for (k = 0; (uint16_t)(i + k) < re; k++){             /* the subtrees match exactly */
            i_Field *rf = &r->fields[i + k];
            const i_Field *pf;
            if ((uint16_t)(j + k) >= pub->nfields){ rant_schema_free(r, alloc, user); return NULL; }
            pf = &pub->fields[j + k];
            rf->offset = pf->offset;
            rf->var_ord = pf->var_ord;
        }
        i = re;
    }
    r->size = pub->size;                                      /* and the writer's bounds, so */
    r->n_var = pub->n_var;   /* validate and walk see pub's messages */
    return r;
}
/* the variable tail */
uint32_t rant_schema_msg_min(const RantSchema *s){
    return s ? s->size + 4u * s->n_var : 0;
}

uint32_t rant_schema_msg_len(const RantSchema *s, const void *buf, size_t cap){
    const uint8_t *p = (const uint8_t *)buf;
    uint64_t total; uint16_t i;
    if (!s || !p) return 0;
    total = s->size;
    for (i = 0; i < s->n_var; i++){                     /* hop the frames, bounds checked */
        if (total + 4u > cap) return 0;
        total += 4u + (uint64_t)i_rant_le_r32(p + total);
        if (total > cap) return 0;
    }
    return total <= 0xFFFFFFFFu ? (uint32_t)total : 0;
}

/* the payload view of variable field ordinal ord, {NULL,0} on any bound break */
static RantBytes i_rant_schema_frame(const RantSchema *s, RantBytes msg, uint16_t ord){
    uint64_t pos = s->size; uint32_t flen; uint16_t i;
    for (i = 0; i <= ord && i < s->n_var; i++){
        if (pos + 4u > msg.len) break;
        flen = i_rant_le_r32(msg.data + pos);
        if (pos + 4u + flen > msg.len) break;
        if (i == ord) return rant_bytes(msg.data + pos + 4u, flen);
        pos += 4u + (uint64_t)flen;
    }
    return rant_bytes(NULL, 0);
}

/* Resizes variable field ord's frame to new_len, moving the rest of the tail. src NULL
 * keeps the existing prefix and zeroes any growth. src must not alias buf. */
static int i_rant_schema_frame_write(const RantSchema *s, uint8_t *buf, size_t cap,
                                     uint16_t ord, const void *src, size_t new_len){
    uint64_t pos = s->size, total; uint32_t old; uint16_t i;
    if (new_len && !src && src != NULL) return 0;
    if (new_len > 0xFFFFFFFFu - 4u) return 0;
    total = rant_schema_msg_len(s, buf, cap);
    if (total == 0) return 0;                           /* a malformed or uninitialized buffer */
    for (i = 0; i < ord; i++) pos += 4u + (uint64_t)i_rant_le_r32(buf + pos);
    old = i_rant_le_r32(buf + pos);                     /* bounds proven by msg_len's walk */
    if (total - old + new_len > cap) return 0;          /* never silently truncate */
    memmove(buf + pos + 4u + new_len, buf + pos + 4u + old,
            (size_t)(total - (pos + 4u + old)));
    i_rant_le_w32(buf + pos, (uint32_t)new_len);
    if (src){ if (new_len) memcpy(buf + pos + 4u, src, new_len); }
    else if (new_len > old) memset(buf + pos + 4u + old, 0, new_len - old);
    return 1;
}
static int i_rant_schema_set_frame(const RantSchema *s, uint8_t *buf, size_t cap,
                                   uint16_t ord, const void *src, size_t src_len){
    if (src_len && !src) return 0;
    return i_rant_schema_frame_write(s, buf, cap, ord, src_len ? src : (const void *)"", src_len);
}

/* reading a message */
int rant_schema_validate(const RantSchema *s, RantBytes msg){
    uint32_t total;
    if (!s) return 0;
    if (s->n_var == 0) return msg.len == s->size;
    total = rant_schema_msg_len(s, msg.data, msg.len);    /* the frames must consume it exactly */
    return total != 0 && total == msg.len;
}

/* Where field f's bytes sit in msg for array element index, bounds checked. A fixed
 * struct array strides from element 0, a variable one strides inside the array's frame. */
static int i_rant_field_addr(const RantSchema *s, RantBytes msg, const i_Field *f,
                             uint32_t index, size_t *out_off){
    size_t off;
    if (i_rant_kind_var(f->kind)){ *out_off = 0; return 1; }     /* frames locate themselves */
    if (f->arr_parent == I_RANT_NO_PARENT){
        off = f->offset;
    } else {
        const i_Field *a = &s->fields[f->arr_parent];
        if (a->elem_size == 0) return 0;
        if (a->kind == RANT_ARR){
            if (index >= a->count) return 0;
            off = (size_t)f->offset + (size_t)index * a->elem_size;
        } else {
            RantBytes fr = i_rant_schema_frame(s, msg, a->var_ord);
            if (!fr.data || (uint64_t)index * a->elem_size + a->elem_size > fr.len) return 0;
            off = (size_t)(fr.data - msg.data) + (size_t)index * a->elem_size + f->offset;
        }
    }
    if (off + f->size > msg.len) return 0;
    *out_off = off;
    return 1;
}

/* Resolves a possibly indexed path against a message. NULL if unknown, out of range or
 * the message is too short. */
static const i_Field *i_rant_schema_read_lookup(const RantSchema *s, RantBytes msg,
                                                const char *field, size_t *off){
    uint32_t index = 0;
    const i_Field *f = i_rant_schema_field_by_path(s, field, &index);
    if (!f || !i_rant_field_addr(s, msg, f, index, off)) return NULL;
    return f;
}

static uint64_t i_rant_schema_read_uint(uint8_t kind, const uint8_t *p){
    switch (kind){
        case RANT_U8: case RANT_BOOL: return p[0];
        case RANT_U16: return i_rant_le_r16(p);
        case RANT_U32: return i_rant_le_r32(p);
        case RANT_U64: return i_rant_le_r64(p);
        default: return 0;
    }
}
static int64_t i_rant_schema_read_int(uint8_t kind, const uint8_t *p){
    switch (kind){
        case RANT_I8:    return (int8_t)p[0];
        case RANT_I16: return (int16_t)i_rant_le_r16(p);
        case RANT_I32: return (int32_t)i_rant_le_r32(p);
        case RANT_I64: return (int64_t)i_rant_le_r64(p);
        default: return 0;
    }
}
static double i_rant_schema_read_f64(uint8_t kind, const uint8_t *p){
    if (kind == RANT_F64){ uint64_t b = i_rant_le_r64(p); double d; memcpy(&d, &b, 8); return d; }
    if (kind == RANT_F32){ uint32_t b = i_rant_le_r32(p); float      x; memcpy(&x, &b, 4); return (double)x; }
    return 0.0;
}

uint64_t rant_get_uint(RantBytes msg, const RantSchema *s, const char *field){
    size_t off; const i_Field *f = i_rant_schema_read_lookup(s, msg, field, &off);
    if (!f) return 0;
    if (f->kind == RANT_ENUM) return (uint64_t)i_rant_enum_read_val(f->elem, msg.data + off);
    return i_rant_schema_read_uint(f->kind, msg.data + off);
}

int64_t rant_get_int(RantBytes msg, const RantSchema *s, const char *field){
    size_t off; const i_Field *f = i_rant_schema_read_lookup(s, msg, field, &off);
    if (!f) return 0;
    if (f->kind == RANT_ENUM) return i_rant_enum_read_val(f->elem, msg.data + off);
    return i_rant_schema_read_int(f->kind, msg.data + off);
}

double rant_get_f64(RantBytes msg, const RantSchema *s, const char *field){
    size_t off; const i_Field *f = i_rant_schema_read_lookup(s, msg, field, &off);
    return f ? i_rant_schema_read_f64(f->kind, msg.data + off) : 0.0;
}

float rant_get_f32(RantBytes msg, const RantSchema *s, const char *field){
    size_t off; const i_Field *f = i_rant_schema_read_lookup(s, msg, field, &off);
    if (!f || f->kind != RANT_F32) return 0.0f;
    { uint32_t b = i_rant_le_r32(msg.data + off); float x; memcpy(&x, &b, 4); return x; }
}

RantBytes rant_get_array(RantBytes msg, const RantSchema *s, const char *field){
    RantBytes out; size_t off; const i_Field *f = i_rant_schema_read_lookup(s, msg, field, &off);
    out.data = NULL; out.len = 0;
    if (!f) return out;
    if (f->kind == RANT_VARR){
        RantBytes fr = i_rant_schema_frame(s, msg, f->var_ord);
        if (!fr.data || !f->elem_size) return out;
        out.data = fr.data; out.len = fr.len - fr.len % f->elem_size;   /* whole elements only */
        return out;
    }
    if (f->kind != RANT_ARR) return out;
    out.data = msg.data + off; out.len = f->size;
    return out;
}

/* the live element count of an array field */
static uint32_t i_rant_array_count(const RantSchema *s, RantBytes msg, const i_Field *f){
    if (!f || !f->elem_size) return 0;
    if (f->kind == RANT_ARR) return f->count;
    if (f->kind == RANT_VARR){
        RantBytes fr = i_rant_schema_frame(s, msg, f->var_ord);
        return fr.data ? (uint32_t)(fr.len / f->elem_size) : 0;
    }
    return 0;
}

uint32_t rant_get_array_count(RantBytes msg, const RantSchema *s, const char *field){
    size_t off; const i_Field *f = i_rant_schema_read_lookup(s, msg, field, &off);
    return i_rant_array_count(s, msg, f);
}

uint32_t rant_array_count_at(RantBytes msg, const RantSchema *s, uint16_t field){
    if (!s || field >= s->nfields) return 0;
    return i_rant_array_count(s, msg, &s->fields[field]);
}

/* one slot's live string, the length clamped to the cap against a hostile message */
static RantString i_rant_schema_str_view(const uint8_t *slot, uint16_t cap){
    uint16_t len = i_rant_le_r16(slot);
    if (len > cap) len = cap;
    return rant_string((const char *)(slot + 2), len);
}

RantString rant_get_string(RantBytes msg, const RantSchema *s, const char *field){
    size_t off; const i_Field *f = i_rant_schema_read_lookup(s, msg, field, &off);
    if (!f) return rant_string(NULL, 0);
    if (f->kind == RANT_VSTR){
        RantBytes fr = i_rant_schema_frame(s, msg, f->var_ord);       /* the frame is the string */
        return rant_string((const char *)fr.data, fr.data ? fr.len : 0);
    }
    if (f->kind != RANT_STR) return rant_string(NULL, 0);
    return i_rant_schema_str_view(msg.data + off, f->str_cap);
}

RantString rant_get_string_at(RantBytes msg, const RantSchema *s, const char *field,
                              uint16_t index){
    size_t off; const i_Field *f = i_rant_schema_read_lookup(s, msg, field, &off);
    if (!f || f->elem != RANT_STR) return rant_string(NULL, 0);
    if (f->kind == RANT_VARR){
        RantBytes fr = i_rant_schema_frame(s, msg, f->var_ord);
        uint32_t esz = f->elem_size;
        if (!fr.data || !esz || ((uint64_t)index + 1u) * esz > fr.len) return rant_string(NULL, 0);
        return i_rant_schema_str_view(fr.data + (size_t)index * esz, f->str_cap);
    }
    if (f->kind != RANT_ARR || index >= f->count) return rant_string(NULL, 0);
    return i_rant_schema_str_view(msg.data + off + (size_t)index * f->elem_size, f->str_cap);
}

RantBytes rant_get_map(RantBytes msg, const RantSchema *s, const char *field){
    size_t off; const i_Field *f = i_rant_schema_read_lookup(s, msg, field, &off);
    if (!f || f->kind != RANT_MAP) return rant_bytes(NULL, 0);
    return i_rant_schema_frame(s, msg, f->var_ord);
}

RantString rant_get_enum(RantBytes msg, const RantSchema *s, const char *field){
    size_t off; const i_Field *f = i_rant_schema_read_lookup(s, msg, field, &off);
    if (!f || f->kind != RANT_ENUM) return rant_string(NULL, 0);
    return rant_enum_name_of(s, (uint16_t)(f - s->fields),
                             i_rant_enum_read_val(f->elem, msg.data + off));
}

int rant_get_value_at(RantBytes msg, const RantSchema *s, uint16_t field, uint32_t elem,
                      RantValue *out){
    const i_Field *f; const uint8_t *p; size_t off;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!s || field >= s->nfields) return 0;
    f = &s->fields[field];
    if (!i_rant_field_addr(s, msg, f, elem, &off)) return 0;
    p = msg.data + off;
    out->kind = f->kind; out->elem = f->elem; out->count = f->count; out->str_cap = f->str_cap;
    switch (f->kind){
        case RANT_U8: case RANT_U16: case RANT_U32: case RANT_U64: case RANT_BOOL:
            out->v.u = i_rant_schema_read_uint(f->kind, p); break;
        case RANT_I8: case RANT_I16: case RANT_I32: case RANT_I64:
            out->v.i = i_rant_schema_read_int(f->kind, p); break;
        case RANT_F32: case RANT_F64:
            out->v.f = i_rant_schema_read_f64(f->kind, p); break;
        case RANT_ARR: case RANT_STRUCT:
            out->bytes = rant_bytes(p, f->size); break;
        case RANT_STR: {
            RantString sv = i_rant_schema_str_view(p, f->str_cap);
            out->bytes = rant_bytes(sv.data, sv.len); break;
        }
        case RANT_VSTR: {
            RantBytes fr = i_rant_schema_frame(s, msg, f->var_ord);
            if (!fr.data) return 0;
            out->bytes = fr; break;
        }
        case RANT_VARR: {
            RantBytes fr = i_rant_schema_frame(s, msg, f->var_ord);
            uint64_t n;
            if (!fr.data || !f->elem_size) return 0;
            n = fr.len / f->elem_size;
            out->bytes = rant_bytes(fr.data, (size_t)(n * f->elem_size));
            out->count = n > 0xFFFFu ? 0xFFFFu : (uint16_t)n;   /* saturated, bytes.len rules */
            break;
        }
        case RANT_MAP: {
            RantBytes fr = i_rant_schema_frame(s, msg, f->var_ord);
            if (!fr.data) return 0;
            out->bytes = fr;
            out->count = rant_map_count(fr); break;
        }
        case RANT_ENUM:     /* v.i is the value, elem and count carry the backing and options */
            out->v.i = i_rant_enum_read_val(f->elem, p); break;
        default: return 0;
    }
    return 1;
}

int rant_get_value(RantBytes msg, const RantSchema *s, uint16_t field, RantValue *out){
    return rant_get_value_at(msg, s, field, 0, out);
}

/* writing a message */
int rant_schema_message_default(const RantSchema *s, void *buf, size_t cap){
    size_t min;
    if (!s || !buf) return 0;
    min = (size_t)s->size + 4u * s->n_var;
    if (cap < min) return 0;
    memset(buf, 0, min);            /* zeroed fixed fields plus one empty frame per variable */
    return 1;
}

static const i_Field *i_rant_schema_set_lookup(const RantSchema *s, const void *buf, size_t cap,
                                               const char *field, size_t *off){
    if (!buf) return NULL;
    return i_rant_schema_read_lookup(s, rant_bytes(buf, cap), field, off);
}

static int i_rant_schema_write_uint(const i_Field *f, uint8_t *p, uint64_t v){
    switch (f->kind){
        case RANT_U8:     p[0] = (uint8_t)v;  return 1;
        case RANT_BOOL: p[0] = v ? 1u : 0u; return 1;
        case RANT_U16: i_rant_le_w16(p, (uint16_t)v); return 1;
        case RANT_U32: i_rant_le_w32(p, (uint32_t)v); return 1;
        case RANT_U64: i_rant_le_w64(p, v); return 1;
        default: return 0;
    }
}
static int i_rant_schema_write_int(const i_Field *f, uint8_t *p, int64_t v){
    switch (f->kind){
        case RANT_I8:    p[0] = (uint8_t)v; return 1;
        case RANT_I16: i_rant_le_w16(p, (uint16_t)v); return 1;
        case RANT_I32: i_rant_le_w32(p, (uint32_t)v); return 1;
        case RANT_I64: i_rant_le_w64(p, (uint64_t)v); return 1;
        default: return 0;
    }
}
static int i_rant_schema_write_f64(const i_Field *f, uint8_t *p, double v){
    if (f->kind == RANT_F64){ uint64_t b; memcpy(&b, &v, 8); i_rant_le_w64(p, b); return 1; }
    if (f->kind == RANT_F32){ float x = (float)v; uint32_t b; memcpy(&b, &x, 4); i_rant_le_w32(p, b); return 1; }
    return 0;
}
/* one string slot: [u16 len][bytes][zeroed tail]. Refuses v.len over the cap */
static int i_rant_schema_write_str_slot(uint8_t *p, uint16_t cap, RantString v){
    if (v.len > cap || (v.len && !v.data)) return 0;             /* never silently truncate */
    i_rant_le_w16(p, (uint16_t)v.len);
    if (v.len) memcpy(p + 2, v.data, v.len);
    memset(p + 2 + v.len, 0, (size_t)cap - v.len);
    return 1;
}
/* element payload sanity shared by fixed and variable arrays: whole elements, and every
 * string slot's length prefix within its cap */
static int i_rant_schema_check_elems(const i_Field *f, RantBytes elems, uint32_t esz){
    if (!esz || elems.len % esz != 0) return 0;     /* never silently truncate */
    if (elems.len && !elems.data) return 0;
    if (f->elem == RANT_STR){
        size_t off;
        for (off = 0; off + esz <= elems.len; off += esz)
            if (i_rant_le_r16(elems.data + off) > f->str_cap) return 0;
    }
    return 1;
}
/* copy elems over the front of a fixed array and zero the rest */
static int i_rant_schema_write_array(const i_Field *f, uint8_t *p, RantBytes elems){
    if (f->kind != RANT_ARR) return 0;
    if (elems.len > f->size || !i_rant_schema_check_elems(f, elems, f->elem_size)) return 0;
    if (elems.len) memcpy(p, elems.data, elems.len);
    memset(p + elems.len, 0, f->size - elems.len);
    return 1;
}
/* a variable array's frame becomes exactly elems */
static int i_rant_schema_write_var_array(const RantSchema *s, uint8_t *buf, size_t cap,
                                         const i_Field *f, RantBytes elems){
    if (!i_rant_schema_check_elems(f, elems, f->elem_size)) return 0;
    return i_rant_schema_set_frame(s, buf, cap, f->var_ord, elems.data, elems.len);
}

int rant_set_uint(void *buf, size_t cap, const RantSchema *s, const char *field, uint64_t v){
    size_t off; const i_Field *f = i_rant_schema_set_lookup(s, buf, cap, field, &off);
    if (!f) return 0;
    if (f->kind == RANT_ENUM){ i_rant_enum_write_val(f->elem, (uint8_t *)buf + off, (int64_t)v); return 1; }
    return i_rant_schema_write_uint(f, (uint8_t *)buf + off, v);
}

int rant_set_int(void *buf, size_t cap, const RantSchema *s, const char *field, int64_t v){
    size_t off; const i_Field *f = i_rant_schema_set_lookup(s, buf, cap, field, &off);
    if (!f) return 0;
    if (f->kind == RANT_ENUM){ i_rant_enum_write_val(f->elem, (uint8_t *)buf + off, v); return 1; }
    return i_rant_schema_write_int(f, (uint8_t *)buf + off, v);
}

int rant_set_f64(void *buf, size_t cap, const RantSchema *s, const char *field, double v){
    size_t off; const i_Field *f = i_rant_schema_set_lookup(s, buf, cap, field, &off);
    return f ? i_rant_schema_write_f64(f, (uint8_t *)buf + off, v) : 0;
}

int rant_set_f32(void *buf, size_t cap, const RantSchema *s, const char *field, float v){
    size_t off; const i_Field *f = i_rant_schema_set_lookup(s, buf, cap, field, &off); uint32_t b;
    if (!f || f->kind != RANT_F32) return 0;
    memcpy(&b, &v, 4); i_rant_le_w32((uint8_t *)buf + off, b);
    return 1;
}

int rant_set_array(void *buf, size_t cap, const RantSchema *s, const char *field, RantBytes elems){
    size_t off; const i_Field *f = i_rant_schema_set_lookup(s, buf, cap, field, &off);
    if (!f) return 0;
    if (f->kind == RANT_VARR)
        return i_rant_schema_write_var_array(s, (uint8_t *)buf, cap, f, elems);
    return i_rant_schema_write_array(f, (uint8_t *)buf + off, elems);
}

int rant_set_array_count(void *buf, size_t cap, const RantSchema *s, const char *field,
                         uint32_t count){
    size_t off; const i_Field *f = i_rant_schema_set_lookup(s, buf, cap, field, &off);
    if (!f || f->kind != RANT_VARR || !f->elem_size) return 0;
    if ((uint64_t)count * f->elem_size > 0xFFFFFFFFu) return 0;
    return i_rant_schema_frame_write(s, (uint8_t *)buf, cap, f->var_ord, NULL,
                                     (size_t)count * f->elem_size);
}

int rant_set_string(void *buf, size_t cap, const RantSchema *s, const char *field, RantString v){
    size_t off; const i_Field *f = i_rant_schema_set_lookup(s, buf, cap, field, &off);
    if (!f) return 0;
    if (f->kind == RANT_VSTR){
        if (v.len && !v.data) return 0;
        return i_rant_schema_set_frame(s, (uint8_t *)buf, cap, f->var_ord, v.data, v.len);
    }
    if (f->kind != RANT_STR) return 0;
    return i_rant_schema_write_str_slot((uint8_t *)buf + off, f->str_cap, v);
}

int rant_set_string_at(void *buf, size_t cap, const RantSchema *s, const char *field,
                       uint16_t index, RantString v){
    size_t off; const i_Field *f = i_rant_schema_set_lookup(s, buf, cap, field, &off);
    if (!f || f->elem != RANT_STR) return 0;
    if (f->kind == RANT_VARR){                        /* in place, under the live count */
        RantBytes fr = i_rant_schema_frame(s, rant_bytes(buf, cap), f->var_ord);
        uint32_t esz = f->elem_size;
        if (!fr.data || !esz || ((uint64_t)index + 1u) * esz > fr.len) return 0;
        return i_rant_schema_write_str_slot((uint8_t *)fr.data + (size_t)index * esz,
                                            f->str_cap, v);
    }
    if (f->kind != RANT_ARR || index >= f->count) return 0;
    return i_rant_schema_write_str_slot((uint8_t *)buf + off + (size_t)index * f->elem_size,
                                        f->str_cap, v);
}

int rant_set_map(void *buf, size_t cap, const RantSchema *s, const char *field, RantBytes map){
    size_t off; const i_Field *f = i_rant_schema_set_lookup(s, buf, cap, field, &off);
    if (!f || f->kind != RANT_MAP) return 0;
    if (!rant_map_valid(map)) return 0;               /* malformed bytes never enter a message */
    return i_rant_schema_set_frame(s, (uint8_t *)buf, cap, f->var_ord, map.data, map.len);
}

int rant_set_enum(void *buf, size_t cap, const RantSchema *s, const char *field, const char *name){
    size_t off; const i_Field *f = i_rant_schema_set_lookup(s, buf, cap, field, &off);
    int64_t v;
    if (!f || f->kind != RANT_ENUM) return 0;
    if (!rant_enum_value_of(s, (uint16_t)(f - s->fields), name, &v)) return 0;
    i_rant_enum_write_val(f->elem, (uint8_t *)buf + off, v);
    return 1;
}

int rant_set_value_at(void *buf, size_t cap, const RantSchema *s, uint16_t field, uint32_t elem,
                      const RantValue *val){
    const i_Field *f; uint8_t *p; size_t off;
    if (!buf || !val || !s || field >= s->nfields) return 0;
    f = &s->fields[field];
    if (!i_rant_field_addr(s, rant_bytes(buf, cap), f, elem, &off)) return 0;
    p = (uint8_t *)buf + off;
    switch (f->kind){
        case RANT_U8: case RANT_U16: case RANT_U32: case RANT_U64: case RANT_BOOL:
            return i_rant_schema_write_uint(f, p, val->v.u);
        case RANT_I8: case RANT_I16: case RANT_I32: case RANT_I64:
            return i_rant_schema_write_int(f, p, val->v.i);
        case RANT_F32: case RANT_F64:
            return i_rant_schema_write_f64(f, p, val->v.f);
        case RANT_ARR:
            return i_rant_schema_write_array(f, p, val->bytes);
        case RANT_STR:
            return i_rant_schema_write_str_slot(p, f->str_cap,
                       rant_string((const char *)val->bytes.data, val->bytes.len));
        case RANT_STRUCT:     /* raw bytes, a short source zero fills the tail */
            if (val->bytes.len > f->size || (val->bytes.len && !val->bytes.data)) return 0;
            if (val->bytes.len) memcpy(p, val->bytes.data, val->bytes.len);
            memset(p + val->bytes.len, 0, f->size - val->bytes.len);
            return 1;
        case RANT_VSTR:
            if (val->bytes.len && !val->bytes.data) return 0;
            return i_rant_schema_set_frame(s, (uint8_t *)buf, cap, f->var_ord,
                                           val->bytes.data, val->bytes.len);
        case RANT_VARR:
            return i_rant_schema_write_var_array(s, (uint8_t *)buf, cap, f, val->bytes);
        case RANT_MAP:
            if (!rant_map_valid(val->bytes)) return 0;
            return i_rant_schema_set_frame(s, (uint8_t *)buf, cap, f->var_ord,
                                           val->bytes.data, val->bytes.len);
        case RANT_ENUM:
            i_rant_enum_write_val(f->elem, p, val->v.i);
            return 1;
        default: return 0;
    }
}

int rant_set_value(void *buf, size_t cap, const RantSchema *s, uint16_t field, const RantValue *val){
    return rant_set_value_at(buf, cap, s, field, 0, val);
}
/* The map. Readers walk hostile bytes: every read is bounds checked, the kind vocabulary
 * is closed and nesting is depth capped. The body grammar is in spec/schema.md. */

/* Reads (out set) or skips (out NULL) one value at r->pos. 1, or 0 with r->fail. */
static int i_rant_map_value(i_Rd *r, uint16_t depth, RantValue *out){
    uint8_t k = i_rant_rd_u8(r);
    uint32_t sz;
    if (r->fail) return 0;
    if (out){ memset(out, 0, sizeof *out); out->kind = k; }
    sz = rant_schema_scalar_size((RantSchemaTypeKind)k);
    if (sz){
        const uint8_t *p = r->w + r->pos;
        i_rant_rd_skip(r, sz);
        if (r->fail) return 0;
        if (out) switch (k){
            case RANT_U8: case RANT_U16: case RANT_U32: case RANT_U64: case RANT_BOOL:
                out->v.u = i_rant_schema_read_uint(k, p); break;
            case RANT_I8: case RANT_I16: case RANT_I32: case RANT_I64:
                out->v.i = i_rant_schema_read_int(k, p); break;
            default:
                out->v.f = i_rant_schema_read_f64(k, p); break;
        }
        return 1;
    }
    if (k == RANT_VSTR){
        uint16_t len = i_rant_rd_u16(r);
        const uint8_t *p = r->w + r->pos;
        i_rant_rd_skip(r, len);
        if (r->fail) return 0;
        if (out) out->bytes = rant_bytes(p, len);
        return 1;
    }
    if (k == RANT_MAP || k == RANT_VARR){
        size_t start = r->pos; uint16_t n, i;
        if (depth + 1u >= RANT_SCHEMA_MAX_DEPTH){ r->fail = 1; return 0; }
        n = i_rant_rd_u16(r);
        for (i = 0; i < n && !r->fail; i++){
            if (k == RANT_MAP){
                uint8_t kl = i_rant_rd_u8(r);
                i_rant_rd_skip(r, kl);
            }
            if (!i_rant_map_value(r, (uint16_t)(depth + 1), NULL)) return 0;
        }
        if (r->fail) return 0;
        if (out){ out->bytes = rant_bytes(r->w + start, r->pos - start); out->count = n; }
        return 1;
    }
    r->fail = 1;                                        /* unknown kind: reject */
    return 0;
}

uint16_t rant_map_count(RantBytes map){
    return (map.data && map.len >= 2) ? i_rant_le_r16(map.data) : 0;
}
uint16_t rant_map_array_count(RantBytes arr){ return rant_map_count(arr); }

int rant_map_at(RantBytes map, uint16_t index, RantString *key, RantValue *out){
    i_Rd r; uint16_t n, i;
    if (!map.data || map.len < 2) return 0;
    r.w = map.data; r.n = map.len; r.pos = 0; r.fail = 0;
    n = i_rant_rd_u16(&r);
    if (index >= n) return 0;
    for (i = 0; i <= index; i++){
        uint8_t kl = i_rant_rd_u8(&r);
        const char *kp = (const char *)(r.w + r.pos);
        i_rant_rd_skip(&r, kl);
        if (r.fail) return 0;
        if (i == index){
            if (!i_rant_map_value(&r, 0, out)) return 0;
            if (key) *key = rant_string(kp, kl);
            return 1;
        }
        if (!i_rant_map_value(&r, 0, NULL)) return 0;
    }
    return 0;
}

int rant_map_get(RantBytes map, const char *key, RantValue *out){
    i_Rd r; uint16_t n, i; size_t want;
    if (!map.data || map.len < 2 || !key) return 0;
    want = strlen(key);
    r.w = map.data; r.n = map.len; r.pos = 0; r.fail = 0;
    n = i_rant_rd_u16(&r);
    for (i = 0; i < n; i++){
        uint8_t kl = i_rant_rd_u8(&r);
        const char *kp = (const char *)(r.w + r.pos);
        i_rant_rd_skip(&r, kl);
        if (r.fail) return 0;
        if (kl == want && (want == 0 || memcmp(kp, key, want) == 0))
            return i_rant_map_value(&r, 0, out);
        if (!i_rant_map_value(&r, 0, NULL)) return 0;
    }
    return 0;
}

int rant_map_array_at(RantBytes arr, uint16_t index, RantValue *out){
    i_Rd r; uint16_t n, i;
    if (!arr.data || arr.len < 2) return 0;
    r.w = arr.data; r.n = arr.len; r.pos = 0; r.fail = 0;
    n = i_rant_rd_u16(&r);
    if (index >= n) return 0;
    for (i = 0; i < index; i++)
        if (!i_rant_map_value(&r, 0, NULL)) return 0;
    return i_rant_map_value(&r, 0, out);
}

int rant_map_valid(RantBytes map){
    i_Rd r; uint16_t n, i;
    if (map.len == 0) return 1;                         /* an empty body is an empty map */
    if (!map.data || map.len < 2) return 0;
    r.w = map.data; r.n = map.len; r.pos = 0; r.fail = 0;
    n = i_rant_rd_u16(&r);
    for (i = 0; i < n; i++){
        uint8_t kl = i_rant_rd_u8(&r);
        i_rant_rd_skip(&r, kl);
        if (r.fail || !i_rant_map_value(&r, 0, NULL)) return 0;
    }
    return r.pos == r.n;                                /* no trailing garbage */
}

/* the map writer */
RantMapWriter rant_map_begin(void *buf, size_t cap){
    RantMapWriter w;
    memset(&w, 0, sizeof w);
    w.buf = (uint8_t *)buf; w.cap = cap;
    if (!buf || cap < 2){ w.err = -1; return w; }
    w.buf[0] = 0; w.buf[1] = 0;                         /* the root map's count */
    w.count_pos[0] = 0; w.len = 2;
    w.depth = 1;
    return w;
}

/* room for extra more bytes. The buffer is the caller's, so no growth */
static int i_rant_map_room(RantMapWriter *w, size_t extra){
    if (w->err) return 0;
    if (w->len + extra > w->cap){ w->err = -1; return 0; }
    return 1;
}
/* start an entry: a key in a map, none in an array. Bumps the open count */
static int i_rant_map_entry(RantMapWriter *w, const char *key){
    size_t kl = 0;
    if (w->err) return 0;
    if (w->depth == 0){ w->err = -3; return 0; }        /* a finished writer */
    if (w->is_arr[w->depth - 1]){
        if (key){ w->err = -3; return 0; }              /* array elements carry no key */
    } else {
        if (!key){ w->err = -3; return 0; }
        kl = strlen(key);
        if (kl > 255){ w->err = -4; return 0; }
    }
    if (w->count[w->depth - 1] == 0xFFFFu){ w->err = -5; return 0; }
    if (key){
        if (!i_rant_map_room(w, 1 + kl)) return 0;
        w->buf[w->len++] = (uint8_t)kl;
        if (kl) memcpy(w->buf + w->len, key, kl);
        w->len += kl;
    }
    w->count[w->depth - 1]++;
    return 1;
}
static int i_rant_map_put_scalar(RantMapWriter *w, const char *key, uint8_t kind,
                                 const uint8_t *le, uint32_t sz){
    if (!w || !i_rant_map_entry(w, key) || !i_rant_map_room(w, 1u + sz)) return 0;
    w->buf[w->len++] = kind;
    memcpy(w->buf + w->len, le, sz);
    w->len += sz;
    return 1;
}

int rant_map_put_uint(RantMapWriter *w, const char *key, uint64_t v){
    uint8_t le[8];                                       /* the smallest kind that fits */
    uint8_t k = v <= 0xFFu ? RANT_U8 : v <= 0xFFFFu ? RANT_U16
              : v <= 0xFFFFFFFFu ? RANT_U32 : RANT_U64;
    i_rant_le_w64(le, v);
    return i_rant_map_put_scalar(w, key, k, le, rant_schema_scalar_size((RantSchemaTypeKind)k));
}

int rant_map_put_int(RantMapWriter *w, const char *key, int64_t v){
    uint8_t le[8];
    uint8_t k = (v >= -128 && v <= 127) ? RANT_I8
              : (v >= -32768 && v <= 32767) ? RANT_I16
              : (v >= -2147483647 - 1 && v <= 2147483647) ? RANT_I32 : RANT_I64;
    i_rant_le_w64(le, (uint64_t)v);                        /* two's complement LE: the low bytes */
    return i_rant_map_put_scalar(w, key, k, le, rant_schema_scalar_size((RantSchemaTypeKind)k));
}

int rant_map_put_f64(RantMapWriter *w, const char *key, double v){
    uint8_t le[8]; uint64_t b;
    memcpy(&b, &v, 8); i_rant_le_w64(le, b);
    return i_rant_map_put_scalar(w, key, (uint8_t)RANT_F64, le, 8);
}

int rant_map_put_f32(RantMapWriter *w, const char *key, float v){
    uint8_t le[4]; uint32_t b;
    memcpy(&b, &v, 4); i_rant_le_w32(le, b);
    return i_rant_map_put_scalar(w, key, (uint8_t)RANT_F32, le, 4);
}

int rant_map_put_bool(RantMapWriter *w, const char *key, int v){
    uint8_t b = v ? 1u : 0u;
    return i_rant_map_put_scalar(w, key, (uint8_t)RANT_BOOL, &b, 1);
}

int rant_map_put_string(RantMapWriter *w, const char *key, RantString v){
    if (!w) return 0;
    if (v.len > 0xFFFFu || (v.len && !v.data)){ w->err = -4; return 0; }
    if (!i_rant_map_entry(w, key) || !i_rant_map_room(w, 3u + v.len)) return 0;
    w->buf[w->len++] = (uint8_t)RANT_VSTR;
    i_rant_le_w16(w->buf + w->len, (uint16_t)v.len); w->len += 2;
    if (v.len) memcpy(w->buf + w->len, v.data, v.len);
    w->len += v.len;
    return 1;
}

static int i_rant_map_open(RantMapWriter *w, const char *key, uint8_t kind, uint8_t is_arr){
    if (!w || !i_rant_map_entry(w, key)) return 0;
    if (w->depth >= RANT_SCHEMA_MAX_DEPTH){ w->err = -2; return 0; }
    if (!i_rant_map_room(w, 3)) return 0;
    w->buf[w->len++] = kind;
    w->count_pos[w->depth] = w->len;                    /* the nested body's count */
    w->buf[w->len++] = 0; w->buf[w->len++] = 0;
    w->count[w->depth] = 0; w->is_arr[w->depth] = is_arr;
    w->depth++;
    return 1;
}
int rant_map_open_map(RantMapWriter *w, const char *key){
    return i_rant_map_open(w, key, (uint8_t)RANT_MAP, 0);
}
int rant_map_open_array(RantMapWriter *w, const char *key){
    return i_rant_map_open(w, key, (uint8_t)RANT_VARR, 1);
}
int rant_map_close(RantMapWriter *w){
    if (!w || w->err) return 0;
    if (w->depth <= 1){ w->err = -3; return 0; }        /* the root closes in finish */
    w->depth--;
    i_rant_le_w16(w->buf + w->count_pos[w->depth], w->count[w->depth]);
    return 1;
}

uint32_t rant_map_finish(RantMapWriter *w){
    if (!w || w->err || w->depth != 1) return 0;        /* else an unbalanced open and close */
    i_rant_le_w16(w->buf + w->count_pos[0], w->count[0]);
    w->depth = 0;
    return (uint32_t)w->len;
}
/* The schema DSL. The grammar is in spec/schema.md. A type word that is not a built in
 * resolves against the definitions, the environment and the standard library. */
#ifndef RANT_NO_STDTYPES
const char *i_rant_std_lookup(const char *name);     /* serialize/stdtypes.c */
#else
#define i_rant_std_lookup(name) ((const char *)0)
#endif

typedef struct { const char *p; const char *err; } i_RantDsl;

/* The definition registry: one arena holding each named type's name and compiled type,
 * plus an index. A definition compiles in its own scratch builder before it is appended. */
#define I_RANT_DSL_MAX_DEFS 64u
typedef struct {
    RantSchemaBuilder arena;
    struct { uint32_t noff, toff, tlen; uint8_t nlen; } e[I_RANT_DSL_MAX_DEFS];
    uint16_t n;
    uint16_t rec;                                  /* standard type expansion depth */
    const RantSchema *const *env; size_t n_env;
} i_RantDefs;

static void i_rant_dsl_ws(i_RantDsl *d){
    for (;;){
        while (*d->p==' ' || *d->p=='\t' || *d->p=='\r' || *d->p=='\n') d->p++;
        if (d->p[0]=='-' && d->p[1]=='-'){ while (*d->p && *d->p!='\n') d->p++; continue; }
        return;
    }
}
static void i_rant_dsl_fail(i_RantDsl *d, const char *at){ if (!d->err) d->err = at; }
/* an identifier into out[256], NUL terminated. 0 and err when missing or overlong */
static int i_rant_dsl_ident(i_RantDsl *d, char out[256]){
    const char *q = d->p; size_t n;
    if (!((*q>='A'&&*q<='Z') || (*q>='a'&&*q<='z') || *q=='_')){ i_rant_dsl_fail(d, q); return 0; }
    while ((*q>='A'&&*q<='Z') || (*q>='a'&&*q<='z') || (*q>='0'&&*q<='9') || *q=='_') q++;
    n = (size_t)(q - d->p);
    if (n > 255){ i_rant_dsl_fail(d, d->p); return 0; }
    memcpy(out, d->p, n); out[n] = '\0';
    d->p = q;
    return 1;
}
static int i_rant_dsl_expect(i_RantDsl *d, char c){
    if (*d->p == c){ d->p++; return 1; }
    i_rant_dsl_fail(d, d->p);
    return 0;
}
/* a decimal array count, 1 to 65535 */
static int i_rant_dsl_count(i_RantDsl *d, uint16_t *out){
    const char *at = d->p; uint32_t v = 0;
    while (*d->p>='0' && *d->p<='9'){
        v = v*10u + (uint32_t)(*d->p - '0');
        if (v > 0xFFFFu){ i_rant_dsl_fail(d, at); return 0; }
        d->p++;
    }
    if (d->p == at || v == 0){ i_rant_dsl_fail(d, at); return 0; }
    *out = (uint16_t)v;
    return 1;
}
static int i_rant_dsl_kind(const char *s, RantSchemaTypeKind *k){
    static const struct { const char *word; uint8_t kind; } table[] = {
        {"u8",RANT_U8},{"u16",RANT_U16},{"u32",RANT_U32},{"u64",RANT_U64},
        {"i8",RANT_I8},{"i16",RANT_I16},{"i32",RANT_I32},{"i64",RANT_I64},
        {"f32",RANT_F32},{"f64",RANT_F64},{"bool",RANT_BOOL} };
    size_t i;
    for (i = 0; i < sizeof table / sizeof table[0]; i++)
        if (strcmp(s, table[i].word) == 0){ *k = (RantSchemaTypeKind)table[i].kind; return 1; }
    return 0;
}
/* a signed decimal enum value fitting int64 */
static int i_rant_dsl_enum_value(i_RantDsl *d, int64_t *out){
    const char *at = d->p; int neg = 0, any = 0; uint64_t v = 0, lim;
    if (*d->p == '-'){ neg = 1; d->p++; }
    lim = neg ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    while (*d->p >= '0' && *d->p <= '9'){
        v = v * 10u + (uint64_t)(*d->p - '0');
        if (v > lim){ i_rant_dsl_fail(d, at); return 0; }
        d->p++; any = 1;
    }
    if (!any){ i_rant_dsl_fail(d, at); return 0; }
    *out = neg ? -(int64_t)v : (int64_t)v;
    return 1;
}
/* enum<uN> { Name [= value], ... } after the word enum was read. Streams the options. */
static void i_rant_dsl_enum(i_RantDsl *d, RantSchemaBuilder *b, const char *name){
    char wname[256]; RantSchemaTypeKind backing; size_t count_pos; uint16_t count = 0; int64_t next = 0;
    i_rant_dsl_ws(d);
    if (!i_rant_dsl_expect(d, '<')) return;
    i_rant_dsl_ws(d);
    if (!i_rant_dsl_ident(d, wname)) return;
    if (!i_rant_dsl_kind(wname, &backing) || !i_rant_enum_backing_ok((uint8_t)backing)){
        i_rant_dsl_fail(d, d->p); return;                  /* the backing must be an integer kind */
    }
    i_rant_dsl_ws(d);
    if (!i_rant_dsl_expect(d, '>')) return;
    i_rant_dsl_ws(d);
    if (!i_rant_dsl_expect(d, '{')) return;
    count_pos = i_rant_schema_field_enum_open(b, name, backing);
    for (;;){
        char vname[256]; int64_t v;
        i_rant_dsl_ws(d);
        if (*d->p == '}' || *d->p == '\0' || d->err || b->err) break;
        if (!i_rant_dsl_ident(d, vname)) return;
        i_rant_dsl_ws(d);
        if (*d->p == '='){   /* an explicit value, else auto increment */
            d->p++; i_rant_dsl_ws(d);
            if (!i_rant_dsl_enum_value(d, &v)) return;
        } else v = next;
        if (!i_rant_enum_val_fits((uint8_t)backing, v)){ i_rant_dsl_fail(d, d->p); return; }
        i_rant_schema_field_enum_add(b, backing, v, vname, strlen(vname));
        count++; next = v + 1;
        i_rant_dsl_ws(d);
        if (*d->p == ',') d->p++;                        /* an optional separator */
    }
    if (!i_rant_dsl_expect(d, '}')) return;
    i_rant_schema_field_enum_finish(b, count_pos, count);
}

static void i_rant_dsl_field_type(i_RantDsl *d, RantSchemaBuilder *b, i_RantDefs *defs,
                                  const char *name);
static void i_rant_dsl_fields(i_RantDsl *d, RantSchemaBuilder *b, i_RantDefs *defs);

/* records a named type: its name bytes then its compiled type, both in the arena */
static int i_rant_defs_add_bytes(i_RantDefs *defs, const char *name, size_t nlen,
                                 const uint8_t *type, size_t tlen){
    uint32_t noff, toff;
    if (defs->n >= I_RANT_DSL_MAX_DEFS || nlen == 0 || nlen > 255 || tlen == 0) return 0;
    noff = (uint32_t)defs->arena.len;
    i_rant_schema_builder_put_raw(&defs->arena, name, nlen);
    toff = (uint32_t)defs->arena.len;
    i_rant_schema_builder_put_raw(&defs->arena, type, tlen);
    if (defs->arena.err) return 0;
    defs->e[defs->n].noff = noff; defs->e[defs->n].nlen = (uint8_t)nlen;
    defs->e[defs->n].toff = toff; defs->e[defs->n].tlen = (uint32_t)tlen;
    defs->n++;
    return 1;
}

static int i_rant_defs_find(i_RantDefs *defs, const char *name, uint32_t *toff, uint32_t *tlen){
    size_t nlen = strlen(name); uint16_t i;
    for (i = 0; i < defs->n; i++)
        if (defs->e[i].nlen == nlen &&
            memcmp(defs->arena.buf + defs->e[i].noff, name, nlen) == 0){
            *toff = defs->e[i].toff; *tlen = defs->e[i].tlen;
            return 1;
        }
    return 0;
}

/* compiles one type spelling, a standard library entry or any type text, as a definition */
static int i_rant_defs_add_text(i_RantDefs *defs, const char *name, const char *text){
    RantSchemaBuilder sb; i_RantDsl sd; int ok = 0;
    if (defs->rec >= 8u) return 0;                       /* the roster is acyclic, but be sure */
    defs->rec++;
    sb = i_rant_schema_begin_raw(defs->arena.alloc, defs->arena.user);
    sd.p = text; sd.err = NULL;
    i_rant_dsl_field_type(&sd, &sb, defs, "");
    i_rant_dsl_ws(&sd);
    if (*sd.p) i_rant_dsl_fail(&sd, sd.p);
    if (!sd.err && !sb.err && sb.len)
        ok = i_rant_defs_add_bytes(defs, name, strlen(name), sb.buf, sb.len);
    if (sb.buf) sb.alloc(sb.user, sb.buf, 0);
    defs->rec--;
    return ok;
}

/* Resolves a type name to its encoding in the arena: the text's own definitions first,
 * then the environment schemas, then the standard library. */
static int i_rant_dsl_ref(i_RantDefs *defs, const char *name, uint32_t *toff, uint32_t *tlen){
    size_t nlen = strlen(name), i;
    const char *std;
    if (i_rant_defs_find(defs, name, toff, tlen)) return 1;
    for (i = 0; i < defs->n_env; i++){
        const RantSchema *t = defs->env ? defs->env[i] : NULL;
        const uint8_t *tb; size_t tl;
        if (!t || t->name.len != nlen || memcmp(t->name.data, name, nlen) != 0) continue;
        if (!i_rant_schema_root_type(t, &tb, &tl)) continue;
        if (!i_rant_defs_add_bytes(defs, name, nlen, tb, tl)) return 0;
        return i_rant_defs_find(defs, name, toff, tlen);
    }
    std = i_rant_std_lookup(name);
    if (std && i_rant_defs_add_text(defs, name, std))
        return i_rant_defs_find(defs, name, toff, tlen);
    return 0;
}

/* turns the type just written at b->buf[head..len) into an array of itself by splicing
 * the array head in front of it, since the count follows the body in the text */
static void i_rant_dsl_splice_array(RantSchemaBuilder *b, size_t head, uint16_t count,
                                    int variable){
    size_t extra = variable ? 1u : 3u, tail;
    if (b->err) return;
    if (!i_rant_schema_builder_reserve(b, extra)) return;
    tail = b->len - head;
    memmove(b->buf + head + extra, b->buf + head, tail);
    if (variable){
        b->buf[head] = (uint8_t)RANT_VARR;
    } else {
        b->buf[head] = (uint8_t)RANT_ARR;
        i_rant_le_w16(b->buf + head + 1u, count);
    }
    b->len += extra;
}

/* an optional [N] or [] suffix. 1 if one was read, variable for the [] form */
static int i_rant_dsl_suffix(i_RantDsl *d, uint16_t *count, int *variable){
    *count = 0; *variable = 0;
    i_rant_dsl_ws(d);
    if (*d->p != '[') return 0;
    d->p++;
    i_rant_dsl_ws(d);
    if (*d->p == ']'){ d->p++; *variable = 1; return 1; }
    if (!i_rant_dsl_count(d, count)) return 0;
    i_rant_dsl_ws(d);
    if (!i_rant_dsl_expect(d, ']')) return 0;
    return 1;
}

/* Name, Name[N] or Name[]: a reference to an already resolved named type */
static void i_rant_dsl_emit_ref(i_RantDsl *d, RantSchemaBuilder *b, i_RantDefs *defs,
                                const char *name, const char *tname,
                                uint32_t toff, uint32_t tlen){
    uint16_t cnt; int variable, arr;
    size_t nlen = strlen(tname);
    arr = i_rant_dsl_suffix(d, &cnt, &variable);
    if (d->err) return;
    if (arr && variable && !i_rant_schema_builder_var_ok(b)) return;
    i_rant_schema_builder_count(b);
    i_rant_schema_builder_put_name(b, name);
    if (arr){
        if (variable) i_rant_schema_builder_put(b, (uint8_t)RANT_VARR);
        else { i_rant_schema_builder_put(b, (uint8_t)RANT_ARR); i_rant_schema_builder_put_u16(b, cnt); }
    }
    i_rant_schema_builder_put(b, (uint8_t)RANT_NAMED);
    i_rant_schema_builder_put(b, (uint8_t)nlen);
    i_rant_schema_builder_put_raw(b, tname, nlen);
    i_rant_schema_builder_put_raw(b, defs->arena.buf + toff, tlen);
}

/* One type whose leading word is already in tname, with at pointing at it for errors:
 * whatever follows plus the field it defines. A bare root passes name "". */
static void i_rant_dsl_word_type(i_RantDsl *d, RantSchemaBuilder *b, i_RantDefs *defs,
                                 const char *name, const char *tname, const char *at){
    RantSchemaTypeKind k = RANT_U8;
    int is_str = 0, has_cap = 0; uint16_t str_cap = 0;
    uint16_t cnt; int variable;
    if (strcmp(tname, "map") == 0){
        rant_schema_field_map(b, name);
        return;
    }
    if (strcmp(tname, "enum") == 0){
        i_rant_dsl_enum(d, b, name);
        return;
    }
    if (strcmp(tname, "string") == 0){                   /* string or string<cap> */
        is_str = 1;
        i_rant_dsl_ws(d);
        if (*d->p == '<'){
            d->p++; has_cap = 1;
            i_rant_dsl_ws(d);
            if (!i_rant_dsl_count(d, &str_cap)) return;
            i_rant_dsl_ws(d);
            if (!i_rant_dsl_expect(d, '>')) return;
        }
    } else if (!i_rant_dsl_kind(tname, &k)){               /* a named type */
        uint32_t toff, tlen;
        if (!i_rant_dsl_ref(defs, tname, &toff, &tlen)){ i_rant_dsl_fail(d, at); return; }
        i_rant_dsl_emit_ref(d, b, defs, name, tname, toff, tlen);
        return;
    }
    if (i_rant_dsl_suffix(d, &cnt, &variable)){
        if (d->err) return;
        if (is_str && !has_cap){ i_rant_dsl_fail(d, at); return; }    /* string[] is ragged */
        if (variable){
            if (is_str) rant_schema_field_var_string_array(b, name, str_cap);
            else        rant_schema_field_var_array(b, name, k);
        } else {
            if (is_str) rant_schema_field_string_array(b, name, str_cap, cnt);
            else        rant_schema_field_array(b, name, k, cnt);
        }
    } else if (d->err){
        return;
    } else if (is_str){
        if (has_cap) rant_schema_field_string(b, name, str_cap);
        else         rant_schema_field_var_string(b, name);
    } else {
        rant_schema_field(b, name, k);
    }
}

static void i_rant_dsl_field_type(i_RantDsl *d, RantSchemaBuilder *b, i_RantDefs *defs,
                                  const char *name){
    char tname[256]; const char *at;
    i_rant_dsl_ws(d);
    if (*d->p == '{'){                                   /* a struct, or an array of them */
        size_t head; uint16_t cnt; int variable;
        d->p++;
        i_rant_schema_builder_count(b);
        i_rant_schema_builder_put_name(b, name);
        head = b->len;
        i_rant_schema_builder_open_struct(b);
        i_rant_dsl_fields(d, b, defs);
        if (!i_rant_dsl_expect(d, '}')) return;
        rant_schema_end_struct(b);
        if (i_rant_dsl_suffix(d, &cnt, &variable) && !d->err)
            i_rant_dsl_splice_array(b, head, cnt, variable);
        return;
    }
    at = d->p;
    if (!i_rant_dsl_ident(d, tname)) return;
    i_rant_dsl_word_type(d, b, defs, name, tname, at);
}

/* the fields of one struct body, up to and not consuming the closing brace */
static void i_rant_dsl_fields(i_RantDsl *d, RantSchemaBuilder *b, i_RantDefs *defs){
    char name[256];
    for (;;){
        i_rant_dsl_ws(d);
        if (*d->p == '}' || *d->p == '\0' || d->err || b->err) return;
        if (!i_rant_dsl_ident(d, name)) return;
        i_rant_dsl_ws(d);
        if (!i_rant_dsl_expect(d, ':')) return;
        i_rant_dsl_field_type(d, b, defs, name);
        if (d->err) return;
        i_rant_dsl_ws(d);
        if (*d->p == ',') d->p++;                        /* an optional separator */
    }
}

/* Name = type: compiles the body on its own, then keeps it, or verifies it matches an
 * existing or reserved definition of the same name exactly. */
static void i_rant_dsl_def(i_RantDsl *d, i_RantDefs *defs, const char *name){
    RantSchemaBuilder sb; uint32_t toff = 0, tlen = 0; int ok = 0, exists;
    exists = i_rant_dsl_ref(defs, name, &toff, &tlen);
    sb = i_rant_schema_begin_raw(defs->arena.alloc, defs->arena.user);
    i_rant_dsl_field_type(d, &sb, defs, "");
    if (!d->err && !sb.err && sb.len){
        if (exists)                                      /* redefining is fine if identical */
            ok = (tlen == sb.len && memcmp(defs->arena.buf + toff, sb.buf, sb.len) == 0);
        else
            ok = i_rant_defs_add_bytes(defs, name, strlen(name), sb.buf, sb.len);
    }
    if (sb.buf) sb.alloc(sb.user, sb.buf, 0);
    if (!ok) i_rant_dsl_fail(d, d->p);
}

RantSchema *rant_schema_compile_env(RantAllocFn alloc, void *user, const char *text,
                                    const RantSchema *const *env, size_t n_env,
                                    const char **err){
    i_RantDsl d; i_RantDefs defs; RantSchemaBuilder root;
    RantSchema *s = NULL;
    char rootbuf[256];
    const char *rname = NULL; size_t rnlen = 0;
    const uint8_t *rtype = NULL; size_t rtlen = 0;
    int have_root = 0, is_struct = 0;

    if (err) *err = NULL;
    if (!alloc || !text) return NULL;
    memset(&defs, 0, sizeof defs);
    defs.env = env; defs.n_env = n_env;
    defs.arena = i_rant_schema_begin_raw(alloc, user);
    root = i_rant_schema_begin_raw(alloc, user);
    d.p = text; d.err = NULL;
    if (defs.arena.err || root.err) i_rant_dsl_fail(&d, text);

    while (!d.err){
        const char *at;
        i_rant_dsl_ws(&d);
        if (!*d.p) break;
        at = d.p;
        if (!i_rant_dsl_ident(&d, rootbuf)) break;
        i_rant_dsl_ws(&d);
        if (*d.p == '='){                                /* a definition */
            d.p++;
            i_rant_dsl_def(&d, &defs, rootbuf);
            continue;
        }
        if (*d.p == '{'){                                /* Name { fields }: a struct root */
            d.p++;
            i_rant_schema_builder_open_struct(&root);
            i_rant_dsl_fields(&d, &root, &defs);
            if (!i_rant_dsl_expect(&d, '}')) break;
            rant_schema_end_struct(&root);
            rnlen = strlen(rootbuf); rname = rootbuf;
            is_struct = 1;
        } else {                                         /* a bare type, maybe a reference */
            d.p = at;
            i_rant_dsl_field_type(&d, &root, &defs, "");
        }
        have_root = 1;
        i_rant_dsl_ws(&d);
        if (*d.p) i_rant_dsl_fail(&d, d.p);                /* the root ends the text */
        break;
    }
    if (!d.err && root.err) i_rant_dsl_fail(&d, d.p);
    if (!d.err && defs.arena.err) i_rant_dsl_fail(&d, d.p);

    if (!d.err){
        if (have_root){
            rtype = root.buf; rtlen = root.len;
            if (!is_struct && rtlen >= 2 && rtype[0] == RANT_NAMED){
                size_t nl = rtype[1];                    /* a bare reference is an alias root */
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
        if (!rtype || !rtlen) i_rant_dsl_fail(&d, d.p);
    }
    /* the reservation covers definitions and references, never a plain Name { ... } root,
     * since nothing can reference a root. See spec/schema.md. */
    if (!d.err){
        size_t wlen = 2u + rnlen + rtlen;
        uint8_t *w = (uint8_t *)alloc(user, NULL, wlen);
        if (w){
            w[0] = (uint8_t)RANT_SCHEMA_WIRE_VERSION;
            w[1] = (uint8_t)rnlen;
            if (rnlen) memcpy(w + 2, rname, rnlen);
            memcpy(w + 2 + rnlen, rtype, rtlen);
            s = rant_schema_parse(w, wlen, alloc, user);
            alloc(user, w, 0);
        }
        if (!s) i_rant_dsl_fail(&d, d.p);
    }
    if (root.buf) alloc(user, root.buf, 0);
    if (defs.arena.buf) alloc(user, defs.arena.buf, 0);
    if (!s && err) *err = d.err ? d.err : d.p;
    return s;
}

RantSchema *rant_schema_compile(RantAllocFn alloc, void *user, const char *text, const char **err){
    return rant_schema_compile_env(alloc, user, text, NULL, 0, err);
}
