"""The standard types of docs/stdtypes.md as the type checker sees them: every field
is the Python type it decodes to. The runtime is types.py."""

from dataclasses import dataclass
from enum import IntEnum
from typing import TypeAlias

Covariance3: TypeAlias = list[list[float]]
Covariance6: TypeAlias = list[list[float]]
Duration: TypeAlias = int
Matrix3x3: TypeAlias = list[list[float]]
Matrix4x4: TypeAlias = list[list[float]]
Timestamp: TypeAlias = int
Uri: TypeAlias = str
Uuid: TypeAlias = bytes

class DistortionModel(IntEnum):
    None_ = 0
    BrownConrady = 1
    Fisheye = 2
    Rational = 3

class GnssStatus(IntEnum):
    NoFix = 0
    Fix = 1
    Differential = 2
    RtkFloat = 3
    RtkFixed = 4

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

class RangeKind(IntEnum):
    Unknown = 0
    Ultrasound = 1
    Infrared = 2
    Laser = 3
    Radar = 4

class ScalarKind(IntEnum):
    I8 = 1
    U8 = 2
    I16 = 3
    U16 = 4
    I32 = 5
    U32 = 6
    F32 = 7
    F64 = 8
    I64 = 9
    U64 = 10

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
class Accel:
    linear: Double3 = ...
    angular: Double3 = ...

@dataclass
class Accel2D:
    linear: Double2 = ...
    angular: float = 0.0

@dataclass
class AlignedBox:
    min: Double3 = ...
    max: Double3 = ...

@dataclass
class AlignedBox2D:
    min: Double2 = ...
    max: Double2 = ...

@dataclass
class CameraIntrinsics:
    width: int = 0
    height: int = 0
    fx: float = 0.0
    fy: float = 0.0
    cx: float = 0.0
    cy: float = 0.0
    model: DistortionModel = DistortionModel.None_
    coeffs: list[float] = ...

@dataclass
class Capsule:
    axis: Segment = ...
    radius: float = 0.0

@dataclass
class Capsule2D:
    axis: Segment2D = ...
    radius: float = 0.0

@dataclass
class Circle:
    center: Double2 = ...
    radius: float = 0.0

@dataclass
class Color:
    r: int = 0
    g: int = 0
    b: int = 0
    a: int = 255

@dataclass
class Cone:
    base: Double3 = ...
    tip: Double3 = ...
    radius: float = 0.0

@dataclass
class Cylinder:
    axis: Segment = ...
    radius: float = 0.0

@dataclass
class Detection2D:
    bounds: OrientedBox2D = ...
    label: str = ''
    score: float = 0.0
    track: int = 0

@dataclass
class Detection3D:
    bounds: OrientedBox = ...
    label: str = ''
    score: float = 0.0
    track: int = 0

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
class Empty: ...

@dataclass
class ExternalVideoStream:
    kind: VideoStreamKind = VideoStreamKind.Rtsp
    codec: VideoCodec = VideoCodec.Unknown
    width: int = 0
    height: int = 0
    url: Uri = ''
    name: str = ''

@dataclass
class File:
    name: str = ''
    media_type: str = ''
    data: bytes = b''

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
class GnssFix:
    position: GeoPoint = ...
    covariance: Covariance3 = ...
    status: GnssStatus = GnssStatus.NoFix

@dataclass
class Grid:
    origin: Transform = ...
    resolution: float = 0.0
    cells_x: int = 0
    cells_y: int = 0
    cells_z: int = 0
    fields: list[GridField] = ...

@dataclass
class GridField:
    name: str = ''
    kind: ScalarKind = ScalarKind.F32
    data: bytes = b''

@dataclass
class Image:
    width: int = 0
    height: int = 0
    stride: int = 0
    format: ImageFormat = ImageFormat.Mono8
    data: bytes = b''

@dataclass
class Imu:
    orientation: Quaternion = ...
    orientation_covariance: Covariance3 = ...
    angular_velocity: Double3 = ...
    angular_velocity_covariance: Covariance3 = ...
    linear_acceleration: Double3 = ...
    linear_acceleration_covariance: Covariance3 = ...

@dataclass
class Inertia:
    mass: float = 0.0
    center: Double3 = ...
    ixx: float = 0.0
    ixy: float = 0.0
    ixz: float = 0.0
    iyy: float = 0.0
    iyz: float = 0.0
    izz: float = 0.0

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
class JointTrajectory:
    points: list[JointTrajectoryPoint] = ...

@dataclass
class JointTrajectoryPoint:
    time: Duration = 0
    position: list[float] = ...
    velocity: list[float] = ...
    acceleration: list[float] = ...
    effort: list[float] = ...

@dataclass
class LaserScan:
    angle_min: float = 0.0
    angle_step: float = 0.0
    time_step: float = 0.0
    range_min: float = 0.0
    range_max: float = 0.0
    echoes: int = 0
    ranges: list[float] = ...
    intensities: list[float] = ...

@dataclass
class Mesh:
    vertices: PointCloud = ...
    triangles: list[int] = ...

@dataclass
class Odometry:
    pose: PoseWithCovariance = ...
    twist: TwistWithCovariance = ...
    parent: str = ''

@dataclass
class OrientedBox:
    pose: Pose = ...
    size: Double3 = ...

@dataclass
class OrientedBox2D:
    pose: Pose2D = ...
    size: Double2 = ...

@dataclass
class Path:
    poses: list[Pose] = ...

@dataclass
class Path2D:
    poses: list[Pose2D] = ...

@dataclass
class Plane:
    position: Double3 = ...
    normal: Double3 = ...

@dataclass
class Plane2D:
    position: Double2 = ...
    normal: Double2 = ...

@dataclass
class PointCloud:
    origin: Transform = ...
    width: int = 0
    fields: list[PointField] = ...
    point_step: int = 0
    dense: bool = False
    data: bytes = b''

@dataclass
class PointField:
    name: str = ''
    offset: int = 0
    kind: ScalarKind = ScalarKind.F32
    count: int = 0

@dataclass
class Polygon:
    points: list[Double3] = ...

@dataclass
class Polygon2D:
    points: list[Double2] = ...

@dataclass
class Polyline:
    points: list[Double3] = ...

@dataclass
class Polyline2D:
    points: list[Double2] = ...

@dataclass
class Pose:
    position: Double3 = ...
    orientation: Quaternion = ...

@dataclass
class Pose2D:
    position: Double2 = ...
    angle: float = 0.0

@dataclass
class PoseWithCovariance:
    pose: Pose = ...
    covariance: Covariance6 = ...

@dataclass
class Prism:
    pose: Pose = ...
    base: Polygon2D = ...
    height: float = 0.0

@dataclass
class Quaternion:
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0
    w: float = 1.0

@dataclass
class RangeReading:
    distance: float = 0.0
    min: float = 0.0
    max: float = 0.0
    field_of_view: float = 0.0
    kind: RangeKind = RangeKind.Unknown

@dataclass
class Ray:
    origin: Double3 = ...
    direction: Double3 = ...

@dataclass
class Ray2D:
    origin: Double2 = ...
    direction: Double2 = ...

@dataclass
class Segment:
    a: Double3 = ...
    b: Double3 = ...

@dataclass
class Segment2D:
    a: Double2 = ...
    b: Double2 = ...

@dataclass
class Sphere:
    center: Double3 = ...
    radius: float = 0.0

@dataclass
class Tensor:
    kind: ScalarKind = ScalarKind.F32
    shape: list[int] = ...
    data: bytes = b''

@dataclass
class Transform:
    pose: Pose = ...
    parent: str = ''

@dataclass
class Transform2D:
    pose: Pose2D = ...
    parent: str = ''

@dataclass
class Twist:
    linear: Double3 = ...
    angular: Double3 = ...

@dataclass
class Twist2D:
    linear: Double2 = ...
    angular: float = 0.0

@dataclass
class TwistWithCovariance:
    twist: Twist = ...
    covariance: Covariance6 = ...

@dataclass
class VideoFrame:
    codec: VideoCodec = VideoCodec.Unknown
    width: int = 0
    height: int = 0
    keyframe: bool = False
    pts: Timestamp = 0
    data: bytes = b''

@dataclass
class Wrench:
    force: Double3 = ...
    torque: Double3 = ...

@dataclass
class Wrench2D:
    force: Double2 = ...
    torque: float = 0.0

def now() -> int: ...
