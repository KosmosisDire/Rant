// What the schema cannot say in Rust, pasted after the generated types.

impl Timestamp {
    /// The wall clock now.
    pub fn now() -> Timestamp {
        let since = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH);
        Timestamp { us: since.map_or(0, |d| i64::try_from(d.as_micros()).unwrap_or(i64::MAX)) }
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

impl Quaternion {
    /// No rotation, unlike the zero a default Quaternion holds.
    pub const IDENTITY: Quaternion = Quaternion { x: 0.0, y: 0.0, z: 0.0, w: 1.0 };
}

impl Pose {
    /// At the origin with no rotation.
    pub fn identity() -> Pose {
        Pose { position: Double3::default(), orientation: Quaternion::IDENTITY }
    }
}

impl Transform {
    /// No translation, no rotation, no parent.
    pub fn identity() -> Transform {
        Transform { pose: Pose::identity(), parent: String::new() }
    }
}

// A vector up to f64 as it is. Down to f32 or to integers loses what it holds, so those have no From.
macro_rules! widen {
    ($($from:ident => $to:ident { $($f:ident),+ }),+ $(,)?) => {$(
        impl From<$from> for $to {
            fn from(v: $from) -> $to {
                $to { $($f: f64::from(v.$f)),+ }
            }
        }
    )+};
}
widen!(Float2 => Double2 { x, y }, Float3 => Double3 { x, y, z }, Float4 => Double4 { x, y, z, w },
       Int2 => Double2 { x, y }, Int3 => Double3 { x, y, z }, Int4 => Double4 { x, y, z, w });

/// The standard types to and from nalgebra's, each a copy of its fields. A rotation into
/// `UnitQuaternion` must be made whole first, which Rant Math does.
#[cfg(feature = "nalgebra")]
mod nalgebra_casts {
    use super::*;
    use nalgebra as na;

    macro_rules! vectors {
        ($($rant:ident, $na:ident, $t:ty { $($f:ident),+ });+ $(;)?) => {$(
            impl From<$rant> for na::$na<$t> {
                fn from(v: $rant) -> Self {
                    na::$na::new($(v.$f),+)
                }
            }
            impl From<na::$na<$t>> for $rant {
                fn from(v: na::$na<$t>) -> Self {
                    $rant { $($f: v.$f),+ }
                }
            }
        )+};
    }
    vectors!(Double2, Vector2, f64 { x, y }; Double3, Vector3, f64 { x, y, z }; Double4, Vector4, f64 { x, y, z, w };
             Float2, Vector2, f32 { x, y }; Float3, Vector3, f32 { x, y, z }; Float4, Vector4, f32 { x, y, z, w });

    macro_rules! matrices {
        ($($rant:ident, $na:ident, $t:ty, $n:literal);+ $(;)?) => {$(
            impl From<$rant> for na::$na<$t> {
                fn from(v: $rant) -> Self {
                    na::$na::from_fn(|r, c| v.m[r][c])
                }
            }
            impl From<na::$na<$t>> for $rant {
                fn from(v: na::$na<$t>) -> Self {
                    let mut m = [[0.0; $n]; $n];
                    for (r, row) in m.iter_mut().enumerate() {
                        for (c, x) in row.iter_mut().enumerate() {
                            *x = v[(r, c)];
                        }
                    }
                    $rant { m }
                }
            }
        )+};
    }
    matrices!(Matrix3x3, Matrix3, f32, 3; Matrix4x4, Matrix4, f32, 4; Covariance3, Matrix3, f64, 3; Covariance6, Matrix6, f64, 6);

    impl From<Quaternion> for na::Quaternion<f64> {
        fn from(q: Quaternion) -> Self {
            na::Quaternion::new(q.w, q.x, q.y, q.z)
        }
    }

    impl From<na::Quaternion<f64>> for Quaternion {
        fn from(q: na::Quaternion<f64>) -> Self {
            Quaternion { x: q.i, y: q.j, z: q.k, w: q.w }
        }
    }

    impl From<na::UnitQuaternion<f64>> for Quaternion {
        fn from(q: na::UnitQuaternion<f64>) -> Self {
            q.into_inner().into()
        }
    }

    impl From<na::Isometry3<f64>> for Pose {
        fn from(p: na::Isometry3<f64>) -> Self {
            Pose { position: p.translation.vector.into(), orientation: p.rotation.into() }
        }
    }
}
