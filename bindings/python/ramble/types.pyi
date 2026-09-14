"""The standard types of docs/stdtypes.md as the type checker sees them: every field
is the Python type it decodes to. The runtime is types.py."""

from dataclasses import dataclass
from enum import IntEnum
from typing import TypeAlias

Duration: TypeAlias = int
Matrix3x3: TypeAlias = list[float]
Matrix4x4: TypeAlias = list[float]
Timestamp: TypeAlias = int
Uri: TypeAlias = str
Uuid: TypeAlias = bytes

class DistortionModel(IntEnum):
    NoDistortion = 0
    BrownConrady = 1
    Fisheye = 2
    Rational = 3

class ImageFormat(IntEnum):
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

class VideoCodec(IntEnum):
    Unknown = 0
    Mjpeg = 1
    H264 = 2
    H265 = 3
    Av1 = 4

class VideoStreamKind(IntEnum):
    Rtsp = 0
    WebrtcWhep = 1
    Hls = 2
    Srt = 3
    Rtp = 4
    HttpMjpeg = 5
    Other = 15

@dataclass
class CameraIntrinsics:
    width: int = 0
    height: int = 0
    fx: float = 0.0
    fy: float = 0.0
    cx: float = 0.0
    cy: float = 0.0
    model: DistortionModel = DistortionModel.NoDistortion
    coeffs: list[float] = ...

@dataclass
class Color:
    r: int = 0
    g: int = 0
    b: int = 0
    a: int = 255

@dataclass
class Double2:
    x: float = 0.0
    y: float = 0.0

@dataclass
class Double3:
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0

@dataclass
class Double4:
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0
    w: float = 0.0

@dataclass
class ExternalVideoStream:
    kind: VideoStreamKind = VideoStreamKind.Rtsp
    codec: VideoCodec = VideoCodec.Unknown
    width: int = 0
    height: int = 0
    url: Uri = ''
    name: str = ''

@dataclass
class Float2:
    x: float = 0.0
    y: float = 0.0

@dataclass
class Float3:
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0

@dataclass
class Float4:
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0
    w: float = 0.0

@dataclass
class GeoPoint:
    lat: float = 0.0
    lon: float = 0.0
    alt: float = 0.0

@dataclass
class Image:
    width: int = 0
    height: int = 0
    stride: int = 0
    format: ImageFormat = ImageFormat.Mono8
    data: bytes = b''

@dataclass
class Int2:
    x: int = 0
    y: int = 0

@dataclass
class Int3:
    x: int = 0
    y: int = 0
    z: int = 0

@dataclass
class Int4:
    x: int = 0
    y: int = 0
    z: int = 0
    w: int = 0

@dataclass
class JointNames:
    name: list[str] = ...

@dataclass
class JointState:
    position: list[float] = ...
    velocity: list[float] = ...
    effort: list[float] = ...

@dataclass
class Quaternion:
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0
    w: float = 1.0

@dataclass
class Rect:
    x: float = 0.0
    y: float = 0.0
    w: float = 0.0
    h: float = 0.0

@dataclass
class RectI:
    x: int = 0
    y: int = 0
    w: int = 0
    h: int = 0

@dataclass
class Transform:
    translation: Double3 = ...
    rotation: Quaternion = ...
    parent: str = ''

@dataclass
class Twist:
    linear: Double3 = ...
    angular: Double3 = ...

@dataclass
class VideoFrame:
    codec: VideoCodec = VideoCodec.Unknown
    width: int = 0
    height: int = 0
    keyframe: bool = False
    pts: Timestamp = 0
    data: bytes = b''

def now() -> int: ...
