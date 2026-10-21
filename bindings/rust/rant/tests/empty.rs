// Messages of zero bytes, such as rant::types::Empty: they encode, send and answer like any
// other, through functions, variables and topics.

mod common;

use std::time::Duration;

use common::{node, options, wait_for};
use rant::types::Empty;
use rant::{Bytes, CallOptions, CallStatus, EntityKind, FunctionOptions, Node, Qos, Request, Threading, VariableOptions};

#[test]
fn an_empty_request_and_reply_are_answered() {
    let a = node("empty_def", 50);
    let b = node("empty_call", 50);
    let _reset = a
        .function_definition("Reset", FunctionOptions::new(), |_: Request<Empty, Empty>| Ok::<_, &str>(Empty {}))
        .unwrap();
    let typed = b.remote_function::<Empty, Empty>("Reset", FunctionOptions::new()).unwrap();
    assert!(wait_for(4000, None, || typed.match_count() > 0));

    let r = typed.call(&Empty {}, CallOptions::new()).unwrap();
    assert_eq!(r.status(), CallStatus::Ok, "{}", r.message());

    let c = Node::new("empty_raw", options(50, Threading::ServiceThread).fetch_details(true)).unwrap();
    let reflection = c.reflection();
    let found = |r: &rant::Reflection| r.find(EntityKind::Function, "Reset").filter(|e| e.schema.is_some() && e.response_schema.is_some());
    assert!(wait_for(4000, None, || found(&reflection).is_some()), "the schemas are reflected");
    let found = found(&reflection).unwrap();
    let (req, rsp) = (found.schema.unwrap(), found.response_schema.unwrap());
    let raw = c.remote_function::<Bytes, Bytes>("Reset", FunctionOptions::new().request_schema(&req).response_schema(&rsp)).unwrap();
    assert!(wait_for(4000, None, || raw.match_count() > 0));
    let r = raw.call(&[][..], CallOptions::new()).unwrap();
    assert_eq!(r.status(), CallStatus::Ok, "{}", r.message());
}

#[test]
fn an_empty_variable_and_topic_carry() {
    let a = node("empty_var_owner", 51);
    let b = node("empty_var_remote", 51);
    let owner = a.variable_definition::<Empty>("Flag", VariableOptions::new()).unwrap();
    owner.set(&Empty {}).unwrap();
    assert_eq!(owner.get().unwrap(), Some(Empty {}));
    let remote = b.remote_variable::<Empty>("Flag", VariableOptions::new()).unwrap();
    assert!(remote.wait(Duration::from_secs(4)));
    assert_eq!(remote.get().unwrap(), Some(Empty {}));

    let got = std::sync::Arc::new(std::sync::atomic::AtomicU32::new(0));
    let g = std::sync::Arc::clone(&got);
    let _sub = b
        .subscriber::<Empty>("Ping", Qos::new().reliable(true), move |_| {
            g.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        })
        .unwrap();
    let p = a.publisher::<Empty>("Ping", Qos::new().reliable(true)).unwrap();
    assert!(wait_for(4000, None, || p.match_count() > 0 && p.ready()));
    p.send(&Empty {}).unwrap();
    assert!(wait_for(3000, None, || got.load(std::sync::atomic::Ordering::SeqCst) == 1), "an empty message arrives");
}
