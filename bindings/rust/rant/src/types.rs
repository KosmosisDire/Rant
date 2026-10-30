//! The standard types, the same names and wire in every language. docs/stdtypes.md has the
//! roster. Physical frames are right handed with z up, and image rows run top first.

use crate::message::{Decoder, Encoder, Message, Spell};
use crate::{Enum, Message as DeriveMessage};

macro_rules! standard {
    ($( $(#[$m:meta])* $name:ident { $( $f:ident : $t:ty ),* $(,)? } )*) => {$(
        $(#[$m])*
        #[derive(DeriveMessage, Clone, Debug, Default, PartialEq)]
        #[rant(standard)]
        #[allow(missing_docs)]
        pub struct $name { $( pub $f: $t, )* }
    )*};
}

standard! {
    /// Two f32.
    Float2 { x: f32, y: f32 }
    /// Three f32.
    Float3 { x: f32, y: f32, z: f32 }
    /// Four f32.
    Float4 { x: f32, y: f32, z: f32, w: f32 }
    /// Two f64.
    Double2 { x: f64, y: f64 }
    /// Three f64.
    Double3 { x: f64, y: f64, z: f64 }
    /// Four f64.
    Double4 { x: f64, y: f64, z: f64, w: f64 }
    /// Two i32.
    Int2 { x: i32, y: i32 }
    /// Three i32.
    Int3 { x: i32, y: i32, z: i32 }
    /// Four i32.
    Int4 { x: i32, y: i32, z: i32, w: i32 }
    /// A rotation, stored x, y, z, w.
    Quaternion { x: f64, y: f64, z: f64, w: f64 }
    /// sRGB with straight alpha.
    Color { r: u8, g: u8, b: u8, a: u8 }
    /// A position and an orientation.
    Pose { position: Double3, orientation: Quaternion }
    /// A planar pose, angle in radians from +x toward +y.
    Pose2D { position: Double2, angle: f64 }
    /// A pose in a named parent frame, "" = unstated.
    Transform { pose: Pose, parent: crate::String<30> }
    /// A planar pose in a named parent frame.
    Transform2D { pose: Pose2D, parent: crate::String<30> }
    /// Linear m/s and angular rad/s.
    Twist { linear: Double3, angular: Double3 }
    /// Planar linear m/s and angular rad/s.
    Twist2D { linear: Double2, angular: f64 }
    /// Force N and torque N m about the frame origin.
    Wrench { force: Double3, torque: Double3 }
    /// Planar force and torque.
    Wrench2D { force: Double2, torque: f64 }
    /// Linear m/s^2 and angular rad/s^2.
    Accel { linear: Double3, angular: Double3 }
    /// Planar linear m/s^2 and angular rad/s^2.
    Accel2D { linear: Double2, angular: f64 }
    /// A pose and its covariance over x, y, z, then the turn about each.
    PoseWithCovariance { pose: Pose, covariance: Covariance6 }
    /// A twist and its covariance, the linear part first.
    TwistWithCovariance { twist: Twist, covariance: Covariance6 }
    /// Dead reckoning: the pose in the parent frame and the twist in the body frame.
    Odometry { pose: PoseWithCovariance, twist: TwistWithCovariance, parent: crate::String<30> }
    /// Degrees, degrees, meters.
    GeoPoint { lat: f64, lon: f64, alt: f64 }
    /// min <= max on every axis.
    AlignedBox { min: Double3, max: Double3 }
    /// The planar aligned box.
    AlignedBox2D { min: Double2, max: Double2 }
    /// A posed box, full edge lengths.
    OrientedBox { pose: Pose, size: Double3 }
    /// The planar posed box.
    OrientedBox2D { pose: Pose2D, size: Double2 }
    /// Any point on it and its unit normal.
    Plane { position: Double3, normal: Double3 }
    /// A line segment.
    Segment { a: Double3, b: Double3 }
    /// A line from origin, endless along its unit direction.
    Ray { origin: Double3, direction: Double3 }
    /// A sphere.
    Sphere { center: Double3, radius: f64 }
    /// A capsule around a segment.
    Capsule { axis: Segment, radius: f64 }
    /// A cylinder whose axis ends are the cap centers.
    Cylinder { axis: Segment, radius: f64 }
    /// A cone, radius at the base.
    Cone { base: Double3, tip: Double3, radius: f64 }
    /// A closed planar polygon.
    Polygon { points: Vec<Double3> }
    /// A planar line: any point on it and its unit normal.
    Plane2D { position: Double2, normal: Double2 }
    /// A planar segment.
    Segment2D { a: Double2, b: Double2 }
    /// A planar line from origin, endless along its unit direction.
    Ray2D { origin: Double2, direction: Double2 }
    /// A circle.
    Circle { center: Double2, radius: f64 }
    /// A planar capsule.
    Capsule2D { axis: Segment2D, radius: f64 }
    /// A closed planar polygon.
    Polygon2D { points: Vec<Double2> }
    /// A polygon in the xy plane of pose, raised height along its +z.
    Prism { pose: Pose, base: Polygon2D, height: f64 }
    /// An open line through points in order.
    Polyline { points: Vec<Double3> }
    /// An open planar line through points in order.
    Polyline2D { points: Vec<Double2> }
    /// A route of poses to follow in order.
    Path { poses: Vec<Pose> }
    /// A route of planar poses to follow in order.
    Path2D { poses: Vec<Pose2D> }
    /// Mass in kg, the center of mass in the body frame, and kg m^2 about that center.
    Inertia { mass: f64, center: Double3, ixx: f64, ixy: f64, ixz: f64, iyy: f64, iyz: f64, izz: f64 }
    /// Zero bytes on the wire.
    Empty {}
    /// Pixels: stride is bytes per row, rows top first, data laid out per format.
    Image { width: u32, height: u32, stride: u32, format: ImageFormat, data: Vec<u8> }
    /// One encoded video frame. Width and height 0 = unstated.
    VideoFrame { codec: VideoCodec, width: u32, height: u32, keyframe: bool, pts: Timestamp, data: Vec<u8> }
    /// A stream a viewer connects to by URL. Codec and size are hints.
    ExternalVideoStream { kind: VideoStreamKind, codec: VideoCodec, width: u32, height: u32, url: Uri, name: crate::String<32> }
    /// The pinhole model and its lens distortion, coeffs zero past the model's count.
    CameraIntrinsics { width: u32, height: u32, fx: f64, fy: f64, cx: f64, cy: f64, model: DistortionModel, coeffs: [f64; 8] }
    /// SI positions, velocities and efforts. Velocity and effort may be empty.
    JointState { position: Vec<f64>, velocity: Vec<f64>, effort: Vec<f64> }
    /// The order every JointState array follows, published once as a variable.
    JointNames { name: Vec<String> }
    /// The joints at a time from the start. Velocity, acceleration and effort may be empty.
    JointTrajectoryPoint { time: Duration, position: Vec<f64>, velocity: Vec<f64>, acceleration: Vec<f64>, effort: Vec<f64> }
    /// A motion of the joints over time.
    JointTrajectory { points: Vec<JointTrajectoryPoint> }
    /// An inertial reading in its own frame. A covariance with -1 first is not measured.
    Imu {
        orientation: Quaternion, orientation_covariance: Covariance3,
        angular_velocity: Double3, angular_velocity_covariance: Covariance3,
        linear_acceleration: Double3, linear_acceleration_covariance: Covariance3
    }
    /// A planar sweep, each beam's echoes together in ranges, +inf when nothing returns.
    LaserScan {
        angle_min: f32, angle_step: f32, time_step: f32, range_min: f32, range_max: f32,
        echoes: u8, ranges: Vec<f32>, intensities: Vec<f32>
    }
    /// One distance along +x, +inf when nothing is in range.
    RangeReading { distance: f64, min: f64, max: f64, field_of_view: f64, kind: RangeKind }
    /// A satellite fix, covariance east, north and up in m^2.
    GnssFix { position: GeoPoint, covariance: Covariance3, status: GnssStatus }
    /// One value of every point: offset in bytes from the point start, count 1 for one number.
    PointField { name: String, offset: u32, kind: ScalarKind, count: u32 }
    /// The PCL layout, point_step bytes per point, little endian. Width 0 when it has no rows.
    PointCloud { origin: Transform, width: u32, fields: Vec<PointField>, point_step: u32, dense: bool, data: Vec<u8> }
    /// Triangles of three vertex indices, counterclockwise seen from outside.
    Mesh { vertices: PointCloud, triangles: Vec<u32> }
    /// One number per cell, x fastest, then y, then z.
    GridField { name: String, kind: ScalarKind, data: Vec<u8> }
    /// Origin is the outer corner of cell 0, cells_z 1 for a 2D grid.
    Grid { origin: Transform, resolution: f64, cells_x: u32, cells_y: u32, cells_z: u32, fields: Vec<GridField> }
    /// Numbers of any dimensions, shape outermost first, last dimension fastest.
    Tensor { kind: ScalarKind, shape: Vec<u32>, data: Vec<u8> }
    /// A whole file, media_type "" when unknown.
    File { name: String, media_type: crate::String<64>, data: Vec<u8> }
    /// An object in an image or a 2D frame, score 0 to 1, track 0 when not tracked.
    Detection2D { bounds: OrientedBox2D, label: String, score: f32, track: u32 }
    /// An object in a 3D frame.
    Detection3D { bounds: OrientedBox, label: String, score: f32, track: u32 }
}

/// Image.format. 16 and up is a compressed container.
#[derive(Enum, Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
#[repr(u8)]
#[rant(standard)]
#[allow(missing_docs)]
pub enum ImageFormat {
    #[default]
    Mono8 = 0,
    Mono16 = 1,
    Rgb8 = 2,
    Rgba8 = 3,
    Bgr8 = 4,
    Yuyv = 5,
    Nv12 = 6,
    Monof32 = 7,
    Jpeg = 16,
    Png = 17,
}

/// VideoFrame.codec, Unknown = unstated.
#[derive(Enum, Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
#[repr(u8)]
#[rant(standard)]
#[allow(missing_docs)]
pub enum VideoCodec {
    #[default]
    Unknown = 0,
    Mjpeg = 1,
    H264 = 2,
    H265 = 3,
    Av1 = 4,
}

/// ExternalVideoStream.kind.
#[derive(Enum, Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
#[repr(u8)]
#[rant(standard)]
#[allow(missing_docs)]
pub enum VideoStreamKind {
    #[default]
    Rtsp = 0,
    WebrtcWhep = 1,
    Hls = 2,
    Srt = 3,
    Rtp = 4,
    HttpMjpeg = 5,
    Other = 15,
}

/// CameraIntrinsics.model.
#[derive(Enum, Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
#[repr(u8)]
#[rant(standard)]
#[allow(missing_docs)]
pub enum DistortionModel {
    #[default]
    NoDistortion = 0,
    BrownConrady = 1,
    Fisheye = 2,
    Rational = 3,
}

/// RangeReading.kind.
#[derive(Enum, Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
#[repr(u8)]
#[rant(standard)]
#[allow(missing_docs)]
pub enum RangeKind {
    #[default]
    Unknown = 0,
    Ultrasound = 1,
    Infrared = 2,
    Laser = 3,
    Radar = 4,
}

/// GnssFix.status.
#[derive(Enum, Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
#[repr(u8)]
#[rant(standard)]
#[allow(missing_docs)]
pub enum GnssStatus {
    #[default]
    NoFix = 0,
    Fix = 1,
    Differential = 2,
    RtkFloat = 3,
    RtkFixed = 4,
}

/// The number kinds of PointField, GridField and Tensor, numbered as in PCL.
#[derive(Enum, Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
#[repr(u8)]
#[rant(standard)]
#[allow(missing_docs)]
pub enum ScalarKind {
    I8 = 1,
    U8 = 2,
    I16 = 3,
    U16 = 4,
    I32 = 5,
    U32 = 6,
    #[default]
    F32 = 7,
    F64 = 8,
    I64 = 9,
    U64 = 10,
}

/// An alias type: spelled by its standard name, carried as its one field.
macro_rules! alias {
    ($( $(#[$m:meta])* $name:ident ( $f:ident : $t:ty ) $(, $derive:ident)* ; )*) => {$(
        $(#[$m])*
        #[derive(Clone, Copy, Debug, Default, PartialEq $(, $derive)*)]
        pub struct $name {
            #[allow(missing_docs)]
            pub $f: $t,
        }

        impl Message for $name {
            fn __spell(s: &mut Spell) {
                s.standard_alias(stringify!($name), <$t as Message>::__spell)
            }
            fn __encode(&self, e: &mut Encoder<'_>) -> Result<(), String> {
                self.$f.__encode(e)
            }
            fn __decode(d: &mut Decoder<'_>) -> Result<Self, String> {
                Ok($name { $f: <$t as Message>::__decode(d)? })
            }
        }
    )*};
}

alias! {
    /// RFC 4122 bytes in order, all zero = unset.
    Uuid(bytes: [u8; 16]), Eq, Hash;
    /// Microseconds since the Unix epoch, UTC: the clock written times use.
    Timestamp(us: i64), Eq, Hash, PartialOrd, Ord;
    /// Microseconds.
    Duration(us: i64), Eq, Hash, PartialOrd, Ord;
    /// Indexed m[row][column].
    Matrix3x3(m: [[f32; 3]; 3]);
    /// Indexed m[row][column].
    Matrix4x4(m: [[f32; 4]; 4]);
    /// A covariance indexed m[row][column], in the units of its values squared.
    Covariance3(m: [[f64; 3]; 3]);
    /// Over x, y, z, then the turn about each.
    Covariance6(m: [[f64; 6]; 6]);
    /// A URL.
    Uri(value: crate::String<256>), Eq, Hash;
}

impl Timestamp {
    /// The wall clock now.
    pub fn now() -> Timestamp {
        // SAFETY: a plain clock read
        Timestamp { us: unsafe { crate::ffi::rant_timestamp_now() } }
    }
}

impl From<Duration> for std::time::Duration {
    /// A negative duration clamps to zero.
    fn from(d: Duration) -> std::time::Duration {
        std::time::Duration::from_micros(d.us.max(0) as u64)
    }
}

impl From<std::time::Duration> for Duration {
    fn from(d: std::time::Duration) -> Duration {
        Duration { us: i64::try_from(d.as_micros()).unwrap_or(i64::MAX) }
    }
}

impl Uuid {
    /// A random version 4 uuid.
    pub fn random() -> Uuid {
        let mut u = crate::ffi::RantUuid::default();
        // SAFETY: the C fills the 16 bytes
        unsafe { crate::ffi::rant_uuid_new(&mut u) };
        Uuid { bytes: u.bytes }
    }

    /// True when all zero, the conventional unset.
    pub fn is_nil(&self) -> bool {
        self.bytes == [0; 16]
    }
}

impl Color {
    /// From 0xRRGGBBAA, the order CSS reads.
    pub fn from_hex(rgba: u32) -> Color {
        let [r, g, b, a] = rgba.to_be_bytes();
        Color { r, g, b, a }
    }

    /// To 0xRRGGBBAA.
    pub fn to_hex(&self) -> u32 {
        u32::from_be_bytes([self.r, self.g, self.b, self.a])
    }
}

fn to_c3(v: &Double3) -> crate::ffi::RantDouble3 {
    crate::ffi::RantDouble3 { x: v.x, y: v.y, z: v.z }
}

fn from_c3(v: crate::ffi::RantDouble3) -> Double3 {
    Double3 { x: v.x, y: v.y, z: v.z }
}

impl Double3 {
    /// The length.
    pub fn length(&self) -> f64 {
        // SAFETY: plain arithmetic
        unsafe { crate::ffi::rant_double3_length(to_c3(self)) }
    }

    /// The unit vector of the same direction. The zero vector stays zero.
    pub fn normalized(&self) -> Double3 {
        // SAFETY: plain arithmetic
        from_c3(unsafe { crate::ffi::rant_double3_normalize(to_c3(self)) })
    }
}

impl Quaternion {
    /// No rotation.
    pub const IDENTITY: Quaternion = Quaternion { x: 0.0, y: 0.0, z: 0.0, w: 1.0 };

    /// The unit quaternion of the same rotation.
    pub fn normalized(&self) -> Quaternion {
        let q = crate::ffi::RantQuaternion { x: self.x, y: self.y, z: self.z, w: self.w };
        // SAFETY: plain arithmetic
        let n = unsafe { crate::ffi::rant_quaternion_normalize(q) };
        Quaternion { x: n.x, y: n.y, z: n.z, w: n.w }
    }

    /// `v` rotated by this rotation.
    pub fn rotate(&self, v: &Double3) -> Double3 {
        let q = crate::ffi::RantQuaternion { x: self.x, y: self.y, z: self.z, w: self.w };
        // SAFETY: plain arithmetic
        from_c3(unsafe { crate::ffi::rant_quaternion_rotate(q, to_c3(v)) })
    }
}

impl Transform {
    /// No translation, no rotation, no parent.
    pub fn identity() -> Transform {
        Transform { pose: Pose { position: Double3::default(), orientation: Quaternion::IDENTITY }, parent: crate::String::new() }
    }
}
