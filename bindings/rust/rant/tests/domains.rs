// The census watch counts a node on its domain and drops it when it closes.

mod common;

use common::{node, wait_for};
use rant::{Domains, DomainsOptions};

#[test]
fn the_census_counts_a_node_on_its_domain() {
    let mut watch = Domains::new(DomainsOptions::new().multicast_interface("127.0.0.1")).unwrap();
    let domain = common::domain(50);
    let mut count = || watch.list().iter().find(|d| d.domain == domain).map_or(0, |d| d.nodes);
    let n = node("domains_node", 50);
    assert!(wait_for(3000, None, || count() == 1));
    drop(n);
    assert!(wait_for(3000, None, || count() == 0));
}
