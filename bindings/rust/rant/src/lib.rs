//! Rant for Rust. A [`Node`] joins the mesh, and its factories make every handle:
//!
//! ```no_run
//! use rant::{Node, NodeOptions, Qos};
//!
//! #[derive(rant::Message)]
//! struct Conveyor { speed: f64, running: bool, part: rant::String<16> }
//!
//! let node = Node::new("line1", NodeOptions::new())?;
//! let conveyor = node.publisher::<Conveyor>("line1/conveyor", Qos::new())?;
//! let _watch = node.subscriber::<Conveyor>("line1/conveyor", Qos::new(), |c| println!("{}", c.speed))?;
//! conveyor.send(&Conveyor { speed: 0.5, running: true, part: "A7".parse()? })?;
//! # Ok::<(), rant::Error>(())
//! ```
//!
//! docs/rust.md is the guide.

#![deny(missing_docs)]
#![deny(unsafe_op_in_unsafe_fn)]

extern crate self as rant;

#[allow(non_camel_case_types, non_upper_case_globals, non_snake_case, dead_code, missing_docs, unsafe_op_in_unsafe_fn, clippy::all)]
mod ffi;

mod callback;
mod error;
mod function;
mod gate;
mod logs;
mod map;
mod message;
mod node;
mod options;
mod pattern;
mod reflect;
mod schema;
mod status;
mod string;
mod task;
mod topic;
pub mod types;
mod variable;

pub use error::{Error, ErrorKind, SendError};
pub use function::{Deferred, FunctionDefinition, FunctionOptions, RemoteFunction, Request};
pub use logs::{Level, LogLine, Origin};
pub use map::{MapItem, MapReader, MapValue, MapWriter};
pub use message::{Bytes, Message, Payload};
pub use node::{Event, EventKind, Node, NodeOptions, NodeStats, Queue, QueueStats, Threading};
pub use options::Qos;
pub use pattern::{CallError, CallOptions, CallStatus, IntoReply, PendingCall, Progress, Response, TaskCall};
pub use rant_macros::{Enum, Message};
pub use reflect::{Entity, EntityKind, MetaNode, MetaProcess, MetaSections, MetaSnapshot, Peer, Reflection};
pub use schema::{EnumVariant, Field, FieldType, FieldView, MessageBuilder, MessageView, Schema};
pub use status::{Condition, StatusOptions, StatusUpdate};
pub use string::String;
pub use task::{PendingTask, RemoteTask, TaskDefinition, TaskOptions, TaskRequest};
pub use topic::{Publisher, Received, Subscriber, TopicCounts, TopicQueueStats};
pub use variable::{RemoteVariable, VariableDefinition, VariableOptions, VariableUpdate};

/// A timeout with no end.
pub const FOREVER: std::time::Duration = std::time::Duration::MAX;

/// The compiled library's version, such as "0.0.23".
pub fn version() -> &'static str {
    // SAFETY: the C returns a static NUL terminated string
    unsafe { std::ffi::CStr::from_ptr(ffi::rant_version()) }.to_str().unwrap_or("")
}

/// What the derive macros expand to. Not part of the API.
#[doc(hidden)]
pub mod __private {
    pub use crate::message::{Decoder, Encoder, Spell, kind};
}

/// Every constant the generated FFI declares for a C enum, so a test can check that each
/// has a Rust variant of its own.
#[cfg(test)]
pub(crate) fn c_enum_values(c_type: &str) -> Vec<(std::string::String, i64)> {
    let tail = format!(": {c_type} = ");
    include_str!("ffi.rs")
        .lines()
        .filter_map(|l| {
            let rest = l.strip_prefix("pub const ")?;
            let (name, value) = rest.split_once(&tail)?;
            Some((name.to_owned(), value.trim_end_matches(';').parse().ok()?))
        })
        .collect()
}
