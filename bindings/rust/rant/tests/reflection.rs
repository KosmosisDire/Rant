// Reflection, meta snapshots, the built in log and status, and static memory mode.

mod common;

use std::sync::{Arc, Mutex};
use std::time::Duration;

use common::{block_on, domain, node, options, wait_for};
use rant::{
    CallOptions, EntityKind, ErrorKind, EventKind, FunctionOptions, Level, MetaSections, Node, NodeOptions, Origin, Qos, Request,
    SendError, StatusOptions, TaskOptions, TaskRequest, Threading, VariableOptions,
};

#[derive(rant::Message, Clone, Debug, PartialEq)]
struct Ping {
    seq: u32,
}

fn observer(name: &str, offset: u16) -> Node {
    Node::new(name, options(offset, Threading::ServiceThread).fetch_details(true)).unwrap()
}

#[test]
fn the_mesh_folds_every_kind() {
    let a = node("refl_a", 40);
    let b = observer("refl_b", 40);
    let _pose = a.publisher::<Ping>("Pose", Qos::new()).unwrap();
    let _add = a.function_definition("Add", FunctionOptions::new(), |r: Request<Ping, u32>| r.seq + 1).unwrap();
    let _move = a.task_definition("Move", TaskOptions::new().no_cancel(true), |r: TaskRequest<Ping, f32, u32>| r.seq).unwrap();
    let _speed = a.variable_definition::<f64>("Speed", VariableOptions::new().initial(1.0)).unwrap();
    let r = b.reflection();

    assert!(wait_for(4000, None, || r.peers().iter().any(|p| p.name == "refl_a" && p.active)));
    let peer = r.peers().into_iter().find(|p| p.name == "refl_a").unwrap();
    assert!(peer.address.contains(':'), "{}", peer.address);

    let complete = || {
        let mesh = r.mesh();
        ["Pose", "Add", "Move", "Speed"].iter().all(|n| mesh.iter().any(|e| e.name == *n && !e.incomplete && e.schema.is_some()))
    };
    assert!(wait_for(5000, None, complete), "the observer learns every entity with its schema: {:?}", r.mesh());
    let mesh = r.mesh();
    let kind = |n: &str| mesh.iter().find(|e| e.name == n).map(|e| e.kind);
    assert_eq!(kind("Pose"), Some(EntityKind::Topic));
    assert_eq!(kind("Add"), Some(EntityKind::Function));
    assert_eq!(kind("Move"), Some(EntityKind::Task));
    assert_eq!(kind("Speed"), Some(EntityKind::Variable));
    assert!(mesh.iter().any(|e| e.name == "@rant/meta"), "the builtins are listed like any entity");

    let add = r.find(EntityKind::Function, "Add").unwrap();
    assert_eq!(add.schema.as_ref().map(|s| s.name()), Some("Ping".to_owned()));
    assert_eq!(add.response_schema.as_ref().map(|s| s.to_dsl().trim().to_owned()), Some("u32".to_owned()));
    assert_eq!(add.providers, 1);
    let mv = r.find(EntityKind::Task, "Move").unwrap();
    assert!(!mv.cancellable, "no_cancel shows on the folded task");
    assert!(mv.progress_schema.is_some());
    let speed = r.find(EntityKind::Variable, "Speed").unwrap();
    assert!(speed.writable && !speed.forceable);

    let offered = r.entities(peer.id);
    assert!(offered.iter().any(|e| e.name == "Add"), "one peer's own entities");
    assert!(r.entities(0).iter().all(|e| e.name.starts_with("@rant/")), "the observer offers only its builtins");

    let before = r.epoch();
    let _late = a.publisher::<Ping>("Late", Qos::new()).unwrap();
    assert!(wait_for(4000, None, || r.epoch() != before), "the epoch moves on a change");
}

#[test]
fn meta_snapshots_a_peer() {
    let a = node("meta_a", 41);
    let b = node("meta_b", 41);
    let r = b.reflection();
    assert!(wait_for(4000, None, || r.peers().iter().any(|p| p.name == "meta_a" && p.active)));
    let peer = r.peers().into_iter().find(|p| p.name == "meta_a").unwrap();

    let snap = r.meta(peer.id, MetaSections::ALL, Duration::from_secs(3)).unwrap();
    assert_eq!(snap.provider, peer.id);
    assert_eq!(snap.node.name, "meta_a");
    assert!(snap.node.uptime_us > 0 && snap.node.mem_in_use > 0, "{:?}", snap.node);
    assert!(snap.info.contains_key("node") && snap.info.contains_key("topics"));

    let only_node = block_on(r.meta_async(peer.id, MetaSections::NODE).unwrap()).unwrap();
    assert_eq!(only_node.node.name, "meta_a");
    assert!(!only_node.info.contains_key("peers"), "a section not asked for is left out");

    let quiet = Node::new("meta_off", options(41, Threading::ServiceThread).disable_meta(true)).unwrap();
    let e = quiet.reflection().meta(peer.id, MetaSections::ALL, Duration::from_millis(100)).err().unwrap();
    assert_eq!(e.kind(), ErrorKind::State, "{e}");
    drop(a);
}

#[test]
fn logs_cross_nodes_but_not_back() {
    let a = node("log_a", 42);
    let b = node("log_b", 42);
    let lines = Arc::new(Mutex::new(Vec::new()));
    let l = Arc::clone(&lines);
    a.on_log(move |line| {
        if line.origin == Origin::App && line.wall_us != 0 {
            l.lock().unwrap().push((line.level, line.node.to_owned(), line.text.into_owned()));
        }
    })
    .unwrap();
    assert!(wait_for(4000, None, || a.reflection().peers().iter().any(|p| p.name == "log_b" && p.active)));
    std::thread::sleep(Duration::from_millis(300));
    b.log(Level::Debug, "below the level").unwrap();
    b.log(Level::Warn, "pressure high").unwrap();
    a.log(Level::Info, "my own").unwrap();
    let want = (Level::Warn, "log_b".to_owned(), "pressure high".to_owned());
    assert!(wait_for(4000, None, || lines.lock().unwrap().contains(&want)), "{:?}", lines.lock().unwrap());
    assert!(!lines.lock().unwrap().iter().any(|l| l.2 == "my own"), "a node never receives its own lines");
    assert!(!lines.lock().unwrap().iter().any(|l| l.2 == "below the level"), "a line below the level is never sent");

    let mute = Node::new("log_off", options(42, Threading::ServiceThread).disable_logs(true)).unwrap();
    assert_eq!(mute.log(Level::Info, "x"), Err(SendError::NoSys));
    assert!(mute.on_log(|_| {}).is_err());
    let loud = Node::new("log_debug", options(42, Threading::ServiceThread).log_level(Level::Debug)).unwrap();
    assert_eq!(loud.log(Level::Debug, "x"), Ok(()));
}

#[test]
fn status_reaches_late_binders_and_clears() {
    type Seen = Vec<(String, Level, bool, u64, String)>;
    let a = node("status_a", 44);
    let b = node("status_b", 44);
    let cleared = Arc::new(Mutex::new(Vec::new()));
    let c = Arc::clone(&cleared);
    a.on_event(move |e| {
        if e.kind == EventKind::StatusCleared {
            c.lock().unwrap().push(e.detail.clone().unwrap_or_default());
        }
    });
    a.set_status("estop", Level::Error, "pressed", StatusOptions::new()).unwrap();
    a.set_status("hot", Level::Warn, "motor hot", StatusOptions::new().clearable(true)).unwrap();
    assert_eq!(a.set_status(&"k".repeat(49), Level::Info, "x", StatusOptions::new()), Err(SendError::TooBig));

    let seen: Arc<Mutex<(u32, usize, Seen)>> = Arc::new(Mutex::new((0, 0, Vec::new())));
    let s = Arc::clone(&seen);
    b.on_status(move |u| {
        if u.node != "status_a" {
            return;
        }
        let mut g = s.lock().unwrap();
        g.0 = u.node_id;
        g.1 += 1;
        g.2 = u.conditions.iter().map(|c| (c.key.to_string(), c.level, c.clearable, c.since_us, c.text.to_string())).collect();
    })
    .unwrap();
    let count = || seen.lock().unwrap().2.len();
    assert!(wait_for(4000, None, || count() == 2), "{:?}", seen.lock().unwrap());
    let (from, hot_since) = {
        let g = seen.lock().unwrap();
        assert_eq!(g.2[0].0, "estop");
        assert_eq!((g.2[0].1, g.2[0].2), (Level::Error, false));
        assert_eq!((g.2[1].0.as_str(), g.2[1].1, g.2[1].2, g.2[1].4.as_str()), ("hot", Level::Warn, true, "motor hot"));
        assert!(g.2[1].3 > 0);
        (g.0, g.2[1].3)
    };
    b.clear_peer_status(from, "hot", hot_since).unwrap();
    assert!(wait_for(4000, None, || *cleared.lock().unwrap() == ["hot"]), "{:?}", cleared.lock().unwrap());
    assert!(wait_for(4000, None, || count() == 1));
    a.clear_status("estop").unwrap();
    assert!(wait_for(4000, None, || count() == 0));
    a.set_status("estop", Level::Error, "pressed again", StatusOptions::new()).unwrap();
    assert!(wait_for(4000, None, || count() == 1));
    let before = seen.lock().unwrap().1;
    drop(a);
    assert!(wait_for(8000, None, || {
        let g = seen.lock().unwrap();
        g.1 > before && g.2.is_empty()
    }), "a node that went away reads as empty");

    let off = Node::new("status_off", options(44, Threading::ServiceThread).disable_status(true)).unwrap();
    assert_eq!(off.set_status("k", Level::Info, "x", StatusOptions::new()), Err(SendError::NoSys));
    assert!(off.on_status(|_| {}).is_err());
}

#[test]
fn static_memory_runs_in_one_block() {
    let opts = |name_offset: u16| NodeOptions::new().domain(domain(name_offset)).multicast_interface("127.0.0.1");
    let tiny = Node::new("tiny", opts(43).memory(256)).err().unwrap();
    assert_eq!(tiny.kind(), ErrorKind::Oom, "{tiny}");

    let a = Node::new("static_a", opts(43).memory(4 << 20).disable_shm(true)).unwrap();
    let b = node("static_b", 43);
    let got = Arc::new(Mutex::new(Vec::new()));
    let g = Arc::clone(&got);
    let _sub = a.subscriber::<Ping>("tick", Qos::new().reliable(true), move |p| g.lock().unwrap().push(p.seq)).unwrap();
    let p = b.publisher::<Ping>("tick", Qos::new().reliable(true)).unwrap();
    assert!(wait_for(4000, None, || p.match_count() > 0 && p.ready()));
    for seq in 0..5 {
        p.send(&Ping { seq }).unwrap();
    }
    assert!(wait_for(3000, None, || got.lock().unwrap().len() == 5));
    let stats = a.stats();
    assert!(stats.memory_in_use > 0 && stats.memory_peak <= 4 << 20, "{stats:?}");

    // a function call through a static node too
    let _add = a.function_definition("Inc", FunctionOptions::new(), |r: Request<Ping, u32>| r.seq + 1).unwrap();
    let inc = b.remote_function::<Ping, u32>("Inc", FunctionOptions::new()).unwrap();
    // both sides, so the answer finds the caller matched too
    assert!(wait_for(4000, None, || inc.match_count() > 0 && _add.match_count() > 0));
    assert_eq!(inc.call(&Ping { seq: 9 }, CallOptions::new()).unwrap().into_result().unwrap(), 10);
}
