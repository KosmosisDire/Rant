"""The standard types of docs/stdtypes.md as Python classes: a field annotated with one
spells as that type on the wire, so the same schema hashes the same in every language."""

import dataclasses as _dataclasses
import enum as _pyenum
from . import _native as _c
from . import _StdAlias, _std, _String, u8, u32, u64, i32, i64, f32, f64, string, enum

Timestamp = _StdAlias("Timestamp", i64)     # microseconds since the Unix epoch, UTC
Duration  = _StdAlias("Duration", i64)      # microseconds
Uuid      = _StdAlias("Uuid", u8[16])       # RFC 4122 byte order
Matrix3x3 = _StdAlias("Matrix3x3", f32[3][3])  # m[row][column]
Matrix4x4 = _StdAlias("Matrix4x4", f32[4][4])
Covariance3 = _StdAlias("Covariance3", f64[3][3])
Covariance6 = _StdAlias("Covariance6", f64[6][6])  # x, y, z, then the turn about each
Uri       = _StdAlias("Uri", _String(256))


def _square(n):
    return _dataclasses.field(default_factory=lambda: [[0.0] * n for _ in range(n)])


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


@_std("Pose")
class Pose:
    position: Double3 = _dataclasses.field(default_factory=Double3)
    orientation: Quaternion = _dataclasses.field(default_factory=Quaternion)


@_std("Pose2D")
class Pose2D:
    position: Double2 = _dataclasses.field(default_factory=Double2)
    angle: f64 = 0.0    # radians, from +x toward +y


# parent "" = unstated, the cap keeps the packed 88 bytes 8 aligned.
@_std("Transform")
class Transform:
    pose: Pose = _dataclasses.field(default_factory=Pose)
    parent: string(30) = ""


@_std("Transform2D")
class Transform2D:
    pose: Pose2D = _dataclasses.field(default_factory=Pose2D)
    parent: string(30) = ""


@_std("Twist")
class Twist:
    linear: Double3 = _dataclasses.field(default_factory=Double3)    # m/s
    angular: Double3 = _dataclasses.field(default_factory=Double3)   # rad/s


@_std("Twist2D")
class Twist2D:
    linear: Double2 = _dataclasses.field(default_factory=Double2)    # m/s
    angular: f64 = 0.0                                                # rad/s, from +x toward +y


@_std("Wrench")
class Wrench:
    force: Double3 = _dataclasses.field(default_factory=Double3)     # N
    torque: Double3 = _dataclasses.field(default_factory=Double3)    # N m about the frame origin


@_std("Wrench2D")
class Wrench2D:
    force: Double2 = _dataclasses.field(default_factory=Double2)     # N
    torque: f64 = 0.0                                                 # N m, from +x toward +y


@_std("Accel")
class Accel:
    linear: Double3 = _dataclasses.field(default_factory=Double3)    # m/s^2
    angular: Double3 = _dataclasses.field(default_factory=Double3)   # rad/s^2 about each axis


@_std("Accel2D")
class Accel2D:
    linear: Double2 = _dataclasses.field(default_factory=Double2)    # m/s^2
    angular: f64 = 0.0                                                # rad/s^2, from +x toward +y


@_std("PoseWithCovariance")
class PoseWithCovariance:
    pose: Pose = _dataclasses.field(default_factory=Pose)
    covariance: Covariance6 = _square(6)


@_std("TwistWithCovariance")
class TwistWithCovariance:
    twist: Twist = _dataclasses.field(default_factory=Twist)
    covariance: Covariance6 = _square(6)


# pose in the parent frame, twist in the body's own frame
@_std("Odometry")
class Odometry:
    pose: PoseWithCovariance = _dataclasses.field(default_factory=PoseWithCovariance)
    twist: TwistWithCovariance = _dataclasses.field(default_factory=TwistWithCovariance)
    parent: string(30) = ""


@_std("GeoPoint")
class GeoPoint:
    lat: f64 = 0.0      # degrees
    lon: f64 = 0.0      # degrees
    alt: f64 = 0.0      # meters


# The shapes, in the units of their frame.

@_std("AlignedBox")
class AlignedBox:
    min: Double3 = _dataclasses.field(default_factory=Double3)       # min <= max on every axis
    max: Double3 = _dataclasses.field(default_factory=Double3)


@_std("AlignedBox2D")
class AlignedBox2D:
    min: Double2 = _dataclasses.field(default_factory=Double2)
    max: Double2 = _dataclasses.field(default_factory=Double2)


@_std("OrientedBox")
class OrientedBox:
    pose: Pose = _dataclasses.field(default_factory=Pose)
    size: Double3 = _dataclasses.field(default_factory=Double3)      # full edge lengths


@_std("OrientedBox2D")
class OrientedBox2D:
    pose: Pose2D = _dataclasses.field(default_factory=Pose2D)
    size: Double2 = _dataclasses.field(default_factory=Double2)


@_std("Plane")
class Plane:
    position: Double3 = _dataclasses.field(default_factory=Double3)  # any point on the plane
    normal: Double3 = _dataclasses.field(default_factory=Double3)    # unit length


@_std("Segment")
class Segment:
    a: Double3 = _dataclasses.field(default_factory=Double3)
    b: Double3 = _dataclasses.field(default_factory=Double3)


@_std("Ray")
class Ray:
    origin: Double3 = _dataclasses.field(default_factory=Double3)
    direction: Double3 = _dataclasses.field(default_factory=Double3)  # unit length, away from origin


@_std("Sphere")
class Sphere:
    center: Double3 = _dataclasses.field(default_factory=Double3)
    radius: f64 = 0.0


@_std("Capsule")
class Capsule:
    axis: Segment = _dataclasses.field(default_factory=Segment)
    radius: f64 = 0.0


@_std("Cylinder")
class Cylinder:
    axis: Segment = _dataclasses.field(default_factory=Segment)      # the cap centers
    radius: f64 = 0.0


@_std("Cone")
class Cone:
    base: Double3 = _dataclasses.field(default_factory=Double3)
    tip: Double3 = _dataclasses.field(default_factory=Double3)
    radius: f64 = 0.0                                                 # at the base


@_std("Polygon")
class Polygon:
    points: list[Double3] = _dataclasses.field(default_factory=list)  # closed, planar


@_std("Plane2D")
class Plane2D:
    position: Double2 = _dataclasses.field(default_factory=Double2)  # any point on the line
    normal: Double2 = _dataclasses.field(default_factory=Double2)    # unit length


@_std("Segment2D")
class Segment2D:
    a: Double2 = _dataclasses.field(default_factory=Double2)
    b: Double2 = _dataclasses.field(default_factory=Double2)


@_std("Ray2D")
class Ray2D:
    origin: Double2 = _dataclasses.field(default_factory=Double2)
    direction: Double2 = _dataclasses.field(default_factory=Double2)


@_std("Circle")
class Circle:
    center: Double2 = _dataclasses.field(default_factory=Double2)
    radius: f64 = 0.0


@_std("Capsule2D")
class Capsule2D:
    axis: Segment2D = _dataclasses.field(default_factory=Segment2D)
    radius: f64 = 0.0


@_std("Polygon2D")
class Polygon2D:
    points: list[Double2] = _dataclasses.field(default_factory=list)  # closed


@_std("Prism")
class Prism:
    pose: Pose = _dataclasses.field(default_factory=Pose)
    base: Polygon2D = _dataclasses.field(default_factory=Polygon2D)  # in the xy plane of pose
    height: f64 = 0.0                                                 # along +z of pose


@_std("Polyline")
class Polyline:
    points: list[Double3] = _dataclasses.field(default_factory=list)  # open


@_std("Polyline2D")
class Polyline2D:
    points: list[Double2] = _dataclasses.field(default_factory=list)


@_std("Path")
class Path:
    poses: list[Pose] = _dataclasses.field(default_factory=list)


@_std("Path2D")
class Path2D:
    poses: list[Pose2D] = _dataclasses.field(default_factory=list)


@_std("Inertia")
class Inertia:
    mass: f64 = 0.0                                                   # kg
    center: Double3 = _dataclasses.field(default_factory=Double3)    # m, in the body frame
    ixx: f64 = 0.0                                                    # kg m^2 about the center
    ixy: f64 = 0.0
    ixz: f64 = 0.0
    iyy: f64 = 0.0
    iyz: f64 = 0.0
    izz: f64 = 0.0


@_std("Empty")
class Empty:
    pass


# The video family. Each member value IS the wire value, so the names and numbers
# below are the same in every binding. A field of one spells as its standard name.

class ImageFormat(_pyenum.IntEnum):
    """How an Image's data is laid out. A value >= 16 is a compressed container, so
    `data` holds the file bytes rather than pixels."""
    __rant_std__ = "ImageFormat"
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
    __rant_std__ = "VideoCodec"
    Unknown = 0
    Mjpeg = 1
    H264 = 2
    H265 = 3
    Av1 = 4


class VideoStreamKind(_pyenum.IntEnum):
    """The protocol an ExternalVideoStream's url speaks."""
    __rant_std__ = "VideoStreamKind"
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
    width: u32 = 0
    height: u32 = 0
    kind: enum(VideoStreamKind, u8) = VideoStreamKind.Rtsp
    codec: enum(VideoCodec, u8) = VideoCodec.Unknown
    url: Uri = ""
    name: string(30) = ""


class DistortionModel(_pyenum.IntEnum):
    """A lens distortion model. None_ is an ideal pinhole, spelled None on the wire."""
    __rant_std__ = "DistortionModel"
    None_ = 0
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
    coeffs: f64[8] = _dataclasses.field(default_factory=lambda: [0.0] * 8)
    model: enum(DistortionModel, u64) = DistortionModel.None_


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
    name: list[str] = _dataclasses.field(default_factory=list)


# time from the start of the trajectory. velocity, acceleration and effort may be empty.
@_std("JointTrajectoryPoint")
class JointTrajectoryPoint:
    time: Duration = 0
    position: list[f64] = _dataclasses.field(default_factory=list)
    velocity: list[f64] = _dataclasses.field(default_factory=list)
    acceleration: list[f64] = _dataclasses.field(default_factory=list)
    effort: list[f64] = _dataclasses.field(default_factory=list)


@_std("JointTrajectory")
class JointTrajectory:
    points: list[JointTrajectoryPoint] = _dataclasses.field(default_factory=list)


# In its own frame. A covariance with -1 first means that value is not measured.
@_std("Imu")
class Imu:
    orientation: Quaternion = _dataclasses.field(default_factory=Quaternion)
    orientation_covariance: Covariance3 = _square(3)
    angular_velocity: Double3 = _dataclasses.field(default_factory=Double3)       # rad/s
    angular_velocity_covariance: Covariance3 = _square(3)
    linear_acceleration: Double3 = _dataclasses.field(default_factory=Double3)    # m/s^2
    linear_acceleration_covariance: Covariance3 = _square(3)


# ranges holds each beam's echoes together, +inf when nothing returns. echoes 0 reads as 1.
@_std("LaserScan")
class LaserScan:
    angle_min: f32 = 0.0                        # rad
    angle_step: f32 = 0.0                       # rad
    time_step: f32 = 0.0                        # s
    range_min: f32 = 0.0                        # m
    range_max: f32 = 0.0                        # m
    echoes: u8 = 0
    ranges: list[f32] = _dataclasses.field(default_factory=list)
    intensities: list[f32] = _dataclasses.field(default_factory=list)


class RangeKind(_pyenum.IntEnum):
    """What a range sensor measures with."""
    __rant_std__ = "RangeKind"
    Unknown = 0
    Ultrasound = 1
    Infrared = 2
    Laser = 3
    Radar = 4


@_std("RangeReading")
class RangeReading:
    distance: f64 = 0.0                         # m, +inf when nothing is in range
    min: f64 = 0.0
    max: f64 = 0.0
    field_of_view: f64 = 0.0                    # rad, the full cone
    kind: enum(RangeKind, u64) = RangeKind.Unknown


class GnssStatus(_pyenum.IntEnum):
    """The quality of a satellite position fix."""
    __rant_std__ = "GnssStatus"
    NoFix = 0
    Fix = 1
    Differential = 2
    RtkFloat = 3
    RtkFixed = 4


@_std("GnssFix")
class GnssFix:
    position: GeoPoint = _dataclasses.field(default_factory=GeoPoint)
    covariance: Covariance3 = _square(3)       # m^2, east, north and up
    status: enum(GnssStatus, u64) = GnssStatus.NoFix


class ScalarKind(_pyenum.IntEnum):
    """The kind of number in a block of raw data, numbered as in PCL."""
    __rant_std__ = "ScalarKind"
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


@_std("PointField")
class PointField:
    name: str = ""
    offset: u32 = 0                             # bytes from the start of the point
    kind: enum(ScalarKind, u8) = ScalarKind.F32
    count: u32 = 0                              # numbers in a row, 1 for a single number


# The PCL layout: point_step bytes per point, little endian. width 0 when it has no rows.
@_std("PointCloud")
class PointCloud:
    origin: Transform = _dataclasses.field(default_factory=Transform)
    width: u32 = 0
    fields: list[PointField] = _dataclasses.field(default_factory=list)
    point_step: u32 = 0
    dense: bool = False
    data: list[u8] = b""


@_std("Mesh")
class Mesh:
    vertices: PointCloud = _dataclasses.field(default_factory=PointCloud)
    triangles: list[u32] = _dataclasses.field(default_factory=list)  # counterclockwise seen from outside


@_std("GridField")
class GridField:
    name: str = ""
    kind: enum(ScalarKind, u8) = ScalarKind.F32
    data: list[u8] = b""                        # one number per cell, x fastest, then y, then z


# origin is the outer corner of cell 0. cells_z is 1 for a 2D grid.
@_std("Grid")
class Grid:
    origin: Transform = _dataclasses.field(default_factory=Transform)
    resolution: f64 = 0.0                       # m
    cells_x: u32 = 0
    cells_y: u32 = 0
    cells_z: u32 = 0
    fields: list[GridField] = _dataclasses.field(default_factory=list)


@_std("Tensor")
class Tensor:
    kind: enum(ScalarKind, u8) = ScalarKind.F32
    shape: list[u32] = _dataclasses.field(default_factory=list)  # outermost first
    data: list[u8] = b""


@_std("File")
class File:
    name: str = ""
    media_type: string(64) = ""
    data: list[u8] = b""


# score 0 to 1, track 0 when not tracked
@_std("Detection2D")
class Detection2D:
    bounds: OrientedBox2D = _dataclasses.field(default_factory=OrientedBox2D)
    label: str = ""
    score: f32 = 0.0
    track: u32 = 0


@_std("Detection3D")
class Detection3D:
    bounds: OrientedBox = _dataclasses.field(default_factory=OrientedBox)
    label: str = ""
    score: f32 = 0.0
    track: u32 = 0


def now():
    """The wall clock in Timestamp units, microseconds since the Unix epoch UTC, the clock a
        message's written_us uses."""
    return int(_c.load().rant_timestamp_now())


# the vocabulary only built the roster, so it stays on rant. and not on rant.types.
del _StdAlias, _std, _String, _square, u8, u32, u64, i32, i64, f32, f64, string, enum
