// Variables: the owner's value replicated to remotes, writes both ways, the change and
// write events, force, read only owners and raw bytes.

mod common;

use std::sync::{Arc, Mutex};
use std::time::Duration;

use common::{node, wait_for};
use rant::{Bytes, MessageBuilder, SendError, VariableOptions};

#[derive(rant::Message, Clone, Debug, PartialEq)]
struct Speed {
    value: f64,
    unit: rant::String<8>,
}

fn speed(value: f64) -> Speed {
    Speed { value, unit: "m/s".parse().unwrap() }
}

#[test]
fn the_owner_value_reaches_remotes_and_writes_come_back() {
    let a = node("var_owner", 30);
    let b = node("var_remote", 30);
    let owner = a.variable_definition::<Speed>("Speed", VariableOptions::new().initial(speed(1.5))).unwrap();
    assert_eq!(owner.get().unwrap(), Some(speed(1.5)), "the owner holds its initial value");
    let remote = b.remote_variable::<Speed>("Speed", VariableOptions::new()).unwrap();
    assert!(remote.wait(Duration::from_secs(4)), "a late remote gets the latest value");
    assert_eq!(remote.get().unwrap(), Some(speed(1.5)));
    assert!(wait_for(4000, None, || remote.match_count() > 0 && owner.match_count() > 0));

    let seen = Arc::new(Mutex::new(Vec::new()));
    let s = Arc::clone(&seen);
    owner
        .on_change(move |u| s.lock().unwrap().push((u.value, u.source() != 0, u.forced())))
        .unwrap();
    assert_eq!(seen.lock().unwrap().clone(), vec![(1.5, false, false)], "on_change replays the current value");

    remote.set(&speed(2.5)).unwrap();
    assert!(wait_for(3000, None, || owner.get().unwrap() == Some(speed(2.5))), "a remote write reaches the owner");
    assert!(wait_for(3000, None, || remote.get().unwrap() == Some(speed(2.5))), "and comes back as the published value");
    assert_eq!(seen.lock().unwrap().last().cloned(), Some((2.5, true, false)), "the change names a remote source");

    owner.set(&speed(3.0)).unwrap();
    assert!(wait_for(3000, None, || remote.get().unwrap() == Some(speed(3.0))));
}

#[test]
fn writes_and_changes_are_told_apart() {
    let a = node("var_events", 31);
    let owner = a.variable_definition::<u32>("Count", VariableOptions::new().initial(1)).unwrap();
    let (changes, writes) = (Arc::new(Mutex::new(Vec::new())), Arc::new(Mutex::new(Vec::new())));
    let (c, w) = (Arc::clone(&changes), Arc::clone(&writes));
    owner.on_change(move |u| c.lock().unwrap().push(*u)).unwrap();
    owner.on_write(move |u| w.lock().unwrap().push((*u, u.write_seq()))).unwrap();
    owner.set(&2).unwrap();
    owner.set(&2).unwrap();
    owner.set(&3).unwrap();
    assert_eq!(*changes.lock().unwrap(), vec![1, 2, 3], "a same value write is no change");
    let writes = writes.lock().unwrap().clone();
    assert_eq!(writes.iter().map(|w| w.0).collect::<Vec<_>>(), vec![2, 2, 3], "every write is told, no replay");
    assert!(writes.windows(2).all(|p| p[1].1 > p[0].1), "the write counter climbs");

    // a later handler replaces the earlier one
    let again = Arc::new(Mutex::new(0));
    let g = Arc::clone(&again);
    owner.on_change(move |_| *g.lock().unwrap() += 1).unwrap();
    owner.set(&4).unwrap();
    assert_eq!(*again.lock().unwrap(), 2, "the replay and the change");
    assert_eq!(changes.lock().unwrap().len(), 3, "the replaced handler is silent");
}

#[test]
fn force_holds_the_value_until_unforce() {
    let a = node("var_force", 32);
    let b = node("var_force_remote", 32);
    let plain = a.variable_definition::<f32>("Plain", VariableOptions::new().initial(1.0)).unwrap();
    assert_eq!(plain.force(&9.0), Err(SendError::State), "force needs allow_force on the definition");

    let owner = a.variable_definition::<f32>("Valve", VariableOptions::new().initial(1.0).allow_force(true)).unwrap();
    let remote = b.remote_variable::<f32>("Valve", VariableOptions::new()).unwrap();
    assert!(remote.wait(Duration::from_secs(4)));
    assert!(wait_for(4000, None, || remote.match_count() > 0));
    owner.force(&9.0).unwrap();
    assert!(owner.forced());
    owner.set(&2.0).unwrap();
    assert_eq!(owner.get().unwrap(), Some(9.0), "a write while forced is held back");
    assert!(wait_for(3000, None, || remote.get().unwrap() == Some(9.0) && remote.forced()));
    owner.unforce().unwrap();
    assert_eq!(owner.get().unwrap(), Some(2.0), "unforce restores the latest write");
    assert!(wait_for(3000, None, || remote.get().unwrap() == Some(2.0) && !remote.forced()));
}

#[test]
fn a_read_only_owner_refuses_remote_writes() {
    let a = node("var_ro", 33);
    let b = node("var_ro_remote", 33);
    let _owner = a.variable_definition::<i64>("Odometer", VariableOptions::new().initial(7).read_only(true)).unwrap();
    let remote = b.remote_variable::<i64>("Odometer", VariableOptions::new()).unwrap();
    assert!(remote.wait(Duration::from_secs(4)));
    assert_eq!(remote.set(&1), Err(SendError::BadRole));
    let nobody = b.remote_variable::<i64>("Nobody", VariableOptions::new()).unwrap();
    assert_eq!(nobody.set(&1), Err(SendError::NoTopic), "no owner matched");
    assert!(!nobody.wait(Duration::from_millis(100)));
    assert_eq!(nobody.get().unwrap(), None);
}

#[test]
fn raw_variables_carry_bytes() {
    let a = node("var_raw", 34);
    let b = node("var_raw_remote", 34);
    let sa = a.schema("Gauge { psi: f64, ok: bool }").unwrap();
    let mut m = MessageBuilder::new(&sa);
    m.set_f64("psi", 31.5).unwrap().set_bool("ok", true).unwrap();
    let owner = a.variable_definition::<Bytes>("Gauge", VariableOptions::new().schema(&sa).initial(m.bytes().to_vec())).unwrap();
    let remote = b.remote_variable::<Bytes>("Gauge", VariableOptions::new().reflect_from_mesh(true)).unwrap();
    // made before the owner was seen, it adopts the owner's schema at a refresh once the
    // owner's details arrived
    let adopted = wait_for(4000, None, || {
        remote.refresh().unwrap();
        remote.schema().is_some()
    });
    assert!(adopted, "a reflecting remote adopts the owner's schema");
    assert!(remote.wait(Duration::from_secs(4)));
    let value = remote.get().unwrap().unwrap();
    let schema = remote.schema().unwrap();
    assert_eq!(schema.view(&value).get_f64("psi"), 31.5);
    assert!(schema.view(&value).get_bool("ok"));
    assert_eq!(owner.schema().map(|s| s.hash()), Some(schema.hash()));

    let e = a.variable_definition::<u8>("Typed", VariableOptions::new().schema(&sa)).err().unwrap();
    assert!(e.to_string().contains("typed handle"), "a typed handle takes no schema: {e}");
}

#[test]
fn a_variable_outlives_its_node_safely() {
    let a = node("var_closed", 35);
    let owner = a.variable_definition::<u16>("Level", VariableOptions::new().initial(3)).unwrap();
    a.close().unwrap();
    assert_eq!(owner.get().unwrap(), None);
    assert_eq!(owner.set(&4), Err(SendError::NoTopic));
    assert!(!owner.wait(Duration::from_millis(10)));
    assert_eq!(owner.on_change(|_| {}).err(), Some(SendError::NoTopic));
}
