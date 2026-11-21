use std::f64::consts::FRAC_PI_2;
use std::time::Duration;

use rant::types::{Double2, Pose2D};
use rant::{Node, NodeOptions, Qos};

/// Drives a point around the unit circle and publishes where it is.
fn main() -> Result<(), rant::Error> {
    let node = Node::new("talker", NodeOptions::new())?;
    let pose = node.publisher::<Pose2D>("Pose", Qos::new())?;
    node.settle(None);
    let mut t = 0.0f64;
    loop {
        pose.send(&Pose2D { position: Double2 { x: t.cos(), y: t.sin() }, angle: t + FRAC_PI_2 })?;
        t += 0.1;
        std::thread::sleep(Duration::from_millis(500));
    }
}
