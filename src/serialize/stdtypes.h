/* The standard type library: named types always in scope in the DSL, with layout
 * identical C mirrors. docs/stdtypes.md has the roster and conventions. */
#ifndef DART_STDTYPES_H
#define DART_STDTYPES_H

#include "../common/api.h"
#include "schema.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The roster in table order. DART_STD_NONE is not a standard type. */
typedef enum {
    DART_STD_NONE = 0,
    DART_STD_FLOAT2, DART_STD_FLOAT3, DART_STD_FLOAT4,
    DART_STD_DOUBLE2, DART_STD_DOUBLE3, DART_STD_DOUBLE4,
    DART_STD_INT2, DART_STD_INT3, DART_STD_INT4,
    DART_STD_QUATERNION,
    DART_STD_COLOR,
    DART_STD_RECT, DART_STD_RECTI,
    DART_STD_TRANSFORM, DART_STD_TWIST,
    DART_STD_GEOPOINT,
    DART_STD_UUID,
    DART_STD_TIMESTAMP, DART_STD_DURATION,
    DART_STD_MATRIX3X3, DART_STD_MATRIX4X4,
    DART_STD_URI,
    DART_STD_IMAGE, DART_STD_VIDEOFRAME, DART_STD_EXTERNALVIDEOSTREAM,
    DART_STD_CAMERAINTRINSICS, DART_STD_JOINTSTATE, DART_STD_JOINTNAMES,
    DART_STD_COUNT
} DartStdType;

/* The type's name and its canonical spelling, the two halves of the definition the DSL
 * knows. NULL for DART_STD_NONE or an out of range value. */
DART_API const char *dart_std_name(DartStdType t);
DART_API const char *dart_std_text(DartStdType t);
/* A name lookup only. Use dart_std_recognize when a peer's shape must be verified too. */
DART_API DartStdType dart_std_by_name(DartString name);

/* One standard type compiled as a schema of its own, for a topic whose payload is one. */
DART_API DartSchema *dart_std_schema(DartStdType t, DartAllocFn alloc, void *user);

/* Recognizes a standard type in a schema we did not write: the name and the shape must
 * both match. The result is stable per schema, so cache it. */
DART_API DartStdType dart_std_recognize(const DartSchema *s, DartAllocFn alloc, void *user);
DART_API DartStdType dart_std_recognize_field(const DartSchema *s, uint16_t field,
                                              DartAllocFn alloc, void *user);
DART_API DartStdType dart_std_recognize_elem(const DartSchema *s, uint16_t field,
                                             DartAllocFn alloc, void *user);

/* The C mirrors, layout identical to the wire on a little endian target. */
typedef struct { float  x, y;       } DartFloat2;
typedef struct { float  x, y, z;    } DartFloat3;
typedef struct { float  x, y, z, w; } DartFloat4;
typedef struct { double x, y;       } DartDouble2;
typedef struct { double x, y, z;    } DartDouble3;
typedef struct { double x, y, z, w; } DartDouble4;
typedef struct { int32_t x, y;       } DartInt2;
typedef struct { int32_t x, y, z;    } DartInt3;
typedef struct { int32_t x, y, z, w; } DartInt4;
typedef struct { double x, y, z, w; } DartQuaternion;   /* x, y, z, w in that order */
typedef struct { uint8_t r, g, b, a; } DartColor;       /* sRGB, straight alpha */
typedef struct { float   x, y, w, h; } DartRect;
typedef struct { int32_t x, y, w, h; } DartRectI;
typedef struct { DartDouble3 translation; DartQuaternion rotation;
                 uint16_t parent_len; char parent[30]; } DartTransform;
typedef struct { DartDouble3 linear, angular; } DartTwist;   /* m/s and rad/s */
typedef struct { double lat, lon, alt; } DartGeoPoint;       /* degrees, degrees, meters */
typedef struct { uint8_t bytes[16]; } DartUuid;              /* RFC 4122 byte order */
typedef int64_t DartTimestamp;   /* microseconds since the Unix epoch, UTC */
typedef int64_t DartDuration;                                /* microseconds */
typedef struct { float m[9];  } DartMatrix3x3;               /* row major */
typedef struct { float m[16]; } DartMatrix4x4;               /* row major */

/* Image.format, VideoFrame.codec and ExternalVideoStream.kind as the schema declares
 * them. A format of 16 or more is a compressed container. */
typedef enum {
    DART_IMAGE_MONO8 = 0, DART_IMAGE_MONO16 = 1, DART_IMAGE_RGB8 = 2, DART_IMAGE_RGBA8 = 3,
    DART_IMAGE_BGR8 = 4, DART_IMAGE_YUYV = 5, DART_IMAGE_NV12 = 6,
    DART_IMAGE_MONOF32 = 7,
    DART_IMAGE_JPEG = 16, DART_IMAGE_PNG = 17
} DartImageFormat;
typedef enum {
    DART_VIDEO_UNKNOWN = 0, DART_VIDEO_MJPEG = 1, DART_VIDEO_H264 = 2,
    DART_VIDEO_H265 = 3, DART_VIDEO_AV1 = 4
} DartVideoCodec;
typedef enum {
    DART_DISTORTION_NONE = 0, DART_DISTORTION_BROWN_CONRADY = 1,
    DART_DISTORTION_FISHEYE = 2, DART_DISTORTION_RATIONAL = 3
} DartDistortionModel;
typedef enum {
    DART_STREAM_RTSP = 0, DART_STREAM_WEBRTC_WHEP = 1, DART_STREAM_HLS = 2,
    DART_STREAM_SRT = 3, DART_STREAM_RTP = 4, DART_STREAM_HTTP_MJPEG = 5,
    DART_STREAM_OTHER = 15
} DartStreamKind;

/* The layout pins. A mirror that ever gained padding would silently mis decode. */
typedef char i_dart_std_size_check[
      (sizeof(DartFloat3) == 12 && sizeof(DartFloat4) == 16 &&
       sizeof(DartDouble3) == 24 && sizeof(DartQuaternion) == 32 &&
       sizeof(DartColor) == 4 && sizeof(DartRect) == 16 &&
       sizeof(DartTransform) == 88 && sizeof(DartTwist) == 48 &&
       sizeof(DartUuid) == 16 && sizeof(DartMatrix4x4) == 64) ? 1 : -1];

/* The thin operations, header only. */
static inline DartFloat3 dart_float3(float x, float y, float z){
    DartFloat3 v; v.x = x; v.y = y; v.z = z; return v;
}
static inline DartDouble3 dart_double3(double x, double y, double z){
    DartDouble3 v; v.x = x; v.y = y; v.z = z; return v;
}
static inline DartDouble3 dart_double3_add(DartDouble3 a, DartDouble3 b){
    return dart_double3(a.x + b.x, a.y + b.y, a.z + b.z);
}
static inline DartDouble3 dart_double3_sub(DartDouble3 a, DartDouble3 b){
    return dart_double3(a.x - b.x, a.y - b.y, a.z - b.z);
}
static inline DartDouble3 dart_double3_scale(DartDouble3 a, double k){
    return dart_double3(a.x * k, a.y * k, a.z * k);
}
static inline double dart_double3_dot(DartDouble3 a, DartDouble3 b){
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static inline DartDouble3 dart_double3_cross(DartDouble3 a, DartDouble3 b){
    return dart_double3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
DART_API double        dart_double3_length(DartDouble3 a);        /* needs a square root, in stdtypes.c */
DART_API DartDouble3   dart_double3_normalize(DartDouble3 a);     /* the zero vector maps to itself */

static inline DartQuaternion dart_quaternion(double x, double y, double z, double w){
    DartQuaternion q; q.x = x; q.y = y; q.z = z; q.w = w; return q;
}
static inline DartQuaternion dart_quaternion_identity(void){
    return dart_quaternion(0.0, 0.0, 0.0, 1.0);
}
static inline DartQuaternion dart_quaternion_conjugate(DartQuaternion q){
    return dart_quaternion(-q.x, -q.y, -q.z, q.w);
}
/* The rotation a followed by b, the Hamilton product b times a. */
static inline DartQuaternion dart_quaternion_mul(DartQuaternion a, DartQuaternion b){
    return dart_quaternion(
        b.w * a.x + b.x * a.w + b.y * a.z - b.z * a.y,
        b.w * a.y - b.x * a.z + b.y * a.w + b.z * a.x,
        b.w * a.z + b.x * a.y - b.y * a.x + b.z * a.w,
        b.w * a.w - b.x * a.x - b.y * a.y - b.z * a.z);
}
DART_API DartQuaternion dart_quaternion_normalize(DartQuaternion q);
DART_API DartDouble3    dart_quaternion_rotate(DartQuaternion q, DartDouble3 v);

static inline DartTransform dart_transform_identity(void){
    DartTransform t = {{0}};   /* zero fills the parent name too, it reaches the wire */
    t.rotation = dart_quaternion_identity(); return t;
}
static inline DartColor dart_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a){
    DartColor c; c.r = r; c.g = g; c.b = b; c.a = a; return c;
}
/* 0xRRGGBBAA both ways, the CSS order with the alpha last. */
static inline DartColor dart_color_from_hex(uint32_t rgba){
    return dart_color((uint8_t)(rgba >> 24), (uint8_t)(rgba >> 16),
                      (uint8_t)(rgba >> 8),  (uint8_t)rgba);
}
static inline uint32_t dart_color_to_hex(DartColor c){
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) | ((uint32_t)c.b << 8) | c.a;
}
/* Timestamp arithmetic in microseconds. */
static inline DartDuration  dart_timestamp_diff(DartTimestamp a, DartTimestamp b){ return a - b; }
static inline DartTimestamp dart_timestamp_add (DartTimestamp t, DartDuration d){ return t + d; }
static inline DartDuration  dart_duration_ms   (int64_t ms){ return ms * 1000; }
static inline DartDuration  dart_duration_s    (double s){ return (int64_t)(s * 1000000.0); }
static inline double        dart_duration_to_s (DartDuration d){ return (double)d / 1000000.0; }
/* All zeroes, the conventional unset. */
static inline int dart_uuid_is_nil(DartUuid u){
    int i; for (i = 0; i < 16; i++) if (u.bytes[i]) return 0;
    return 1;
}

#ifdef __cplusplus
}
#endif
#endif /* DART_STDTYPES_H */
