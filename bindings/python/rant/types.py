"""The standard types of docs/stdtypes.md as Python classes: a field annotated with one
spells as that type on the wire, so the same schema hashes the same in every language."""

import dataclasses as _dataclasses
import enum as _pyenum
from . import _native as _c
from . import _StdAlias, _std, _String, u8, u32, i32, i64, f32, f64, string, enum

Timestamp = _StdAlias("Timestamp", i64)     # microseconds since the Unix epoch, UTC
Duration  = _StdAlias("Duration", i64)      # microseconds
Uuid      = _StdAlias("Uuid", u8[16])       # RFC 4122 byte order
Matrix3x3 = _StdAlias("Matrix3x3", f32[9])  # row-major
Matrix4x4 = _StdAlias("Matrix4x4", f32[16])
Uri       = _StdAlias("Uri", _String(256))


@_std("Float2")
class Float2:
    x: f32 = 0.0
    y: f32 = 0.0


@_std("Float3")
class Float3:
    x: f32 = 0.0
    y: f32 = 0.0
    z: f32 = 0.0


@_std("Float4")
class Float4:
    x: f32 = 0.0
    y: f32 = 0.0
    z: f32 = 0.0
    w: f32 = 0.0


@_std("Double2")
class Double2:
    x: f64 = 0.0
    y: f64 = 0.0


@_std("Double3")
class Double3:
    x: f64 = 0.0
    y: f64 = 0.0
    z: f64 = 0.0


@_std("Double4")
class Double4:
    x: f64 = 0.0
    y: f64 = 0.0
    z: f64 = 0.0
    w: f64 = 0.0


@_std("Int2")
class Int2:
    x: i32 = 0
    y: i32 = 0


@_std("Int3")
class Int3:
    x: i32 = 0
    y: i32 = 0
    z: i32 = 0


@_std("Int4")
class Int4:
    x: i32 = 0
    y: i32 = 0
    z: i32 = 0
    w: i32 = 0


@_std("Quaternion")
class Quaternion:
    x: f64 = 0.0
    y: f64 = 0.0
    z: f64 = 0.0
    w: f64 = 1.0     # identity rotation


@_std("Color")
class Color:
    r: u8 = 0
    g: u8 = 0
    b: u8 = 0
    a: u8 = 255


@_std("Rect")
class Rect:
    x: f32 = 0.0
    y: f32 = 0.0
    w: f32 = 0.0
    h: f32 = 0.0


@_std("RectI")
class RectI:
    x: i32 = 0
    y: i32 = 0
    w: i32 = 0
    h: i32 = 0


# Meters and radians. parent "" = unstated, the cap keeps the packed 88 bytes 8 aligned.
@_std("Transform")
class Transform:
    translation: Double3 = _dataclasses.field(default_factory=Double3)
    rotation: Quaternion = _dataclasses.field(default_factory=Quaternion)
    parent: string(30) = ""


@_std("Twist")
class Twist:
    linear: Double3 = _dataclasses.field(default_factory=Double3)    # m/s
    angular: Double3 = _dataclasses.field(default_factory=Double3)   # rad/s


@_std("GeoPoint")
class GeoPoint:
    lat: f64 = 0.0      # degrees
    lon: f64 = 0.0      # degrees
    alt: f64 = 0.0      # meters


# The video family. Each member value IS the wire value, so the names and numbers
# below are the same in every binding.

class ImageFormat(_pyenum.IntEnum):
    """How an Image's data is laid out. A value >= 16 is a compressed container, so
    `data` holds the file bytes rather than pixels."""
    Mono8 = 0
    Mono16 = 1
    Rgb8 = 2
    Rgba8 = 3
    Bgr8 = 4
    Yuyv = 5
    Nv12 = 6
    Monof32 = 7
    Jpeg = 16
    Png = 17


class VideoCodec(_pyenum.IntEnum):
    """The codec a VideoFrame's data is encoded with. Unknown is the unstated
    codec hint (an ExternalVideoStream that does not state one)."""
    Unknown = 0
    Mjpeg = 1
    H264 = 2
    H265 = 3
    Av1 = 4


class VideoStreamKind(_pyenum.IntEnum):
    """The protocol an ExternalVideoStream's url speaks."""
    Rtsp = 0
    WebrtcWhep = 1
    Hls = 2
    Srt = 3
    Rtp = 4
    HttpMjpeg = 5
    Other = 15


@_std("Image")
class Image:
    width: u32 = 0
    height: u32 = 0
    stride: u32 = 0                             # bytes per row, 0 = tightly packed
    format: enum(ImageFormat, u8) = ImageFormat.Mono8
    data: list[u8] = b""                        # pixels, or the file bytes if compressed


@_std("VideoFrame")
class VideoFrame:
    codec: enum(VideoCodec, u8) = VideoCodec.Unknown
    width: u32 = 0                              # the coded size, 0 = unstated
    height: u32 = 0
    keyframe: bool = False
    pts: Timestamp = 0                          # presentation time, the Timestamp clock
    data: list[u8] = b""


# Fully fixed, so it works as a latched variable: hand a viewer a URL, not pixels. codec,
# width and height are hints for pickers, the stream stays authoritative once connected.
@_std("ExternalVideoStream")
class ExternalVideoStream:
    kind: enum(VideoStreamKind, u8) = VideoStreamKind.Rtsp
    codec: enum(VideoCodec, u8) = VideoCodec.Unknown
    width: u32 = 0
    height: u32 = 0
    url: Uri = ""
    name: string(32) = ""


class DistortionModel(_pyenum.IntEnum):
    """A lens distortion model. NoDistortion is an ideal pinhole."""
    NoDistortion = 0
    BrownConrady = 1
    Fisheye = 2
    Rational = 3


# The pinhole model and its lens distortion. coeffs is zero filled past the model's count.
@_std("CameraIntrinsics")
class CameraIntrinsics:
    width: u32 = 0                              # the resolution these numbers are valid for
    height: u32 = 0
    fx: f64 = 0.0
    fy: f64 = 0.0
    cx: f64 = 0.0
    cy: f64 = 0.0
    model: enum(DistortionModel, u8) = DistortionModel.NoDistortion
    coeffs: f64[8] = _dataclasses.field(default_factory=lambda: [0.0] * 8)


# SI: radians or meters, per second, and newtons or newton meters. velocity and effort
# may be empty. The names ride a JointNames variable, not every sample.
@_std("JointState")
class JointState:
    position: list[f64] = _dataclasses.field(default_factory=list)
    velocity: list[f64] = _dataclasses.field(default_factory=list)
    effort: list[f64] = _dataclasses.field(default_factory=list)


# Published once as a variable. The order every JointState array follows.
@_std("JointNames")
class JointNames:
    name: list[string(32)] = _dataclasses.field(default_factory=list)


def now():
    """The wall clock in Timestamp units, microseconds since the Unix epoch UTC, the clock a
        message's written_us uses."""
    return int(_c.load().rant_timestamp_now())


# the vocabulary only built the roster, so it stays on rant. and not on rant.types.
del _StdAlias, _std, _String, u8, u32, i32, i64, f32, f64, string, enum
