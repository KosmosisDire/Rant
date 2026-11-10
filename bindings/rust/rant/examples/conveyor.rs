use std::time::Duration;

use rant::{CallOptions, FunctionOptions, Node, NodeOptions, Request, TaskOptions, TaskRequest, VariableOptions};

/// A conveyor node offers a speed variable, a speed limit function and a run task, and a
/// panel node in the same process uses all three.
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let line = Node::new("line1", NodeOptions::new())?;
    let _speed = line.variable_definition::<f64>("line1/speed", VariableOptions::new().initial(0.0))?;
    let _limit = line.function_definition("line1/check_speed", FunctionOptions::new(), |req: Request<f64, f64>| {
        if *req > 2.0 { Err(format!("{} m/s is over the 2 m/s limit", *req)) } else { Ok(*req) }
    })?;
    let _run = line.task_definition("line1/run", TaskOptions::new(), |req: TaskRequest<u32, f32, u32>| {
        let parts = *req;
        let task = req.defer();
        std::thread::spawn(move || {
            for part in 1..=parts {
                if task.cancelled() {
                    return task.complete_cancelled("stopped", Some(&(part - 1)));
                }
                std::thread::sleep(Duration::from_millis(100));
                task.progress(&(part as f32 / parts as f32))?;
            }
            task.complete(&parts)
        });
    })?;

    let panel = Node::new("panel", NodeOptions::new())?;
    let shown = panel.remote_variable::<f64>("line1/speed", VariableOptions::new())?;
    shown.on_change(|v| println!("speed is {} m/s", *v))?;
    let check = panel.remote_function::<f64, f64>("line1/check_speed", FunctionOptions::new())?;
    let run = panel.remote_task::<u32, f32, u32>("line1/run", TaskOptions::new())?;
    panel.settle(None);

    for wanted in [1.5, 3.0] {
        match check.call(&wanted, CallOptions::new())?.into_result() {
            Ok(ok) => shown.set(&ok)?,
            Err(e) => println!("refused: {}", e.message()),
        }
    }
    let done = run.call(&5, CallOptions::new(), |p| {
        if let Some(share) = p.value() {
            println!("run {:.0}%", share * 100.0);
        }
    })?;
    println!("ran {} parts", done.into_result()?);
    Ok(())
}
