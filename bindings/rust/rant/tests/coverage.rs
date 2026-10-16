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

/// Bound by a later step of the binding: functions and tasks, variables, then reflection and logs.
const PENDING: &[&str] = &[
    "rant_node_create_function_definition", "rant_node_create_remote_function", "rant_node_create_task_definition",
    "rant_node_create_remote_task", "rant_function_call", "rant_function_call_async", "rant_function_cancel",
    "rant_function_cancelled", "rant_function_complete", "rant_function_match_count", "rant_function_on_cancel",
    "rant_function_progress", "rant_function_progress_schema", "rant_function_refresh", "rant_function_request_schema",
    "rant_function_response_schema", "rant_function_retire", "rant_request_defer", "rant_request_fail",
    "rant_request_reply", "rant_request_start", "rant_node_meta_function",
    "rant_node_create_variable_definition", "rant_node_create_remote_variable", "rant_variable_force",
    "rant_variable_forced", "rant_variable_get", "rant_variable_match_count", "rant_variable_on_change",
    "rant_variable_on_write", "rant_variable_refresh", "rant_variable_retire", "rant_variable_schema",
    "rant_variable_set", "rant_variable_unforce", "rant_variable_wait",
    "rant_node_peers_next", "rant_node_entities_next", "rant_node_mesh_next", "rant_node_mesh_find",
    "rant_node_mesh_epoch", "rant_node_lock", "rant_node_unlock", "rant_node_log_text", "rant_node_log_topic",
    "rant_node_set_log_queue",
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
    let called: BTreeSet<&str> = declared.iter().copied().filter(|f| code.contains(&format!("ffi::{f}("))).collect();
    let listed: BTreeSet<&str> = SKIPPED.iter().map(|(f, _)| *f).chain(PENDING.iter().copied()).collect();

    let unbound: Vec<_> = declared.iter().filter(|f| !called.contains(*f) && !listed.contains(*f)).collect();
    assert!(unbound.is_empty(), "C functions neither called nor listed in tests/coverage.rs: {unbound:?}");
    let stale: Vec<_> = listed.iter().filter(|f| called.contains(*f) || !declared.contains(*f)).collect();
    assert!(stale.is_empty(), "listed in tests/coverage.rs but called or gone from the C: {stale:?}");
}
