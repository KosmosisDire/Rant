#include "internal.h"

/* The doc parse: the DSL compiles the text and records where each comment, definition and
 * member starts (text.c), then the comments attach by line. spec/schema.md, Docs. */

typedef struct {
    const RantSchema *schema;
    RantString        about;
    uint32_t          line, first_note, n_notes;
    uint8_t           kind;
} i_RantDocDef;

struct RantSchemaDoc {
    RantAllocator   alloc;           /* the caller's, copied in and reset at free */
    i_RantRegistry  reg;
    i_RantDocDef   *defs;  uint32_t n_defs;
    RantSchemaNote *notes;
};

/* one comment as the attaching reads it: its line, whether it stands on a line of its own,
 * and its words without the -- and the spaces around them */
typedef struct { uint32_t line; int own; const char *text; size_t len; } i_RantDocLine;

/* everything a parse works on, freed or kept by the end */
typedef struct {
    const char *text;
    RantAllocFn alloc; void *user;
    uint32_t   *nl; uint32_t n_nl;              /* the offset of every newline */
    i_RantDocLine *lines; uint32_t n_lines;      /* the comments in text order */
    int oom;
} i_RantDocWork;

static void *i_rant_doc_new(i_RantDocWork *w, size_t size){
    void *p;
    if (w->oom || !size) return NULL;
    p = w->alloc(w->user, NULL, size);
    if (!p) w->oom = 1;
    return p;
}

/* the line of p, from 1 */
static uint32_t i_rant_doc_line(const i_RantDocWork *w, const char *p){
    uint32_t off = (uint32_t)(p - w->text), lo = 0, hi = w->n_nl;
    while (lo < hi){
        uint32_t mid = lo + (hi - lo) / 2u;
        if (w->nl[mid] < off) lo = mid + 1u; else hi = mid;
    }
    return lo + 1u;
}

/* the comment on line, or NULL */
static const i_RantDocLine *i_rant_doc_on(const i_RantDocWork *w, uint32_t line){
    uint32_t lo = 0, hi = w->n_lines;
    while (lo < hi){
        uint32_t mid = lo + (hi - lo) / 2u;
        if (w->lines[mid].line < line) lo = mid + 1u; else hi = mid;
    }
    return lo < w->n_lines && w->lines[lo].line == line ? &w->lines[lo] : NULL;
}

/* The run of comments on lines of their own that ends on the line above line, and starts
 * after floor. first is its top line, 0 when there is none. */
static uint32_t i_rant_doc_run(const i_RantDocWork *w, uint32_t line, const char *floor){
    uint32_t top = 0;
    while (line > 1){
        const i_RantDocLine *c = i_rant_doc_on(w, line - 1u);
        if (!c || !c->own || c->text < floor) break;
        top = --line;
    }
    return top;
}

/* Joins the run from top to the line above end, then tail when given, by newlines. */
static RantString i_rant_doc_join(i_RantDocWork *w, uint32_t top, uint32_t end,
                                  const i_RantDocLine *tail){
    size_t n = 0; uint32_t l; char *s, *o;
    if (top) for (l = top; l < end; l++) n += i_rant_doc_on(w, l)->len + 1u;
    if (tail) n += tail->len + 1u;
    if (!n) return rant_string(NULL, 0);
    s = o = (char *)i_rant_doc_new(w, n);
    if (!s) return rant_string(NULL, 0);
    if (top) for (l = top; l < end; l++){
        const i_RantDocLine *c = i_rant_doc_on(w, l);
        memcpy(o, c->text, c->len); o += c->len; *o++ = '\n';
    }
    if (tail){ memcpy(o, tail->text, tail->len); o += tail->len; *o++ = '\n'; }
    o[-1] = '\0';                                    /* the last newline becomes the end */
    return rant_string(s, n - 1u);
}

/* "[m or px] any point" is unit "m or px" and text "any point" */
static RantSchemaNote i_rant_doc_split(RantString s){
    RantSchemaNote n; size_t i;
    n.unit = rant_string(NULL, 0); n.text = s;
    if (!s.len || s.data[0] != '[') return n;
    for (i = 1; i < s.len && s.data[i] != ']' && s.data[i] != '\n'; i++) {}
    if (i >= s.len || s.data[i] != ']') return n;
    n.unit = rant_string(s.data + 1, i - 1u);
    for (i++; i < s.len && s.data[i] == ' '; i++) {}
    n.text = rant_string(s.data + i, s.len - i);
    return n;
}

static void i_rant_doc_place(const char *text, const char *at, RantSchemaError *err){
    uint32_t line = 1, column = 1; const char *p;
    for (p = text; p < at && *p; p++){
        if (*p == '\n'){ line++; column = 1; } else column++;
    }
    err->line = line; err->column = column;
}

static void i_rant_doc_fail(RantSchemaError *err, const char *why){
    if (err){ err->why = why; err->line = 0; err->column = 0; }
}

RantSchemaDoc *i_rant_schema_doc_parse(RantAllocator *alloc, const char *text, int standalone,
                                       RantSchemaError *err){
    RantAllocator pool; RantSchemaDoc *d; i_RantDocRec rec; i_RantDocWork w;
    i_RantSchemaErr ce; RantSchema *s; const char *p, *refused = NULL, *refused_at = NULL;
    uint32_t i, k, n_members = 0;

    if (err){ err->why = NULL; err->line = 0; err->column = 0; }
    if (!alloc || !text){ i_rant_doc_fail(err, "no allocator or no text"); return NULL; }
    pool = *alloc;                                   /* the doc's own pool from here on */
    d = (RantSchemaDoc *)rant_allocator_alloc(&pool, NULL, sizeof *d);
    if (!d){ rant_allocator_reset(&pool); i_rant_doc_fail(err, "out of memory"); return NULL; }
    memset(d, 0, sizeof *d);
    d->alloc = pool;
    i_rant_registry_init(&d->reg, rant_allocator_alloc, &d->alloc);
    d->reg.standalone = (uint8_t)(standalone != 0);

    memset(&rec, 0, sizeof rec);
    rec.alloc = rant_allocator_alloc; rec.user = &d->alloc;
    s = i_rant_registry_compile_rec(&d->reg, text, &ce, &rec);
    if (!s || rec.err){
        if (!s && err){
            err->why = ce.why;
            i_rant_doc_place(text, ce.at ? ce.at : text + strlen(text), err);
        } else i_rant_doc_fail(err, "out of memory");
        rant_schema_doc_free(d);
        return NULL;
    }

    memset(&w, 0, sizeof w);
    w.text = text; w.alloc = rant_allocator_alloc; w.user = &d->alloc;
    for (p = text; *p; p++) if (*p == '\n') w.n_nl++;
    w.nl = (uint32_t *)i_rant_doc_new(&w, (size_t)w.n_nl * sizeof *w.nl);
    if (w.nl) for (p = text, i = 0; *p; p++) if (*p == '\n') w.nl[i++] = (uint32_t)(p - text);
    w.lines = (i_RantDocLine *)i_rant_doc_new(&w, (size_t)rec.n_comments * sizeof *w.lines);
    for (i = 0; i < rec.n_marks; i++){
        if (rec.marks[i].what == I_RANT_DOC_DEF) d->n_defs++;
        else if (rec.marks[i].what == I_RANT_DOC_MEMBER) n_members++;
    }
    d->defs = (i_RantDocDef *)i_rant_doc_new(&w, (size_t)d->n_defs * sizeof *d->defs);
    d->notes = (RantSchemaNote *)i_rant_doc_new(&w, (size_t)n_members * sizeof *d->notes);
    if (w.oom){ rant_schema_doc_free(d); i_rant_doc_fail(err, "out of memory"); return NULL; }
    for (i = 0; i < rec.n_comments; i++){
        const char *a = rec.comments[i].text, *b = rec.comments[i].end, *q = a - 2;
        i_RantDocLine *l = &w.lines[w.n_lines++];
        while (q > text && (q[-1] == ' ' || q[-1] == '\t')) q--;
        l->own = q == text || q[-1] == '\n';
        while (a < b && (*a == ' ' || *a == '\t')) a++;
        while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r')) b--;
        l->line = i_rant_doc_line(&w, rec.comments[i].text);
        l->text = a; l->len = (size_t)(b - a);
    }

    /* the marks run in text order: a definition, its body, then its members */
    {
        i_RantDocDef *cur = NULL; const char *body = text; uint32_t def_line = 0, note = 0;
        for (i = 0, k = 0; i < rec.n_marks && !refused; i++){
            const i_RantDocMark *m = &rec.marks[i];
            if (m->what == I_RANT_DOC_DEF){
                char word[256], wire[256]; size_t n = i_rant_ident_len(m->at);
                if (n > sizeof word - 1u) n = sizeof word - 1u;   /* it compiled, so never */
                memcpy(word, m->at, n); word[n] = '\0';
                cur = &d->defs[k++];
                memset(cur, 0, sizeof *cur);
                def_line = i_rant_doc_line(&w, m->at);
                cur->line = def_line;
                cur->first_note = note;
                cur->about = i_rant_doc_join(&w, i_rant_doc_run(&w, def_line, text), def_line, NULL);
                body = m->at;
                /* the text checks the wire rules on its last statement alone, so a definition
                   nothing there uses can break one, and only compiling it by name shows it */
                if (i_rant_name_normalize(word, wire, sizeof wire, 1))
                    cur->schema = i_rant_registry_compile(&d->reg, wire, &ce);
                if (cur->schema)
                    cur->kind = cur->schema->value_root ? cur->schema->fields[0].kind : (uint8_t)RANT_STRUCT;
                else { refused = ce.why ? ce.why : "out of memory"; refused_at = m->at; }
            } else if (m->what == I_RANT_DOC_BODY){
                body = m->at;
            } else if (cur){
                uint32_t line = i_rant_doc_line(&w, m->at);
                const i_RantDocLine *tail = i_rant_doc_on(&w, line);
                uint32_t j;
                /* a comment after a member belongs to the last member that starts on its line */
                for (j = i + 1u; tail && j < rec.n_marks; j++){
                    if (rec.marks[j].what != I_RANT_DOC_MEMBER) continue;
                    if (i_rant_doc_line(&w, rec.marks[j].at) == line) tail = NULL;
                    break;
                }
                if (tail && (tail->own || tail->text < m->at)) tail = NULL;
                d->notes[note++] = i_rant_doc_split(
                    i_rant_doc_join(&w, i_rant_doc_run(&w, line, body), line, tail));
                cur->n_notes++;
            }
        }
    }
    if (refused){
        if (err){ err->why = refused; i_rant_doc_place(text, refused_at, err); }
        rant_schema_doc_free(d);
        return NULL;
    }
    if (w.oom){ rant_schema_doc_free(d); i_rant_doc_fail(err, "out of memory"); return NULL; }
    return d;
}

RantSchemaDoc *rant_schema_doc_parse(RantAllocator *alloc, const char *text, RantSchemaError *err){
    return i_rant_schema_doc_parse(alloc, text, 0, err);
}

uint32_t rant_schema_doc_count(const RantSchemaDoc *d){ return d ? d->n_defs : 0; }

int rant_schema_doc_at(const RantSchemaDoc *d, uint32_t i, RantSchemaDef *out){
    if (!d || !out || i >= d->n_defs) return 0;
    out->schema = d->defs[i].schema;
    out->kind   = d->defs[i].kind;
    out->about  = d->defs[i].about;
    out->line   = d->defs[i].line;
    return 1;
}

RantSchemaNote rant_schema_doc_note(const RantSchemaDoc *d, uint32_t def, uint16_t member){
    RantSchemaNote none;
    none.unit = rant_string(NULL, 0); none.text = rant_string(NULL, 0);
    if (!d || def >= d->n_defs || member >= d->defs[def].n_notes) return none;
    return d->notes[d->defs[def].first_note + member];
}

void rant_schema_doc_free(RantSchemaDoc *d){
    RantAllocator pool;
    if (!d) return;
    pool = d->alloc;                                 /* the doc lives in the pool it resets */
    rant_allocator_reset(&pool);
}
