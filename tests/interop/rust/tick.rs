// The Rust side of an interop pair, as tests/interop/tick.cpp: `pub <seconds>` publishes Tick,
// `sub <seconds> <publisher>` exits 0 once twenty ticks from that node decoded right.

use std::process::ExitCode;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use rant::types::{Float2, Timestamp, Transform};
use rant::{Node, NodeOptions, Qos, Threading};

const TICK_WANT: u32 = 20;

#[derive(rant::Message, Clone, Default)]
struct Hop {
    name: String,
    pts: Vec<Float2>,
}

#[derive(rant::Message, Clone, Default)]
struct Tick {
    seq: u64,
    when: Timestamp,
    value: f64,
    at: Transform,
    hops: Vec<Hop>,
}

fn near(a: f64, b: f64) -> bool {
    (a - b).abs() < 1e-9
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let sub = args.first().is_some_and(|a| a == "sub");
    let seconds: f64 = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(10.0);
    let from = args.get(2).cloned().unwrap_or_default();
    let options = NodeOptions::new().multicast_interface("127.0.0.1").threading(Threading::Manual);
    let node = match Node::new(if sub { "rust-tick-sub" } else { "rust-tick-pub" }, options) {
        Ok(n) => n,
        Err(e) => {
            println!("FAIL: {e}");
            return ExitCode::FAILURE;
        }
    };
    let qos = Qos::new().reliable(true).keep_last(64);
    let deadline = Instant::now() + Duration::from_secs_f64(seconds);
    if sub {
        let (good, bad) = (Arc::new(AtomicU32::new(0)), Arc::new(AtomicU32::new(0)));
        let (g, b) = (Arc::clone(&good), Arc::clone(&bad));
        let _ticks = node
            .subscriber::<Tick>("tick", qos, move |t| {
                let angle = t.seq as f64 * 0.01;
                let p = &t.at.pose.position;
                let hops = t.hops.len() == 1 && t.hops[0].name == format!("h{}", t.seq) && t.hops[0].pts.len() == 1
                    && t.hops[0].pts[0].x == (t.seq % 1000) as f32 && t.hops[0].pts[0].y == 0.5;
                let ok = t.publisher_name() == from && t.when.us > 0 && near(t.value, angle.sin()) && near(p.x, angle.cos()) && near(p.y, angle.sin()) && hops;
                if ok {
                    g.fetch_add(1, Ordering::SeqCst);
                } else if b.fetch_add(1, Ordering::SeqCst) == 0 {
                    println!("FAIL: tick {} decoded wrong", t.seq);
                }
            })
            .unwrap();
        while Instant::now() < deadline && bad.load(Ordering::SeqCst) == 0 && good.load(Ordering::SeqCst) < TICK_WANT {
            node.poll(Duration::from_millis(2)).unwrap();
        }
        let (good, bad) = (good.load(Ordering::SeqCst), bad.load(Ordering::SeqCst));
        let ok = good >= TICK_WANT && bad == 0;
        println!("{}: {good} ticks decoded right, {bad} wrong", if ok { "PASS" } else { "FAIL" });
        return if ok { ExitCode::SUCCESS } else { ExitCode::FAILURE };
    }
    let ticks = node.publisher::<Tick>("tick", qos).unwrap();
    let mut seq = 0u64;
    while Instant::now() < deadline {
        if ticks.match_count() > 0 {
            let angle = seq as f64 * 0.01;
            let hops = vec![Hop { name: format!("h{seq}"), pts: vec![Float2 { x: (seq % 1000) as f32, y: 0.5 }] }];
            let mut t = Tick { seq, when: Timestamp::now(), value: angle.sin(), at: Transform::identity(), hops };
            t.at.pose.position.x = angle.cos();
            t.at.pose.position.y = angle.sin();
            let _ = ticks.send(&t);
            seq += 1;
        }
        node.poll(Duration::from_millis(2)).unwrap();
    }
    ExitCode::SUCCESS
}
