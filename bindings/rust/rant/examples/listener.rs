use rant::types::Pose2D;
use rant::{FOREVER, Node, NodeOptions, Qos};

/// Prints every pose the talker publishes.
fn main() -> Result<(), rant::Error> {
    let node = Node::new("listener", NodeOptions::new())?;
    let mut pose = node.pulled_subscriber::<Pose2D>("Pose", Qos::new())?;
    while let Some(p) = pose.take(FOREVER)? {
        println!("x {:.2}  y {:.2}  heading {:.2} rad", p.position.x, p.position.y, p.angle);
    }
    Ok(())
}
