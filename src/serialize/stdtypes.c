#include "internal.h"

/* The one definition of each type, compiled by the DSL on demand in any order. The comment
 * above a type describes it, and a comment after a field gives its [unit] and then a note. */
typedef struct { const char *name, *text; } i_RantStdEntry;

static const i_RantStdEntry i_rant_std_table[] = {
    { "Float2",  "-- A 2D vector of f32\n{ x: f32, y: f32 }" },
    { "Float3",  "-- A 3D vector of f32\n{ x: f32, y: f32, z: f32 }" },
    { "Float4",  "-- A 4D vector of f32\n{ x: f32, y: f32, z: f32, w: f32 }" },
    { "Double2", "-- A 2D vector of f64\n{ x: f64, y: f64 }" },
    { "Double3", "-- A 3D vector of f64\n{ x: f64, y: f64, z: f64 }" },
    { "Double4", "-- A 4D vector of f64\n{ x: f64, y: f64, z: f64, w: f64 }" },
    { "Int2",    "-- A 2D vector of i32, such as a pixel or a grid cell\n{ x: i32, y: i32 }" },
    { "Int3",    "-- A 3D vector of i32\n{ x: i32, y: i32, z: i32 }" },
    { "Int4",    "-- A 4D vector of i32\n{ x: i32, y: i32, z: i32, w: i32 }" },
    { "Quaternion",
                 "-- A 3D rotation as a unit quaternion, stored x, y, z, w\n"
                 "{ x: f64, y: f64, z: f64, w: f64 }" },
    { "Color",   "-- An sRGB color with straight alpha, not premultiplied\n"
                 "{ r: u8\n"
                 "  g: u8\n"
                 "  b: u8\n"
                 "  a: u8                    -- 255 is opaque\n"
                 "}" },
    { "Pose",    "-- A position and orientation in a 3D frame\n"
                 "{ position: Double3        -- [m]\n"
                 "  orientation: Quaternion\n"
                 "}" },
    { "Pose2D",  "-- A position and heading in a 2D frame, the top view of a Pose\n"
                 "{ position: Double2        -- [m or px]\n"
                 "  angle: f64               -- [rad] from +x toward +y\n"
                 "}" },
    { "Transform",
                 "-- A Pose and the frame it is measured in\n"
                 "{ pose: Pose\n"
                 "  parent: string<30>       -- the parent frame, empty when unstated\n"
                 "}" },
    { "Transform2D",
                 "-- A Pose2D and the frame it is measured in\n"
                 "{ pose: Pose2D\n"
                 "  parent: string<30>       -- the parent frame, empty when unstated\n"
                 "}" },
    { "Twist",   "-- Linear and angular velocity in 3D\n"
                 "{ linear: Double3          -- [m/s]\n"
                 "  angular: Double3         -- [rad/s] about each axis\n"
                 "}" },
    { "Twist2D", "-- Linear and angular velocity in 2D\n"
                 "{ linear: Double2          -- [m/s]\n"
                 "  angular: f64             -- [rad/s] from +x toward +y\n"
                 "}" },
    { "Wrench",  "-- A force and a torque in 3D\n"
                 "{ force: Double3           -- [N]\n"
                 "  torque: Double3          -- [N m] about the frame origin\n"
                 "}" },
    { "Wrench2D","-- A force and a torque in 2D\n"
                 "{ force: Double2           -- [N]\n"
                 "  torque: f64              -- [N m] from +x toward +y\n"
                 "}" },
    { "GeoPoint","-- A point on the Earth\n"
                 "{ lat: f64                 -- [deg] latitude\n"
                 "  lon: f64                 -- [deg] longitude\n"
                 "  alt: f64                 -- [m] altitude\n"
                 "}" },
    { "AlignedBox",
                 "-- A box along the frame axes\n"
                 "{ min: Double3             -- [m] the corner with the lowest x, y and z\n"
                 "  max: Double3             -- [m] the opposite corner\n"
                 "}" },
    { "AlignedBox2D",
                 "-- A rectangle along the frame axes, also a region of an image\n"
                 "{ min: Double2             -- [m or px] the corner with the lowest x and y\n"
                 "  max: Double2             -- [m or px] the opposite corner\n"
                 "}" },
    { "OrientedBox",
                 "-- A box at any orientation\n"
                 "{ pose: Pose               -- the center and orientation\n"
                 "  size: Double3            -- [m] the full edge lengths\n"
                 "}" },
    { "OrientedBox2D",
                 "-- A rectangle at any angle\n"
                 "{ pose: Pose2D             -- the center and angle\n"
                 "  size: Double2            -- [m or px] the full edge lengths\n"
                 "}" },
    { "Plane",   "-- An endless flat plane\n"
                 "{ position: Double3        -- [m] any point on the plane\n"
                 "  normal: Double3          -- unit length\n"
                 "}" },
    { "Segment", "-- A straight line between two points\n"
                 "{ a: Double3               -- [m]\n"
                 "  b: Double3               -- [m]\n"
                 "}" },
    { "Sphere",  "-- A ball around a center point\n"
                 "{ center: Double3          -- [m]\n"
                 "  radius: f64              -- [m]\n"
                 "}" },
    { "Capsule", "-- A cylinder with rounded ends\n"
                 "{ axis: Segment            -- the centers of the rounded ends\n"
                 "  radius: f64              -- [m]\n"
                 "}" },
    { "Cylinder","-- A cylinder with flat ends\n"
                 "{ axis: Segment            -- the centers of the flat ends\n"
                 "  radius: f64              -- [m]\n"
                 "}" },
    { "Cone",    "-- A cone from a round base to a tip\n"
                 "{ base: Double3            -- [m] the center of the base\n"
                 "  tip: Double3             -- [m]\n"
                 "  radius: f64              -- [m] at the base\n"
                 "}" },
    { "Polygon", "-- A closed flat polygon in 3D\n"
                 "{ points: Double3[]        -- [m] the corners in order\n"
                 "}" },
    { "Plane2D", "-- An endless line, the 2D form of a Plane\n"
                 "{ position: Double2        -- [m or px] any point on the line\n"
                 "  normal: Double2          -- unit length\n"
                 "}" },
    { "Segment2D",
                 "-- A straight line between two points in 2D\n"
                 "{ a: Double2               -- [m or px]\n"
                 "  b: Double2               -- [m or px]\n"
                 "}" },
    { "Circle",  "-- A circle around a center point\n"
                 "{ center: Double2          -- [m or px]\n"
                 "  radius: f64              -- [m or px]\n"
                 "}" },
    { "Capsule2D",
                 "-- A rectangle with rounded ends\n"
                 "{ axis: Segment2D          -- the centers of the rounded ends\n"
                 "  radius: f64              -- [m or px]\n"
                 "}" },
    { "Polygon2D",
                 "-- A closed polygon in 2D\n"
                 "{ points: Double2[]        -- [m or px] the corners in order\n"
                 "}" },
    { "Empty",   "-- No data, for a signal such as a trigger\n{ }" },
    { "Uuid",    "-- A 16 byte unique id\nu8[16]                     -- RFC 4122 byte order" },
    { "Timestamp",
                 "-- A moment in time, counted from the Unix epoch in UTC\ni64     -- [us]" },
    { "Duration","-- A length of time\ni64                        -- [us]" },
    { "Matrix3x3",
                 "-- A 3 by 3 matrix, row major\nf32[9]" },
    { "Matrix4x4",
                 "-- A 4 by 4 matrix, row major\nf32[16]" },
    { "Uri",     "-- A URL or other URI\nstring<256>" },
    { "Image",   "-- A picture, as raw pixels or a compressed file\n"
                 "{ width: u32               -- [px]\n"
                 "  height: u32              -- [px]\n"
                 "  stride: u32              -- [bytes] from one row to the next, 0 when rows are packed\n"
                 "  format: enum<u8> {       -- how data is laid out\n"
                 "    Mono8 = 0              -- one 8 bit channel\n"
                 "    Mono16 = 1             -- one 16 bit channel\n"
                 "    Rgb8 = 2               -- red, green and blue, 8 bits each\n"
                 "    Rgba8 = 3              -- red, green, blue and alpha, 8 bits each\n"
                 "    Bgr8 = 4               -- blue, green and red, 8 bits each\n"
                 "    Yuyv = 5               -- YUV 4:2:2, two pixels in four bytes\n"
                 "    Nv12 = 6               -- YUV 4:2:0, a Y plane then interleaved U and V\n"
                 "    Monof32 = 7            -- one 32 bit float channel\n"
                 "    Jpeg = 16              -- data is a JPEG file\n"
                 "    Png = 17               -- data is a PNG file\n"
                 "  }\n"
                 "  data: u8[]               -- the pixels from the top row, or the file\n"
                 "}" },
    { "VideoFrame",
                 "-- One encoded frame of a video stream\n"
                 "{ codec: enum<u8> {        -- the bitstream format\n"
                 "    Unknown = 0\n"
                 "    Mjpeg = 1\n"
                 "    H264 = 2\n"
                 "    H265 = 3\n"
                 "    Av1 = 4\n"
                 "  }\n"
                 "  width: u32               -- [px] the coded size, 0 when unstated\n"
                 "  height: u32              -- [px] the coded size, 0 when unstated\n"
                 "  keyframe: bool           -- decodes without earlier frames\n"
                 "  pts: Timestamp           -- [us] when to show the frame\n"
                 "  data: u8[]               -- the encoded bitstream\n"
                 "}" },
    { "ExternalVideoStream",
                 "-- Where to watch a video stream, so a viewer connects to it directly\n"
                 "{ kind: enum<u8> {         -- the protocol of url\n"
                 "    Rtsp = 0\n"
                 "    WebrtcWhep = 1         -- WebRTC set up over WHEP\n"
                 "    Hls = 2\n"
                 "    Srt = 3\n"
                 "    Rtp = 4\n"
                 "    HttpMjpeg = 5          -- MJPEG over HTTP\n"
                 "    Other = 15\n"
                 "  }\n"
                 "  codec: enum<u8> {        -- a hint, Unknown when unstated\n"
                 "    Unknown = 0\n"
                 "    Mjpeg = 1\n"
                 "    H264 = 2\n"
                 "    H265 = 3\n"
                 "    Av1 = 4\n"
                 "  }\n"
                 "  width: u32               -- [px] a hint, 0 when unstated\n"
                 "  height: u32              -- [px] a hint, 0 when unstated\n"
                 "  url: Uri                 -- the stream address\n"
                 "  name: string<32>         -- a label for the stream\n"
                 "}" },
    { "CameraIntrinsics",
                 "-- The pinhole model of a camera and its lens distortion\n"
                 "{ width: u32               -- [px] the resolution these values hold for\n"
                 "  height: u32              -- [px]\n"
                 "  fx: f64                  -- [px] focal length along x\n"
                 "  fy: f64                  -- [px] focal length along y\n"
                 "  cx: f64                  -- [px] principal point x\n"
                 "  cy: f64                  -- [px] principal point y\n"
                 "  model: enum<u8> {        -- the lens distortion model\n"
                 "    None = 0               -- an ideal pinhole\n"
                 "    BrownConrady = 1\n"
                 "    Fisheye = 2\n"
                 "    Rational = 3\n"
                 "  }\n"
                 "  coeffs: f64[8]           -- the model coefficients, 0 past its count\n"
                 "}" },
    { "JointState",
                 "-- The joints of a robot, in the order of its JointNames\n"
                 "{ position: f64[]          -- [rad or m] rad for a turning joint, m for a sliding one\n"
                 "  velocity: f64[]          -- [rad/s or m/s] may be empty\n"
                 "  effort: f64[]            -- [N m or N] may be empty\n"
                 "}" },
    { "JointNames",
                 "-- The joint names a JointState follows, published once\n"
                 "{ name: string<32>[] }" }
};

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

/* No math.h, so a consumer's build line never grows an -lm. Newton from the halved
 * exponent seed converges to full double precision within five steps. */
static double i_rant_sqrt(double x){
    uint64_t b; double r; int i;
    if (!(x > 0.0)) return 0.0;                       /* 0, negatives and NaN alike */
    memcpy(&b, &x, 8);
    b = (b >> 1) + 0x1FF8000000000000ull;
    memcpy(&r, &b, 8);
    for (i = 0; i < 5; i++) r = 0.5 * (r + x / r);
    return r;
}

double rant_double3_length(RantDouble3 a){
    return i_rant_sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}

RantDouble3 rant_double3_normalize(RantDouble3 a){
    double n = rant_double3_length(a);
    return n > 0.0 ? rant_double3_scale(a, 1.0 / n) : a;
}

RantQuaternion rant_quaternion_normalize(RantQuaternion q){
    double n = i_rant_sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n <= 0.0) return rant_quaternion_identity();
    n = 1.0 / n;
    return rant_quaternion(q.x * n, q.y * n, q.z * n, q.w * n);
}

/* v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v), the branch free rotation form */
RantDouble3 rant_quaternion_rotate(RantQuaternion q, RantDouble3 v){
    RantDouble3 u = rant_double3(q.x, q.y, q.z);
    RantDouble3 t = rant_double3_cross(u, v);
    t = rant_double3_add(t, rant_double3_scale(v, q.w));
    return rant_double3_add(v, rant_double3_scale(rant_double3_cross(u, t), 2.0));
}
