// Shared by the test programs: isolated loopback domains, waits bounded by the clock, and
// the golden schema hashes every binding pins.
#![allow(dead_code)]

use std::sync::OnceLock;
use std::time::{Duration, Instant};

use rant::{Node, NodeOptions, Threading};

/// ctest hands each program a block of 64 domains through RANT_TEST_DOMAIN, a run by hand
/// takes one from the clock, so parallel runs never meet. Each test owns one offset.
pub fn domain(offset: u16) -> u16 {
    static BASE: OnceLock<u16> = OnceLock::new();
    let base = *BASE.get_or_init(|| {
        let env = std::env::var("RANT_TEST_DOMAIN").ok().and_then(|v| v.parse::<u16>().ok());
        env.filter(|v| *v > 0 && *v <= 65535 - 64).unwrap_or_else(|| {
            let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_micros();
            1000 + (now % 900) as u16 * 64
        })
    });
    base + offset
}

pub fn options(offset: u16, threading: Threading) -> NodeOptions {
    NodeOptions::new().domain(domain(offset)).multicast_interface("127.0.0.1").threading(threading)
}

pub fn node(name: &str, offset: u16) -> Node {
    Node::new(name, options(offset, Threading::ServiceThread)).expect("node opens")
}

/// Polls `pred` until true or the time is up, pumping `pump` when it runs Manual.
pub fn wait_for(ms: u64, pump: Option<&Node>, mut pred: impl FnMut() -> bool) -> bool {
    let deadline = Instant::now() + Duration::from_millis(ms);
    while Instant::now() < deadline {
        if pred() {
            return true;
        }
        match pump {
            Some(n) => n.poll(Duration::from_millis(2)).unwrap(),
            None => std::thread::sleep(Duration::from_millis(2)),
        }
    }
    pred()
}

/// tests/golden/schemas.txt: the hash every binding pins for a schema text.
pub fn golden(text: &str) -> u64 {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../../tests/golden/schemas.txt");
    let file = std::fs::read_to_string(path).expect("tests/golden/schemas.txt");
    file.lines()
        .filter(|l| !l.starts_with('#'))
        .find_map(|l| {
            let mut parts = l.split_whitespace();
            (parts.next() == Some(text)).then(|| u64::from_str_radix(parts.next().unwrap(), 16).unwrap())
        })
        .unwrap_or_else(|| panic!("no golden line for {text}"))
}
