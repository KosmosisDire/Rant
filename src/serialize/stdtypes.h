/* The standard type library: named types always in scope in the DSL, with layout
 * identical C mirrors. docs/stdtypes.md has the roster and conventions. */
#ifndef RAMBLE_STDTYPES_H
#define RAMBLE_STDTYPES_H

#include "../common/api.h"
#include "schema.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The roster in table order. RAMBLE_STD_NONE is not a standard type. */
typedef enum {
    RAMBLE_STD_NONE = 0,
    RAMBLE_STD_FLOAT2, RAMBLE_STD_FLOAT3, RAMBLE_STD_FLOAT4,
    RAMBLE_STD_DOUBLE2, RAMBLE_STD_DOUBLE3, RAMBLE_STD_DOUBLE4,
    RAMBLE_STD_INT2, RAMBLE_STD_INT3, RAMBLE_STD_INT4,
    RAMBLE_STD_QUATERNION,
    RAMBLE_STD_COLOR,
    RAMBLE_STD_RECT, RAMBLE_STD_RECTI,
    RAMBLE_STD_TRANSFORM, RAMBLE_STD_TWIST,
    RAMBLE_STD_GEOPOINT,
    RAMBLE_STD_UUID,
    RAMBLE_STD_TIMESTAMP, RAMBLE_STD_DURATION,
    RAMBLE_STD_MATRIX3X3, RAMBLE_STD_MATRIX4X4,
    RAMBLE_STD_URI,
    RAMBLE_STD_IMAGE, RAMBLE_STD_VIDEOFRAME, RAMBLE_STD_EXTERNALVIDEOSTREAM,
    RAMBLE_STD_CAMERAINTRINSICS, RAMBLE_STD_JOINTSTATE, RAMBLE_STD_JOINTNAMES,
    RAMBLE_STD_COUNT
} RambleStdType;

/* The type's name and its canonical spelling, the two halves of the definition the DSL
 * knows. NULL for RAMBLE_STD_NONE or an out of range value. */
RAMBLE_API const char *ramble_std_name(RambleStdType t);
RAMBLE_API const char *ramble_std_text(RambleStdType t);
/* A name lookup only. Use ramble_std_recognize when a peer's shape must be verified too. */
RAMBLE_API RambleStdType ramble_std_by_name(RambleString name);

/* One standard type compiled as a schema of its own, for a topic whose payload is one. */
RAMBLE_API RambleSchema *ramble_std_schema(RambleStdType t, RambleAllocFn alloc, void *user);

/* Recognizes a standard type in a schema we did not write: the name and the shape must
 * both match. The result is stable per schema, so cache it. */
RAMBLE_API RambleStdType ramble_std_recognize(const RambleSchema *s, RambleAllocFn alloc, void *user);
RAMBLE_API RambleStdType ramble_std_recognize_field(const RambleSchema *s, uint16_t field,
                                              RambleAllocFn alloc, void *user);
RAMBLE_API RambleStdType ramble_std_recognize_elem(const RambleSchema *s, uint16_t field,
                                             RambleAllocFn alloc, void *user);

/* The C mirrors, layout identical to the wire on a little endian target. */
typedef struct { float  x, y;       } RambleFloat2;
typedef struct { float  x, y, z;    } RambleFloat3;
typedef struct { float  x, y, z, w; } RambleFloat4;
typedef struct { double x, y;       } RambleDouble2;
typedef struct { double x, y, z;    } RambleDouble3;
typedef struct { double x, y, z, w; } RambleDouble4;
typedef struct { int32_t x, y;       } RambleInt2;
typedef struct { int32_t x, y, z;    } RambleInt3;
typedef struct { int32_t x, y, z, w; } RambleInt4;
typedef struct { double x, y, z, w; } RambleQuaternion;   /* x, y, z, w in that order */
typedef struct { uint8_t r, g, b, a; } RambleColor;       /* sRGB, straight alpha */
typedef struct { float   x, y, w, h; } RambleRect;
typedef struct { int32_t x, y, w, h; } RambleRectI;
typedef struct { RambleDouble3 translation; RambleQuaternion rotation;
                 uint16_t parent_len; char parent[30]; } RambleTransform;
typedef struct { RambleDouble3 linear, angular; } RambleTwist;   /* m/s and rad/s */
typedef struct { double lat, lon, alt; } RambleGeoPoint;         /* degrees, degrees, meters */
typedef struct { uint8_t bytes[16]; } RambleUuid;                /* RFC 4122 byte order */
typedef int64_t RambleTimestamp;   /* microseconds since the Unix epoch, UTC */
typedef int64_t RambleDuration;                                /* microseconds */
typedef struct { float m[9];  } RambleMatrix3x3;               /* row major */
typedef struct { float m[16]; } RambleMatrix4x4;               /* row major */

/* Image.format, VideoFrame.codec and ExternalVideoStream.kind as the schema declares
 * them. A format of 16 or more is a compressed container. */
typedef enum {
    RAMBLE_IMAGE_MONO8 = 0, RAMBLE_IMAGE_MONO16 = 1, RAMBLE_IMAGE_RGB8 = 2, RAMBLE_IMAGE_RGBA8 = 3,
    RAMBLE_IMAGE_BGR8 = 4, RAMBLE_IMAGE_YUYV = 5, RAMBLE_IMAGE_NV12 = 6,
    RAMBLE_IMAGE_MONOF32 = 7,
    RAMBLE_IMAGE_JPEG = 16, RAMBLE_IMAGE_PNG = 17
} RambleImageFormat;
typedef enum {
    RAMBLE_VIDEO_UNKNOWN = 0, RAMBLE_VIDEO_MJPEG = 1, RAMBLE_VIDEO_H264 = 2,
    RAMBLE_VIDEO_H265 = 3, RAMBLE_VIDEO_AV1 = 4
} RambleVideoCodec;
typedef enum {
    RAMBLE_DISTORTION_NONE = 0, RAMBLE_DISTORTION_BROWN_CONRADY = 1,
    RAMBLE_DISTORTION_FISHEYE = 2, RAMBLE_DISTORTION_RATIONAL = 3
} RambleDistortionModel;
typedef enum {
    RAMBLE_STREAM_RTSP = 0, RAMBLE_STREAM_WEBRTC_WHEP = 1, RAMBLE_STREAM_HLS = 2,
    RAMBLE_STREAM_SRT = 3, RAMBLE_STREAM_RTP = 4, RAMBLE_STREAM_HTTP_MJPEG = 5,
    RAMBLE_STREAM_OTHER = 15
} RambleStreamKind;

/* The layout pins. A mirror that ever gained padding would silently mis decode. */
typedef char i_ramble_std_size_check[
      (sizeof(RambleFloat3) == 12 && sizeof(RambleFloat4) == 16 &&
       sizeof(RambleDouble3) == 24 && sizeof(RambleQuaternion) == 32 &&
       sizeof(RambleColor) == 4 && sizeof(RambleRect) == 16 &&
       sizeof(RambleTransform) == 88 && sizeof(RambleTwist) == 48 &&
       sizeof(RambleUuid) == 16 && sizeof(RambleMatrix4x4) == 64) ? 1 : -1];

/* The thin operations, header only. */
static inline RambleFloat3 ramble_float3(float x, float y, float z){
    RambleFloat3 v; v.x = x; v.y = y; v.z = z; return v;
}
static inline RambleDouble3 ramble_double3(double x, double y, double z){
    RambleDouble3 v; v.x = x; v.y = y; v.z = z; return v;
}
static inline RambleDouble3 ramble_double3_add(RambleDouble3 a, RambleDouble3 b){
    return ramble_double3(a.x + b.x, a.y + b.y, a.z + b.z);
}
static inline RambleDouble3 ramble_double3_sub(RambleDouble3 a, RambleDouble3 b){
    return ramble_double3(a.x - b.x, a.y - b.y, a.z - b.z);
}
static inline RambleDouble3 ramble_double3_scale(RambleDouble3 a, double k){
    return ramble_double3(a.x * k, a.y * k, a.z * k);
}
static inline double ramble_double3_dot(RambleDouble3 a, RambleDouble3 b){
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static inline RambleDouble3 ramble_double3_cross(RambleDouble3 a, RambleDouble3 b){
    return ramble_double3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
RAMBLE_API double          ramble_double3_length(RambleDouble3 a);        /* needs a square root, in stdtypes.c */
RAMBLE_API RambleDouble3   ramble_double3_normalize(RambleDouble3 a);     /* the zero vector maps to itself */

static inline RambleQuaternion ramble_quaternion(double x, double y, double z, double w){
    RambleQuaternion q; q.x = x; q.y = y; q.z = z; q.w = w; return q;
}
static inline RambleQuaternion ramble_quaternion_identity(void){
    return ramble_quaternion(0.0, 0.0, 0.0, 1.0);
}
static inline RambleQuaternion ramble_quaternion_conjugate(RambleQuaternion q){
    return ramble_quaternion(-q.x, -q.y, -q.z, q.w);
}
/* The rotation a followed by b, the Hamilton product b times a. */
static inline RambleQuaternion ramble_quaternion_mul(RambleQuaternion a, RambleQuaternion b){
    return ramble_quaternion(
        b.w * a.x + b.x * a.w + b.y * a.z - b.z * a.y,
        b.w * a.y - b.x * a.z + b.y * a.w + b.z * a.x,
        b.w * a.z + b.x * a.y - b.y * a.x + b.z * a.w,
        b.w * a.w - b.x * a.x - b.y * a.y - b.z * a.z);
}
RAMBLE_API RambleQuaternion ramble_quaternion_normalize(RambleQuaternion q);
RAMBLE_API RambleDouble3    ramble_quaternion_rotate(RambleQuaternion q, RambleDouble3 v);

static inline RambleTransform ramble_transform_identity(void){
    RambleTransform t = {{0}};   /* zero fills the parent name too, it reaches the wire */
    t.rotation = ramble_quaternion_identity(); return t;
}
static inline RambleColor ramble_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a){
    RambleColor c; c.r = r; c.g = g; c.b = b; c.a = a; return c;
}
/* 0xRRGGBBAA both ways, the CSS order with the alpha last. */
static inline RambleColor ramble_color_from_hex(uint32_t rgba){
    return ramble_color((uint8_t)(rgba >> 24), (uint8_t)(rgba >> 16),
                      (uint8_t)(rgba >> 8),  (uint8_t)rgba);
}
static inline uint32_t ramble_color_to_hex(RambleColor c){
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) | ((uint32_t)c.b << 8) | c.a;
}
/* Timestamp arithmetic in microseconds. */
static inline RambleDuration  ramble_timestamp_diff(RambleTimestamp a, RambleTimestamp b){ return a - b; }
static inline RambleTimestamp ramble_timestamp_add (RambleTimestamp t, RambleDuration d){ return t + d; }
static inline RambleDuration  ramble_duration_ms   (int64_t ms){ return ms * 1000; }
static inline RambleDuration  ramble_duration_s    (double s){ return (int64_t)(s * 1000000.0); }
static inline double          ramble_duration_to_s (RambleDuration d){ return (double)d / 1000000.0; }
/* All zeroes, the conventional unset. */
static inline int ramble_uuid_is_nil(RambleUuid u){
    int i; for (i = 0; i < 16; i++) if (u.bytes[i]) return 0;
    return 1;
}

#ifdef __cplusplus
}
#endif
#endif /* RAMBLE_STDTYPES_H */
