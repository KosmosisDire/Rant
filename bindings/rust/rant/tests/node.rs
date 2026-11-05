// The node: open, close, threading, events, the close gate and callback safety. Two node
// tests discover on loopback, each on a domain of its own.

mod common;

use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use common::{node, options, wait_for};
use rant::{Bytes, ErrorKind, EventKind, Node, NodeOptions, Qos, SendError, Threading};

#[test]
fn version_matches_header_and_package() {
    let file = std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/../../../VERSION")).unwrap();
    assert_eq!(rant::version(), file.trim(), "the library's version is VERSION");
    assert_eq!(env!("CARGO_PKG_VERSION"), file.trim(), "the crate's version is VERSION");
}

#[test]
fn opens_and_closes_in_every_threading() {
    for (i, t) in [Threading::ServiceThread, Threading::Manual, Threading::Dispatch].into_iter().enumerate() {
        let n = Node::new(&format!("open{i}"), options(0, t)).unwrap();
        assert_eq!(n.threading(), t);
        assert_eq!(n.poll(Duration::ZERO).is_ok(), t == Threading::Manual, "poll only runs Manual");
        assert_eq!(n.dispatch(0, Duration::ZERO).is_ok(), t == Threading::Dispatch, "dispatch only runs Dispatch");
        n.close().unwrap();
    }
}

#[test]
fn bad_options_refuse_the_open() {
    let e = Node::new("bad", options(0, Threading::Manual).seed_peers(["not an address"])).unwrap_err();
    assert_eq!(e.kind(), ErrorKind::BadAddress, "{e}");
    let e = Node::new("bad", options(0, Threading::Manual).prefix("a@b")).unwrap_err();
    assert_eq!(e.kind(), ErrorKind::BadPrefix, "{e}");
    let e = Node::new("bad\0name", NodeOptions::new()).unwrap_err();
    assert_eq!(e.kind(), ErrorKind::BadName, "{e}");
}

#[test]
fn a_handle_outliving_its_node_answers_no_topic() {
    let n = node("outlive", 1);
    let p = n.publisher::<Bytes>("o/raw", Qos::new()).unwrap();
    drop(n);
    assert_eq!(p.send(b"x"), Err(SendError::NoTopic));
    assert_eq!(p.match_count(), 0);
    assert!(p.schema().is_none());
    assert!(p.close().is_ok(), "closing after the node is a no op");
}

#[test]
fn close_waits_out_a_sender_on_another_thread() {
    let n = node("racer", 2);
    let p = n.publisher::<f64>("r/value", Qos::new()).unwrap();
    let sent = Arc::new(AtomicUsize::new(0));
    let closing = Arc::new(AtomicBool::new(false));
    let (s, c) = (Arc::clone(&sent), Arc::clone(&closing));
    let sender = std::thread::spawn(move || {
        loop {
            match p.send(&1.0) {
                Err(SendError::NoTopic) if c.load(Ordering::SeqCst) => return,
                _ => s.fetch_add(1, Ordering::Relaxed),
            };
        }
    });
    wait_for(2000, None, || sent.load(Ordering::Relaxed) > 100);
    closing.store(true, Ordering::SeqCst);
    n.close().unwrap();
    sender.join().unwrap();
    assert!(sent.load(Ordering::Relaxed) > 100, "the sender ran before the close");
}

#[test]
fn close_is_not_held_up_by_a_take_with_no_end() {
    let n = node("taker", 3);
    let mut s = n.pulled_subscriber::<f64>("t/value", Qos::new()).unwrap();
    let taker = std::thread::spawn(move || s.take(rant::FOREVER).map(|v| v.is_some()));
    std::thread::sleep(Duration::from_millis(100));
    let start = Instant::now();
    n.close().unwrap();
    assert!(start.elapsed() < Duration::from_secs(1), "close took {:?}", start.elapsed());
    assert!(taker.join().unwrap().is_err(), "the take ends with an error once the node closed");
}

#[test]
fn a_node_dropped_inside_its_own_callback_closes_after_it() {
    let a = node("dropper", 4);
    let b = node("feeder", 4);
    let slot: Arc<Mutex<Option<Node>>> = Arc::new(Mutex::new(None));
    let inner = Arc::clone(&slot);
    let fired = Arc::new(AtomicBool::new(false));
    let f = Arc::clone(&fired);
    let sub = a
        .subscriber::<u32>("d/value", Qos::new(), move |_| {
            let taken = inner.lock().unwrap().take();
            drop(taken);
            f.store(true, Ordering::SeqCst);
        })
        .unwrap();
    let p = b.publisher::<u32>("d/value", Qos::new()).unwrap();
    assert!(wait_for(4000, None, || p.match_count() > 0), "matched");
    *slot.lock().unwrap() = Some(a);
    assert!(wait_for(4000, None, || {
        let _ = p.send(&7);
        fired.load(Ordering::SeqCst)
    }));
    assert!(wait_for(2000, None, || sub.counts() == Default::default()), "the deferred close ran");
}

#[test]
fn a_handler_panic_becomes_an_error_event() {
    let a = node("panicky", 5);
    let b = node("feeder", 5);
    let seen = Arc::new(Mutex::new(Vec::new()));
    let s = Arc::clone(&seen);
    a.on_event(move |e| {
        if e.is_error() {
            s.lock().unwrap().push((e.error, e.detail.clone(), e.topic_name.clone()));
        }
    });
    let calls = Arc::new(AtomicUsize::new(0));
    let c = Arc::clone(&calls);
    let _sub = a.subscriber::<u32>("p/value", Qos::new(), move |v| {
        c.fetch_add(1, Ordering::SeqCst);
        if *v == 1 {
            panic!("boom on one");
        }
    });
    let p = b.publisher::<u32>("p/value", Qos::new().reliable(true)).unwrap();
    assert!(wait_for(4000, None, || p.match_count() > 0));
    p.send(&1).unwrap();
    p.send(&2).unwrap();
    assert!(wait_for(3000, None, || calls.load(Ordering::SeqCst) >= 2), "the handler runs on after a panic");
    let seen = seen.lock().unwrap();
    let hit = seen.iter().find(|(k, _, _)| *k == ErrorKind::None).expect("a panic event");
    assert_eq!(hit.1.as_deref(), Some("boom on one"));
    assert_eq!(hit.2.as_deref(), Some("p/value"));
}

#[test]
fn peers_raise_events() {
    let a = node("watcher", 6);
    let ups = Arc::new(Mutex::new(Vec::new()));
    let u = Arc::clone(&ups);
    a.on_event(move |e| {
        if e.kind == EventKind::PeerUp {
            u.lock().unwrap().push((e.peer_name.clone(), e.address));
        }
    });
    let _b = node("visitor", 6);
    assert!(wait_for(4000, None, || ups.lock().unwrap().iter().any(|(n, _)| n.as_deref() == Some("visitor"))));
    let ups = ups.lock().unwrap();
    let addr = ups.iter().find(|(n, _)| n.as_deref() == Some("visitor")).unwrap().1.expect("an address");
    assert!(addr.ip().is_loopback() || addr.port() != 0, "{addr}");
}

#[test]
fn dispatch_threading_runs_callbacks_inside_dispatch() {
    let a = Node::new("dispatcher", options(7, Threading::Dispatch)).unwrap();
    let b = node("feeder", 7);
    let me = std::thread::current().id();
    let ran_here = Arc::new(AtomicUsize::new(0));
    let r = Arc::clone(&ran_here);
    let _sub = a.subscriber::<u32>("q/value", Qos::new(), move |_| {
        assert_eq!(std::thread::current().id(), me);
        r.fetch_add(1, Ordering::SeqCst);
    });
    let p = b.publisher::<u32>("q/value", Qos::new().reliable(true)).unwrap();
    assert!(wait_for(4000, None, || p.match_count() > 0));
    p.send(&1).unwrap();
    std::thread::sleep(Duration::from_millis(300));
    assert_eq!(ran_here.load(Ordering::SeqCst), 0, "nothing runs before dispatch");
    assert!(wait_for(3000, None, || {
        a.dispatch(0, Duration::from_millis(10)).unwrap();
        ran_here.load(Ordering::SeqCst) == 1
    }));
}

#[test]
fn a_queue_runs_its_handles_on_the_draining_thread() {
    let a = node("queued", 8);
    let b = node("feeder", 8);
    let q = a.create_queue().unwrap();
    let hits = Arc::new(AtomicUsize::new(0));
    let h = Arc::clone(&hits);
    let _sub = a.subscriber::<u32>("q/value", Qos::new().queue(&q), move |_| {
        h.fetch_add(1, Ordering::SeqCst);
    });
    let p = b.publisher::<u32>("q/value", Qos::new().reliable(true)).unwrap();
    assert!(wait_for(4000, None, || p.match_count() > 0));
    p.send(&1).unwrap();
    p.send(&2).unwrap();
    assert!(wait_for(3000, None, || q.stats().waiting == 2), "parked: {:?}", q.stats());
    assert_eq!(hits.load(Ordering::SeqCst), 0);
    let drainer = std::thread::spawn(move || q.dispatch(0, Duration::from_secs(1)));
    assert_eq!(drainer.join().unwrap(), Ok(2));
    assert_eq!(hits.load(Ordering::SeqCst), 2);
    let other = node("other", 9);
    let foreign = other.create_queue().unwrap();
    let e = a.subscriber::<u32>("q/other", Qos::new().queue(&foreign), |_| {}).err().unwrap();
    assert_eq!(e.kind(), ErrorKind::State, "{e}");
}

#[test]
fn close_drops_handlers_that_hold_handles_of_their_node() {
    struct OnDrop(Arc<AtomicBool>);
    impl Drop for OnDrop {
        fn drop(&mut self) {
            self.0.store(true, Ordering::SeqCst);
        }
    }
    let n = node("cycle", 1);
    let dropped = Arc::new(AtomicBool::new(false));
    let token = OnDrop(Arc::clone(&dropped));
    let echo = n.publisher::<u32>("cy/echo", Qos::new()).unwrap();
    let _sub = n.subscriber::<u32>("cy/in", Qos::new(), move |v| {
        let _keep = &token;
        let _ = echo.send(&v);
    });
    n.close().unwrap();
    assert!(dropped.load(Ordering::SeqCst), "the handler and what it holds went with the close");
}
