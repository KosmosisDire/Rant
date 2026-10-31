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
