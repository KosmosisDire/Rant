// Functions and tasks: answers by return value, deferred answers, futures with no runtime,
// progress, cancel by drop and by id, and every outcome a caller can see.

mod common;

use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use common::{block_on, node, wait_for};
use rant::{
    Bytes, CallOptions, CallStatus, FunctionOptions, MessageBuilder, Qos, Request, SendError, TaskOptions, TaskRequest,
};

#[derive(rant::Message, Clone, Debug, PartialEq)]
struct AddReq {
    a: i32,
    b: i32,
}

#[derive(rant::Message, Clone, Debug, PartialEq)]
struct Goal {
    steps: u32,
}

#[derive(rant::Message, Clone, Debug, PartialEq)]
struct Done {
    steps: u32,
}

#[test]
fn a_return_value_is_the_answer() {
    let a = node("fn_def", 20);
    let b = node("fn_call", 20);
    let _add = a.function_definition("Add", FunctionOptions::new(), |req: Request<AddReq, i32>| req.a + req.b).unwrap();
    let _div = a
        .function_definition("Div", FunctionOptions::new(), |req: Request<AddReq, i32>| {
            if req.b == 0 { Err(format!("{} over zero", req.a)) } else { Ok(req.a / req.b) }
        })
        .unwrap();
    let _boom = a.function_definition("Boom", FunctionOptions::new(), |_: Request<AddReq, i32>| -> i32 { panic!("no luck") }).unwrap();
    let caller = a
        .function_definition("Who", FunctionOptions::new(), |req: Request<AddReq, rant::String<32>>| {
            req.caller_name().parse::<rant::String<32>>().unwrap()
        })
        .unwrap();

    let add = b.remote_function::<AddReq, i32>("Add", FunctionOptions::new()).unwrap();
    let div = b.remote_function::<AddReq, i32>("Div", FunctionOptions::new()).unwrap();
    let boom = b.remote_function::<AddReq, i32>("Boom", FunctionOptions::new()).unwrap();
    let who = b.remote_function::<AddReq, rant::String<32>>("Who", FunctionOptions::new()).unwrap();
    assert!(wait_for(4000, None, || add.match_count() > 0 && div.match_count() > 0 && boom.match_count() > 0 && who.match_count() > 0));
    assert_eq!(caller.match_count(), 1, "a definition counts its callers");

    let r = add.call(&AddReq { a: 1, b: 2 }, CallOptions::new()).unwrap();
    assert_eq!((r.status(), r.value().copied()), (CallStatus::Ok, Some(3)));
    assert!(r.provider() != 0, "the answering peer is named");
    assert_eq!(add.call(&AddReq { a: 40, b: 2 }, CallOptions::new()).unwrap().into_result().unwrap(), 42);

    let r = div.call(&AddReq { a: 7, b: 0 }, CallOptions::new()).unwrap();
    assert_eq!(r.status(), CallStatus::AppError);
    assert_eq!(r.message(), "7 over zero", "an Err answers AppError with its text");
    assert_eq!(div.call(&AddReq { a: 9, b: 3 }, CallOptions::new()).unwrap().into_result().unwrap(), 3);

    let r = boom.call(&AddReq { a: 0, b: 0 }, CallOptions::new()).unwrap();
    assert_eq!(r.status(), CallStatus::AppError);
    assert!(r.message().contains("no luck"), "a panic answers AppError with its text: {}", r.message());

    assert_eq!(who.call(&AddReq { a: 0, b: 0 }, CallOptions::new()).unwrap().into_result().unwrap().as_str(), "fn_call");

    let e = div.call(&AddReq { a: 1, b: 0 }, CallOptions::new()).unwrap().into_result().unwrap_err();
    assert_eq!(e.status(), CallStatus::AppError);
    let as_error: rant::Error = e.into();
    assert!(as_error.to_string().contains("over zero"), "a CallError converts for ?: {as_error}");
}

#[test]
fn a_future_needs_no_runtime() {
    let a = node("fut_def", 21);
    let b = node("fut_call", 21);
    let _add = a.function_definition("Add", FunctionOptions::new(), |req: Request<AddReq, i32>| req.a + req.b).unwrap();
    let add = b.remote_function::<AddReq, i32>("Add", FunctionOptions::new()).unwrap();
    assert!(wait_for(4000, None, || add.match_count() > 0));
    let pending = add.call_async(&AddReq { a: 5, b: 6 }, CallOptions::new()).unwrap();
    let r = block_on(pending);
    assert_eq!(r.into_result().unwrap(), 11);

    // two in flight at once, awaited out of order
    let p1 = add.call_async(&AddReq { a: 1, b: 1 }, CallOptions::new()).unwrap();
    let p2 = add.call_async(&AddReq { a: 2, b: 2 }, CallOptions::new()).unwrap();
    assert_eq!(block_on(p2).into_result().unwrap(), 4);
    assert_eq!(block_on(p1).into_result().unwrap(), 2);
}

#[test]
fn a_deferred_answer_completes_from_any_thread() {
    let a = node("defer_def", 22);
    let b = node("defer_call", 22);
    let _later = a
        .function_definition("Later", FunctionOptions::new(), |req: Request<AddReq, i32>| {
            let sum = req.a + req.b;
            let answer = req.defer();
            std::thread::spawn(move || {
                std::thread::sleep(Duration::from_millis(50));
                answer.complete(&sum).unwrap();
            });
        })
        .unwrap();
    let _dropped = a
        .function_definition("Dropped", FunctionOptions::new(), |req: Request<AddReq, i32>| {
            let answer = req.defer();
            std::thread::spawn(move || drop(answer));
        })
        .unwrap();
    let later = b.remote_function::<AddReq, i32>("Later", FunctionOptions::new()).unwrap();
    let dropped = b.remote_function::<AddReq, i32>("Dropped", FunctionOptions::new()).unwrap();
    assert!(wait_for(4000, None, || later.match_count() > 0 && dropped.match_count() > 0));
    assert_eq!(later.call(&AddReq { a: 2, b: 3 }, CallOptions::new()).unwrap().into_result().unwrap(), 5);
    let r = dropped.call(&AddReq { a: 0, b: 0 }, CallOptions::new().timeout(Duration::from_secs(3))).unwrap();
    assert_eq!(r.status(), CallStatus::AppError, "a dropped deferred answer never leaves the caller waiting");
    assert!(r.message().contains("dropped"), "{}", r.message());
}

#[test]
fn outcomes_decided_on_the_caller() {
    let b = node("nobody_call", 23);
    let lonely = b.remote_function::<AddReq, i32>("Nobody", FunctionOptions::new()).unwrap();
    let r = lonely.call(&AddReq { a: 0, b: 0 }, CallOptions::new().timeout(Duration::from_millis(300))).unwrap();
    assert!(matches!(r.status(), CallStatus::NoProvider | CallStatus::Timeout), "{:?}", r.status());
    assert!(r.value().is_none());
    assert!(!r.message().is_empty(), "a failed call carries text for a person");

    // a remote closed with a call in flight answers it Cancelled
    let pending = lonely.call_async(&AddReq { a: 0, b: 0 }, CallOptions::new()).unwrap();
    lonely.close().unwrap();
    assert_eq!(block_on(pending).status(), CallStatus::Cancelled);

    // and a node closed with one in flight does the same
    let c = node("closing_call", 23);
    let remote = c.remote_function::<AddReq, i32>("Nobody", FunctionOptions::new()).unwrap();
    let pending = remote.call_async(&AddReq { a: 0, b: 0 }, CallOptions::new()).unwrap();
    c.close().unwrap();
    assert_eq!(block_on(pending).status(), CallStatus::Cancelled);
    assert_eq!(remote.call(&AddReq { a: 0, b: 0 }, CallOptions::new()).err(), Some(SendError::NoTopic), "a handle outliving its node");
}

#[test]
fn a_blocking_call_is_refused_inside_a_callback() {
    let a = node("inline_def", 24);
    let b = node("inline_call", 24);
    let _add = a.function_definition("Add", FunctionOptions::new(), |req: Request<AddReq, i32>| req.a + req.b).unwrap();
    let add = Arc::new(b.remote_function::<AddReq, i32>("Add", FunctionOptions::new()).unwrap());
    assert!(wait_for(4000, None, || add.match_count() > 0));
    let seen = Arc::new(Mutex::new(None));
    let (s, f) = (Arc::clone(&seen), Arc::clone(&add));
    let _sub = b
        .subscriber::<u32>("kick", Qos::new().reliable(true), move |_| {
            let blocking = f.call(&AddReq { a: 1, b: 1 }, CallOptions::new()).err();
            let pending = f.call_async(&AddReq { a: 1, b: 1 }, CallOptions::new()).is_ok();
            *s.lock().unwrap() = Some((blocking, pending));
        })
        .unwrap();
    let kick = a.publisher::<u32>("kick", Qos::new().reliable(true)).unwrap();
    assert!(wait_for(4000, None, || kick.match_count() > 0 && kick.ready()));
    kick.send(&1).unwrap();
    assert!(wait_for(3000, None, || seen.lock().unwrap().is_some()));
    assert_eq!(*seen.lock().unwrap(), Some((Some(SendError::State), true)), "call_async is the tool inside a callback");
}

#[test]
fn raw_functions_carry_bytes_and_fields() {
    let a = node("raw_fn_def", 25);
    let b = node("raw_fn_call", 25);
    let sa = a.schema("Sum { total: i64 }").unwrap();
    let qa = a.schema("Pair { x: i64, y: i64 }").unwrap();
    let (sa2, qa2) = (sa.clone(), qa.clone());
    let _def = a
        .function_definition(
            "Pair",
            FunctionOptions::new().request_schema(&qa).response_schema(&sa),
            move |req: Request<Bytes, Bytes>| {
                assert!(req.has_schema() && qa2.hash() != 0);
                let mut m = MessageBuilder::new(&sa2);
                m.set_int("total", req.get_int("x") + req.get_int("y")).unwrap();
                m.bytes().to_vec()
            },
        )
        .unwrap();
    let qb = b.schema("Pair { x: i64, y: i64 }").unwrap();
    let sb = b.schema("Sum { total: i64 }").unwrap();
    let pair = b.remote_function::<Bytes, Bytes>("Pair", FunctionOptions::new().request_schema(&qb).response_schema(&sb)).unwrap();
    assert!(wait_for(4000, None, || pair.match_count() > 0));
    let mut m = MessageBuilder::new(&qb);
    m.set_int("x", 40).unwrap().set_int("y", 2).unwrap();
    let r = pair.call(m.bytes(), CallOptions::new()).unwrap();
    assert!(r.is_ok(), "{}", r.message());
    assert_eq!(r.fields().get_int("total"), 42, "a raw response reads its fields by name");
    assert_eq!(pair.response_schema().map(|s| s.hash()), Some(sb.hash()));
}

#[test]
fn tasks_stream_progress_and_answer() {
    let a = node("task_def", 26);
    let b = node("task_call", 26);
    let _mover = a
        .task_definition("Move", TaskOptions::new(), |req: TaskRequest<Goal, f32, Done>| {
            let steps = req.steps;
            let task = req.defer();
            std::thread::spawn(move || {
                for i in 1..=steps {
                    task.progress(&(i as f32 / steps as f32)).unwrap();
                }
                task.complete(&Done { steps }).unwrap();
            });
        })
        .unwrap();
    let _quick = a.task_definition("Quick", TaskOptions::new(), |req: TaskRequest<Goal, f32, Done>| Done { steps: req.steps * 2 }).unwrap();
    let _silent = a.task_definition("Silent", TaskOptions::new(), |_: TaskRequest<Goal, f32, Done>| {}).unwrap();
    let mover = b.remote_task::<Goal, f32, Done>("Move", TaskOptions::new()).unwrap();
    let quick = b.remote_task::<Goal, f32, Done>("Quick", TaskOptions::new()).unwrap();
    let silent = b.remote_task::<Goal, f32, Done>("Silent", TaskOptions::new()).unwrap();
    assert!(wait_for(4000, None, || mover.match_count() > 0 && quick.match_count() > 0 && silent.match_count() > 0));

    let mut seen = Vec::new();
    let mut started = 0;
    let r = mover
        .call(&Goal { steps: 4 }, CallOptions::new(), |p| match p.value() {
            Some(v) => seen.push(*v),
            None => started += 1,
        })
        .unwrap();
    assert_eq!(r.into_result().unwrap(), Done { steps: 4 });
    assert_eq!(started, 1, "the start of the work arrives once with no value");
    assert_eq!(seen, vec![0.25, 0.5, 0.75, 1.0], "reliable progress arrives whole and in order");

    assert_eq!(quick.call(&Goal { steps: 3 }, CallOptions::new(), |_| {}).unwrap().into_result().unwrap(), Done { steps: 6 });
    let r = silent.call(&Goal { steps: 1 }, CallOptions::new(), |_| {}).unwrap();
    assert_eq!(r.status(), CallStatus::AppError, "a task handler returning nothing and deferring nothing fails");

    let updates = Arc::new(AtomicU32::new(0));
    let u = Arc::clone(&updates);
    let run = mover.call_async(&Goal { steps: 3 }, CallOptions::new(), move |_| {
        u.fetch_add(1, Ordering::SeqCst);
    });
    let r = block_on(run.unwrap());
    assert_eq!(r.into_result().unwrap(), Done { steps: 3 });
    assert_eq!(updates.load(Ordering::SeqCst), 4, "three updates and the start");
}

#[test]
fn tasks_cancel_by_id_and_by_drop() {
    let a = node("cancel_def", 27);
    let b = node("cancel_call", 27);
    let cancels = Arc::new(AtomicU32::new(0));
    let def = a
        .task_definition("Spin", TaskOptions::new(), |req: TaskRequest<Goal, u32, Done>| {
            let task = req.defer();
            std::thread::spawn(move || {
                let mut n = 0;
                while !task.cancelled() {
                    n += 1;
                    if n % 10 == 0 {
                        let _ = task.progress(&n);
                    }
                    std::thread::sleep(Duration::from_millis(5));
                    if n > 2000 {
                        return task.fail("never cancelled").unwrap();
                    }
                }
                task.complete_cancelled("stopped", Some(&Done { steps: n })).unwrap();
            });
        })
        .unwrap();
    let c = Arc::clone(&cancels);
    def.on_cancel(move |_| {
        c.fetch_add(1, Ordering::SeqCst);
    })
    .unwrap();
    let spin = Arc::new(b.remote_task::<Goal, u32, Done>("Spin", TaskOptions::new()).unwrap());
    assert!(wait_for(4000, None, || spin.match_count() > 0));

    // by id, from the progress handler's thread while the caller blocks
    let canceller = Arc::clone(&spin);
    let r = spin
        .call(&Goal { steps: 0 }, CallOptions::new(), move |p| {
            if p.value().is_some_and(|v| *v >= 20) {
                let _ = canceller.cancel(p.call_id());
            }
        })
        .unwrap();
    assert_eq!(r.status(), CallStatus::Cancelled, "{}", r.message());
    assert_eq!(r.message(), "stopped");
    assert!(r.value().is_some_and(|d| d.steps >= 20), "a cancelled task hands back what got done");

    // by dropping the future before its outcome
    let run = spin.call_async(&Goal { steps: 0 }, CallOptions::new(), |_| {}).unwrap();
    std::thread::sleep(Duration::from_millis(200));
    let before = cancels.load(Ordering::SeqCst);
    drop(run);
    assert!(wait_for(3000, None, || cancels.load(Ordering::SeqCst) > before), "dropping a task call cancels it");

    // a detached call runs on: cancel it by id afterwards
    let before = cancels.load(Ordering::SeqCst);
    let run = spin.call_async(&Goal { steps: 0 }, CallOptions::new(), |_| {}).unwrap();
    let id = run.id();
    run.detach();
    std::thread::sleep(Duration::from_millis(200));
    assert_eq!(cancels.load(Ordering::SeqCst), before, "a detached call is not cancelled");
    spin.cancel(id).unwrap();
    assert!(wait_for(3000, None, || cancels.load(Ordering::SeqCst) > before));
}

#[test]
fn a_dropped_pending_task_answers() {
    let a = node("drop_def", 28);
    let b = node("drop_call", 28);
    let _def = a
        .task_definition("Lost", TaskOptions::new(), |req: TaskRequest<Goal, u32, Done>| {
            let task = req.defer();
            std::thread::spawn(move || drop(task));
        })
        .unwrap();
    let lost = b.remote_task::<Goal, u32, Done>("Lost", TaskOptions::new()).unwrap();
    assert!(wait_for(4000, None, || lost.match_count() > 0));
    let r = lost.call(&Goal { steps: 0 }, CallOptions::new(), |_| {}).unwrap();
    assert_eq!(r.status(), CallStatus::AppError);
    assert!(r.message().contains("dropped"), "{}", r.message());
    assert_eq!(lost.progress_schema().map(|s| s.to_dsl().trim().to_owned()), Some("u32".to_owned()));
}
