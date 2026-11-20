// Field handles: same name subscribers typed as one field each, field remotes of one
// variable that read and write their own part, and the refusals.

mod common;

use std::sync::{Arc, Mutex};
use std::time::Duration;

use common::{node, wait_for};
use rant::{ErrorKind, Qos, VariableOptions};

#[derive(rant::Message, Clone, Debug, PartialEq, Default)]
struct Corner {
    x: f32,
    y: f32,
}

#[derive(rant::Message, Clone, Debug, PartialEq, Default)]
struct Gauge {
    origin: Corner,
    id: u8,
    level: f32,
}

fn gauge(x: f32, id: u8) -> Gauge {
    Gauge { origin: Corner { x, y: 2.0 }, id, level: 3.0 }
}

#[test]
fn topic_field_handles_read_their_field() {
    let a = node("field_pub", 61);
    let b = node("field_sub", 61);
    let rel = Qos::new().reliable(true);
    let (whole, ids, xs) = (Arc::new(Mutex::new(0)), Arc::new(Mutex::new(Vec::new())), Arc::new(Mutex::new(Vec::new())));
    let (w, i, x) = (Arc::clone(&whole), Arc::clone(&ids), Arc::clone(&xs));
    let p = a.publisher::<Gauge>("f/gauge", rel.clone()).unwrap();
    let s_whole = b.subscriber::<Gauge>("f/gauge", rel.clone(), move |_| *w.lock().unwrap() += 1).unwrap();
    let _s_id = b.subscriber::<u8>("f/gauge.id", rel.clone(), move |v| i.lock().unwrap().push(*v)).unwrap();
    let _s_at = b.subscriber::<Corner>("f/gauge.origin", rel.clone(), move |c| x.lock().unwrap().push(c.x)).unwrap();
    assert!(wait_for(4000, None, || p.match_count() > 0 && p.ready()));
    p.send(&gauge(1.5, 9)).unwrap();
    assert!(wait_for(3000, None, || *whole.lock().unwrap() == 1 && *ids.lock().unwrap() == [9] && *xs.lock().unwrap() == [1.5]), "every handle reads its part");
    s_whole.close().unwrap();
    p.send(&gauge(5.0, 4)).unwrap();
    assert!(wait_for(3000, None, || ids.lock().unwrap().last() == Some(&4) && xs.lock().unwrap().last() == Some(&5.0)), "the open handles still receive");
    assert_eq!(*whole.lock().unwrap(), 1, "the closed handle no longer fires");
    let e = a.publisher::<u8>("f/gauge.id", rel).err().unwrap();
    assert_eq!(e.kind(), ErrorKind::State, "a field handle never publishes: {e}");
}

#[test]
fn field_remotes_read_and_write_their_part() {
    let a = node("field_owner", 62);
    let b = node("field_remote", 62);
    let initial = Gauge { origin: Corner { x: 1.0, y: 1.0 }, id: 3, level: 2.0 };
    let owner = a.variable_definition::<Gauge>("f/var", VariableOptions::new().initial(initial)).unwrap();
    let whole = b.remote_variable::<Gauge>("f/var", VariableOptions::new()).unwrap();
    let id = b.remote_variable::<u8>("f/var.id", VariableOptions::new()).unwrap();
    let at = b.remote_variable::<Corner>("f/var.origin", VariableOptions::new()).unwrap();
    assert!(id.wait(Duration::from_secs(4)));
    assert_eq!(id.get().unwrap(), Some(3), "a field remote reads its field");
    assert_eq!(at.get().unwrap(), Some(Corner { x: 1.0, y: 1.0 }));

    let changes = Arc::new(Mutex::new(Vec::new()));
    let c = Arc::clone(&changes);
    id.on_change(move |u| c.lock().unwrap().push(*u)).unwrap();
    assert_eq!(*changes.lock().unwrap(), [3], "on_change replays at registration");
    id.set(&7).unwrap();
    assert!(wait_for(3000, None, || owner.get().unwrap().is_some_and(|g| g.id == 7 && g.origin.x == 1.0 && g.level == 2.0)), "a field write sets only its field");
    at.set(&Corner { x: 6.0, y: 8.0 }).unwrap();
    assert!(wait_for(3000, None, || owner.get().unwrap().is_some_and(|g| g.id == 7 && g.origin == Corner { x: 6.0, y: 8.0 })), "both writes hold");
    assert!(wait_for(3000, None, || whole.get().unwrap().is_some_and(|g| g.id == 7 && g.origin.y == 8.0)), "the whole remote sees both");
    assert_eq!(*changes.lock().unwrap(), [3, 7], "on_change fires only for its own field");

    let e = a.variable_definition::<u8>("f/other.id", VariableOptions::new()).err().unwrap();
    assert_eq!(e.kind(), ErrorKind::BadName, "a definition takes no path: {e}");
    let q = b.create_queue().unwrap();
    let e = b.remote_variable::<u8>("f/var.id", VariableOptions::new().queue(&q)).err().unwrap();
    assert_eq!(e.kind(), ErrorKind::State, "a later remote on another queue is refused: {e}");
}
