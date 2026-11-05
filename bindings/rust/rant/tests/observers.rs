// Call observers: a third node sees a function's call and answer, and a task's whole life.

mod common;

use std::sync::{Arc, Mutex};

use common::{node, options, wait_for};
use rant::{CallEventKind, CallObserverOptions, CallOptions, CallStatus, FunctionOptions, Node, Request, TaskOptions, TaskRequest, Threading};

#[derive(rant::Message, Clone, Debug, PartialEq)]
struct ObsReq {
    x: i32,
}

#[derive(rant::Message, Clone, Debug, PartialEq)]
struct ObsRsp {
    y: i32,
}

#[derive(Clone)]
struct Seen {
    kind: CallEventKind,
    caller: String,
    caller_lo: u32,
    call_id: u32,
    status: CallStatus,
    message: String,
    field: i64,
}

fn watcher(log: &Arc<Mutex<Vec<Seen>>>) -> impl for<'a> FnMut(rant::CallEvent<'a>) + Send + 'static {
    let log = Arc::clone(log);
    move |e| {
        let field = if !e.has_schema() {
            0
        } else if e.kind() == CallEventKind::Call {
            e.fields().get_int("x")
        } else {
            e.fields().get_int("y")
        };
        log.lock().unwrap().push(Seen {
            kind: e.kind(),
            caller: e.caller_name().to_owned(),
            caller_lo: e.caller_lo(),
            call_id: e.call_id(),
            status: e.status(),
            message: e.message().to_owned(),
            field,
        });
    }
}

#[test]
fn a_third_node_sees_a_call_and_its_answer() {
    let a = node("obs_prov", 40);
    let b = node("obs_call", 40);
    let w = Node::new("obs_watch", options(40, Threading::ServiceThread).fetch_details(true)).unwrap();
    let log = Arc::new(Mutex::new(Vec::new()));
    let watch = w.call_observer("obs/fn", CallObserverOptions::new().reflect_from_mesh(true), watcher(&log)).unwrap();
    assert_eq!(watch.name(), "obs/fn");
    let _def = a
        .function_definition("obs/fn", FunctionOptions::new(), |r: Request<ObsReq, ObsRsp>| ObsRsp { y: r.x + 1 })
        .unwrap();
    let remote = b.remote_function::<ObsReq, ObsRsp>("obs/fn", FunctionOptions::new()).unwrap();
    assert!(wait_for(6000, None, || watch.match_count() == 1 && remote.match_count() == 1));
    assert!(wait_for(3000, None, || w.reflection().find(rant::EntityKind::Function, "obs/fn").is_some_and(|e| !e.incomplete)));
    let r = remote.call(&ObsReq { x: 41 }, CallOptions::new()).unwrap();
    assert_eq!(r.status(), CallStatus::Ok);
    assert!(wait_for(3000, None, || log.lock().unwrap().len() >= 2));
    let seen = log.lock().unwrap().clone();
    assert_eq!(seen.len(), 2);
    assert_eq!((seen[0].kind, seen[1].kind), (CallEventKind::Call, CallEventKind::End));
    assert_eq!((seen[0].caller.as_str(), seen[1].caller.as_str()), ("obs_call", "obs_call"));
    assert_eq!((seen[0].call_id, seen[0].caller_lo), (seen[1].call_id, seen[1].caller_lo));
    assert_eq!((seen[0].field, seen[1].field), (41, 42), "fields read through the mesh's schemas");
    watch.close().unwrap();
}

#[test]
fn a_third_node_sees_a_tasks_whole_life() {
    let a = node("obs_tprov", 41);
    let b = node("obs_tcall", 41);
    let w = node("obs_twatch", 41);
    let log = Arc::new(Mutex::new(Vec::new()));
    let watch = w.call_observer("obs/job", CallObserverOptions::new(), watcher(&log)).unwrap();
    let pending = Arc::new(Mutex::new(None));
    let slot = Arc::clone(&pending);
    let _job = a
        .task_definition("obs/job", TaskOptions::new(), move |t: TaskRequest<ObsReq, ObsReq, ObsRsp>| {
            let task = t.defer();
            task.progress(&ObsReq { x: 1 }).unwrap();
            *slot.lock().unwrap() = Some(task);
        })
        .unwrap();
    let remote = b.remote_task::<ObsReq, ObsReq, ObsRsp>("obs/job", TaskOptions::new()).unwrap();
    assert!(wait_for(6000, None, || watch.match_count() == 1 && remote.match_count() == 1));

    let call = remote.call_async(&ObsReq { x: 0 }, CallOptions::new(), |_| {}).unwrap();
    assert!(wait_for(3000, None, || log.lock().unwrap().len() >= 3), "the call, RUNNING and progress");
    remote.cancel(call.id()).unwrap();
    assert!(wait_for(3000, None, || log.lock().unwrap().len() >= 4), "the cancel");
    let task = pending.lock().unwrap().take().unwrap();
    task.complete_cancelled("stopped", None).unwrap();
    assert!(wait_for(3000, None, || log.lock().unwrap().len() >= 5), "the end");
    let seen = log.lock().unwrap().clone();
    assert_eq!(seen[0].kind, CallEventKind::Call);
    assert_eq!((seen[3].kind, seen[3].message.as_str()), (CallEventKind::Cancel, "obs_tcall"));
    assert_eq!((seen[4].kind, seen[4].status, seen[4].message.as_str()), (CallEventKind::End, CallStatus::Cancelled, "stopped"));
    assert!(seen.iter().all(|s| s.call_id == call.id()));
    assert!(a.call_observer("obs/job", CallObserverOptions::new(), |_| {}).is_err(), "a node cannot define and observe one name");
}
