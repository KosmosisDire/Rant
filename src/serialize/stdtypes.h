/* The standard type library: named types always in scope in the DSL, with layout
 * identical C mirrors. docs/stdtypes.md has the roster and conventions. */
#ifndef RANT_STDTYPES_H
#define RANT_STDTYPES_H

#include "../common/api.h"
#include "schema.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The roster in table order. RANT_STD_NONE is not a standard type. */
typedef enum {
    RANT_STD_NONE = 0,
    RANT_STD_FLOAT2, RANT_STD_FLOAT3, RANT_STD_FLOAT4,
    RANT_STD_DOUBLE2, RANT_STD_DOUBLE3, RANT_STD_DOUBLE4,
    RANT_STD_INT2, RANT_STD_INT3, RANT_STD_INT4,
    RANT_STD_QUATERNION,
    RANT_STD_COLOR,
    RANT_STD_RECT, RANT_STD_RECTI,
    RANT_STD_TRANSFORM, RANT_STD_TWIST,
    RANT_STD_GEOPOINT,
    RANT_STD_UUID,
    RANT_STD_TIMESTAMP, RANT_STD_DURATION,
    RANT_STD_MATRIX3X3, RANT_STD_MATRIX4X4,
    RANT_STD_URI,
    RANT_STD_IMAGE, RANT_STD_VIDEOFRAME, RANT_STD_EXTERNALVIDEOSTREAM,
    RANT_STD_CAMERAINTRINSICS, RANT_STD_JOINTSTATE, RANT_STD_JOINTNAMES,
    RANT_STD_COUNT
} RantStdType;

/* The type's name and its canonical spelling, the two halves of the definition the DSL
 * knows. NULL for RANT_STD_NONE or an out of range value. */
RANT_API const char *rant_std_name(RantStdType t);
RANT_API const char *rant_std_text(RantStdType t);
/* A name lookup only. Use rant_std_recognize when a peer's shape must be verified too. */
RANT_API RantStdType rant_std_by_name(RantString name);

/* One standard type compiled as a schema of its own, for a topic whose payload is one. */
RANT_API RantSchema *rant_std_schema(RantStdType t, RantAllocFn alloc, void *user);

/* Recognizes a standard type in a schema we did not write: the name and the shape must
 * both match. The result is stable per schema, so cache it. */
RANT_API RantStdType rant_std_recognize(const RantSchema *s, RantAllocFn alloc, void *user);
RANT_API RantStdType rant_std_recognize_field(const RantSchema *s, uint16_t field,
                                              RantAllocFn alloc, void *user);
RANT_API RantStdType rant_std_recognize_elem(const RantSchema *s, uint16_t field,
                                             RantAllocFn alloc, void *user);

/* The C mirrors, layout identical to the wire on a little endian target. */
typedef struct { float  x, y;       } RantFloat2;
typedef struct { float  x, y, z;    } RantFloat3;
typedef struct { float  x, y, z, w; } RantFloat4;
typedef struct { double x, y;       } RantDouble2;
typedef struct { double x, y, z;    } RantDouble3;
typedef struct { double x, y, z, w; } RantDouble4;
typedef struct { int32_t x, y;       } RantInt2;
typedef struct { int32_t x, y, z;    } RantInt3;
typedef struct { int32_t x, y, z, w; } RantInt4;
typedef struct { double x, y, z, w; } RantQuaternion;     /* x, y, z, w in that order */
typedef struct { uint8_t r, g, b, a; } RantColor;         /* sRGB, straight alpha */
typedef struct { float   x, y, w, h; } RantRect;
typedef struct { int32_t x, y, w, h; } RantRectI;
typedef struct { RantDouble3 translation; RantQuaternion rotation;
                 uint16_t parent_len; char parent[30]; } RantTransform;
typedef struct { RantDouble3 linear, angular; } RantTwist;       /* m/s and rad/s */
typedef struct { double lat, lon, alt; } RantGeoPoint;           /* degrees, degrees, meters */
typedef struct { uint8_t bytes[16]; } RantUuid;                  /* RFC 4122 byte order */
typedef int64_t RantTimestamp;     /* microseconds since the Unix epoch, UTC */
typedef int64_t RantDuration;                                  /* microseconds */
typedef struct { float m[9];  } RantMatrix3x3;                 /* row major */
typedef struct { float m[16]; } RantMatrix4x4;                 /* row major */

/* Image.format, VideoFrame.codec and ExternalVideoStream.kind as the schema declares
 * them. A format of 16 or more is a compressed container. */
typedef enum {
    RANT_IMAGE_MONO8 = 0, RANT_IMAGE_MONO16 = 1, RANT_IMAGE_RGB8 = 2, RANT_IMAGE_RGBA8 = 3,
    RANT_IMAGE_BGR8 = 4, RANT_IMAGE_YUYV = 5, RANT_IMAGE_NV12 = 6,
    RANT_IMAGE_MONOF32 = 7,
    RANT_IMAGE_JPEG = 16, RANT_IMAGE_PNG = 17
} RantImageFormat;
typedef enum {
    RANT_VIDEO_UNKNOWN = 0, RANT_VIDEO_MJPEG = 1, RANT_VIDEO_H264 = 2,
    RANT_VIDEO_H265 = 3, RANT_VIDEO_AV1 = 4
} RantVideoCodec;
typedef enum {
    RANT_DISTORTION_NONE = 0, RANT_DISTORTION_BROWN_CONRADY = 1,
    RANT_DISTORTION_FISHEYE = 2, RANT_DISTORTION_RATIONAL = 3
} RantDistortionModel;
typedef enum {
    RANT_STREAM_RTSP = 0, RANT_STREAM_WEBRTC_WHEP = 1, RANT_STREAM_HLS = 2,
    RANT_STREAM_SRT = 3, RANT_STREAM_RTP = 4, RANT_STREAM_HTTP_MJPEG = 5,
    RANT_STREAM_OTHER = 15
} RantStreamKind;

/* The layout pins. A mirror that ever gained padding would silently mis decode. */
typedef char i_rant_std_size_check[
      (sizeof(RantFloat3) == 12 && sizeof(RantFloat4) == 16 &&
       sizeof(RantDouble3) == 24 && sizeof(RantQuaternion) == 32 &&
       sizeof(RantColor) == 4 && sizeof(RantRect) == 16 &&
       sizeof(RantTransform) == 88 && sizeof(RantTwist) == 48 &&
       sizeof(RantUuid) == 16 && sizeof(RantMatrix4x4) == 64) ? 1 : -1];

/* The thin operations, header only. */
static inline RantFloat3 rant_float3(float x, float y, float z){
    RantFloat3 v; v.x = x; v.y = y; v.z = z; return v;
}
static inline RantDouble3 rant_double3(double x, double y, double z){
    RantDouble3 v; v.x = x; v.y = y; v.z = z; return v;
}
static inline RantDouble3 rant_double3_add(RantDouble3 a, RantDouble3 b){
    return rant_double3(a.x + b.x, a.y + b.y, a.z + b.z);
}
static inline RantDouble3 rant_double3_sub(RantDouble3 a, RantDouble3 b){
    return rant_double3(a.x - b.x, a.y - b.y, a.z - b.z);
}
static inline RantDouble3 rant_double3_scale(RantDouble3 a, double k){
    return rant_double3(a.x * k, a.y * k, a.z * k);
}
static inline double rant_double3_dot(RantDouble3 a, RantDouble3 b){
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static inline RantDouble3 rant_double3_cross(RantDouble3 a, RantDouble3 b){
    return rant_double3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
RANT_API double            rant_double3_length(RantDouble3 a);            /* needs a square root, in stdtypes.c */
RANT_API RantDouble3       rant_double3_normalize(RantDouble3 a);         /* the zero vector maps to itself */

static inline RantQuaternion rant_quaternion(double x, double y, double z, double w){
    RantQuaternion q; q.x = x; q.y = y; q.z = z; q.w = w; return q;
}
static inline RantQuaternion rant_quaternion_identity(void){
    return rant_quaternion(0.0, 0.0, 0.0, 1.0);
}
static inline RantQuaternion rant_quaternion_conjugate(RantQuaternion q){
    return rant_quaternion(-q.x, -q.y, -q.z, q.w);
}
/* The rotation a followed by b, the Hamilton product b times a. */
static inline RantQuaternion rant_quaternion_mul(RantQuaternion a, RantQuaternion b){
    return rant_quaternion(
        b.w * a.x + b.x * a.w + b.y * a.z - b.z * a.y,
        b.w * a.y - b.x * a.z + b.y * a.w + b.z * a.x,
        b.w * a.z + b.x * a.y - b.y * a.x + b.z * a.w,
        b.w * a.w - b.x * a.x - b.y * a.y - b.z * a.z);
}
RANT_API RantQuaternion rant_quaternion_normalize(RantQuaternion q);
RANT_API RantDouble3        rant_quaternion_rotate(RantQuaternion q, RantDouble3 v);

static inline RantTransform rant_transform_identity(void){
    RantTransform t = {{0}};     /* zero fills the parent name too, it reaches the wire */
    t.rotation = rant_quaternion_identity(); return t;
}
static inline RantColor rant_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a){
    RantColor c; c.r = r; c.g = g; c.b = b; c.a = a; return c;
}
/* 0xRRGGBBAA both ways, the CSS order with the alpha last. */
static inline RantColor rant_color_from_hex(uint32_t rgba){
    return rant_color((uint8_t)(rgba >> 24), (uint8_t)(rgba >> 16),
                      (uint8_t)(rgba >> 8),  (uint8_t)rgba);
}
static inline uint32_t rant_color_to_hex(RantColor c){
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) | ((uint32_t)c.b << 8) | c.a;
}
/* Timestamp arithmetic in microseconds. */
static inline RantDuration    rant_timestamp_diff(RantTimestamp a, RantTimestamp b){ return a - b; }
static inline RantTimestamp rant_timestamp_add (RantTimestamp t, RantDuration d){ return t + d; }
static inline RantDuration    rant_duration_ms     (int64_t ms){ return ms * 1000; }
static inline RantDuration    rant_duration_s      (double s){ return (int64_t)(s * 1000000.0); }
static inline double          rant_duration_to_s (RantDuration d){ return (double)d / 1000000.0; }
/* All zeroes, the conventional unset. */
static inline int rant_uuid_is_nil(RantUuid u){
    int i; for (i = 0; i < 16; i++) if (u.bytes[i]) return 0;
    return 1;
}

#ifdef __cplusplus
}
#endif
#endif /* RANT_STDTYPES_H */
