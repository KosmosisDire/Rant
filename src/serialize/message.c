/* Message validate, get and set over a compiled schema. */
#include "internal.h"

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

/* Where field f's bytes sit in msg, bounds checked. elems holds one index per enclosing
 * struct array, outermost first. Each strides by its element size, and only the outermost
 * can be variable, since an element has one size, so its frame is the base. */
static int i_rant_field_addr(const RantSchema *s, RantBytes msg, const i_Field *f,
                             const uint32_t *elems, uint16_t n_elems, size_t *out_off){
    uint64_t off = f->offset;
    const i_Field *a = f;
    uint16_t k = n_elems;
    if (n_elems != f->arr_depth || (n_elems && !elems)) return 0;
    if (i_rant_kind_var(f->kind)){ *out_off = 0; return 1; }     /* frames locate themselves */
    while (a->arr_parent != I_RANT_NO_PARENT){
        const i_Field *p = &s->fields[a->arr_parent];
        uint32_t idx = elems[--k];
        if (p->elem_size == 0) return 0;
        if (p->kind == RANT_ARR){
            if (idx >= p->count) return 0;
        } else {
            RantBytes fr = i_rant_schema_frame(s, msg, p->var_ord);
            if (!fr.data || ((uint64_t)idx + 1u) * p->elem_size > fr.len) return 0;
            off += (uint64_t)(fr.data - msg.data);
        }
        off += (uint64_t)idx * p->elem_size;
        a = p;
    }
    if (off + f->size > msg.len) return 0;
    *out_off = (size_t)off;
    return 1;
}

/* Resolves a possibly indexed path against a message. NULL if unknown, out of range or
 * the message is too short. */
static const i_Field *i_rant_schema_read_lookup(const RantSchema *s, RantBytes msg,
                                                const char *field, size_t *off){
    uint32_t elems[RANT_SCHEMA_MAX_DEPTH]; uint16_t n = 0;
    const i_Field *f = i_rant_schema_field_by_path(s, field, elems, &n);
    if (!f || !i_rant_field_addr(s, msg, f, elems, n, off)) return NULL;
    return f;
}

uint64_t i_rant_schema_read_uint(uint8_t kind, const uint8_t *p){
    switch (kind){
        case RANT_U8: case RANT_BOOL: return p[0];
        case RANT_U16: return i_rant_le_r16(p);
        case RANT_U32: return i_rant_le_r32(p);
        case RANT_U64: return i_rant_le_r64(p);
        default: return 0;
    }
}
int64_t i_rant_schema_read_int(uint8_t kind, const uint8_t *p){
    switch (kind){
        case RANT_I8:    return (int8_t)p[0];
        case RANT_I16: return (int16_t)i_rant_le_r16(p);
        case RANT_I32: return (int32_t)i_rant_le_r32(p);
        case RANT_I64: return (int64_t)i_rant_le_r64(p);
        default: return 0;
    }
}
double i_rant_schema_read_f64(uint8_t kind, const uint8_t *p){
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

int rant_get_value_at(RantBytes msg, const RantSchema *s, uint16_t field,
                      const uint32_t *elems, uint16_t n_elems, RantValue *out){
    const i_Field *f; const uint8_t *p; size_t off;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!s || field >= s->nfields) return 0;
    f = &s->fields[field];
    if (!i_rant_field_addr(s, msg, f, elems, n_elems, &off)) return 0;
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
    return rant_get_value_at(msg, s, field, NULL, 0, out);
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

int rant_set_value_at(void *buf, size_t cap, const RantSchema *s, uint16_t field,
                      const uint32_t *elems, uint16_t n_elems, const RantValue *val){
    const i_Field *f; uint8_t *p; size_t off;
    if (!buf || !val || !s || field >= s->nfields) return 0;
    f = &s->fields[field];
    if (!i_rant_field_addr(s, rant_bytes(buf, cap), f, elems, n_elems, &off)) return 0;
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
    return rant_set_value_at(buf, cap, s, field, NULL, 0, val);
}
