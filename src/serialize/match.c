/* reader and writer compatibility */
#include "internal.h"

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
    ra.w = sa->wire.data; ra.n = sa->wire.len; ra.pos = a->type_off; ra.fail = 0; ra.why = NULL;
    rb.w = sb->wire.data; rb.n = sb->wire.len; rb.pos = b->type_off; rb.fail = 0; rb.why = NULL;
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

RantSchema *i_rant_schema_rebase(const RantSchema *sub, const RantSchema *pub,
                                 RantAllocFn alloc, void *user){
    RantSchema *r; uint16_t i = 0;
    if (!alloc || !rant_schema_subset(sub, pub)) return NULL;
    r = i_rant_schema_parse(sub->wire.data, sub->wire.len, alloc, user);
    if (!r) return NULL;
    while (i < r->nfields){                                   /* the writer's layout */
        const i_Field *p = i_rant_schema_find(pub, r->fields[i].name);
        uint16_t re = i_rant_subtree_end(r, i), j, k;
        if (!p || r->fields[i].depth != 0){ i_rant_schema_free(r, alloc, user); return NULL; }
        j = (uint16_t)(p - pub->fields);
        for (k = 0; (uint16_t)(i + k) < re; k++){             /* the subtrees match exactly */
            i_Field *rf = &r->fields[i + k];
            const i_Field *pf;
            if ((uint16_t)(j + k) >= pub->nfields){ i_rant_schema_free(r, alloc, user); return NULL; }
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
