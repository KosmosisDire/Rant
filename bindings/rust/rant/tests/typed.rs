// Typed messages: the derive, the codec, rant::String, the standard types, bare roots, a
// rebased decode, and the golden hashes every binding pins.

mod common;

use std::fmt::Debug;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use common::{golden, options, wait_for};
use rant::types::{Double3, Float3, Image, ImageFormat, Pose, Quaternion, Timestamp, Transform, Uuid, VideoCodec, VideoFrame};
use rant::{Bytes, ErrorKind, Message, MessageBuilder, Node, Qos, Threading};

#[derive(rant::Message, Clone, Debug, PartialEq)]
struct Conveyor {
    speed: f64,
    running: bool,
    part: rant::String<16>,
}

#[derive(rant::Enum, Clone, Copy, Debug, PartialEq)]
#[repr(u8)]
enum Mode {
    Idle,
    Run = 2,
    Fault,
}

#[derive(rant::Message, Clone, Debug, PartialEq, Default)]
struct Corner {
    x: f32,
    y: f32,
}

#[derive(rant::Message, Clone, Debug, PartialEq)]
struct Shape {
    mode: Mode,
    bounds: [Corner; 2],
    corners: Vec<Corner>,
    weights: Vec<f32>,
    label: String,
    tags: [rant::String<8>; 2],
    names: Vec<rant::String<8>>,
    at: Transform,
    when: Timestamp,
    id: Uuid,
    scale: [f64; 3],
    origin: Corner,
}

fn shape() -> Shape {
    Shape {
        mode: Mode::Fault,
        bounds: [Corner { x: -1.0, y: -2.0 }, Corner { x: 3.0, y: 4.0 }],
        corners: vec![Corner { x: 0.5, y: 0.25 }, Corner { x: 1.5, y: 2.5 }, Corner { x: 9.0, y: 8.0 }],
        weights: vec![1.0, 2.0, 3.0],
        label: "a label of any length".to_owned(),
        tags: ["red".parse().unwrap(), "fast".parse().unwrap()],
        names: vec!["one".parse().unwrap(), "two".parse().unwrap()],
        at: Transform { pose: Pose { position: Double3 { x: 1.0, y: 2.0, z: 3.0 }, orientation: Quaternion::IDENTITY }, parent: "map".parse().unwrap() },
        when: Timestamp { us: 1_700_000_000_000_000 },
        id: Uuid { bytes: [7; 16] },
        scale: [1.0, 2.0, 0.5],
        origin: Corner { x: 0.0, y: 1.0 },
    }
}

/// Sends `value` from one node to a typed subscriber on another and returns what arrived.
fn round_trip<T: Message + Clone + Send + Debug>(offset: u16, name: &str, value: T) -> T {
    let opts = options(offset, Threading::ServiceThread).max_topics(32);
    let a = Node::new("typed_pub", opts.clone()).unwrap();
    let b = Node::new("typed_sub", opts).unwrap();
    let got: Arc<Mutex<Option<T>>> = Arc::new(Mutex::new(None));
    let g = Arc::clone(&got);
    let _sub = b.subscriber::<T>(name, Qos::new().reliable(true), move |v| *g.lock().unwrap() = Some(v.into_inner())).unwrap();
    let p = a.publisher::<T>(name, Qos::new().reliable(true)).unwrap();
    assert!(wait_for(4000, None, || p.match_count() > 0 && p.ready()), "{name} matched");
    p.send(&value).unwrap();
    assert!(wait_for(3000, None, || got.lock().unwrap().is_some()), "{name} arrived");
    got.lock().unwrap().take().unwrap()
}

#[test]
fn a_derived_struct_round_trips_with_its_envelope() {
    let opts = options(20, Threading::ServiceThread);
    let a = Node::new("line1", opts.clone()).unwrap();
    let b = Node::new("watcher", opts).unwrap();
    let got = Arc::new(Mutex::new(None));
    let g = Arc::clone(&got);
    let _sub = b
        .subscriber::<Conveyor>("line1/conveyor", Qos::new().reliable(true), move |c| {
            *g.lock().unwrap() = Some((c.speed, c.publisher_name().to_owned(), c.view().get_f64("speed"), c.into_inner()));
        })
        .unwrap();
    let p = a.publisher::<Conveyor>("line1/conveyor", Qos::new().reliable(true)).unwrap();
    assert!(wait_for(4000, None, || p.match_count() > 0 && p.ready()));
    let sent = Conveyor { speed: 0.5, running: true, part: "A7".parse().unwrap() };
    p.send(&sent).unwrap();
    assert!(wait_for(3000, None, || got.lock().unwrap().is_some()));
    let (speed, from, by_name, whole) = got.lock().unwrap().take().unwrap();
    assert_eq!((speed, from.as_str(), by_name), (0.5, "line1", 0.5));
    assert_eq!(whole, sent);
}

#[test]
fn the_derived_schema_is_the_dsl_every_binding_spells() {
    let opts = options(21, Threading::Manual);
    let a = Node::new("spell_a", opts.clone()).unwrap();
    let b = Node::new("spell_b", opts).unwrap();
    let conveyor = a.publisher::<Conveyor>("c", Qos::new()).unwrap().schema().unwrap();
    assert_eq!(conveyor.hash(), b.schema("Conveyor { speed: f64, running: bool, part: string<16> }").unwrap().hash());
    let shape = a.publisher::<Shape>("s", Qos::new()).unwrap().schema().unwrap();
    let text = "Corner { x: f32, y: f32 }\n\
                Shape { mode: enum<u8> { Idle=0, Run=2, Fault=3 }, bounds: Corner[2], corners: Corner[], \
                weights: f32[], label: string, tags: string<8>[2], names: string<8>[], at: Transform, \
                when: Timestamp, id: Uuid, scale: f64[3], origin: Corner }";
    assert_eq!(shape.hash(), b.schema(text).unwrap().hash(), "Shape spells as\n{}", shape.to_dsl());
}

#[test]
fn standard_types_and_bare_roots_hash_as_golden() {
    let a = Node::new("golden", options(22, Threading::Manual).max_topics(16)).unwrap();
    fn hash<T: Message>(n: &Node, name: &str) -> u64 {
        n.publisher::<T>(name, Qos::new()).unwrap().schema().unwrap().hash()
    }
    assert_eq!(hash::<Float3>(&a, "f3"), golden("Float3"));
    assert_eq!(hash::<Image>(&a, "img"), golden("Image"));
    assert_eq!(hash::<VideoFrame>(&a, "vf"), golden("VideoFrame"));
    assert_eq!(hash::<rant::types::ExternalVideoStream>(&a, "evs"), golden("ExternalVideoStream"));
    assert_eq!(hash::<bool>(&a, "b"), golden("bool"));
    assert_eq!(hash::<Vec<f32>>(&a, "fa"), golden("f32[]"));
}

#[test]
fn nested_structs_arrays_and_standard_members_round_trip() {
    assert_eq!(round_trip(23, "shape", shape()), shape());
}

#[test]
fn standard_types_round_trip() {
    let img = Image { width: 2, height: 1, stride: 6, format: ImageFormat::Rgb8, data: vec![1, 2, 3, 4, 5, 6] };
    assert_eq!(round_trip(24, "img", img.clone()), img);
    let frame = VideoFrame { codec: VideoCodec::H264, width: 0, height: 0, keyframe: true, pts: Timestamp { us: 5 }, data: vec![9; 300] };
    assert_eq!(round_trip(25, "frame", frame.clone()), frame);
    assert_eq!(round_trip(26, "xf", Transform::identity()), Transform::identity());
}

#[test]
fn bare_roots_round_trip() {
    assert_eq!(round_trip(27, "f", 2.5f32), 2.5);
    assert_eq!(round_trip(28, "s", "hello".to_owned()), "hello");
    assert_eq!(round_trip(29, "v", vec![1.5f64, -2.0]), vec![1.5, -2.0]);
    assert_eq!(round_trip(30, "a", [1u16, 2, 3]), [1, 2, 3]);
    assert_eq!(round_trip(31, "m", Mode::Run), Mode::Run);
    let corners = vec![Corner { x: 1.0, y: 2.0 }, Corner { x: 3.0, y: 4.0 }];
    assert_eq!(round_trip(32, "c", corners.clone()), corners);
}

#[derive(rant::Message, Debug, PartialEq)]
#[rant(name = "Reading")]
struct ReadingSubset {
    c: rant::String<8>,
    a: f64,
}

#[test]
fn a_reader_of_fewer_fields_decodes_the_writers_layout() {
    let opts = options(33, Threading::ServiceThread);
    let a = Node::new("wide_writer", opts.clone()).unwrap();
    let b = Node::new("narrow_reader", opts).unwrap();
    let got = Arc::new(Mutex::new(None));
    let g = Arc::clone(&got);
    let _sub = b.subscriber::<ReadingSubset>("r", Qos::new().reliable(true), move |r| *g.lock().unwrap() = Some(r.into_inner())).unwrap();
    let wide = a.schema("Reading { a: f64, b: u32, c: string<8> }").unwrap();
    let p = a.publisher::<Bytes>("r", Qos::new().reliable(true).schema(&wide)).unwrap();
    assert!(wait_for(4000, None, || p.match_count() > 0 && p.ready()));
    let mut m = MessageBuilder::new(&wide);
    m.set_f64("a", 4.25).unwrap().set_uint("b", 9).unwrap().set_string("c", "ok").unwrap();
    p.send(m.bytes()).unwrap();
    assert!(wait_for(3000, None, || got.lock().unwrap().is_some()));
    assert_eq!(got.lock().unwrap().take().unwrap(), ReadingSubset { c: "ok".parse().unwrap(), a: 4.25 });
}

#[derive(rant::Message)]
struct Unwired {
    names: Vec<String>,
}

#[test]
fn shapes_the_wire_cannot_carry_are_refused_at_creation() {
    let a = Node::new("refuser", options(34, Threading::Manual)).unwrap();
    let e = a.publisher::<Unwired>("u", Qos::new()).err().unwrap();
    assert_eq!(e.kind(), ErrorKind::BadSchema, "{e}");
    let s = a.schema("{ v: f64 }").unwrap();
    let e = a.publisher::<f64>("t", Qos::new().schema(&s)).err().unwrap();
    assert_eq!(e.kind(), ErrorKind::State, "a typed handle refuses a schema: {e}");
    let e = a.subscriber::<f64>("t", Qos::new().reflect_from_mesh(true), |_| {}).err().unwrap();
    assert_eq!(e.kind(), ErrorKind::State, "a typed handle refuses reflect_from_mesh: {e}");
    assert!("seventeen bytes!!".parse::<rant::String<16>>().is_err(), "an over capacity string is refused");
}

#[test]
fn a_pulled_typed_subscriber_takes_values() {
    let opts = options(35, Threading::ServiceThread);
    let a = Node::new("pt_pub", opts.clone()).unwrap();
    let b = Node::new("pt_sub", opts).unwrap();
    let mut sub = b.pulled_subscriber::<Conveyor>("pt", Qos::new().reliable(true).keep_last(4)).unwrap();
    let p = a.publisher::<Conveyor>("pt", Qos::new().reliable(true).keep_last(4)).unwrap();
    assert!(wait_for(4000, None, || p.match_count() > 0 && p.ready()));
    let c = Conveyor { speed: 1.25, running: false, part: rant::String::new() };
    p.send(&c).unwrap();
    let mut got = None;
    assert!(wait_for(3000, None, || {
        got = sub.take(Duration::from_millis(10)).unwrap();
        got.is_some()
    }));
    assert_eq!(got.unwrap(), c);
}
