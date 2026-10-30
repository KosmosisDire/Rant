#include "internal.h"
#include "stdtable.c"

/* the table is indexed by RantStdType minus 1, a mismatch would shift every name */
typedef char i_rant_std_table_check[
    (sizeof i_rant_std_table / sizeof i_rant_std_table[0] == (size_t)RANT_STD_COUNT - 1u) ? 1 : -1];

const char *rant_std_name(RantStdType t){
    if (t <= RANT_STD_NONE || t >= RANT_STD_COUNT) return 0;
    return i_rant_std_table[(int)t - 1].name;
}
const char *rant_std_text(RantStdType t){
    if (t <= RANT_STD_NONE || t >= RANT_STD_COUNT) return 0;
    return i_rant_std_table[(int)t - 1].text;
}

RantStdType rant_std_by_name(RantString name){
    int i;
    if (!name.data || !name.len) return RANT_STD_NONE;
    for (i = 0; i < (int)RANT_STD_COUNT - 1; i++){
        const char *n = i_rant_std_table[i].name;
        size_t k = strlen(n);
        if (k == name.len && memcmp(n, name.data, k) == 0) return (RantStdType)(i + 1);
    }
    return RANT_STD_NONE;
}

/* The one seam the DSL calls: an unknown type word is looked up here and compiled from its text. */
const char *i_rant_std_lookup(const char *name){
    int i;
    if (!name) return 0;
    for (i = 0; i < (int)RANT_STD_COUNT - 1; i++)
        if (strcmp(i_rant_std_table[i].name, name) == 0) return i_rant_std_table[i].text;
    return 0;
}

/* Does type match the registry's type bytes for a standard name. */
static int i_rant_std_shape_eq(i_RantRegistry *r, RantString name, RantBytes type){
    char nm[256]; const uint8_t *tb; size_t tl;
    if (!r || !type.data || name.len >= sizeof nm) return 0;
    memcpy(nm, name.data, name.len); nm[name.len] = '\0';
    if (!i_rant_registry_ref(r, nm, &tb, &tl)) return 0;
    return type.len == tl && memcmp(type.data, tb, tl) == 0;
}

/* peel a NAMED wrapper: fills *name and returns the inner type bytes */
static int i_rant_std_peel(RantBytes type, RantString *name, RantBytes *inner){
    size_t nl;
    if (!type.data || type.len < 2u || type.data[0] != RANT_NAMED) return 0;
    nl = type.data[1];
    if (2u + nl > type.len || nl == 0) return 0;
    *name  = rant_string((const char *)(type.data + 2), nl);
    *inner = rant_bytes(type.data + 2 + nl, type.len - 2 - nl);
    return 1;
}

static RantStdType i_rant_std_check(i_RantRegistry *r, RantString name, RantBytes inner){
    RantStdType t = rant_std_by_name(name);
    if (t == RANT_STD_NONE) return RANT_STD_NONE;
    return i_rant_std_shape_eq(r, name, inner) ? t : RANT_STD_NONE;
}

RantStdType i_rant_std_recognize(i_RantRegistry *r, const RantSchema *s){
    RantBytes w = rant_schema_wire(s);
    RantString nm = rant_schema_name(s);
    size_t off = 2u + nm.len;
    if (!w.data || off >= w.len) return RANT_STD_NONE;
    return i_rant_std_check(r, nm, rant_bytes(w.data + off, w.len - off));
}

RantStdType i_rant_std_recognize_field(i_RantRegistry *r, const RantSchema *s, uint16_t field){
    RantBytes type = rant_schema_field_type_wire(s, field), inner;
    RantString name;
    if (!i_rant_std_peel(type, &name, &inner)) return RANT_STD_NONE;
    return i_rant_std_check(r, name, inner);
}
