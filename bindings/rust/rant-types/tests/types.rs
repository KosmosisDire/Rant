//! The standard types with no middleware.

use rant_types::*;

#[test]
fn a_default_value_is_zero() {
    let camera = CameraIntrinsics::default();
    assert_eq!((camera.fx, camera.coeffs[7]), (0.0, 0.0));
    assert_eq!(camera.model, DistortionModel::None);
    assert_eq!(Image::default().format, ImageFormat::Mono8);
    assert_eq!(PointField::default().kind, ScalarKind::I8, "an enum with no 0 option starts on its first");
    assert_eq!(Quaternion::default().w, 0.0);
    assert!(Uuid::default().is_nil());
}

#[test]
fn a_fixed_type_is_its_wire_bytes() {
    assert_eq!(std::mem::size_of::<RangeReading>(), 40);
    assert_eq!(std::mem::size_of::<ExternalVideoStream>(), 300);
    let mut camera = CameraIntrinsics { fx: 2.0, model: DistortionModel::Rational, ..Default::default() };
    camera.coeffs[7] = -0.5;
    // SAFETY: repr(C) with no padding, so every byte is a number
    let bytes = unsafe { std::slice::from_raw_parts((&camera as *const CameraIntrinsics).cast::<u8>(), 112) };
    assert_eq!(bytes[8..16], 2.0f64.to_le_bytes());
    assert_eq!(bytes[96..104], (-0.5f64).to_le_bytes());
    assert_eq!(bytes[104..112], 3u64.to_le_bytes());
}

#[test]
fn the_identities() {
    assert_eq!(Quaternion::IDENTITY.w, 1.0);
    assert_eq!(Pose::identity().orientation.w, 1.0);
    let t = Transform::identity();
    assert_eq!((t.pose.orientation.w, t.parent.as_str()), (1.0, ""));
}

#[test]
fn a_capped_string_refuses_what_does_not_fit() {
    let s: String<4> = "ab".parse().unwrap();
    assert_eq!(s, "ab");
    assert_eq!(s.capacity(), 4);
    let refused: Result<String<2>, _> = "abc".parse();
    assert!(refused.unwrap_err().to_string().contains("over 2"));
}

#[test]
fn time_and_color() {
    assert!(Timestamp::now().us > 1_600_000_000_000_000);
    assert!(Timestamp { us: 1 } < Timestamp { us: 2 });
    let d: std::time::Duration = Duration { us: 1500 }.into();
    assert_eq!(d.as_micros(), 1500);
    assert_eq!(Duration::from(std::time::Duration::from_micros(7)).us, 7);
    assert_eq!(Color::from_hex(0x11223344).to_hex(), 0x11223344);
    assert_eq!(Color::from_hex(0x11223344).a, 0x44);
}

#[cfg(feature = "nalgebra")]
#[test]
fn nalgebra_casts() {
    use nalgebra as na;
    let v = Double3 { x: 1.0, y: 2.0, z: 3.0 };
    assert_eq!(Double3::from(na::Vector3::<f64>::from(v)), v);
    let m = Matrix3x3 { m: [[1.0, 2.0, 3.0], [4.0, 5.0, 6.0], [7.0, 8.0, 9.0]] };
    let n = na::Matrix3::<f32>::from(m);
    assert_eq!(n[(0, 2)], 3.0);
    assert_eq!(Matrix3x3::from(n), m);
    let q = Quaternion { x: 0.1, y: 0.2, z: 0.3, w: 0.4 };
    assert_eq!(Quaternion::from(na::Quaternion::<f64>::from(q)), q);
    let pose = Pose::from(na::Isometry3::new(na::Vector3::new(1.0, 2.0, 3.0), na::Vector3::zeros()));
    assert_eq!((pose.position, pose.orientation.w), (Double3 { x: 1.0, y: 2.0, z: 3.0 }, 1.0));
}
