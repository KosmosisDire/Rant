// Every C entry point the generated FFI declares is called by the binding, or listed here
// with the reason it is not. A new C function fails this until the binding takes it.

use std::collections::BTreeSet;

/// Never called on purpose, with the reason.
const SKIPPED: &[(&str, &str)] = &[
    ("rant_node_log", "variadic: the binding formats in Rust and calls rant_node_log_text"),
    ("rant_node_stop", "threading is chosen at open and close stops the loop, as C++"),
    ("rant_node_is_started", "threading is chosen at open, as C++"),
    ("rant_node_set_pump_probe", "a transport benchmark probe, not an app API"),
    ("rant_node_shm_stats", "the @rant/meta snapshot carries the counts, as C++"),
    ("rant_topic_pending_count", "Publisher::ready is the one convergence query, as C++ and Python"),
    ("rant_topic_repair_stats", "a transport diagnostic no binding exposes"),
    ("rant_topic_subscriber_progress", "a transport diagnostic no binding exposes"),
    ("rant_field_name", "Rust names are the wire spelling, and the parser normalizes any other"),
    ("rant_type_name", "as rant_field_name"),
    ("rant_get_value", "the codec reads by resolved offset and tail path, as C++"),
    ("rant_get_value_at", "as rant_get_value"),
    ("rant_set_value", "as rant_get_value"),
    ("rant_set_value_at", "as rant_get_value"),
    ("rant_array_count_at", "as rant_get_value"),
    ("rant_enum_name_of", "Schema::enum_variants lists both directions, MessageView::get_enum_name reads one"),
    ("rant_enum_value_of", "MessageBuilder::set_enum takes the name"),
    ("rant_map_valid", "MessageBuilder::set_map validates through rant_set_map"),
    ("rant_schema_validate", "the C validates every delivery and every typed send"),
    ("rant_schema_field_type_wire", "no binding exposes it"),
    ("rant_std_by_name", "the standard types are spelled by name, recognition is a tool concern"),
    ("rant_std_name", "as rant_std_by_name"),
    ("rant_std_text", "as rant_std_by_name"),
    ("rant_std_recognize", "as rant_std_by_name"),
    ("rant_std_recognize_field", "as rant_std_by_name"),
    ("rant_std_recognize_elem", "as rant_std_by_name"),
];

fn source(name: &str) -> String {
    std::fs::read_to_string(format!("{}/src/{name}", env!("CARGO_MANIFEST_DIR"))).unwrap()
}

#[test]
fn every_c_function_is_bound_or_listed() {
    let ffi = source("ffi.rs");
    let declared: BTreeSet<&str> = ffi
        .split("pub fn ")
        .skip(1)
        .filter_map(|rest| rest.split('(').next())
        .filter(|name| name.starts_with("rant_"))
        .collect();
    let dir = std::fs::read_dir(format!("{}/src", env!("CARGO_MANIFEST_DIR"))).unwrap();
    let code: String = dir
        .map(|e| e.unwrap().path())
        .filter(|p| p.file_name().unwrap() != "ffi.rs" && p.extension().is_some_and(|x| x == "rs"))
        .map(|p| std::fs::read_to_string(p).unwrap())
        .collect();
    // called, or handed on by name to a helper that calls it
    let used = |f: &str| {
        let at = format!("ffi::{f}");
        code.match_indices(&at).any(|(i, _)| !code[i + at.len()..].starts_with(|c: char| c.is_alphanumeric() || c == '_'))
    };
    let called: BTreeSet<&str> = declared.iter().copied().filter(|f| used(f)).collect();
    let listed: BTreeSet<&str> = SKIPPED.iter().map(|(f, _)| *f).collect();

    let unbound: Vec<_> = declared.iter().filter(|f| !called.contains(*f) && !listed.contains(*f)).collect();
    assert!(unbound.is_empty(), "C functions neither called nor listed in tests/coverage.rs: {unbound:?}");
    let stale: Vec<_> = listed.iter().filter(|f| called.contains(*f) || !declared.contains(*f)).collect();
    assert!(stale.is_empty(), "listed in tests/coverage.rs but called or gone from the C: {stale:?}");
}
