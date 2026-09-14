#include "stdtypes.h"
#include <string.h>

/* The roster: a name and the canonical spelling of its type. These strings are the one
 * definition, the DSL compiles them on demand. Order does not matter, resolution is recursive. */
typedef struct { const char *name, *text; } i_RambleStdEntry;

static const i_RambleStdEntry i_ramble_std_table[] = {
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
    { "Transform", "{ translation: Double3, rotation: Quaternion, parent: string<30> }" },
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
                 "                     Bgr8 = 4, Yuyv = 5, Nv12 = 6, Monof32 = 7,"
                 "                     Jpeg = 16, Png = 17 },"
                 "  data: u8[] }" },
    { "VideoFrame", "{ codec: enum<u8> { Unknown = 0, Mjpeg = 1, H264 = 2, H265 = 3, Av1 = 4 },"
                 "  width: u32, height: u32,"
                 "  keyframe: bool, pts: Timestamp, data: u8[] }" },
    { "ExternalVideoStream",
                 "{ kind: enum<u8> { Rtsp = 0, WebrtcWhep = 1, Hls = 2, Srt = 3,"
                 "                   Rtp = 4, HttpMjpeg = 5, Other = 15 },"
                 "  codec: enum<u8> { Unknown = 0, Mjpeg = 1, H264 = 2, H265 = 3, Av1 = 4 },"
                 "  width: u32, height: u32,"
                 "  url: Uri, name: string<32> }" },
    { "CameraIntrinsics",
                 "{ width: u32, height: u32,"
                 "  fx: f64, fy: f64, cx: f64, cy: f64,"
                 "  model: enum<u8> { None = 0, BrownConrady = 1, Fisheye = 2, Rational = 3 },"
                 "  coeffs: f64[8] }" },
    { "JointState", "{ position: f64[], velocity: f64[], effort: f64[] }" },
    { "JointNames", "{ name: string<32>[] }" }
};

/* the table is indexed by RambleStdType minus 1, a mismatch would shift every name */
typedef char i_ramble_std_table_check[
    (sizeof i_ramble_std_table / sizeof i_ramble_std_table[0] == (size_t)RAMBLE_STD_COUNT - 1u) ? 1 : -1];

const char *ramble_std_name(RambleStdType t){
    if (t <= RAMBLE_STD_NONE || t >= RAMBLE_STD_COUNT) return 0;
    return i_ramble_std_table[(int)t - 1].name;
}
const char *ramble_std_text(RambleStdType t){
    if (t <= RAMBLE_STD_NONE || t >= RAMBLE_STD_COUNT) return 0;
    return i_ramble_std_table[(int)t - 1].text;
}

RambleStdType ramble_std_by_name(RambleString name){
    int i;
    if (!name.data || !name.len) return RAMBLE_STD_NONE;
    for (i = 0; i < (int)RAMBLE_STD_COUNT - 1; i++){
        const char *n = i_ramble_std_table[i].name;
        size_t k = strlen(n);
        if (k == name.len && memcmp(n, name.data, k) == 0) return (RambleStdType)(i + 1);
    }
    return RAMBLE_STD_NONE;
}

/* The one seam the DSL calls: an unknown type word is looked up here and compiled from its text. */
const char *i_ramble_std_lookup(const char *name){
    int i;
    if (!name) return 0;
    for (i = 0; i < (int)RAMBLE_STD_COUNT - 1; i++)
        if (strcmp(i_ramble_std_table[i].name, name) == 0) return i_ramble_std_table[i].text;
    return 0;
}

RambleSchema *ramble_std_schema(RambleStdType t, RambleAllocFn alloc, void *user){
    const char *n = ramble_std_name(t);
    if (!n || !alloc) return 0;
    return ramble_schema_compile(alloc, user, n, 0);   /* the name alone resolves to the type */
}

/* Does type match the canonical root type bytes of the standard type. */
static int i_ramble_std_shape_eq(RambleStdType t, RambleBytes type, RambleAllocFn alloc, void *user){
    RambleSchema *c = ramble_std_schema(t, alloc, user);
    RambleBytes w; RambleString nm; size_t off; int ok = 0;
    if (!c) return 0;
    w = ramble_schema_wire(c); nm = ramble_schema_name(c);
    off = 2u + nm.len;
    if (type.data && off < w.len && type.len == w.len - off)
        ok = memcmp(type.data, w.data + off, type.len) == 0;
    ramble_schema_free(c, alloc, user);
    return ok;
}

/* peel a NAMED wrapper: fills *name and returns the inner type bytes */
static int i_ramble_std_peel(RambleBytes type, RambleString *name, RambleBytes *inner){
    size_t nl;
    if (!type.data || type.len < 2u || type.data[0] != RAMBLE_NAMED) return 0;
    nl = type.data[1];
    if (2u + nl > type.len || nl == 0) return 0;
    *name  = ramble_string((const char *)(type.data + 2), nl);
    *inner = ramble_bytes(type.data + 2 + nl, type.len - 2 - nl);
    return 1;
}

static RambleStdType i_ramble_std_check(RambleString name, RambleBytes inner,
                                    RambleAllocFn alloc, void *user){
    RambleStdType t = ramble_std_by_name(name);
    if (t == RAMBLE_STD_NONE || !alloc) return RAMBLE_STD_NONE;
    return i_ramble_std_shape_eq(t, inner, alloc, user) ? t : RAMBLE_STD_NONE;
}

RambleStdType ramble_std_recognize(const RambleSchema *s, RambleAllocFn alloc, void *user){
    RambleBytes w = ramble_schema_wire(s);
    RambleString nm = ramble_schema_name(s);
    size_t off = 2u + nm.len;
    if (!w.data || off >= w.len) return RAMBLE_STD_NONE;
    return i_ramble_std_check(nm, ramble_bytes(w.data + off, w.len - off), alloc, user);
}

RambleStdType ramble_std_recognize_field(const RambleSchema *s, uint16_t field,
                                     RambleAllocFn alloc, void *user){
    RambleBytes type = ramble_schema_field_type_wire(s, field), inner;
    RambleString name;
    if (!i_ramble_std_peel(type, &name, &inner)) return RAMBLE_STD_NONE;
    return i_ramble_std_check(name, inner, alloc, user);
}

RambleStdType ramble_std_recognize_elem(const RambleSchema *s, uint16_t field,
                                    RambleAllocFn alloc, void *user){
    RambleBytes type = ramble_schema_field_type_wire(s, field), inner;
    RambleSchemaFieldInfo fi;
    RambleString name;
    size_t head;
    if (!ramble_schema_field_at(s, field, &fi) || !type.data) return RAMBLE_STD_NONE;
    if (fi.kind == RAMBLE_ARR)       head = 3u;      /* [ARR][u16 count] */
    else if (fi.kind == RAMBLE_VARR) head = 1u;      /* [VARR] */
    else return RAMBLE_STD_NONE;
    if (head >= type.len) return RAMBLE_STD_NONE;
    if (!i_ramble_std_peel(ramble_bytes(type.data + head, type.len - head), &name, &inner))
        return RAMBLE_STD_NONE;
    return i_ramble_std_check(name, inner, alloc, user);
}

/* No math.h, so a consumer's build line never grows an -lm. Newton from the halved
 * exponent seed converges to full double precision within five steps. */
static double i_ramble_sqrt(double x){
    uint64_t b; double r; int i;
    if (!(x > 0.0)) return 0.0;                       /* 0, negatives and NaN alike */
    memcpy(&b, &x, 8);
    b = (b >> 1) + 0x1FF8000000000000ull;
    memcpy(&r, &b, 8);
    for (i = 0; i < 5; i++) r = 0.5 * (r + x / r);
    return r;
}

double ramble_double3_length(RambleDouble3 a){
    return i_ramble_sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}

RambleDouble3 ramble_double3_normalize(RambleDouble3 a){
    double n = ramble_double3_length(a);
    return n > 0.0 ? ramble_double3_scale(a, 1.0 / n) : a;
}

RambleQuaternion ramble_quaternion_normalize(RambleQuaternion q){
    double n = i_ramble_sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n <= 0.0) return ramble_quaternion_identity();
    n = 1.0 / n;
    return ramble_quaternion(q.x * n, q.y * n, q.z * n, q.w * n);
}

/* v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v), the branch free rotation form */
RambleDouble3 ramble_quaternion_rotate(RambleQuaternion q, RambleDouble3 v){
    RambleDouble3 u = ramble_double3(q.x, q.y, q.z);
    RambleDouble3 t = ramble_double3_cross(u, v);
    t = ramble_double3_add(t, ramble_double3_scale(v, q.w));
    return ramble_double3_add(v, ramble_double3_scale(ramble_double3_cross(u, t), 2.0));
}
