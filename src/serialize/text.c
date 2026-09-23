/* The text side: the type writer, the printer and the DSL parser. */
#include "internal.h"

/* room for extra more bytes, growing the wire buffer through the hook */
static int i_rant_schema_builder_reserve(i_RantSchemaBuilder *b, size_t extra){
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
static void i_rant_schema_builder_put(i_RantSchemaBuilder *b, uint8_t v){
    if (!i_rant_schema_builder_reserve(b, 1)) return;
    b->buf[b->len++] = v;
}
static void i_rant_schema_builder_put_u16(i_RantSchemaBuilder *b, uint16_t v){
    if (!i_rant_schema_builder_reserve(b, 2)) return;
    i_rant_le_w16(b->buf + b->len, v); b->len += 2;
}
/* raw bytes, a compiled type encoding from elsewhere */
static void i_rant_schema_builder_put_raw(i_RantSchemaBuilder *b, const void *src, size_t len){
    if (!len) return;
    if (!src){ b->err = -6; return; }
    if (!i_rant_schema_builder_reserve(b, len)) return;
    memcpy(b->buf + b->len, src, len);
    b->len += len;
}
/* bytes little endian bytes of v, an enum option's backing sized value */
static void i_rant_schema_builder_put_le(i_RantSchemaBuilder *b, uint64_t v, uint32_t bytes){
    uint32_t i;
    if (!i_rant_schema_builder_reserve(b, bytes)) return;
    for (i = 0; i < bytes; i++) b->buf[b->len++] = (uint8_t)(v >> (8 * i));
}
static void i_rant_schema_builder_put_name(i_RantSchemaBuilder *b, const char *name){
    size_t n = 0, i; if (name) while (name[n]) n++;
    if (b->depth == 0){                                  /* a type on its own carries no field name */
        if (n) b->err = -4;
        return;
    }
    if (n > 255){ b->err = -4; return; }
    if (!i_rant_schema_builder_reserve(b, 1 + n)) return;
    b->buf[b->len++] = (uint8_t)n;
    for (i = 0; i < n; i++) b->buf[b->len++] = (uint8_t)name[i];
}
/* count a field on the innermost open struct. A type on its own counts nothing */
static void i_rant_schema_builder_count(i_RantSchemaBuilder *b){
    uint16_t *c;
    if (b->err || b->depth == 0) return;
    c = &b->field_count[b->depth - 1];
    if (*c >= 255){ b->err = -5; return; }       /* nfields is a u8 */
    (*c)++;
}
/* open a struct type: [STRUCT][nfields placeholder], push a nesting level */
static void i_rant_schema_builder_open_struct(i_RantSchemaBuilder *b){
    if (b->err) return;
    if (b->depth >= RANT_SCHEMA_MAX_DEPTH){ b->err = -2; return; }
    i_rant_schema_builder_put(b, (uint8_t)RANT_STRUCT);
    b->count_pos[b->depth] = b->len;
    i_rant_schema_builder_put(b, 0);
    b->field_count[b->depth] = 0;
    b->depth++;
}

static void i_rant_schema_builder_close_struct(i_RantSchemaBuilder *b){
    if (b->err) return;
    if (b->depth == 0){ b->err = -3; return; }      /* no open struct */
    b->depth--;
    b->buf[b->count_pos[b->depth]] = (uint8_t)b->field_count[b->depth];
}

static i_RantSchemaBuilder i_rant_schema_builder_begin(RantAllocFn alloc, void *user){
    i_RantSchemaBuilder b;
    memset(&b, 0, sizeof b);
    b.alloc = alloc; b.user = user;
    if (!alloc){ b.err = -1; return b; }
    b.cap = 64u;
    b.buf = (uint8_t *)alloc(user, NULL, b.cap);
    if (!b.buf){ b.err = -1; b.cap = 0; }
    return b;
}

/* Streaming enum construction, so the parser never buffers the option list: open writes
 * the head and returns the count placeholder, add appends, finish backpatches. */
static size_t i_rant_schema_builder_enum_open(i_RantSchemaBuilder *b, RantSchemaTypeKind backing){
    size_t count_pos;
    i_rant_schema_builder_put(b, (uint8_t)RANT_ENUM);
    i_rant_schema_builder_put(b, (uint8_t)backing);
    count_pos = b->len;
    i_rant_schema_builder_put_u16(b, 0);                 /* n, backpatched by finish */
    return count_pos;
}
static void i_rant_schema_builder_enum_add(i_RantSchemaBuilder *b, RantSchemaTypeKind backing,
                                           int64_t value, const char *name, size_t name_len){
    size_t i;
    if (b->err) return;
    if (name_len > 255){ b->err = -4; return; }
    i_rant_schema_builder_put_le(b, (uint64_t)value, rant_schema_scalar_size(backing));
    if (!i_rant_schema_builder_reserve(b, 1 + name_len)) return;
    b->buf[b->len++] = (uint8_t)name_len;
    for (i = 0; i < name_len; i++) b->buf[b->len++] = (uint8_t)name[i];
}
static void i_rant_schema_builder_enum_finish(i_RantSchemaBuilder *b, size_t count_pos, uint16_t count){
    if (b->err) return;
    i_rant_le_w16(b->buf + count_pos, count);
}

/* spelling types back as DSL text */
const char *i_rant_why_kind(uint8_t k){
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
        int is_struct = scan.d[i].toff < scan.n && scan.w[scan.d[i].toff] == RANT_STRUCT;
        i_rant_out_raw(&o, (const char *)(scan.w + scan.d[i].noff), scan.d[i].nlen);
        i_rant_out_str(&o, is_struct ? " " : " = ");     /* Name { } or Name = type */
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

/* The schema DSL. The grammar is in spec/schema.md. A type word that is not a built in
 * resolves against the registry's definitions and then the standard library. */
#ifndef RANT_NO_STDTYPES
const char *i_rant_std_lookup(const char *name);     /* serialize/stdtypes.c */
#else
#define i_rant_std_lookup(name) ((const char *)0)
#endif

typedef struct { const char *p; const char *err; const char *why; } i_RantDsl;

void i_rant_registry_init(i_RantRegistry *r, RantAllocFn alloc, void *user){
    memset(r, 0, sizeof *r);
    r->arena = i_rant_schema_builder_begin(alloc, user);
}

static void i_rant_dsl_ws(i_RantDsl *d){
    for (;;){
        while (*d->p==' ' || *d->p=='\t' || *d->p=='\r' || *d->p=='\n') d->p++;
        if (d->p[0]=='-' && d->p[1]=='-'){ while (*d->p && *d->p!='\n') d->p++; continue; }
        return;
    }
}
/* The wire spelling of a name, see rant_field_name. Words start at an underscore, at a
 * capital after a lowercase letter or a digit, and at the last capital of a run of them. */
size_t i_rant_name_normalize(const char *in, char *out, size_t cap, int as_type){
    size_t n = 0; int boundary = 1; char prev = 0;
    const char *p;
    for (p = in; *p; p++){
        char c = *p;
        int upper = c >= 'A' && c <= 'Z', lower = c >= 'a' && c <= 'z', digit = c >= '0' && c <= '9';
        if (!upper && !lower && !digit){ boundary = 1; continue; }
        if (!boundary && upper){
            int plower = prev >= 'a' && prev <= 'z', pdigit = prev >= '0' && prev <= '9';
            int pupper = prev >= 'A' && prev <= 'Z', nlower = p[1] >= 'a' && p[1] <= 'z';
            if (plower || pdigit || (pupper && nlower)) boundary = 1;
        }
        if (n + 1 >= cap) return 0;
        if (boundary && (n || as_type)) out[n++] = upper ? c : (lower ? (char)(c - 32) : c);
        else out[n++] = upper ? (char)(c + 32) : c;
        boundary = 0; prev = c;
    }
    out[n] = '\0';
    if (!n || (out[0] >= '0' && out[0] <= '9')) return 0;
    return n;
}
size_t rant_field_name(const char *name, char *out, size_t cap){
    if (!name || !out || !cap) return 0;
    return i_rant_name_normalize(name, out, cap, 0);
}
size_t rant_type_name(const char *name, char *out, size_t cap){
    if (!name || !out || !cap) return 0;
    return i_rant_name_normalize(name, out, cap, 1);
}

static void i_rant_dsl_fail(i_RantDsl *d, const char *at, const char *why){
    if (d->err) return;                                  /* the first refusal is the one reported */
    d->err = at; d->why = why;
}
/* the type writer's latched failure as a reason */
static const char *i_rant_builder_why(int err){
    switch (err){
    case -2: return "structs nest too deep";
    case -4: return "a name is at most 255 characters";
    case -5: return "a struct has at most 255 fields";
    default: return "out of memory";
    }
}
/* an identifier into out[256], NUL terminated. 0 and err when missing or overlong */
static int i_rant_dsl_ident(i_RantDsl *d, char out[256]){
    const char *q = d->p; size_t n;
    if (!((*q>='A'&&*q<='Z') || (*q>='a'&&*q<='z') || *q=='_')){ i_rant_dsl_fail(d, q, "expected a name"); return 0; }
    while ((*q>='A'&&*q<='Z') || (*q>='a'&&*q<='z') || (*q>='0'&&*q<='9') || *q=='_') q++;
    n = (size_t)(q - d->p);
    if (n > 255){ i_rant_dsl_fail(d, d->p, i_rant_builder_why(-4)); return 0; }
    memcpy(out, d->p, n); out[n] = '\0';
    d->p = q;
    return 1;
}
static int i_rant_dsl_expect(i_RantDsl *d, char c){
    if (*d->p == c){ d->p++; return 1; }
    i_rant_dsl_fail(d, d->p, c == '{' ? "expected {" : c == '}' ? "expected }" :
                              c == '<' ? "expected <" : c == '>' ? "expected >" :
                              c == ']' ? "expected ]" : "expected : after the field name");
    return 0;
}
/* a decimal array count, 1 to 65535 */
static int i_rant_dsl_count(i_RantDsl *d, uint16_t *out){
    const char *at = d->p; uint32_t v = 0;
    while (*d->p>='0' && *d->p<='9'){
        v = v*10u + (uint32_t)(*d->p - '0');
        if (v > 0xFFFFu){ i_rant_dsl_fail(d, at, "a count is 1 to 65535"); return 0; }
        d->p++;
    }
    if (d->p == at || v == 0){ i_rant_dsl_fail(d, at, "a count is 1 to 65535"); return 0; }
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
        if (v > lim){ i_rant_dsl_fail(d, at, "an enum value is a whole number that fits i64"); return 0; }
        d->p++; any = 1;
    }
    if (!any){ i_rant_dsl_fail(d, at, "an enum value is a whole number that fits i64"); return 0; }
    *out = neg ? -(int64_t)v : (int64_t)v;
    return 1;
}
/* enum<uN> { Name [= value], ... } after the word enum was read. Streams the options. */
static void i_rant_dsl_enum(i_RantDsl *d, i_RantSchemaBuilder *b){
    char wname[256]; RantSchemaTypeKind backing; size_t count_pos; uint16_t count = 0; int64_t next = 0;
    i_rant_dsl_ws(d);
    if (!i_rant_dsl_expect(d, '<')) return;
    i_rant_dsl_ws(d);
    if (!i_rant_dsl_ident(d, wname)) return;
    if (!i_rant_dsl_kind(wname, &backing) || !i_rant_enum_backing_ok((uint8_t)backing)){
        i_rant_dsl_fail(d, d->p, "an enum backing is an integer kind"); return;
    }
    i_rant_dsl_ws(d);
    if (!i_rant_dsl_expect(d, '>')) return;
    i_rant_dsl_ws(d);
    if (!i_rant_dsl_expect(d, '{')) return;
    count_pos = i_rant_schema_builder_enum_open(b, backing);
    for (;;){
        char vname[256], vwire[256]; int64_t v; const char *vat;
        i_rant_dsl_ws(d);
        if (*d->p == '}' || *d->p == '\0' || d->err || b->err) break;
        vat = d->p;
        if (!i_rant_dsl_ident(d, vname)) return;
        if (!i_rant_name_normalize(vname, vwire, sizeof vwire, 1)){
            i_rant_dsl_fail(d, vat, "an option name needs a letter"); return;
        }
        i_rant_dsl_ws(d);
        if (*d->p == '='){   /* an explicit value, else auto increment */
            d->p++; i_rant_dsl_ws(d);
            if (!i_rant_dsl_enum_value(d, &v)) return;
        } else v = next;
        if (!i_rant_enum_val_fits((uint8_t)backing, v)){
            i_rant_dsl_fail(d, d->p, "an enum value does not fit the backing kind"); return;
        }
        i_rant_schema_builder_enum_add(b, backing, v, vwire, strlen(vwire));
        count++; next = v + 1;
        i_rant_dsl_ws(d);
        if (*d->p == ',') d->p++;                        /* an optional separator */
    }
    if (!i_rant_dsl_expect(d, '}')) return;
    i_rant_schema_builder_enum_finish(b, count_pos, count);
}

static void i_rant_dsl_field_type(i_RantDsl *d, i_RantSchemaBuilder *b, i_RantRegistry *defs,
                                  const char *name);
static void i_rant_dsl_fields(i_RantDsl *d, i_RantSchemaBuilder *b, i_RantRegistry *defs);

/* one more slot in a registry array, doubling through the arena's hook */
static int i_rant_registry_room(i_RantRegistry *r, void **arr, uint32_t n, uint32_t *cap, size_t elem){
    uint32_t want; void *na;
    if (n < *cap) return 1;
    want = *cap ? *cap * 2u : 16u;
    na = r->arena.alloc(r->arena.user, *arr, (size_t)want * elem);
    if (!na) return 0;
    *arr = na; *cap = want;
    return 1;
}

/* records a named type: its name bytes then its compiled type, both in the arena */
static int i_rant_defs_add_bytes(i_RantRegistry *defs, const char *name, size_t nlen,
                                 const uint8_t *type, size_t tlen){
    uint32_t noff, toff;
    if (nlen == 0 || nlen > 255 || tlen == 0) return 0;
    if (!i_rant_registry_room(defs, (void **)&defs->defs, defs->n_defs, &defs->cap_defs,
                              sizeof *defs->defs)) return 0;
    noff = (uint32_t)defs->arena.len;
    i_rant_schema_builder_put_raw(&defs->arena, name, nlen);
    toff = (uint32_t)defs->arena.len;
    i_rant_schema_builder_put_raw(&defs->arena, type, tlen);
    if (defs->arena.err) return 0;
    defs->defs[defs->n_defs].noff = noff; defs->defs[defs->n_defs].nlen = (uint8_t)nlen;
    defs->defs[defs->n_defs].toff = toff; defs->defs[defs->n_defs].tlen = (uint32_t)tlen;
    defs->n_defs++;
    return 1;
}

static int i_rant_defs_find(i_RantRegistry *defs, const char *name, uint32_t *toff, uint32_t *tlen){
    size_t nlen = strlen(name); uint32_t i;
    for (i = 0; i < defs->n_defs; i++)
        if (defs->defs[i].nlen == nlen &&
            memcmp(defs->arena.buf + defs->defs[i].noff, name, nlen) == 0){
            *toff = defs->defs[i].toff; *tlen = defs->defs[i].tlen;
            return 1;
        }
    return 0;
}

/* compiles one type spelling, a standard library entry or any type text, as a definition */
static int i_rant_defs_add_text(i_RantRegistry *defs, const char *name, const char *text){
    i_RantSchemaBuilder sb; i_RantDsl sd; int ok = 0;
    if (defs->rec >= 8u) return 0;                       /* the roster is acyclic, but be sure */
    defs->rec++;
    sb = i_rant_schema_builder_begin(defs->arena.alloc, defs->arena.user);
    sd.p = text; sd.err = NULL; sd.why = NULL;
    i_rant_dsl_field_type(&sd, &sb, defs, "");
    i_rant_dsl_ws(&sd);
    if (*sd.p) i_rant_dsl_fail(&sd, sd.p, "a type on its own ends the text");
    if (!sd.err && !sb.err && sb.len)
        ok = i_rant_defs_add_bytes(defs, name, strlen(name), sb.buf, sb.len);
    if (sb.buf) sb.alloc(sb.user, sb.buf, 0);
    defs->rec--;
    return ok;
}

/* Resolves a type name to its encoding in the arena: the registry's definitions first,
 * then the standard library, which is expanded into the registry on first use. */
static int i_rant_dsl_ref(i_RantRegistry *defs, const char *name, uint32_t *toff, uint32_t *tlen){
    const char *std;
    if (i_rant_defs_find(defs, name, toff, tlen)) return 1;
    std = i_rant_std_lookup(name);
    if (std && i_rant_defs_add_text(defs, name, std))
        return i_rant_defs_find(defs, name, toff, tlen);
    return 0;
}

int i_rant_registry_ref(i_RantRegistry *r, const char *name, const uint8_t **type, size_t *tlen){
    uint32_t toff, tl;
    if (!r || !name || !i_rant_dsl_ref(r, name, &toff, &tl)) return 0;
    *type = r->arena.buf + toff; *tlen = tl;
    return 1;
}

/* turns the type just written at b->buf[head..len) into an array of itself by splicing
 * the array head in front of it */
static void i_rant_dsl_splice_array(i_RantSchemaBuilder *b, size_t head, uint16_t count,
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

/* The type whose leading word is in tname, with at pointing at it for errors */
static void i_rant_dsl_word_type(i_RantDsl *d, i_RantSchemaBuilder *b, i_RantRegistry *defs,
                                 const char *tname, const char *at){
    RantSchemaTypeKind k; uint16_t cap; uint32_t toff, tlen; size_t nlen; char twire[256];
    if (strcmp(tname, "map") == 0){
        i_rant_schema_builder_put(b, (uint8_t)RANT_MAP);
        return;
    }
    if (strcmp(tname, "enum") == 0){
        i_rant_dsl_enum(d, b);
        return;
    }
    if (strcmp(tname, "string") == 0){                   /* string or string<cap> */
        i_rant_dsl_ws(d);
        if (*d->p != '<'){ i_rant_schema_builder_put(b, (uint8_t)RANT_VSTR); return; }
        d->p++;
        i_rant_dsl_ws(d);
        if (!i_rant_dsl_count(d, &cap)) return;
        i_rant_dsl_ws(d);
        if (!i_rant_dsl_expect(d, '>')) return;
        i_rant_schema_builder_put(b, (uint8_t)RANT_STR);
        i_rant_schema_builder_put_u16(b, cap);
        return;
    }
    if (i_rant_dsl_kind(tname, &k)){
        i_rant_schema_builder_put(b, (uint8_t)k);
        return;
    }
    if (!i_rant_name_normalize(tname, twire, sizeof twire, 1)){
        i_rant_dsl_fail(d, at, "a type name needs a letter"); return;
    }
    if (!i_rant_dsl_ref(defs, twire, &toff, &tlen)){ i_rant_dsl_fail(d, at, "unknown type"); return; }
    nlen = strlen(twire);
    i_rant_schema_builder_put(b, (uint8_t)RANT_NAMED);
    i_rant_schema_builder_put(b, (uint8_t)nlen);
    i_rant_schema_builder_put_raw(b, twire, nlen);
    i_rant_schema_builder_put_raw(b, defs->arena.buf + toff, tlen);
}

/* One type plus the field it defines. The count follows the body in the text, so an array
 * suffix wraps what was just written. A type on its own passes name "". */
static void i_rant_dsl_field_type(i_RantDsl *d, i_RantSchemaBuilder *b, i_RantRegistry *defs,
                                  const char *name){
    char tname[256]; const char *at; size_t head; uint16_t cnt; int variable; uint8_t kind;
    i_rant_dsl_ws(d);
    at = d->p;
    i_rant_schema_builder_count(b);
    i_rant_schema_builder_put_name(b, name);
    head = b->len;
    if (*d->p == '{'){
        d->p++;
        i_rant_schema_builder_open_struct(b);
        i_rant_dsl_fields(d, b, defs);
        if (!i_rant_dsl_expect(d, '}')) return;
        i_rant_schema_builder_close_struct(b);
    } else {
        if (!i_rant_dsl_ident(d, tname)) return;
        i_rant_dsl_word_type(d, b, defs, tname, at);
    }
    if (d->err || b->err || head >= b->len) return;
    kind = b->buf[head];
    if (kind == RANT_MAP || kind == RANT_ENUM) return;   /* never an array element */
    if (!i_rant_dsl_suffix(d, &cnt, &variable) || d->err) return;
    if (kind == RANT_VSTR){ i_rant_dsl_fail(d, at, "string[] is ragged, give the string a cap"); return; }
    i_rant_dsl_splice_array(b, head, cnt, variable);
}

/* whether the field name written at b->buf[mine] already names an earlier field of the
 * open struct, whose fields start at first */
static int i_rant_dsl_name_taken(const i_RantSchemaBuilder *b, size_t first, size_t mine){
    size_t pos = first; uint8_t nl = b->buf[mine];
    while (pos < mine){
        uint8_t l = b->buf[pos];
        if (l == nl && memcmp(b->buf + pos + 1, b->buf + mine + 1, nl) == 0) return 1;
        pos = i_rant_skip_type(b->buf, b->len, pos + 1u + l);
    }
    return 0;
}

/* the fields of one struct body, up to and not consuming the closing brace */
static void i_rant_dsl_fields(i_RantDsl *d, i_RantSchemaBuilder *b, i_RantRegistry *defs){
    char name[256], wire[256];
    for (;;){
        const char *at; size_t first, mine;
        i_rant_dsl_ws(d);
        if (*d->p == '}' || *d->p == '\0' || d->err || b->err) return;
        at = d->p;
        if (!i_rant_dsl_ident(d, name)) return;
        if (!i_rant_name_normalize(name, wire, sizeof wire, 0)){
            i_rant_dsl_fail(d, at, "a field name needs a letter"); return;
        }
        i_rant_dsl_ws(d);
        if (!i_rant_dsl_expect(d, ':')) return;
        first = b->count_pos[b->depth - 1] + 1u;
        mine = b->len;
        i_rant_dsl_field_type(d, b, defs, wire);
        if (d->err) return;
        if (!b->err && i_rant_dsl_name_taken(b, first, mine)){
            i_rant_dsl_fail(d, at, "two fields of one struct spell the same wire name"); return;
        }
        i_rant_dsl_ws(d);
        if (*d->p == ',') d->p++;                        /* an optional separator */
    }
}

/* One definition, with d->p on its { or =. Name { fields } defines a struct and Name = type
 * names any other type. The body compiles on its own, then is kept, or must match an
 * existing or standard definition of the name exactly. Leaves the type's place in the arena. */
static void i_rant_dsl_def(i_RantDsl *d, i_RantRegistry *defs, const char *name, const char *name_at,
                           uint32_t *toff, uint32_t *tlen){
    i_RantSchemaBuilder sb; int ok = 0, exists, is_struct = *d->p == '{';
    const char *at;
    exists = i_rant_dsl_ref(defs, name, toff, tlen);
    sb = i_rant_schema_builder_begin(defs->arena.alloc, defs->arena.user);
    d->p++;
    i_rant_dsl_ws(d);
    at = d->p;
    if (is_struct){
        i_rant_schema_builder_open_struct(&sb);
        i_rant_dsl_fields(d, &sb, defs);
        if (i_rant_dsl_expect(d, '}')) i_rant_schema_builder_close_struct(&sb);
    } else {
        i_rant_dsl_field_type(d, &sb, defs, "");
        if (!d->err && !sb.err && sb.len && sb.buf[0] == RANT_STRUCT)
            i_rant_dsl_fail(d, at, "a struct is defined as Name { }");
        if (!d->err && !sb.err && sb.len && sb.buf[0] == RANT_NAMED)
            i_rant_dsl_fail(d, at, "a name cannot stand for another name");
    }
    if (!d->err && sb.err) i_rant_dsl_fail(d, at, i_rant_builder_why(sb.err));
    if (!d->err && sb.len){
        if (exists){                                     /* redefining is fine if identical */
            ok = (*tlen == sb.len && memcmp(defs->arena.buf + *toff, sb.buf, sb.len) == 0);
            if (!ok) i_rant_dsl_fail(d, name_at, "the name is already defined with another shape");
        } else {
            ok = i_rant_defs_add_bytes(defs, name, strlen(name), sb.buf, sb.len)
              && i_rant_defs_find(defs, name, toff, tlen);
            if (!ok) i_rant_dsl_fail(d, name_at, i_rant_builder_why(-1));
        }
    }
    if (sb.buf) sb.alloc(sb.user, sb.buf, 0);
}

RantSchema *i_rant_registry_parse(i_RantRegistry *r, const void *wire, size_t wire_len){
    uint32_t i; RantSchema *s;
    if (!r || !wire || !wire_len) return NULL;
    for (i = 0; i < r->n_owned; i++)                    /* one schema per shape */
        if (r->owned[i]->wire.len == wire_len && memcmp(r->owned[i]->wire.data, wire, wire_len) == 0)
            return r->owned[i];
    s = i_rant_schema_parse(wire, wire_len, r->arena.alloc, r->arena.user);
    if (!s) return NULL;
    if (!i_rant_registry_room(r, (void **)&r->owned, r->n_owned, &r->cap_owned, sizeof *r->owned)){
        i_rant_schema_free(s, r->arena.alloc, r->arena.user);
        return NULL;
    }
    r->owned[r->n_owned++] = s;
    return s;
}

/* A text is a run of statements and compiles to its last one. A definition may be followed
 * by more statements. A type on its own (bool, f32[3], Pose, { x: f32 }) ends the text. A
 * failed text leaves no definition behind, so nothing is half kept. */
RantSchema *i_rant_registry_compile(i_RantRegistry *defs, const char *text, i_RantSchemaErr *err){
    i_RantDsl d; i_RantSchemaBuilder root;
    RantSchema *s = NULL;
    char word[256];
    const char *rname = NULL; size_t rnlen = 0;
    const uint8_t *rtype = NULL; size_t rtlen = 0;
    uint32_t toff = 0, tlen = 0;
    size_t kept_len; uint32_t kept_defs;           /* the rollback point */
    int have_type = 0, have_def = 0;

    if (err){ err->why = NULL; err->at = NULL; }
    if (!defs || !text) return NULL;
    kept_len = defs->arena.len; kept_defs = defs->n_defs;
    root = i_rant_schema_builder_begin(defs->arena.alloc, defs->arena.user);
    d.p = text; d.err = NULL; d.why = NULL;
    if (defs->arena.err) i_rant_dsl_fail(&d, text, i_rant_builder_why(defs->arena.err));
    if (root.err) i_rant_dsl_fail(&d, text, i_rant_builder_why(root.err));

    while (!d.err){
        const char *at;
        i_rant_dsl_ws(&d);
        if (!*d.p) break;
        at = d.p;
        if (*d.p != '{'){
            if (!i_rant_dsl_ident(&d, word)) break;
            i_rant_dsl_ws(&d);
            if (*d.p == '{' || *d.p == '='){
                char twire[256];
                if (!i_rant_name_normalize(word, twire, sizeof twire, 1)){
                    i_rant_dsl_fail(&d, at, "a type name needs a letter"); break;
                }
                strcpy(word, twire);
                i_rant_dsl_def(&d, defs, word, at, &toff, &tlen);
                have_def = 1;
                continue;
            }
        }
        d.p = at;
        i_rant_dsl_field_type(&d, &root, defs, "");
        have_type = 1;
        i_rant_dsl_ws(&d);
        if (*d.p) i_rant_dsl_fail(&d, d.p, "a type on its own ends the text");
        break;
    }
    if (!d.err && root.err) i_rant_dsl_fail(&d, d.p, i_rant_builder_why(root.err));
    if (!d.err && defs->arena.err) i_rant_dsl_fail(&d, d.p, i_rant_builder_why(defs->arena.err));

    if (!d.err){
        if (have_type){
            rtype = root.buf; rtlen = root.len;
            if (rtlen >= 2 && rtype[0] == RANT_NAMED){     /* a bare reference takes the name */
                size_t nl = rtype[1];
                if (2u + nl <= rtlen){
                    rname = (const char *)(rtype + 2); rnlen = nl;
                    rtype += 2u + nl; rtlen -= 2u + nl;
                }
            }
        } else if (have_def){                            /* word still holds the last name */
            rname = word; rnlen = strlen(word);
            rtype = defs->arena.buf + toff; rtlen = tlen;
        }
        if (!rtype || !rtlen) i_rant_dsl_fail(&d, d.p, "the text is empty");
    }
    if (!d.err){
        size_t wlen = 2u + rnlen + rtlen; const char *why = NULL;
        uint8_t *w = (uint8_t *)defs->arena.alloc(defs->arena.user, NULL, wlen);
        if (w){
            w[0] = (uint8_t)RANT_SCHEMA_WIRE_VERSION;
            w[1] = (uint8_t)rnlen;
            if (rnlen) memcpy(w + 2, rname, rnlen);
            memcpy(w + 2 + rnlen, rtype, rtlen);
            s = i_rant_registry_parse(defs, w, wlen);
            if (!s) why = i_rant_schema_wire_why(w, wlen);   /* a broken rule, not memory */
            defs->arena.alloc(defs->arena.user, w, 0);
        }
        if (!s) i_rant_dsl_fail(&d, why ? text : d.p, why ? why : i_rant_builder_why(-1));
    }
    if (root.buf) defs->arena.alloc(defs->arena.user, root.buf, 0);
    if (!s){
        defs->arena.len = kept_len; defs->n_defs = kept_defs; defs->arena.err = 0;
        if (err){ err->why = d.why; err->at = d.err; }
    }
    return s;
}
