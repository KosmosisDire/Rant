/* Schemas without a node: the DSL text against the builder wire, layout and refusals, and
 * the schema wire with named types, struct arrays and print round trips.
 * spec/testing.md says what each case pins. */
#include "support/test.h"

/* rant_schema_print is the inverse of compile: print a schema back to DSL, recompile that
   text, and assert an exact wire round-trip plus that the (NULL,0) measure matches. */
static void schema_print_roundtrip(RantAllocator *ma, RantSchema *s, const char *label){
    char buf[1024]; uint32_t need; RantSchema *back;
    if (!s) return;
    need = rant_schema_print(s, NULL, 0);                   /* snprintf-style measure */
    rant_schema_print(s, buf, sizeof buf);
    CHECK(need > 0 && need < sizeof buf && strlen(buf) == (size_t)need,
             "schema-print: %s measures (%u) and matches the written length", label, need);
    back = test_compile(ma, buf, NULL);
    CHECK(back && rant_schema_hash(back) == rant_schema_hash(s),
             "schema-print: %s DSL recompiles to the same wire (hash)", label);
}

/* The schema DSL: text compiles to the same wire bytes (hence hash) the builder
   emits, layout comes out right, and malformed text fails with a useful position. */
static void schema_dsl_checks(void){
    RantAllocator ma = rant_allocator_heap(0);
    static const char POSE[] =
        "Reading\n"
        "{\n"
        "    stamp:    u64,\n"
        "    x:        f64,\n"
        "    y:        f64,\n"
        "    uuid:     u8[16],   -- fixed array\n"
        "    tagCount: u8,\n"
        "    velocity: { dx: f32, dy: f32 }\n"
        "}";
    RantSchema *txt;
    txt = test_compile(&ma, POSE, NULL);
    CHECK(txt != NULL, "schema-dsl: compiles");
    /* pinned: the wire of a nested struct plus a fixed array must never drift */
    CHECK(txt && rant_schema_hash(txt) == 0xb8157f3bb2e197e7ULL,
             "schema-dsl: Reading canonical hash %016llx", (unsigned long long)(txt ? rant_schema_hash(txt) : 0));
    schema_print_roundtrip(&ma, txt, "Reading (nested struct + array)");
    if (txt){
        RantSchemaFieldInfo fi;
        CHECK(rant_schema_size(txt) == 8+8+8+16+1+8, "schema-dsl: size %u", rant_schema_size(txt));
        CHECK(rant_schema_field_count(txt) == 8, "schema-dsl: 8 flat fields (6 top + 2 nested)");
        CHECK(rant_schema_field_index(txt, "tagCount") == 4
              && rant_schema_field_index(txt, "velocity.dx") == 6
              && rant_schema_field_index(txt, "dx") == -1,      /* nested needs its path */
                 "schema-dsl: index by name incl. dotted paths");
        CHECK(rant_schema_field_at(txt, 3, &fi) && fi.kind == RANT_ARR
                 && fi.elem == RANT_U8 && fi.count == 16 && fi.offset == 24 && fi.size == 16,
                 "schema-dsl: array field info (off=%u size=%u count=%u)", fi.offset, fi.size, fi.count);
        CHECK(rant_schema_field_at(txt, 5, &fi) && fi.kind == RANT_STRUCT && fi.size == 8
                 && fi.offset == 41 && fi.depth == 0,
                 "schema-dsl: struct field info (off=%u size=%u)", fi.offset, fi.size);
        CHECK(rant_schema_field_at(txt, 7, &fi) && fi.kind == RANT_F32
                 && fi.offset == 45 && fi.depth == 1,
                 "schema-dsl: nested member is flattened (off=%u depth=%u)", fi.offset, fi.depth);
    }
    if (txt){   /* setters: build a message BY NAME, read it back through the getters */
        uint8_t m[49], uuid[16]; int i, ok;
        memset(m, 0xAA, sizeof m);   /* dirty: the set fields must fully determine it */
        for (i = 0; i < 16; i++) uuid[i] = (uint8_t)i;
        ok  = rant_set_uint (m, sizeof m, txt, "stamp", 42);
        ok &= rant_set_f64    (m, sizeof m, txt, "x", 1.5);
        ok &= rant_set_f64    (m, sizeof m, txt, "y", -2.5);
        ok &= rant_set_array(m, sizeof m, txt, "uuid", rant_bytes(uuid, 5));     /* short write */
        ok &= rant_set_uint (m, sizeof m, txt, "tagCount", 300);   /* narrows like a cast */
        ok &= rant_set_f32    (m, sizeof m, txt, "velocity.dy", 7.5f);           /* nested by path */
        CHECK(ok, "schema-dsl: setters accept (incl. nested path)");
        CHECK(rant_get_uint(rant_bytes(m,sizeof m), txt, "stamp") == 42
              && rant_get_f64 (rant_bytes(m,sizeof m), txt, "y") == -2.5
              && rant_get_uint(rant_bytes(m,sizeof m), txt, "tagCount") == (300u & 0xFF)
              && rant_get_f32 (rant_bytes(m,sizeof m), txt, "velocity.dy") == 7.5f,
                 "schema-dsl: getters read the setters back (incl. nested path)");
        {   RantBytes a = rant_get_array(rant_bytes(m,sizeof m), txt, "uuid");
            CHECK(a.len == 16 && a.data[4] == 4 && a.data[5] == 0 && a.data[15] == 0,
                     "schema-dsl: short array write zero-fills the tail");
        }
        {   RantValue v;                              /* reflection access by flat index */
            CHECK(rant_get_value(rant_bytes(m,sizeof m), txt, 7, &v)
                     && v.kind == RANT_F32 && v.v.f == 7.5,
                     "schema-dsl: rant_get_value reads the nested member");
            v.v.u = 9;
            CHECK(rant_set_value(m, sizeof m, txt, 4, &v)
                     && rant_get_uint(rant_bytes(m,sizeof m), txt, "tagCount") == 9,
                     "schema-dsl: rant_set_value writes by index");
        }
        CHECK(!rant_set_uint (m, sizeof m, txt, "x", 1)   /* f64: wrong family */
              && !rant_set_f64    (m, sizeof m, txt, "velocity", 0.0)   /* struct: no setter */
              && !rant_set_array(m, sizeof m, txt, "uuid", rant_bytes(uuid, 17))     /* overflow */
              && !rant_set_uint (m, 8, txt, "x", 1)                               /* short buffer */
              && !rant_set_uint (m, sizeof m, txt, "nope", 1),   /* unknown field */
                 "schema-dsl: bad sets refused");
    }
    {   /* strings: string<cap> and string<cap>[N] are fixed slots of [u16 len][cap bytes] */
        static const char TAGGED[] =
            "Tagged { id: u32, name: string<12>, labels: string<8>[3], meta: { note: string<4> } }";
        RantSchema *ts;
        ts = test_compile(&ma, TAGGED, NULL);
        CHECK(ts != NULL, "schema-dsl: strings compile");
        CHECK(ts && rant_schema_hash(ts) == 0x36a9acceb68c37c9ULL,
                 "schema-dsl: Tagged canonical hash %016llx", (unsigned long long)(ts ? rant_schema_hash(ts) : 0));
        schema_print_roundtrip(&ma, ts, "Tagged (capped strings + string array)");
        if (ts){
            RantSchemaFieldInfo fi;
            CHECK(rant_schema_size(ts) == 4 + (2+12) + 3*(2+8) + (2+4),
                     "schema-dsl: string sizes (%u)", rant_schema_size(ts));
            CHECK(rant_schema_field_at(ts, 1, &fi) && fi.kind == RANT_STR
                     && fi.str_cap == 12 && fi.offset == 4 && fi.size == 14,
                     "schema-dsl: string field info (off=%u size=%u cap=%u)",
                     fi.offset, fi.size, fi.str_cap);
            CHECK(rant_schema_field_at(ts, 2, &fi) && fi.kind == RANT_ARR && fi.elem == RANT_STR
                     && fi.count == 3 && fi.str_cap == 8 && fi.size == 30,
                     "schema-dsl: string array field info (size=%u cap=%u)", fi.size, fi.str_cap);
        }
        if (ts){
            uint8_t m[54]; RantString v; int ok;
            ok  = rant_schema_message_default(ts, m, sizeof m);
            ok &= rant_set_uint    (m, sizeof m, ts, "id", 7);
            ok &= rant_set_string(m, sizeof m, ts, "name", rant_string("robot-1", 7));
            ok &= rant_set_string_at(m, sizeof m, ts, "labels", 0, rant_string("fast", 4));
            ok &= rant_set_string_at(m, sizeof m, ts, "labels", 2, rant_string("red", 3));
            ok &= rant_set_string(m, sizeof m, ts, "meta.note", rant_string("ok", 2));
            CHECK(ok, "schema-dsl: string setters accept (incl. nested + indexed)");
            v = rant_get_string(rant_bytes(m,sizeof m), ts, "name");
            CHECK(v.len == 7 && memcmp(v.data, "robot-1", 7) == 0,
                     "schema-dsl: string round-trips");
            v = rant_get_string_at(rant_bytes(m,sizeof m), ts, "labels", 2);
            CHECK(v.len == 3 && memcmp(v.data, "red", 3) == 0,
                     "schema-dsl: string array element round-trips");
            v = rant_get_string_at(rant_bytes(m,sizeof m), ts, "labels", 1);
            CHECK(v.len == 0 && v.data != NULL, "schema-dsl: unset string element is empty");
            v = rant_get_string(rant_bytes(m,sizeof m), ts, "meta.note");
            CHECK(v.len == 2 && memcmp(v.data, "ok", 2) == 0,
                     "schema-dsl: nested string round-trips");
            {   RantValue dv;     /* reflection sees the live bytes + the cap */
                CHECK(rant_get_value(rant_bytes(m,sizeof m), ts, 1, &dv)
                         && dv.kind == RANT_STR && dv.str_cap == 12
                         && dv.bytes.len == 7 && memcmp(dv.bytes.data, "robot-1", 7) == 0,
                         "schema-dsl: rant_get_value yields the live string");
            }
            CHECK(!rant_set_string(m, sizeof m, ts, "name", rant_string("a-name-too-long", 15))
                  && !rant_set_string(m, sizeof m, ts, "id", rant_string("x", 1))   /* not string */
                  && !rant_set_string_at(m, sizeof m, ts, "labels", 3, rant_string("x", 1)),
                     "schema-dsl: bad string sets refused");
            {   /* a hostile length prefix reads back clamped to the cap */
                RantSchemaFieldInfo fi;
                rant_schema_field_at(ts, 1, &fi);
                m[fi.offset] = 0xFF; m[fi.offset + 1] = 0xFF;          /* len = 65535 */
                v = rant_get_string(rant_bytes(m,sizeof m), ts, "name");
                CHECK(v.len == 12, "schema-dsl: hostile string length clamps to cap (%u)",
                         (unsigned)v.len);
            }
        }
        {   RantSchema *r_ok    = test_compile(&ma, "Tagged { name: string<12> }", NULL);
            RantSchema *r_bad = test_compile(&ma, "Tagged { name: string<10> }", NULL);
            CHECK(r_ok && r_bad && ts && rant_schema_subset(r_ok, ts) && !rant_schema_subset(r_bad, ts),
                     "schema-dsl: string subset needs the same cap");
        }
    }
    {   /* variable fields ride the tail as [u32 len] frames in schema order, fixed offsets
           are unaffected */
        static const char VDSL[] =
            "Var { id: u32, note: string, samples: f32[], labels: string<6>[], extras: map, tail: u8 }";
        RantSchema *vs;
        vs = test_compile(&ma, VDSL, NULL);
        CHECK(vs != NULL, "schema-var: compiles");
        CHECK(vs && rant_schema_hash(vs) == 0x1c5e7551f4343dbfULL,
                 "schema-var: Var canonical hash %016llx", (unsigned long long)(vs ? rant_schema_hash(vs) : 0));
        schema_print_roundtrip(&ma, vs, "Var (variable string/array/map)");
        if (vs){
            RantSchemaFieldInfo fi;
            CHECK(rant_schema_size(vs) == 5 && rant_schema_msg_min(vs) == 5 + 4*4,
                     "schema-var: fixed size %u, msg_min %u",
                     rant_schema_size(vs), rant_schema_msg_min(vs));
            CHECK(rant_schema_field_at(vs, 5, &fi) && fi.kind == RANT_U8 && fi.offset == 4,
                     "schema-var: fixed fields pack around the variable ones (off=%u)", fi.offset);
            CHECK(rant_schema_field_at(vs, 1, &fi) && fi.kind == RANT_VSTR
                     && fi.offset == 0 && fi.size == 0,
                     "schema-var: variable field reports no static offset");
            CHECK(rant_schema_field_at(vs, 3, &fi) && fi.kind == RANT_VARR
                     && fi.elem == RANT_STR && fi.str_cap == 6 && fi.count == 0,
                     "schema-var: string<6>[] field info (cap=%u)", fi.str_cap);
        }
        if (vs){
            uint8_t m[256], smp[8], slots[16], tmp[128], big[250];
            uint32_t blen, len; int ok; RantString v; RantBytes a, mb;
            {   int i; for (i = 0; i < 8; i++) smp[i] = (uint8_t)(i + 1); }
            memset(slots, 0, sizeof slots);              /* two empty string<6> slots */
            memset(big, 'x', sizeof big);

            ok = rant_schema_message_default(vs, m, sizeof m);
            CHECK(ok && rant_schema_msg_len(vs, m, sizeof m) == rant_schema_msg_min(vs)
                     && rant_schema_validate(vs, rant_bytes(m, rant_schema_msg_min(vs))),
                     "schema-var: the default message is all empty frames and validates");
            ok  = rant_set_uint(m, sizeof m, vs, "id", 7);
            ok &= rant_set_uint(m, sizeof m, vs, "tail", 9);
            ok &= rant_set_array(m, sizeof m, vs, "samples", rant_bytes(smp, 8));    /* 2 live f32 */
            ok &= rant_set_string(m, sizeof m, vs, "note", rant_string("hello, tail", 11));
            ok &= rant_set_array(m, sizeof m, vs, "labels", rant_bytes(slots, 16));
            ok &= rant_set_string_at(m, sizeof m, vs, "labels", 1, rant_string("red", 3));
            CHECK(ok, "schema-var: variable setters accept (out of schema order)");
            CHECK(rant_get_uint(rant_bytes(m,sizeof m), vs, "id") == 7
                  && rant_get_uint(rant_bytes(m,sizeof m), vs, "tail") == 9,
                     "schema-var: fixed fields intact after tail resizes");
            v = rant_get_string(rant_bytes(m,sizeof m), vs, "note");
            CHECK(v.len == 11 && memcmp(v.data, "hello, tail", 11) == 0,
                     "schema-var: variable string round-trips");
            a = rant_get_array(rant_bytes(m,sizeof m), vs, "samples");
            CHECK(a.len == 8 && memcmp(a.data, smp, 8) == 0,
                     "schema-var: variable array survives an earlier frame's resize");
            v = rant_get_string_at(rant_bytes(m,sizeof m), vs, "labels", 1);
            CHECK(v.len == 3 && memcmp(v.data, "red", 3) == 0,
                     "schema-var: variable string-array element round-trips");
            CHECK(!rant_set_string_at(m, sizeof m, vs, "labels", 2, rant_string("x", 1)),
                     "schema-var: index past the live count refused");
            {   RantValue dv;                        /* reflection sees the live extent */
                CHECK(rant_get_value(rant_bytes(m,sizeof m), vs, 2, &dv)
                         && dv.kind == RANT_VARR && dv.elem == RANT_F32
                         && dv.count == 2 && dv.bytes.len == 8,
                         "schema-var: rant_get_value yields the live element count");
            }

            {   RantMapWriter w = rant_map_begin(tmp, sizeof tmp);       /* the escape hatch */
                rant_map_put_uint(&w, "battery", 87);
                rant_map_put_string(&w, "state", rant_string("docked", 6));
                rant_map_open_array(&w, "temps");
                rant_map_put_f64(&w, NULL, 36.5);
                rant_map_put_int(&w, NULL, -3);
                rant_map_close(&w);
                rant_map_open_map(&w, "pose");
                rant_map_put_f64(&w, "x", 1.5);
                rant_map_close(&w);
                blen = rant_map_finish(&w);
            }
            CHECK(blen > 0 && rant_set_map(m, sizeof m, vs, "extras", rant_bytes(tmp, blen)),
                     "schema-var: map writes and installs");
            mb = rant_get_map(rant_bytes(m,sizeof m), vs, "extras");
            CHECK(mb.data && mb.len == blen && rant_map_count(mb) == 4,
                     "schema-var: map body round-trips (%u entries)", rant_map_count(mb));
            {   RantValue dv, e0, e1;
                CHECK(rant_map_get(mb, "battery", &dv) && dv.kind == RANT_U8 && dv.v.u == 87,
                         "schema-var: map uint stores in the smallest kind");
                CHECK(rant_map_get(mb, "state", &dv) && dv.kind == RANT_VSTR
                         && dv.bytes.len == 6 && memcmp(dv.bytes.data, "docked", 6) == 0,
                         "schema-var: map string");
                CHECK(rant_map_get(mb, "temps", &dv) && dv.kind == RANT_VARR && dv.count == 2
                      && rant_map_array_at(dv.bytes, 0, &e0) && e0.kind == RANT_F64 && e0.v.f == 36.5
                      && rant_map_array_at(dv.bytes, 1, &e1) && e1.kind == RANT_I8 && e1.v.i == -3,
                         "schema-var: map array elements (heterogeneous)");
                CHECK(rant_map_get(mb, "pose", &dv) && dv.kind == RANT_MAP
                      && rant_map_count(dv.bytes) == 1
                      && rant_map_get(dv.bytes, "x", &e0) && e0.v.f == 1.5,
                         "schema-var: nested map recurses");
                CHECK(!rant_map_get(mb, "nope", &dv), "schema-var: missing key misses");
            }
            CHECK(!rant_map_valid(rant_bytes(tmp, blen - 1))
                  && !rant_set_map(m, sizeof m, vs, "extras", rant_bytes(tmp, blen - 1)),
                     "schema-var: truncated map body refused");

            len = rant_schema_msg_len(vs, m, sizeof m);
            CHECK(len == 5u + (4+11) + (4+8) + (4+16) + (4+blen),
                     "schema-var: live length (%u)", len);
            CHECK(rant_schema_validate(vs, rant_bytes(m, len))
                  && !rant_schema_validate(vs, rant_bytes(m, len - 1))
                  && !rant_schema_validate(vs, rant_bytes(m, len + 1)),
                     "schema-var: frames must consume the message exactly");
            CHECK(!rant_set_string(m, sizeof m, vs, "note", rant_string((char *)big, 250)),
                     "schema-var: a frame that would exceed the buffer is refused");
            v = rant_get_string(rant_bytes(m,sizeof m), vs, "note");
            CHECK(v.len == 11, "schema-var: refused set leaves the message untouched");
            ok = rant_set_string(m, sizeof m, vs, "note", rant_string("hi", 2));
            a = rant_get_array(rant_bytes(m,sizeof m), vs, "samples");
            CHECK(ok && a.len == 8 && memcmp(a.data, smp, 8) == 0
                     && rant_get_uint(rant_bytes(m,sizeof m), vs, "tail") == 9,
                     "schema-var: shrinking a frame memmoves the tail intact");

            {   /* subset + rebase: the reader skips frames it does not declare */
                RantSchema *sub = test_compile(&ma,
                    "Var { tail: u8, samples: f32[], extras: map }", NULL);
                RantSchema *bad1 = test_compile(&ma,
                    "Var { note: string<8> }", NULL);     /* capped vs variable */
                RantSchema *bad2 = test_compile(&ma,
                    "Var { samples: f64[] }", NULL);      /* element kind differs */
                CHECK(sub && rant_schema_subset(sub, vs), "schema-var: variable subset matches");
                CHECK(bad1 && !rant_schema_subset(bad1, vs)
                      && bad2 && !rant_schema_subset(bad2, vs),
                         "schema-var: capped-vs-variable and element mismatches refused");
                {   RantSchema *rb = i_rant_schema_rebase(sub, vs, rant_allocator_alloc, &ma);
                    CHECK(rb != NULL, "schema-var: rebase");
                    if (rb){
                        uint32_t rlen = rant_schema_msg_len(rb, m, sizeof m);
                        CHECK(rlen == rant_schema_msg_len(vs, m, sizeof m)
                              && rant_schema_validate(rb, rant_bytes(m, rlen)),
                                 "schema-var: rebased schema walks the writer's frames");
                        CHECK(rant_get_uint(rant_bytes(m, rlen), rb, "tail") == 9,
                                 "schema-var: rebased fixed offset");
                        a = rant_get_array(rant_bytes(m, rlen), rb, "samples");
                        CHECK(a.len == 8 && memcmp(a.data, smp, 8) == 0,
                                 "schema-var: rebased variable ordinal finds the right frame");
                        mb = rant_get_map(rant_bytes(m, rlen), rb, "extras");
                        CHECK(mb.data && rant_map_count(mb) == 4,
                                 "schema-var: rebased map field reads");
                    }
                }
            }
        }
    }
    {   /* schema-print: two struct levels deep, and a struct followed by more top-level fields */
        RantSchema *deep = test_compile(&ma,
            "Deep { a: u32, g: { b: u16, inner: { c: i8, d: f64 }, e: u8 }, z: string<5>, arr: i32[3] }", NULL);
        schema_print_roundtrip(&ma, deep, "Deep (two nesting levels)");
    }
    {   /* errors: NULL + err points into the text at the offending spot */
        static const char *bad[] = {
            "Reading { x: f65 }",              /* unknown type */
            "Reading { x f64 }",               /* missing ':' */
            "Reading { x: f64 ",               /* missing '}' */
            "Reading { x: u8[0] }",            /* zero count */
            "Reading { x: u8[70000] }",        /* count > u16 */
            "Reading { x: f64 } y",            /* trailing garbage */
            "Reading = { x: f64 }",            /* a struct is defined as Name { }, never Name = { } */
            "Reading { x: f64 }[2]",           /* a definition takes no array suffix */
            "Reading { x: string[] }",   /* ragged: an array of unbounded strings is a map's job */
            "Reading { x: string[4] }",        /* a fixed string array needs its <cap> */
            "Reading { x: string<0> }",        /* zero cap */
            "Reading { x: string<12 }",        /* missing '>' */
            "Reading { v: { y: u8[] }[2] }",   /* variable field inside an array element */
            "Reading { m: map[3] }",           /* a map has no element form */
            "Reading { m: enum<u8> { A=300 } }",  /* value out of the backing range */
            "Reading { m: enum<f32> { A=0 } }",   /* non-integer backing */
            "Reading { m: enum<u8> A=0 }",        /* missing '{' */
            "Reading { m: enum { A } }"           /* missing <backing> */
        };
        unsigned i, ok = 1;
        for (i = 0; i < sizeof bad / sizeof bad[0]; i++){
            i_RantSchemaErr ep = { NULL, NULL };
            RantSchema *s = test_compile(&ma, bad[i], &ep);
            if (s || !ep.at || !ep.why || ep.at < bad[i] || ep.at > bad[i] + strlen(bad[i])) ok = 0;
        }
        CHECK(ok, "schema-dsl: malformed text rejected with a reason and a position");
    }
    {   /* a shape rule the wire check enforces says which rule, never out of memory */
        static const struct { const char *text, *why; } rules[] = {
            { "Reading { v: { y: u8[] }[2] }",   "a variable array cannot sit inside an array element" },
            { "Reading { v: { s: string }[2] }", "a variable string or a map cannot sit inside an array element" },
        };
        unsigned i, ok = 1;
        for (i = 0; i < sizeof rules / sizeof rules[0]; i++){
            i_RantSchemaErr ep = { NULL, NULL };
            RantSchema *s = test_compile(&ma, rules[i].text, &ep);
            if (s || !ep.why || strcmp(ep.why, rules[i].why) != 0) ok = 0;
        }
        CHECK(ok, "schema-dsl: a broken shape rule is named in the refusal");
    }
    {   RantSchema *anon = test_compile(&ma, "{ x: f64 }", NULL);
        CHECK(anon && rant_schema_name(anon).len == 0 && rant_schema_field_index(anon, "x") == 0,
                 "schema-dsl: a struct body on its own is an anonymous struct");
    }
    {   /* schema enum: a named integer is a fixed field carrying its backing scalar. The name
           table is schema only, so an unknown value stays readable and subset compares the width. */
        static const char *EDSL =
            "Robot { id: u32,"
            " mode: enum<u8> { Idle=0, Running=1, Charging=2, Fault=3 },"
            " step: enum<i8> { Back=-1, Hold, Fwd } }";   /* auto: Hold=0, Fwd=1 */
        RantSchema *es = test_compile(&ma, EDSL, NULL);
        CHECK(es != NULL, "schema-enum: compiles");
        if (es){
            RantSchemaFieldInfo fi; int mi = rant_schema_field_index(es, "mode");
            uint8_t m[32]; RantString nm; int64_t vv; RantValue dv;
            CHECK(rant_schema_field_at(es, (uint16_t)mi, &fi) && fi.kind == RANT_ENUM
                     && fi.elem == RANT_U8 && fi.count == 4 && fi.size == 1,
                     "schema-enum: reflects as ENUM (backing=elem, options=count, size=1)");
            CHECK(rant_schema_size(es) == 4u + 1u + 1u, "schema-enum: fixed size %u", rant_schema_size(es));
            CHECK(rant_schema_enum_count(es, (uint16_t)mi) == 4
                     && rant_schema_enum_variant(es, (uint16_t)mi, 2, &vv, &nm)
                     && vv == 2 && nm.len == 8 && memcmp(nm.data, "Charging", 8) == 0,
                     "schema-enum: variant listing");
            {   int si = rant_schema_field_index(es, "step");    /* signed + auto-increment */
                CHECK(rant_schema_enum_variant(es, (uint16_t)si, 0, &vv, &nm) && vv == -1
                      && rant_schema_enum_variant(es, (uint16_t)si, 2, &vv, &nm) && vv == 1,
                         "schema-enum: signed backing + auto-increment"); }
            CHECK(rant_enum_name_of(es, (uint16_t)mi, 3).len == 5
                     && rant_enum_name_of(es, (uint16_t)mi, 99).data == NULL
                     && rant_enum_value_of(es, (uint16_t)mi, "Running", &vv) && vv == 1
                     && !rant_enum_value_of(es, (uint16_t)mi, "Nope", &vv),
                     "schema-enum: name<->value resolvers");

            rant_schema_message_default(es, m, sizeof m);
            rant_set_uint(m, sizeof m, es, "mode", 1);                 /* by number */
            CHECK(rant_set_enum(m, sizeof m, es, "step", "Fwd")     /* by name */
                     && !rant_set_enum(m, sizeof m, es, "step", "Bad"),
                     "schema-enum: set by name (unknown refused)");
            nm = rant_get_enum(rant_bytes(m, sizeof m), es, "mode");
            CHECK(rant_get_uint(rant_bytes(m,sizeof m), es, "mode") == 1
                     && nm.len == 7 && memcmp(nm.data, "Running", 7) == 0,
                     "schema-enum: read number + label");
            CHECK(rant_get_int(rant_bytes(m,sizeof m), es, "step") == 1,
                     "schema-enum: signed value reads back");
            rant_set_uint(m, sizeof m, es, "mode", 42);                /* unknown value */
            CHECK(rant_get_uint(rant_bytes(m,sizeof m), es, "mode") == 42
                     && rant_get_enum(rant_bytes(m,sizeof m), es, "mode").data == NULL,
                     "schema-enum: an unknown/newer value stays readable, name empty");
            rant_set_uint(m, sizeof m, es, "mode", 2);
            CHECK(rant_get_value(rant_bytes(m,sizeof m), es, (uint16_t)mi, &dv)
                     && dv.kind == RANT_ENUM && dv.elem == RANT_U8 && dv.count == 4 && dv.v.i == 2,
                     "schema-enum: rant_get_value carries the number + backing/options");

            schema_print_roundtrip(&ma, es, "Robot (enum fields)");   /* prints, recompiles */

            {   /* subset: the same width is compatible despite a different option table, a width
                   mismatch is refused */
                RantSchema *rd = test_compile(&ma,
                    "Robot { mode: enum<u8> { Idle=0, Down=7 } }", NULL);   /* fewer options */
                RantSchema *bw = test_compile(&ma,
                    "Robot { mode: enum<u16> { Idle=0 } }", NULL);            /* wrong width */
                CHECK(rd && rant_schema_subset(rd, es), "schema-enum: same-width subset (names advisory)");
                CHECK(bw && !rant_schema_subset(bw, es), "schema-enum: backing-width mismatch refused");
            }
        }
    }
    rant_allocator_reset(&ma);
}

/* The schema wire, pure: named types, alias roots, struct element arrays, variable
   members at any depth, and the static stride refusals. Everything round trips print. */
static RantAllocator sv_ma;
static RantSchema *sv(const char *text){                      /* compile, or NULL */
    return test_compile(&sv_ma, text, NULL);
}
static void sv_free(RantSchema *s){ if (s) i_rant_schema_free(s, rant_allocator_alloc, &sv_ma); }
/* rant_schema_subset over two texts: 1 yes, 0 refused, -1 a text failed to compile */
static int sv_sub(const char *sub, const char *pub){
    RantSchema *a = sv(sub), *b = sv(pub); int r = -1;
    if (a && b) r = rant_schema_subset(a, b);
    sv_free(a); sv_free(b);
    return r;
}
static int sv_roundtrip(const char *text){                  /* print, recompile, the same wire */
    RantSchema *a = sv(text), *b; char buf[2048]; int ok;
    if (!a) return 0;
    rant_schema_print(a, buf, sizeof buf);
    b = sv(buf);
    ok = b && rant_schema_hash(b) == rant_schema_hash(a)
           && rant_schema_print(a, NULL, 0) == (uint32_t)strlen(buf);
    sv_free(b); sv_free(a);
    return ok;
}
static void schema_v8_checks(void){
    sv_ma = rant_allocator_heap(0);

    /* named types NARROW: unnamed reads named, named demands the same name */
    {   RantSchema *c = sv("Celsius = f32"), *f = sv("Fahrenheit = f32"), *bare = sv("f32");
        CHECK(c && f && bare, "schema-v8: alias roots compile");
        if (c && f && bare){
            CHECK(rant_schema_name(c).len == 7 && rant_schema_hash(c) != rant_schema_hash(f)
                     && rant_schema_hash(c) != rant_schema_hash(bare),
                     "schema-v8: an alias root carries its name and its own identity");
            CHECK(rant_schema_subset(bare, c) == 1 && rant_schema_subset(c, bare) == 0
                     && rant_schema_subset(c, f) == 0 && rant_schema_subset(c, c) == 1,
                     "schema-v8: alias roots narrow (unnamed reads named, never the reverse)");
            CHECK(rant_schema_size(c) == 4,
                     "schema-v8: an alias costs no message bytes (%u)", rant_schema_size(c));
        }
        sv_free(c); sv_free(f); sv_free(bare);
    }
    CHECK(sv_sub("A { t: f32 }", "Celsius = f32\nA { t: Celsius }") == 1,
             "schema-v8: an unnamed field reads a named writer field (unwrap is free)");
    CHECK(sv_sub("Celsius = f32\nA { t: Celsius }", "A { t: f32 }") == 0,
             "schema-v8: a named field refuses an unnamed writer field");
    CHECK(sv_sub("Celsius = f32\nA { t: Celsius }", "Fahrenheit = f32\nA { t: Fahrenheit }") == 0,
             "schema-v8: two different names over one shape never cross-wire");
    CHECK(sv_sub("Celsius = f32\nA { t: Celsius }", "Celsius = f32\nA { t: Celsius, u: u8 }") == 1,
             "schema-v8: subset stays top-level under named fields");
    CHECK(sv_sub("A { v: { a: f32 } }", "A { v: { a: f32, b: f32 } }") == 0,
             "schema-v8: a nested struct still compares exactly");
    CHECK(sv_sub("A { m: enum<u8> { P } }", "A { m: enum<u8> { Q, R } }") == 1
             && sv_sub("A { m: enum<u8> { P } }", "A { m: enum<u16> { P } }") == 0,
             "schema-v8: enum option names stay advisory, the backing width gates");
    CHECK(sv_sub("A { x: f32 }", "B { x: f32 }") == 0,
             "schema-v8: struct root names stay strict-equal");

    /* struct-element arrays: one element-0 template, indexed paths */
    {   RantSchema *s = sv("A { p: Float3[3], q: { m: i16 }[] }");
        RantSchemaFieldInfo fi, fx;
        uint8_t m[256]; RantBytes b; int xi;
        CHECK(s != NULL, "schema-v8: struct arrays compile");
        if (s){
            CHECK(rant_schema_field_count(s) == 6,
                     "schema-v8: a struct array flattens its element once (%u fields)",
                     rant_schema_field_count(s));
            rant_schema_field_at(s, 0, &fi);
            rant_schema_field_at(s, 1, &fx);
            CHECK(fi.kind == RANT_ARR && fi.elem == RANT_STRUCT && fi.count == 3
                     && fi.elem_size == 12 && fi.size == 36 && fi.elem_name.len == 6,
                     "schema-v8: Float3[3] reports element kind, stride and name");
            CHECK(fx.depth == 1 && fx.arr_parent == 0 && fx.offset == 0,
                     "schema-v8: its members point back at the array (arr_parent=%u)", fx.arr_parent);
            CHECK(rant_schema_size(s) == 36, "schema-v8: the fixed array packs inline (%u)",
                     rant_schema_size(s));
            xi = rant_schema_field_index(s, "p[2].z");
            CHECK(xi == rant_schema_field_index(s, "p.z") && xi > 0,
                     "schema-v8: an indexed path resolves to the element template");
            CHECK(rant_schema_message_default(s, m, sizeof m), "schema-v8: default message");
            CHECK(rant_set_f32(m, sizeof m, s, "p[2].z", 7.25f)
                     && rant_set_f32(m, sizeof m, s, "p[0].x", 1.5f)
                     && rant_set_f32(m, sizeof m, s, "p[3].z", 1.0f) == 0,
                     "schema-v8: fixed-array element writes land, out of range refuses");
            CHECK(rant_set_array_count(m, sizeof m, s, "q", 4)
                     && rant_set_int(m, sizeof m, s, "q[3].m", -9)
                     && rant_set_int(m, sizeof m, s, "q[4].m", 1) == 0,
                     "schema-v8: a variable struct array grows, then bounds its elements");
            b = rant_bytes(m, rant_schema_msg_len(s, m, sizeof m));
            CHECK(rant_get_f32(b, s, "p[2].z") == 7.25f && rant_get_f32(b, s, "p[0].x") == 1.5f
                     && rant_get_f32(b, s, "p[1].x") == 0.0f,
                     "schema-v8: fixed-array elements read back independently");
            CHECK(rant_get_int(b, s, "q[3].m") == -9 && rant_get_array_count(b, s, "q") == 4
                     && rant_get_array_count(b, s, "p") == 3,
                     "schema-v8: variable-array elements read back, counts report live");
            CHECK(rant_schema_validate(s, b), "schema-v8: the struct-array message validates");
            {   RantValue v; int ok;
                memset(&v, 0, sizeof v); v.kind = RANT_F32; v.v.f = 4.5;
                uint32_t one = 1;
                ok = rant_set_value_at(m, sizeof m, s, (uint16_t)xi, &one, 1, &v)
                     && rant_get_value_at(b, s, (uint16_t)xi, &one, 1, &v) && v.v.f == 4.5
                     && !rant_get_value_at(b, s, (uint16_t)xi, NULL, 0, &v)
                     && rant_array_count_at(b, s, 0) == 3;
                CHECK(ok, "schema-v8: reflection get/set_value_at index an element");
            }
        }
        sv_free(s);
    }

    /* a struct array inside a struct array's element, one index per level */
    {   RantSchema *s = sv("T { rows: { cells: { v: i32 }[3], n: u8 }[2] }");
        RantSchemaFieldInfo fi; uint8_t m[128]; RantBytes b; int vi;
        CHECK(s != NULL, "schema-nest: a fixed struct array nests in a fixed one");
        if (s){
            vi = rant_schema_field_index(s, "rows.cells.v");
            CHECK(vi > 0 && rant_schema_field_at(s, (uint16_t)vi, &fi) && fi.arr_depth == 2
                     && rant_schema_field_index(s, "rows[1].cells[2].v") == vi,
                     "schema-nest: the inner member takes two indices (%u)", fi.arr_depth);
            rant_schema_message_default(s, m, sizeof m);
            CHECK(rant_set_int(m, sizeof m, s, "rows[1].cells[2].v", 42)
                     && rant_set_int(m, sizeof m, s, "rows[0].cells[1].v", -3)
                     && rant_set_uint(m, sizeof m, s, "rows[1].n", 9)
                     && !rant_set_int(m, sizeof m, s, "rows[2].cells[0].v", 1)
                     && !rant_set_int(m, sizeof m, s, "rows[0].cells[3].v", 1),
                     "schema-nest: writes land by path, out of range at either level refuses");
            b = rant_bytes(m, rant_schema_msg_len(s, m, sizeof m));
            CHECK(rant_get_int(b, s, "rows[1].cells[2].v") == 42
                     && rant_get_int(b, s, "rows[0].cells[1].v") == -3
                     && rant_get_int(b, s, "rows[0].cells[2].v") == 0
                     && rant_get_int(b, s, "rows[1].cells[1].v") == 0
                     && rant_get_uint(b, s, "rows[1].n") == 9,
                     "schema-nest: each element reads back on its own");
            {   uint32_t at[2] = { 1, 2 }; RantValue v;
                CHECK(rant_get_value_at(b, s, (uint16_t)vi, at, 2, &v) && v.v.i == 42
                         && !rant_get_value_at(b, s, (uint16_t)vi, at, 1, &v),
                         "schema-nest: get_value_at takes one index per level, exactly");
                at[0] = 0; at[1] = 0; memset(&v, 0, sizeof v); v.kind = RANT_I32; v.v.i = 5;
                CHECK(rant_set_value_at(m, sizeof m, s, (uint16_t)vi, at, 2, &v)
                         && rant_get_int(b, s, "rows[0].cells[0].v") == 5,
                         "schema-nest: set_value_at lands where the path reads");
            }
        }
        sv_free(s);
    }
    {   RantSchema *s = sv("Shape { corners: { x: f32, y: f32 }[4], id: u16 }\n"
                           "S { tag: u8, shapes: Shape[], note: string }");
        uint8_t m[512]; RantBytes b;
        CHECK(s != NULL, "schema-nest: a fixed struct array nests in a variable one");
        if (s){
            rant_schema_message_default(s, m, sizeof m);
            CHECK(rant_set_array_count(m, sizeof m, s, "shapes", 3)
                     && rant_set_f32(m, sizeof m, s, "shapes[2].corners[3].y", 7.5f)
                     && rant_set_f32(m, sizeof m, s, "shapes[0].corners[1].x", 1.25f)
                     && rant_set_uint(m, sizeof m, s, "shapes[1].id", 11)
                     && rant_set_string(m, sizeof m, s, "note", rant_cstr("after"))
                     && !rant_set_f32(m, sizeof m, s, "shapes[3].corners[0].x", 1.0f),
                     "schema-nest: the outer array grows, then its elements' arrays take writes");
            b = rant_bytes(m, rant_schema_msg_len(s, m, sizeof m));
            CHECK(rant_get_f32(b, s, "shapes[2].corners[3].y") == 7.5f
                     && rant_get_f32(b, s, "shapes[0].corners[1].x") == 1.25f
                     && rant_get_f32(b, s, "shapes[2].corners[1].x") == 0.0f
                     && rant_get_uint(b, s, "shapes[1].id") == 11
                     && rant_get_array_count(b, s, "shapes") == 3
                     && rant_get_array_count(b, s, "shapes[1].corners") == 4
                     && rant_schema_validate(s, b),
                     "schema-nest: the nested elements read back inside the frame");
        }
        sv_free(s);
    }

    /* variable members at depth: declaration nests, storage does not */
    {   RantSchema *s = sv("A { hdr: { n: u32, note: string, k: u32 }, tail: u8[] }");
        RantSchemaFieldInfo fi; uint8_t m[256]; RantBytes b;
        CHECK(s != NULL, "schema-v8: a variable member inside a struct compiles");
        if (s){
            CHECK(rant_schema_field_at(s, 0, &fi) && fi.kind == RANT_STRUCT && fi.size == 8
                     && rant_schema_size(s) == 8 && rant_schema_msg_min(s) == 16,
                     "schema-v8: the struct's size is its FIXED part; the frame is top-level");
            rant_schema_message_default(s, m, sizeof m);
            CHECK(rant_set_uint(m, sizeof m, s, "hdr.n", 7)
                     && rant_set_uint(m, sizeof m, s, "hdr.k", 9)
                     && rant_set_string(m, sizeof m, s, "hdr.note", rant_cstr("deep"))
                     && rant_set_array(m, sizeof m, s, "tail", rant_bytes("abc", 3)),
                     "schema-v8: nested variable member sets");
            b = rant_bytes(m, rant_schema_msg_len(s, m, sizeof m));
            CHECK(rant_get_uint(b, s, "hdr.n") == 7 && rant_get_uint(b, s, "hdr.k") == 9
                     && rant_get_string(b, s, "hdr.note").len == 4
                     && rant_get_array(b, s, "tail").len == 3 && rant_schema_validate(s, b),
                     "schema-v8: nested variable member reads back, message validates");
        }
        sv_free(s);
    }
    {   RantSchema *s = sv("A { a: string, g: { b: string, c: string }, d: string }");
        uint8_t m[256]; RantBytes b;
        CHECK(s != NULL, "schema-v8: frames at mixed depths compile");
        if (s){
            rant_schema_message_default(s, m, sizeof m);
            rant_set_string(m, sizeof m, s, "a", rant_cstr("1"));
            rant_set_string(m, sizeof m, s, "g.b", rant_cstr("22"));
            rant_set_string(m, sizeof m, s, "g.c", rant_cstr("333"));
            rant_set_string(m, sizeof m, s, "d", rant_cstr("4444"));
            b = rant_bytes(m, rant_schema_msg_len(s, m, sizeof m));
            CHECK(rant_get_string(b, s, "a").len == 1 && rant_get_string(b, s, "g.b").len == 2
                     && rant_get_string(b, s, "g.c").len == 3 && rant_get_string(b, s, "d").len == 4,
                     "schema-v8: frames sit in depth-first declaration order");
        }
        sv_free(s);
    }

    /* rebase across nested frames and struct arrays */
    {   RantSchema *pub = sv("A { pad: u64, v: { a: f32, note: string }, p: Float3[2], t: string }");
        RantSchema *sub = sv("A { v: { a: f32, note: string }, t: string }");
        RantSchema *rb = NULL; uint8_t m[256]; RantBytes b;
        if (pub && sub) rb = i_rant_schema_rebase(sub, pub, rant_allocator_alloc, &sv_ma);
        CHECK(rb != NULL, "schema-v8: rebase of a nested/array subset");
        if (rb){
            rant_schema_message_default(pub, m, sizeof m);
            rant_set_f32(m, sizeof m, pub, "v.a", 3.5f);
            rant_set_string(m, sizeof m, pub, "v.note", rant_cstr("nn"));
            rant_set_string(m, sizeof m, pub, "t", rant_cstr("tt"));
            b = rant_bytes(m, rant_schema_msg_len(pub, m, sizeof m));
            CHECK(rant_schema_validate(rb, b) && rant_get_f32(b, rb, "v.a") == 3.5f
                     && rant_get_string(b, rb, "v.note").len == 2
                     && rant_get_string(b, rb, "t").len == 2,
                     "schema-v8: the rebased reader walks the writer's frames");
        }
        sv_free(rb); sv_free(pub); sv_free(sub);
    }

    /* refusals: an array element's size must be static */
    {   static const char *bad[] = {
            "A { p: { n: string }[4] }",        /* a variable member inside an element   */
            "A { p: Image[2] }",                /* a standard type that has one          */
            "A { p: f32[2][3] }",               /* an array of arrays                    */
            "A { p: Uuid[2] }",                 /* a named alias that IS an array        */
            "A { p: { q: { r: f32 }[] }[2] }",  /* a variable struct array in an element */
            "A { m: enum<u8> { X }[2] }",       /* enum elements would hide the backing  */
            "Float3 { x: f32 }\nA { p: Float3 }",    /* a reserved name, a wrong shape   */
            "Transform { x: f32 }",             /* likewise as the last statement        */
            "A { x: f32 }\nA { x: f64 }",       /* a name defined twice with two shapes  */
            "W = { x: f32 }",                   /* the removed spelling of a struct      */
            "W { x: f32 }\nA = W",              /* an alias of another name              */
            "W { x: f32 }\nA = W\nB { y: u8 }", /* refused where it is written, used or not */
            "A { p: Nope }",                    /* an unknown type word                  */
            "f32\nB { y: f32 }",                /* a type on its own ends the text       */
            "Temperature: f32"                  /* the old typedef spelling stays an error */
        };
        unsigned i, ok = 1;
        for (i = 0; i < sizeof bad / sizeof bad[0]; i++){
            i_RantSchemaErr ep = { NULL, NULL };
            RantSchema *s = test_compile(&sv_ma, bad[i], &ep);
            if (s){ ok = 0; sv_free(s); }
            else if (!ep.at || !ep.why) ok = 0;
        }
        CHECK(ok, "schema-v8: every malformed/misplaced form is refused with a reason and a position");
    }
    {   RantSchema *a = sv("Float3 { x: f32, y: f32, z: f32 }\nA { p: Float3 }");
        RantSchema *b = sv("A { x: f32 }\nA { x: f32 }");
        RantSchema *c = sv("Float3 { r: f32 }");
        CHECK(a != NULL && b != NULL, "schema-v8: defining a name again IDENTICALLY is allowed");
        CHECK(c == NULL, "schema-v8: a standard name is reserved as the last statement too");
        sv_free(a); sv_free(b); sv_free(c);
    }
    /* statements: a text defines any number of names and compiles to its last line */
    {   RantSchema *two = sv("A { x: f32 }\nB { y: f32 }");
        RantSchema *again = sv("A { x: u8 }\nB { y: u8 }\nA { x: u8 }");
        RantSchema *byname = sv("W { x: u8 }\nV { w: W[3] }\nW");
        RantSchema *plain = sv("W { x: u8 }");
        RantSchema *alias = sv("Celsius = f32\nRoom { t: Celsius }");
        CHECK(two && rant_schema_name(two).len == 1 && rant_schema_name(two).data[0] == 'B',
                 "schema-v8: two definitions compile to the second");
        CHECK(again && rant_schema_name(again).len == 1 && rant_schema_name(again).data[0] == 'A',
                 "schema-v8: a repeated definition as the last statement is the result");
        CHECK(byname && plain && rant_schema_hash(byname) == rant_schema_hash(plain),
                 "schema-v8: a definition taken by its name equals the plain struct");
        CHECK(alias != NULL && sv_roundtrip("Celsius = f32\nRoom { t: Celsius }"),
                 "schema-v8: an alias definition serves a later struct and round-trips");
        sv_free(two); sv_free(again); sv_free(byname); sv_free(plain); sv_free(alias);
    }

    /* hostile wire */
    {   uint8_t w[8]; RantSchema *s;
        w[0] = (uint8_t)RANT_SCHEMA_WIRE_VERSION; w[1] = 0;
        w[2] = (uint8_t)RANT_NAMED; w[3] = 1; w[4] = 'A'; w[5] = (uint8_t)RANT_NAMED;
        w[6] = 1; w[7] = 'B';
        CHECK(i_rant_schema_parse(w, 8, rant_allocator_alloc, &sv_ma) == NULL,
                 "schema-v8: a doubly-wrapped NAMED is refused");
        w[2] = (uint8_t)RANT_NAMED; w[3] = 0; w[4] = (uint8_t)RANT_BOOL;
        CHECK(i_rant_schema_parse(w, 5, rant_allocator_alloc, &sv_ma) == NULL,
                 "schema-v8: a NAMED with an empty name is refused");
        w[2] = (uint8_t)RANT_ARR; w[3] = 2; w[4] = 0; w[5] = (uint8_t)RANT_VSTR;
        CHECK(i_rant_schema_parse(w, 6, rant_allocator_alloc, &sv_ma) == NULL,
                 "schema-v8: a variable array element is refused on the wire");
        w[5] = (uint8_t)RANT_NAMED; w[6] = 1; w[7] = 'A';
        s = i_rant_schema_parse(w, 8, rant_allocator_alloc, &sv_ma);
        CHECK(s == NULL, "schema-v8: a truncated NAMED element is refused");
        sv_free(s);
    }

    /* print: named definitions hoist, dependencies first, and recompile exactly */
    CHECK(sv_roundtrip("Uuid = u8[16]") && sv_roundtrip("Transform") && sv_roundtrip("Image"),
             "schema-v8: alias, composite and variable standard roots round-trip");
    CHECK(sv_roundtrip("A { x: f32, v: { a: u8, b: string<4> }, p: Float3[3],"
                          "    q: { m: i16 }[2], s: string, e: enum<i16> { N = -1, Z } }"),
             "schema-v8: a mixed schema round-trips through print");
    CHECK(sv_roundtrip("Cloud { at: Transform, when: Timestamp, pts: Float3[], meta: map }"),
             "schema-v8: nested standard types round-trip");
    {   RantSchema *s = sv("Cloud { at: Transform, pts: Float3[] }");
        char buf[1024]; const char *d3, *q, *ps;
        if (s){
            rant_schema_print(s, buf, sizeof buf);
            d3 = strstr(buf, "Double3 {"); q = strstr(buf, "Quaternion {"); ps = strstr(buf, "Transform {");
            CHECK(d3 && q && ps && d3 < ps && q < ps,
                     "schema-v8: print emits each named type once, dependencies first");
        }
        sv_free(s);
    }

    /* names: one wire spelling, whatever was written */
    {   RantSchema *s = sv("robot_pose { frame_id: u8, HTTPServer: u8, Imu9Dof: u8, MONO8: u8,\n"
                           "  mode: enum<u8> { IDLE, running_fast } }\nRobotPose");
        RantSchema *same = sv("RobotPose { frameId: u8, httpServer: u8, imu9Dof: u8, mono8: u8,\n"
                              "  mode: enum<u8> { Idle, RunningFast } }");
        char buf[512], w[64];
        CHECK(s && same, "names: a text of mixed spellings compiles");
        if (s && same){
            rant_schema_print(s, buf, sizeof buf);
            CHECK(strstr(buf, "RobotPose {") && strstr(buf, "frame_id:") && strstr(buf, "http_server:")
                     && strstr(buf, "imu9_dof:") && strstr(buf, "mono8:") && strstr(buf, "Idle")
                     && strstr(buf, "RunningFast"),
                     "names: fields spell snake_case, types and options PascalCase (%s)", buf);
            CHECK(rant_schema_field_index(s, "FrameId") == 0 && rant_schema_field_index(s, "frame_id") == 0
                     && rant_schema_field_index(s, "http_server") == 1,
                     "names: a path finds a field in any spelling");
            CHECK(rant_schema_hash(s) == rant_schema_hash(same),
                     "names: the same fields in another spelling are the same schema");
        }
        CHECK(rant_field_name("HTTPServer", w, sizeof w) == 11 && strcmp(w, "http_server") == 0
                 && rant_field_name("wallUs", w, sizeof w) == 7 && strcmp(w, "wall_us") == 0
                 && rant_field_name("Imu9Dof", w, sizeof w) == 8 && strcmp(w, "imu9_dof") == 0
                 && rant_field_name("Pos2D", w, sizeof w) == 6 && strcmp(w, "pos2_d") == 0
                 && rant_type_name("frame_id", w, sizeof w) == 7 && strcmp(w, "FrameId") == 0
                 && rant_type_name("RectI", w, sizeof w) == 5 && strcmp(w, "RectI") == 0
                 && rant_field_name("__", w, sizeof w) == 0 && rant_field_name("_1", w, sizeof w) == 0
                 && rant_field_name("frameId", w, 8) == 0 && rant_field_name("frameId", w, 9) == 8,
                 "names: the public rule");
        CHECK(rant_type_name("point_x_y", w, sizeof w) == 7 && strcmp(w, "PointXy") == 0
                 && rant_type_name("PointXY", w, sizeof w) == 7 && strcmp(w, "PointXy") == 0
                 && rant_type_name("x_y_z", w, sizeof w) == 3 && strcmp(w, "Xyz") == 0
                 && rant_type_name("X_AXIS", w, sizeof w) == 5 && strcmp(w, "XAxis") == 0
                 && rant_type_name("RUNNING_FAST", w, sizeof w) == 11 && strcmp(w, "RunningFast") == 0
                 && rant_type_name("HTTPServer", w, sizeof w) == 10 && strcmp(w, "HttpServer") == 0
                 && rant_field_name("point_x_y", w, sizeof w) == 9 && strcmp(w, "point_x_y") == 0,
                 "names: a run of single letters is one type word, as its PascalCase reads");
        {   /* every spelling over a small alphabet lands on a fixed point, field and type */
            static const char abc[] = "aAbB1_";
            char in[6], once[16], twice[16];
            int len, k, as_type, bad = 0;
            long idx, total;
            for (len = 1; len <= 5; len++){
                for (total = 1, k = 0; k < len; k++) total *= 6;
                for (idx = 0; idx < total; idx++){
                    long v = idx;
                    for (k = 0; k < len; k++){ in[k] = abc[v % 6]; v /= 6; }
                    in[len] = 0;
                    for (as_type = 0; as_type < 2; as_type++){
                        size_t a = i_rant_name_normalize(in, once, sizeof once, as_type);
                        if (a && (i_rant_name_normalize(once, twice, sizeof twice, as_type) != a
                                  || strcmp(once, twice) != 0)) bad++;
                    }
                }
            }
            CHECK(bad == 0, "names: every spelling normalizes to a fixed point (%d do not)", bad);
        }
        CHECK(sv("A { x: u8, X: u8 }") == NULL && sv("A { frame_id: u8, frameId: u8 }") == NULL
                 && sv("A { a: { x: u8, X: u8 } }") == NULL && sv("A { x: u8, b: { x: u8 } }") != NULL,
                 "names: two fields of one struct with one wire name are refused");
        {   i_RantSchemaErr ep = { NULL, NULL };
            CHECK(test_compile(&sv_ma, "M = enum<u8> { Idle, Busy, IDLE }", &ep) == NULL
                     && ep.why && strstr(ep.why, "same wire name") && ep.at && strncmp(ep.at, "IDLE", 4) == 0
                     && sv("M = enum<u8> { RX, R_X }") == NULL
                     && sv("A { m: enum<u8> { running_fast = 2, RunningFast = 3 } }") == NULL
                     && sv("M = enum<u8> { Idle, Busy, IdleNot }") != NULL,
                     "names: two options of one enum with one wire name are refused (%s)",
                     ep.why ? ep.why : "null"); }
    }

    /* the registry: definitions persist across compiles, a failed text keeps nothing */
    {   i_RantRegistry reg; RantSchema *w, *p; i_RantSchemaErr ep = { NULL, NULL };
        i_rant_registry_init(&reg, rant_allocator_alloc, &sv_ma);
        w = i_rant_registry_compile(&reg, "Widget { id: u32, tag: Color }", NULL);
        p = i_rant_registry_compile(&reg, "Panel { w: Widget, more: Widget[2] }", NULL);
        CHECK(w && p, "registry: a definition stays in scope for the next compile");
        CHECK(p && rant_schema_size(p) == 24 && rant_schema_field_index(p, "w.tag.r") >= 0
                 && rant_schema_field_index(p, "more[1].id") >= 0,
                 "registry: the earlier type nests and arrays like any other");
        CHECK(sv("Panel { w: Widget }") == NULL, "registry: a fresh registry does not know the name");
        CHECK(w && i_rant_registry_compile(&reg, "Widget", NULL) == w
                 && i_rant_registry_compile(&reg, "Widget { id: u32, tag: Color }", NULL) == w,
                 "registry: one handle per shape, by name or spelled out");
        CHECK(i_rant_registry_compile(&reg, "Gadget { a: u8 }\nWidget { id: u64 }", &ep) == NULL
                 && ep.at && strncmp(ep.at, "Widget", 6) == 0
                 && ep.why && strstr(ep.why, "another shape") != NULL
                 && i_rant_registry_compile(&reg, "Gadget", NULL) == NULL,
                 "registry: a failed text keeps none of its definitions (%s)", ep.why ? ep.why : "null");
        CHECK(i_rant_registry_compile(&reg, "Gadget { a: u8 }", NULL) != NULL,
                 "registry: it compiles again after a failure");
    }
    rant_allocator_reset(&sv_ma);
}

static const TestCase cases[] = {
    { "dsl", schema_dsl_checks },
    { "wire", schema_v8_checks },
};

TEST_MAIN(cases)
