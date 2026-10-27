/* The wire format and the layout rules are in spec/schema.md. */
#include "internal.h"

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
int i_rant_enum_backing_ok(uint8_t backing){ return backing <= (uint8_t)RANT_I64; }
/* widened to int64, sign extended for a signed kind */
int64_t i_rant_enum_read_val(uint8_t backing, const uint8_t *p){
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
void i_rant_enum_write_val(uint8_t backing, uint8_t *p, int64_t v){
    uint64_t u = (uint64_t)v; uint32_t bs = rant_schema_scalar_size((RantSchemaTypeKind)backing), i;
    for (i = 0; i < bs; i++) p[i] = (uint8_t)(u >> (8 * i));
}
/* a u64 above INT64_MAX is not expressible */
int i_rant_enum_val_fits(uint8_t backing, int64_t v){
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

/* Fails the read with the first rule broken. Returns 0 for the caller to return. */
static uint32_t i_rant_rd_refuse(i_Rd *r, const char *why){
    if (!r->fail) r->why = why;
    r->fail = 1;
    return 0;
}

/* The default payload of an array: [u32 count][u32 end]*count, then each element's default
 * body, when its element is variable, and nothing when the element is fixed. */
static uint64_t i_rant_arr_dmin(uint8_t kind, uint16_t count, uint8_t elem_var, uint64_t elem_body){
    if (!elem_var || kind == RANT_VARR) return elem_var ? 4u : 0u;
    return 4u + (4u + elem_body) * (uint64_t)count;
}

/* What a type at r->pos occupies: fixed bytes inline, the default frames it adds to its
 * record, its own default body when variable, and whether it is variable at all. */
typedef struct { uint64_t size, frames, body; uint8_t var; } i_TypeLen;

/* Measures the type at r->pos into *out, advancing past it, and counts every row it
 * flattens to into *rows. Fails on any rule break. */
static void i_rant_rd_type(i_Rd *r, uint32_t *rows, uint16_t depth, i_TypeLen *out){
    uint8_t k = i_rant_rd_u8(r);
    memset(out, 0, sizeof *out);
    if (r->fail) return;
    switch (k){
        case RANT_U8: case RANT_I8: case RANT_BOOL: out->size = 1; return;
        case RANT_U16: case RANT_I16:               out->size = 2; return;
        case RANT_U32: case RANT_I32: case RANT_F32: out->size = 4; return;
        case RANT_U64: case RANT_I64: case RANT_F64: out->size = 8; return;
        case RANT_STR:
            out->size = 2u + (uint32_t)i_rant_rd_u16(r);          /* [u16 len][cap bytes] */
            return;
        case RANT_ARR: case RANT_VARR: {
            uint16_t count = 0; i_TypeLen e;
            if (depth >= RANT_SCHEMA_MAX_DEPTH){ i_rant_rd_refuse(r, "arrays nest too deep"); return; }
            if (k == RANT_ARR) count = i_rant_rd_u16(r);
            if (r->fail) return;
            if (i_rant_peek_kind(r) == 0xFFu){ r->fail = 1; return; }
            if (rows) (*rows)++;                                   /* the element row */
            i_rant_rd_type(r, rows, (uint16_t)(depth + 1), &e);
            if (r->fail) return;
            if (!e.var && e.size == 0){ i_rant_rd_refuse(r, "an array element cannot be empty"); return; }
            if (k == RANT_ARR && !e.var){
                out->size = (uint64_t)count * e.size;
                if (out->size > 0xFFFFFFFFu) i_rant_rd_refuse(r, "an array is too large");
                return;
            }
            out->var = 1;
            out->body = i_rant_arr_dmin(k, count, e.var, e.body);
            out->frames = 4u + out->body;
            if (out->frames > 0xFFFFFFFFu) i_rant_rd_refuse(r, "an array's default is too large");
            return;
        }
        case RANT_ENUM: {
            uint8_t backing; uint16_t n; uint32_t bs, i;
            backing = i_rant_rd_u8(r);
            n = i_rant_rd_u16(r);
            bs = rant_schema_scalar_size((RantSchemaTypeKind)backing);
            if (r->fail) return;
            if (bs == 0 || !i_rant_enum_backing_ok(backing)){
                i_rant_rd_refuse(r, "an enum is backed by an integer"); return;
            }
            for (i = 0; i < n && !r->fail; i++){                  /* skip the option table */
                i_rant_rd_skip(r, bs);
                i_rant_rd_skip(r, i_rant_rd_u8(r));
            }
            out->size = bs;   /* on the wire: just the backing scalar */
            return;
        }
        case RANT_VSTR: case RANT_MAP:
            out->var = 1; out->frames = 4u;
            return;
        case RANT_STRUCT: {
            uint8_t nf; uint16_t i;
            if (depth >= RANT_SCHEMA_MAX_DEPTH){ i_rant_rd_refuse(r, "structs nest too deep"); return; }
            nf = i_rant_rd_u8(r);
            for (i = 0; i < nf && !r->fail; i++){
                uint8_t fnl = i_rant_rd_u8(r); i_TypeLen m;
                i_rant_rd_skip(r, fnl);
                if (rows) (*rows)++;
                i_rant_rd_type(r, rows, (uint16_t)(depth + 1), &m);
                out->size += m.size; out->frames += m.frames; out->var |= m.var;
            }
            if (out->size > 0xFFFFFFFFu){ i_rant_rd_refuse(r, "a struct is too large"); return; }
            out->body = out->size + out->frames;
            if (out->body > 0xFFFFFFFFu) i_rant_rd_refuse(r, "a struct's default is too large");
            return;
        }
        case RANT_NAMED: {
            uint8_t nl = i_rant_rd_u8(r);
            if (r->fail || nl == 0){ r->fail = 1; return; }      /* a name is required */
            i_rant_rd_skip(r, nl);
            if (r->fail) return;
            if (r->pos < r->n && r->w[r->pos] == RANT_NAMED){
                i_rant_rd_refuse(r, "a name cannot stand for another name"); return;
            }
            i_rant_rd_type(r, rows, depth, out);
            return;
        }
        default: i_rant_rd_refuse(r, "an unknown type kind"); return;
    }
}

/* Offset of the compiled handle in buf: past the wire bytes, 8 aligned. */
static size_t i_rant_schema_handle_off(const uint8_t *buf, size_t wire_len){
    uintptr_t addr = (uintptr_t)buf + wire_len;
    size_t pad = (size_t)((8u - (addr & 7u)) & 7u);
    return wire_len + pad;
}

/* Counts every row of the root into *n. 0 on malformed wire. A bare or alias root is 1
 * plus whatever its type flattens to. */
static int i_rant_schema_wire_fields(const void *wire, size_t wire_len, uint32_t *n,
                                     const char **why){
    i_Rd r; uint8_t ver, rl; i_TypeLen t;
    *n = 0; *why = "the wire is malformed";
    r.w = (const uint8_t *)wire; r.n = wire_len; r.pos = 0; r.fail = 0; r.why = NULL;
    ver = i_rant_rd_u8(&r); if (r.fail || ver != RANT_SCHEMA_WIRE_VERSION) return 0;
    rl = i_rant_rd_u8(&r); i_rant_rd_skip(&r, rl);
    if (r.fail || (size_t)r.pos >= r.n) return 0;
    if (r.w[r.pos] == RANT_NAMED){ *why = "a root's name rides the header"; return 0; }
    if (r.w[r.pos] == RANT_STRUCT){
        i_rant_rd_type(&r, n, 0, &t);
    } else {
        uint32_t sub = 0;
        i_rant_rd_type(&r, &sub, 0, &t);
        *n = 1u + sub;                                 /* the root is the first field */
    }
    if (r.fail){ if (r.why) *why = r.why; return 0; }
    if (t.size + t.frames > 0xFFFFFFFFu){ *why = "the default message is too large"; return 0; }
    *why = NULL;
    return 1;
}

const char *i_rant_schema_wire_why(const void *wire, size_t wire_len){
    uint32_t n; const char *why;
    if (!wire || !wire_len) return "the wire is empty";
    if (!i_rant_schema_wire_fields(wire, wire_len, &n, &why)) return why;
    return n > 0xFFFFu ? "a schema has at most 65535 fields" : NULL;
}

/* flattening a wire type into the field table */
static void i_rant_field_init(i_Field *f, RantString name, uint16_t depth, uint16_t parent,
                              uint32_t offset){
    memset(f, 0, sizeof *f);
    f->name = name;
    f->type_name = rant_string(NULL, 0);
    f->depth = depth; f->parent = parent; f->arr_parent = I_RANT_NO_PARENT;
    f->offset = offset;
}

static uint32_t i_rant_emit_type(uint8_t *buf, size_t wl, i_Rd *r, RantSchema *s,
                                 uint16_t *emitted, uint32_t total,
                                 uint16_t idx, uint16_t depth, uint32_t base);

/* Emits the members of the struct body at r->pos into the table, depth first. Returns its
 * fixed size and ORs any variable member into *var. r->fail on malformed wire. */
static uint32_t i_rant_emit_struct(uint8_t *buf, size_t wl, i_Rd *r, RantSchema *s,
                                   uint16_t *emitted, uint32_t total,
                                   uint16_t depth, uint16_t parent, uint32_t base, uint8_t *var){
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
        *var |= s->fields[idx].var;
        running += sz;
    }
    return running - base;
}

/* Fills row idx from the type at r->pos, NAMED wrappers unwrapped into type_name, and
 * emits the rows it flattens to. base is where the row's storage starts. */
static uint32_t i_rant_emit_type(uint8_t *buf, size_t wl, i_Rd *r, RantSchema *s,
                                 uint16_t *emitted, uint32_t total,
                                 uint16_t idx, uint16_t depth, uint32_t base){
    size_t kpos = r->pos;
    uint8_t k, var = 0;
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
                                    (uint16_t)(depth + 1), idx, base, &var);
            break;
        case RANT_ARR: case RANT_VARR: {
            uint16_t count = 0, e; uint32_t esz;
            if (k == RANT_ARR){
                count = i_rant_rd_u16(r);
                if (r->fail) return 0;
            }
            if (*emitted >= total){ r->fail = 1; return 0; }
            e = (*emitted)++;                         /* the element row, offsets from its start */
            i_rant_field_init(&s->fields[e], rant_string((const char *)(buf + r->pos), 0),
                              (uint16_t)(depth + 1), idx, 0);
            esz = i_rant_emit_type(buf, wl, r, s, emitted, total, e, (uint16_t)(depth + 1), 0);
            if (r->fail) return 0;
            var = s->fields[e].var;
            if (!var && esz == 0){ r->fail = 1; return 0; }
            if (k == RANT_ARR && !var){
                if ((uint64_t)count * esz > 0xFFFFFFFFu){ r->fail = 1; return 0; }
                sz = (uint32_t)((uint64_t)count * esz);
            } else var = 1;
            s->fields[idx].count = count;
            break;
        }
        case RANT_STR:
            f->str_cap = i_rant_rd_u16(r);
            sz = 2u + (uint32_t)f->str_cap;
            break;
        case RANT_ENUM: {
            uint16_t i;
            f->backing = i_rant_rd_u8(r);
            f->count = i_rant_rd_u16(r);
            sz = rant_schema_scalar_size((RantSchemaTypeKind)f->backing);
            if (r->fail || sz == 0 || !i_rant_enum_backing_ok(f->backing)){ r->fail = 1; return 0; }
            for (i = 0; i < f->count && !r->fail; i++){
                i_rant_rd_skip(r, sz);
                i_rant_rd_skip(r, i_rant_rd_u8(r));
            }
            break;
        }
        case RANT_VSTR: case RANT_MAP:
            var = 1;
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
    f->var = var;
    f->next = *emitted;
    return sz;
}

/* Places every framed row in its record, counts each record's frames, and sets every
 * variable row's default body. 0 when a default would pass 4 GB. */
static int i_rant_schema_place(RantSchema *s){
    uint16_t i, ord = 0; uint64_t root = 0;
    for (i = 0; i < s->nfields; i++){         /* parents come first, depth first */
        i_Field *f = &s->fields[i];
        if (f->parent != I_RANT_NO_PARENT){
            const i_Field *p = &s->fields[f->parent];
            f->arr_parent = (p->kind == RANT_ARR || p->kind == RANT_VARR)
                          ? f->parent : p->arr_parent;
        }
        f->arr_depth = f->arr_parent == I_RANT_NO_PARENT ? 0u
                     : (uint16_t)(s->fields[f->arr_parent].arr_depth + 1u);
        if (i_rant_row_framed(s, f)){
            int rec = i_rant_row_record(f);
            f->offset = 0;                     /* a frame has no static offset */
            f->var_ord = rec < 0 ? ord++ : s->fields[rec].n_var++;
        }
    }
    s->n_var = ord;
    for (i = s->nfields; i-- > 0;){           /* children come first, so a record's frames sum */
        i_Field *f = &s->fields[i];
        uint64_t d;
        if (!f->var) continue;
        if (f->kind == RANT_STRUCT){
            if (f->parent == I_RANT_NO_PARENT || s->fields[f->parent].kind == RANT_STRUCT) continue;
            d = (uint64_t)f->size + f->dmin;   /* an element: its fixed part and its frames */
        } else if (f->kind == RANT_ARR || f->kind == RANT_VARR){
            const i_Field *e = &s->fields[i + 1u];
            d = i_rant_arr_dmin(f->kind, f->count, e->var, e->dmin);
        } else d = 0;
        if (d > 0xFFFFFFFFu) return 0;
        f->dmin = (uint32_t)d;
        if (i_rant_row_framed(s, f)){
            int rec = i_rant_row_record(f);
            if (rec < 0) root += 4u + d;
            else {
                uint64_t acc = (uint64_t)s->fields[rec].dmin + 4u + d;
                if (acc > 0xFFFFFFFFu) return 0;
                s->fields[rec].dmin = (uint32_t)acc;
            }
        }
    }
    if ((uint64_t)s->size + root > 0xFFFFFFFFu) return 0;
    s->msg_min = (uint32_t)(s->size + root);
    return 1;
}

/* Compiles the wire bytes at buf[0..wire_len] into a RantSchema placed after them in
 * buf. NULL on a malformed blob or when cap is too small. */
static RantSchema *i_rant_schema_compile(uint8_t *buf, size_t wire_len, size_t cap){
    i_Rd r; uint8_t ver, root_kind, root_namelen;
    const char *root_name; size_t hoff, need; RantSchema *s;
    uint32_t total; uint16_t emitted = 0; const char *why;
    uint8_t root_var = 0;

    if (!i_rant_schema_wire_fields(buf, wire_len, &total, &why) || total > 0xFFFFu) return NULL;

    r.w = buf; r.n = wire_len; r.pos = 0; r.fail = 0; r.why = NULL;
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
    s->value_root = (uint8_t)(root_kind != RANT_STRUCT);
    s->hash = i_rant_fnv1a64(buf, wire_len);
    if (!s->value_root){
        r.pos++;                                 /* past the root's STRUCT kind byte */
        s->size = i_rant_emit_struct(buf, wire_len, &r, s, &emitted, total,
                                     0, I_RANT_NO_PARENT, 0, &root_var);
    } else {                                     /* the bare or alias root is the first field */
        if (total == 0) return NULL;
        i_rant_field_init(&s->fields[0], rant_string((const char *)(buf + r.pos), 0),
                          0, I_RANT_NO_PARENT, 0);
        emitted = 1;
        s->size = i_rant_emit_type(buf, wire_len, &r, s, &emitted, total, 0, 0, 0);
    }
    if (r.fail || emitted != (uint16_t)total || !i_rant_schema_place(s)) return NULL;
    return s;
}

/* bytes a compiled schema needs for wire: the copy, the alignment pad, the handle and
   the field table. 0 if the wire is malformed. */
static size_t i_rant_schema_compiled_size(const void *wire, size_t wire_len){
    uint32_t total; const char *why;
    if (!i_rant_schema_wire_fields(wire, wire_len, &total, &why) || total > 0xFFFFu) return 0;
    return wire_len + 7u + sizeof(RantSchema) + (size_t)(total ? total - 1u : 0u) * sizeof(i_Field);
}

RantSchema *i_rant_schema_parse(const void *wire, size_t wire_len, RantAllocFn alloc, void *user){
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

void i_rant_schema_free(RantSchema *s, RantAllocFn alloc, void *user){
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
        out->name = f->name; out->type_name = f->type_name;
        out->kind = f->kind; out->backing = f->backing;
        out->count = f->count; out->depth = f->depth;
        out->str_cap = f->str_cap; out->arr_parent = f->arr_parent;
        out->arr_depth = f->arr_depth;
        out->offset = f->offset; out->size = f->size;
        out->min_len = f->dmin ? f->dmin : f->size;   /* dmin is set where a default varies */
    }
    return 1;
}

/* the child of row p named name, or of the root when p is NO_PARENT, else NULL */
static const i_Field *i_rant_schema_child(const RantSchema *s, uint16_t p, const char *name,
                                          size_t len){
    uint16_t j = p == I_RANT_NO_PARENT ? 0u : (uint16_t)(p + 1u);
    uint16_t end = p == I_RANT_NO_PARENT ? s->nfields : s->fields[p].next;
    while (j < end){
        const i_Field *f = &s->fields[j];
        if (f->name.len == len && (len == 0 || memcmp(f->name.data, name, len) == 0)) return f;
        j = f->next;
    }
    return NULL;
}

/* The path with each name in its wire spelling, indices and dots as given. 0 when a name
 * is refused or the result does not fit. */
static int i_rant_path_normalize(const char *path, char *out, size_t cap, size_t *len){
    size_t n = 0, i = 0;
    while (path[i]){
        if (path[i] == '.' || path[i] == '['){
            int idx = path[i] == '[';
            if (n + 1 >= cap) return 0;
            out[n++] = path[i++];
            while (idx && path[i]){                       /* an index verbatim, through its bracket */
                char c = path[i++];
                if (n + 1 >= cap) return 0;
                out[n++] = c;
                if (c == ']') break;
            }
        } else {
            char seg[256]; size_t sl = 0, wl;
            while (path[i] && path[i] != '.' && path[i] != '['){
                if (sl >= sizeof seg - 1) return 0;
                seg[sl++] = path[i++];
            }
            seg[sl] = '\0';
            wl = i_rant_name_normalize(seg, out + n, cap - n, 0);
            if (!wl) return 0;
            n += wl;
        }
    }
    out[n] = '\0';
    *len = n;
    return 1;
}

/* Walks the path from the root: a name picks a child, each [N] steps into an array's
 * element, and a name under an array steps into its element 0 first. */
const i_Field *i_rant_schema_field_by_path(const RantSchema *s, const char *path,
                                           uint32_t *elems, uint16_t *n_elems){
    size_t n, pos = 0; char wire[512]; uint32_t got[RANT_SCHEMA_MAX_DEPTH];
    uint16_t k = 0; const i_Field *cur = NULL;
    if (n_elems) *n_elems = 0;
    if (!s || !path) return NULL;
    if (!i_rant_path_normalize(path, wire, sizeof wire, &n)) return NULL;
    for (;;){
        size_t start = pos;
        while (pos < n && wire[pos] != '.' && wire[pos] != '[') pos++;
        if (cur){
            while (cur->kind == RANT_ARR || cur->kind == RANT_VARR){
                if (k >= RANT_SCHEMA_MAX_DEPTH) return NULL;
                got[k++] = 0;
                cur = cur + 1;
            }
            if (cur->kind != RANT_STRUCT || start == pos) return NULL;
        }
        cur = i_rant_schema_child(s, cur ? (uint16_t)(cur - s->fields) : I_RANT_NO_PARENT,
                                  wire + start, pos - start);
        if (!cur) return NULL;
        while (pos < n && wire[pos] == '['){
            uint64_t idx = 0; size_t d = ++pos;
            while (pos < n && wire[pos] >= '0' && wire[pos] <= '9'){
                idx = idx * 10u + (uint64_t)(wire[pos] - '0');
                if (idx > 0xFFFFFFFFu) return NULL;
                pos++;
            }
            if (pos == d || pos >= n || wire[pos] != ']') return NULL;
            pos++;
            if ((cur->kind != RANT_ARR && cur->kind != RANT_VARR) || k >= RANT_SCHEMA_MAX_DEPTH)
                return NULL;
            got[k++] = (uint32_t)idx;
            cur = cur + 1;                                 /* the element row */
        }
        if (pos >= n) break;
        if (wire[pos] != '.') return NULL;
        pos++;
    }
    if (k != cur->arr_depth) return NULL;
    if (elems) memcpy(elems, got, (size_t)k * sizeof *got);
    if (n_elems) *n_elems = k;
    return cur;
}

int rant_schema_field_index(const RantSchema *s, const char *path){
    const i_Field *f = i_rant_schema_field_by_path(s, path, NULL, NULL);
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
    bs  = (uint8_t)rant_schema_scalar_size((RantSchemaTypeKind)f->backing);
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
      if (value) *value = i_rant_enum_read_val(f->backing, w + pos);
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
    char wire[256]; size_t want;
    if (!name) return 0;
    want = i_rant_name_normalize(name, wire, sizeof wire, 1);   /* given in any spelling */
    if (!want) return 0;
    name = wire;
    for (i = 0; i < n; i++){
        int64_t v; RantString nm;
        if (rant_schema_enum_variant(s, field, i, &v, &nm) && nm.len == want &&
            (want == 0 || memcmp(nm.data, name, want) == 0)){ if (out) *out = v; return 1; }
    }
    return 0;
}
