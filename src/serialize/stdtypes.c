#include "stdtypes.h"
#include <string.h>            /* memcpy, memcmp */

/* The roster: a name and the canonical spelling of its type, exactly as a user would
 * write it. These strings ARE the definition -- the DSL compiles them on demand (see
 * i_dart_std_lookup below), so there is one source of truth for the shape, the printed
 * text, and the wire bytes. Composite entries reference other entries by name, which is
 * why the order here does not matter: resolution is recursive. */
typedef struct { const char *name, *text; } i_DartStdEntry;

static const i_DartStdEntry i_dart_std_table[] = {
    { "Float2",  "{ x: f32, y: f32 }" },
    { "Float3",  "{ x: f32, y: f32, z: f32 }" },
    { "Float4",  "{ x: f32, y: f32, z: f32, w: f32 }" },
    { "Double2", "{ x: f64, y: f64 }" },
    { "Double3", "{ x: f64, y: f64, z: f64 }" },
    { "Double4", "{ x: f64, y: f64, z: f64, w: f64 }" },
    { "Int2",    "{ x: i32, y: i32 }" },
    { "Int3",    "{ x: i32, y: i32, z: i32 }" },
    { "Int4",    "{ x: i32, y: i32, z: i32, w: i32 }" },
    { "Quaternion", "{ x: f64, y: f64, z: f64, w: f64 }" },
    { "Color",   "{ r: u8, g: u8, b: u8, a: u8 }" },
    { "Rect",    "{ x: f32, y: f32, w: f32, h: f32 }" },
    { "RectI",   "{ x: i32, y: i32, w: i32, h: i32 }" },
    { "Pose",    "{ position: Double3, orientation: Quaternion }" },
    { "Twist",   "{ linear: Double3, angular: Double3 }" },
    { "GeoPoint","{ lat: f64, lon: f64, alt: f64 }" },
    { "Uuid",      "u8[16]" },
    { "Timestamp", "i64" },
    { "Duration",  "i64" },
    { "Matrix3x3", "f32[9]" },
    { "Matrix4x4", "f32[16]" },
    { "Uri",       "string<256>" },
    { "Image",   "{ width: u32, height: u32, stride: u32,"
                 "  format: enum<u8> { Mono8 = 0, Mono16 = 1, Rgb8 = 2, Rgba8 = 3,"
                 "                     Bgr8 = 4, Yuyv = 5, Nv12 = 6, Jpeg = 16, Png = 17 },"
                 "  data: u8[] }" },
    { "VideoFrame", "{ codec: enum<u8> { Unknown = 0, Mjpeg = 1, H264 = 2, H265 = 3, Av1 = 4 },"
                 "  width: u32, height: u32,"
                 "  keyframe: bool, pts: Timestamp, data: u8[] }" },
    { "ExternalVideoStream",
                 "{ kind: enum<u8> { Rtsp = 0, WebrtcWhep = 1, Hls = 2, Srt = 3,"
                 "                   Rtp = 4, HttpMjpeg = 5, Other = 15 },"
                 "  codec: enum<u8> { Unknown = 0, Mjpeg = 1, H264 = 2, H265 = 3, Av1 = 4 },"
                 "  width: u32, height: u32,"
                 "  url: Uri, name: string<32> }" }
};

/* the table is indexed by DartStdType - 1; a mismatch would silently shift every name */
typedef char i_dart_std_table_check[
    (sizeof i_dart_std_table / sizeof i_dart_std_table[0] == (size_t)DART_STD_COUNT - 1u) ? 1 : -1];

const char *dart_std_name(DartStdType t){
    if (t <= DART_STD_NONE || t >= DART_STD_COUNT) return 0;
    return i_dart_std_table[(int)t - 1].name;
}
const char *dart_std_text(DartStdType t){
    if (t <= DART_STD_NONE || t >= DART_STD_COUNT) return 0;
    return i_dart_std_table[(int)t - 1].text;
}

DartStdType dart_std_by_name(DartString name){
    int i;
    if (!name.data || !name.len) return DART_STD_NONE;
    for (i = 0; i < (int)DART_STD_COUNT - 1; i++){
        const char *n = i_dart_std_table[i].name;
        size_t k = strlen(n);
        if (k == name.len && memcmp(n, name.data, k) == 0) return (DartStdType)(i + 1);
    }
    return DART_STD_NONE;
}

/* The one seam the schema DSL calls: a type word it does not recognize is looked up here
 * (and, when found, compiled from the text above as if the schema had defined it). */
const char *i_dart_std_lookup(const char *name){
    int i;
    if (!name) return 0;
    for (i = 0; i < (int)DART_STD_COUNT - 1; i++)
        if (strcmp(i_dart_std_table[i].name, name) == 0) return i_dart_std_table[i].text;
    return 0;
}

DartSchema *dart_std_schema(DartStdType t, DartAllocFn alloc, void *user){
    const char *n = dart_std_name(t);
    if (!n || !alloc) return 0;
    return dart_schema_compile(alloc, user, n, 0);   /* the name alone resolves to the type */
}

/* ---- recognizing a standard type in someone else's schema ---------------------------- */
/* The canonical type encoding of a standard type: its schema's root type bytes. */
static int i_dart_std_shape_eq(DartStdType t, DartBytes type, DartAllocFn alloc, void *user){
    DartSchema *c = dart_std_schema(t, alloc, user);
    DartBytes w; DartString nm; size_t off; int ok = 0;
    if (!c) return 0;
    w = dart_schema_wire(c); nm = dart_schema_name(c);
    off = 2u + nm.len;
    if (type.data && off < w.len && type.len == w.len - off)
        ok = memcmp(type.data, w.data + off, type.len) == 0;
    dart_schema_free(c, alloc, user);
    return ok;
}

/* peel a NAMED wrapper: fills *name and returns the inner type bytes */
static int i_dart_std_peel(DartBytes type, DartString *name, DartBytes *inner){
    size_t nl;
    if (!type.data || type.len < 2u || type.data[0] != DART_NAMED) return 0;
    nl = type.data[1];
    if (2u + nl > type.len || nl == 0) return 0;
    *name  = dart_string((const char *)(type.data + 2), nl);
    *inner = dart_bytes(type.data + 2 + nl, type.len - 2 - nl);
    return 1;
}

static DartStdType i_dart_std_check(DartString name, DartBytes inner,
                                    DartAllocFn alloc, void *user){
    DartStdType t = dart_std_by_name(name);
    if (t == DART_STD_NONE || !alloc) return DART_STD_NONE;
    return i_dart_std_shape_eq(t, inner, alloc, user) ? t : DART_STD_NONE;
}

DartStdType dart_std_recognize(const DartSchema *s, DartAllocFn alloc, void *user){
    DartBytes w = dart_schema_wire(s);
    DartString nm = dart_schema_name(s);
    size_t off = 2u + nm.len;
    if (!w.data || off >= w.len) return DART_STD_NONE;
    return i_dart_std_check(nm, dart_bytes(w.data + off, w.len - off), alloc, user);
}

DartStdType dart_std_recognize_field(const DartSchema *s, uint16_t field,
                                     DartAllocFn alloc, void *user){
    DartBytes type = dart_schema_field_type_wire(s, field), inner;
    DartString name;
    if (!i_dart_std_peel(type, &name, &inner)) return DART_STD_NONE;
    return i_dart_std_check(name, inner, alloc, user);
}

DartStdType dart_std_recognize_elem(const DartSchema *s, uint16_t field,
                                    DartAllocFn alloc, void *user){
    DartBytes type = dart_schema_field_type_wire(s, field), inner;
    DartSchemaFieldInfo fi;
    DartString name;
    size_t head;
    if (!dart_schema_field_at(s, field, &fi) || !type.data) return DART_STD_NONE;
    if (fi.kind == DART_ARR)       head = 3u;      /* [ARR][u16 count] */
    else if (fi.kind == DART_VARR) head = 1u;      /* [VARR]           */
    else return DART_STD_NONE;
    if (head >= type.len) return DART_STD_NONE;
    if (!i_dart_std_peel(dart_bytes(type.data + head, type.len - head), &name, &inner))
        return DART_STD_NONE;
    return i_dart_std_check(name, inner, alloc, user);
}

/* ---- the few operations that need a square root -------------------------------------- */
/* No <math.h>: DART links against nothing, and a consumer's build line should not have to
 * grow an -lm for four helpers. Newton from the classic halve-the-exponent seed converges
 * to full double precision well within five steps. */
static double i_dart_sqrt(double x){
    uint64_t b; double r; int i;
    if (!(x > 0.0)) return 0.0;                       /* 0, negatives and NaN alike */
    memcpy(&b, &x, 8);
    b = (b >> 1) + 0x1FF8000000000000ull;
    memcpy(&r, &b, 8);
    for (i = 0; i < 5; i++) r = 0.5 * (r + x / r);
    return r;
}

double dart_double3_length(DartDouble3 a){
    return i_dart_sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}

DartDouble3 dart_double3_normalize(DartDouble3 a){
    double n = dart_double3_length(a);
    return n > 0.0 ? dart_double3_scale(a, 1.0 / n) : a;
}

DartQuaternion dart_quaternion_normalize(DartQuaternion q){
    double n = i_dart_sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n <= 0.0) return dart_quaternion_identity();
    n = 1.0 / n;
    return dart_quaternion(q.x * n, q.y * n, q.z * n, q.w * n);
}

/* v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v): the branch-free rotation form */
DartDouble3 dart_quaternion_rotate(DartQuaternion q, DartDouble3 v){
    DartDouble3 u = dart_double3(q.x, q.y, q.z);
    DartDouble3 t = dart_double3_cross(u, v);
    t = dart_double3_add(t, dart_double3_scale(v, q.w));
    return dart_double3_add(v, dart_double3_scale(dart_double3_cross(u, t), 2.0));
}
