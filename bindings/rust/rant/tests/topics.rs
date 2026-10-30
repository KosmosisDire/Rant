// Raw topics: Bytes handles with schemas, MessageBuilder and MessageView, pulled
// subscribers, shared name slots, schema reflection and maps.

mod common;

use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use common::{node, options, wait_for};
use rant::{Bytes, ErrorKind, FieldType, MapItem, MapReader, MapWriter, MessageBuilder, Node, Qos, Threading};

const TICK: &str = "Tick { seq: u64, when: Timestamp, value: f64, at: Transform }";

#[test]
fn raw_fields_cross_by_name() {
    let a = node("raw_pub", 10);
    let b = node("raw_sub", 10);
    let sa = a.schema(TICK).unwrap();
    let sb = b.schema(TICK).unwrap();
    let got = Arc::new(Mutex::new(None));
    let g = Arc::clone(&got);
    let _sub = b
        .subscriber::<Bytes>("tick", Qos::new().reliable(true).schema(&sb), move |m| {
            *g.lock().unwrap() = Some((
                m.get_uint("seq"),
                m.get_int("when"),
                m.get_f64("value"),
                m.get_f64("at.pose.position.x"),
                m.get_string("at.parent").into_owned(),
                m.publisher_name().to_owned(),
                m.schema().map(|s| s.name()),
            ));
        })
        .unwrap();
    let p = a.publisher::<Bytes>("tick", Qos::new().reliable(true).schema(&sa)).unwrap();
    assert!(wait_for(4000, None, || p.match_count() > 0 && p.ready()));
    let mut msg = MessageBuilder::new(&sa);
    msg.set_uint("seq", 7).unwrap().set_int("when", 1234).unwrap().set_f64("value", 0.5).unwrap();
    msg.set_f64("at.pose.position.x", 2.5).unwrap().set_string("at.parent", "map").unwrap();
    let e = msg.set_string("at.parent", "a name far longer than thirty bytes of room").err().unwrap();
    assert_eq!(e.kind(), ErrorKind::SchemaMismatch, "an over capacity string is refused: {e}");
    assert!(msg.set_uint("nope", 1).is_err(), "an unknown field is refused");
    p.send(msg.bytes()).unwrap();
    assert!(wait_for(3000, None, || got.lock().unwrap().is_some()));
    let got = got.lock().unwrap().clone().unwrap();
    assert_eq!(got, (7, 1234, 0.5, 2.5, "map".to_owned(), "raw_pub".to_owned(), Some("Tick".to_owned())));
}

#[test]
fn pulled_subscribers_take_in_order_and_latest() {
    let a = node("pull_pub", 11);
    let b = node("pull_sub", 11);
    let schema = b.schema("Reading { n: u32 }").unwrap();
    let mut sub = b.pulled_subscriber::<Bytes>("pull", Qos::new().reliable(true).keep_last(8).schema(&schema)).unwrap();
    let e = b.pulled_subscriber::<Bytes>("pull", Qos::new().reliable(true).schema(&schema)).err().unwrap();
    assert_eq!(e.kind(), ErrorKind::State, "a second pulled subscriber on a name is refused: {e}");
    let sa = a.schema("Reading { n: u32 }").unwrap();
    let p = a.publisher::<Bytes>("pull", Qos::new().reliable(true).keep_last(8).schema(&sa)).unwrap();
    assert!(wait_for(4000, None, || p.match_count() > 0 && p.ready()));
    for n in 1..=4u32 {
        let mut m = MessageBuilder::new(&sa);
        m.set_uint("n", n as u64).unwrap();
        p.send(m.bytes()).unwrap();
    }
    assert!(p.drain(Duration::from_secs(2)));
    let first = wait_for(3000, None, || sub.take(Duration::ZERO).unwrap().is_some_and(|m| m.get_uint("n") == 1));
    assert!(first, "take reads the oldest first");
    let latest = sub.take_latest(Duration::from_millis(500)).unwrap().map(|m| m.get_uint("n"));
    assert_eq!(latest, Some(4), "take_latest reads the newest and drops the rest");
    assert!(sub.take(Duration::ZERO).unwrap().is_none());

    let mut pushed = b.subscriber::<Bytes>("pushed", Qos::new(), |_| {}).unwrap();
    assert!(pushed.take(Duration::ZERO).is_err(), "a subscriber with a handler cannot take");
}

#[test]
fn same_name_handles_share_one_slot() {
    let a = node("slot", 12);
    let b = node("slot_feeder", 12);
    let schema = a.schema("Slot { v: f64 }").unwrap();
    let hits = Arc::new(AtomicUsize::new(0));
    let h = Arc::clone(&hits);
    let qos = Qos::new().reliable(true).schema(&schema);
    let sub = a.subscriber::<Bytes>("s/v", qos.clone(), move |_| { h.fetch_add(1, Ordering::SeqCst); }).unwrap();
    let pub_same = a.publisher::<Bytes>("s/v", qos.clone()).unwrap();
    let other = a.schema("Other { w: u8 }").unwrap();
    let e = a.publisher::<Bytes>("s/v", Qos::new().schema(&other)).err().unwrap();
    assert_eq!(e.kind(), ErrorKind::SchemaMismatch, "a live name refuses another schema: {e}");

    let sb = b.schema("Slot { v: f64 }").unwrap();
    let p = b.publisher::<Bytes>("s/v", Qos::new().reliable(true).schema(&sb)).unwrap();
    assert!(wait_for(4000, None, || p.match_count() > 0 && p.ready()));
    drop(pub_same);
    let mut m = MessageBuilder::new(&sb);
    m.set_f64("v", 1.0).unwrap();
    p.send(m.bytes()).unwrap();
    assert!(wait_for(3000, None, || hits.load(Ordering::SeqCst) == 1), "the subscriber outlives its sibling publisher");
    sub.close().unwrap();
    let retyped = a.publisher::<Bytes>("s/v", Qos::new().schema(&other));
    assert!(retyped.is_ok(), "the last close retires the name, so it retypes");
}

#[test]
fn schemas_reflect_the_c_field_table() {
    let a = Node::new("schemas", options(13, Threading::Manual)).unwrap();
    let s = a.schema("Mode = enum<u8> { Idle=0, Run=2 }\nShape { mode: Mode, corners: Float2[4], name: string<8>, tags: string, pts: Double2[] }").unwrap();
    assert_eq!(s.name(), "Shape");
    let mode = s.field_index("mode").unwrap();
    let variants: Vec<(i64, String)> = s.enum_variants(mode).into_iter().map(|v| (v.value, v.name)).collect();
    assert_eq!(variants, [(0, "Idle".to_owned()), (2, "Run".to_owned())]);
    let fields = s.fields();
    let at = s.field_index("corners").unwrap() as usize;
    let (corners, elem) = (&fields[at], &fields[at + 1]);
    assert_eq!((corners.kind, corners.count), (FieldType::Array, 4));
    assert_eq!((elem.name.as_str(), elem.kind, elem.type_name.as_deref(), elem.size), ("", FieldType::Struct, Some("Float2"), 8), "the element is the row after its array");
    assert!(s.field_index("corners[2].x").is_some());
    assert_eq!(fields[s.field_index("tags").unwrap() as usize].kind, FieldType::VString);
    assert_eq!(a.schema(&s.to_dsl()).unwrap(), s, "the printed DSL compiles back to the same handle");
    assert_eq!(a.schema_from_wire(&s.wire()).unwrap().hash(), s.hash());

    let wide = a.schema("Wide { a: f64, b: u32 }").unwrap();
    let other = Node::new("schemas2", options(13, Threading::Manual)).unwrap();
    let narrow = other.schema("Wide { a: f64 }").unwrap();
    assert!(narrow.can_read(&wide));
    assert!(!wide.can_read(&narrow));
    assert!(wide.why_not(&narrow).is_some());
    let e = a.schema("Broken { a: nope }").unwrap_err();
    assert_eq!(e.kind(), ErrorKind::BadSchema, "{e}");
}

#[test]
fn map_fields_round_trip() {
    let a = node("maps", 14);
    let b = node("maps_sub", 14);
    let text = "Info { info: map }";
    let got = Arc::new(Mutex::new(None));
    let g = Arc::clone(&got);
    let sb = b.schema(text).unwrap();
    let _sub = b
        .subscriber::<Bytes>("info", Qos::new().reliable(true).schema(&sb), move |m| {
            *g.lock().unwrap() = Some(m.get_map("info").to_map());
        })
        .unwrap();
    let sa = a.schema(text).unwrap();
    let p = a.publisher::<Bytes>("info", Qos::new().reliable(true).schema(&sa)).unwrap();
    assert!(wait_for(4000, None, || p.match_count() > 0 && p.ready()));
    let mut w = MapWriter::new();
    w.put_string("name", "arm").put_uint("joints", 6).put_f64("reach", 1.5);
    w.open_array("ids").put_int("", -1).put_int("", 2).close();
    w.open_map("limits").put_bool("soft", true).close();
    let body = w.finish().unwrap().to_vec();
    assert_eq!(MapReader::new(&body).get("joints").map(|v| v.as_uint()), Some(6));
    let mut m = MessageBuilder::new(&sa);
    m.set_map("info", &body).unwrap();
    p.send(m.bytes()).unwrap();
    assert!(wait_for(3000, None, || got.lock().unwrap().is_some()));
    let map = got.lock().unwrap().take().unwrap();
    assert_eq!(map["name"], MapItem::String("arm".into()));
    assert_eq!(map["joints"], MapItem::UInt(6));
    assert_eq!(map["ids"], MapItem::List(vec![MapItem::Int(-1), MapItem::Int(2)]));
    let MapItem::Map(limits) = &map["limits"] else { panic!("limits is a map") };
    assert_eq!(limits["soft"], MapItem::Bool(true));
}

#[test]
fn counts_and_capture_times_reach_the_reader() {
    let a = node("counts", 15);
    let b = node("counts_sub", 15);
    let captured = Arc::new(AtomicU64::new(0));
    let c = Arc::clone(&captured);
    let sub = b.subscriber::<u64>("c/v", Qos::new().reliable(true), move |m| c.store(m.capture_us(), Ordering::SeqCst)).unwrap();
    let p = a.publisher::<u64>("c/v", Qos::new().reliable(true)).unwrap();
    assert!(wait_for(4000, None, || p.match_count() > 0 && p.ready()));
    p.send_captured(&5, rant::types::Timestamp { us: 42 }).unwrap();
    assert!(wait_for(3000, None, || captured.load(Ordering::SeqCst) == 42));
    assert_eq!(p.counts().tx_msgs, 1);
    assert_eq!(sub.counts().rx_msgs, 1);
}
