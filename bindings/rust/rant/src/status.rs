use std::borrow::Cow;
use std::ffi::CString;
use std::sync::Arc;
use std::sync::atomic::Ordering::SeqCst;

use crate::callback::Callback;
use crate::error::{Error, SendError};
use crate::ffi;
use crate::logs::{Level, Origin, bind_builtin};
use crate::node::{Node, NodeInner, lock};
use crate::options::options;
use crate::schema::MessageView;

options! {
    /// A condition's options: `StatusOptions::new().clearable(true)`.
    pub struct StatusOptions {
        /// Another node may clear it with [`Node::clear_peer_status`], and this node then
        /// gets an `EventKind::StatusCleared` event naming the key.
        clearable: bool = false,
    }
}

/// One active condition in a node's status, valid inside the handler.
#[derive(Clone, Debug)]
#[non_exhaustive]
pub struct Condition<'a> {
    /// Its identity within the node.
    pub key: Cow<'a, str>,
    /// How serious it is.
    pub level: Level,
    /// The app raised it, or Rant itself.
    pub origin: Origin,
    /// Another node may clear it with [`Node::clear_peer_status`].
    pub clearable: bool,
    /// The owner's wall clock in UTC microseconds when it was raised.
    pub since_us: u64,
    /// What a person reads.
    pub text: Cow<'a, str>,
}

/// A node's whole status, as a [`Node::on_status`] handler sees it: every condition active
/// there now. Empty when the node cleared them all or went away.
#[derive(Clone, Debug)]
#[non_exhaustive]
pub struct StatusUpdate<'a> {
    /// The publishing node's name.
    pub node: &'a str,
    /// The publishing node's peer id.
    pub node_id: u32,
    /// Its active conditions.
    pub conditions: Vec<Condition<'a>>,
}

pub(crate) type StatusFn = dyn for<'a> FnMut(StatusUpdate<'a>) + Send;

fn c_text(s: &str) -> Result<CString, SendError> {
    CString::new(s).map_err(|_| SendError::NoTopic)
}

impl Node {
    /// Raises the condition `key` on this node's status, or updates it while it is active:
    /// `since_us` stays from the raise, and the same values again send nothing. Readers see
    /// it after the next loop pass. TooBig for a key over 48 bytes or past 32 conditions,
    /// NoSys when the node was opened with `disable_status`.
    pub fn set_status(&self, key: &str, level: Level, text: &str, options: StatusOptions) -> Result<(), SendError> {
        let (k, t) = (c_text(key)?, c_text(text)?);
        let opts = ffi::RantStatusOpts { clearable: options.clearable as u8 };
        let (_pass, raw) = self.inner.enter()?;
        // SAFETY: the pass keeps the node open, and the strings and options outlive the call
        SendError::check(unsafe { ffi::rant_node_set_status(raw, k.as_ptr(), level.to_c(), t.as_ptr(), &opts) }).map(|_| ())
    }

    /// Clears the condition `key`, Ok whether or not it was active.
    pub fn clear_status(&self, key: &str) -> Result<(), SendError> {
        let k = c_text(key)?;
        let (_pass, raw) = self.inner.enter()?;
        // SAFETY: the pass keeps the node open, and the key outlives the call
        SendError::check(unsafe { ffi::rant_node_clear_status(raw, k.as_ptr()) }).map(|_| ())
    }

    /// Asks another node to clear its clearable condition `key`, the occurrence raised at
    /// `since_us`. Nothing answers: its next [`StatusUpdate`] shows it gone, or still there
    /// when it refused. [`SendError::BadRole`] while this node does not read status with
    /// `on_status`.
    pub fn clear_peer_status(&self, node_id: u32, key: &str, since_us: u64) -> Result<(), SendError> {
        let k = c_text(key)?;
        let (_pass, raw) = self.inner.enter()?;
        // SAFETY: the pass keeps the node open, and the key outlives the call
        SendError::check(unsafe { ffi::rant_node_clear_peer_status(raw, node_id, k.as_ptr(), since_us) }).map(|_| ())
    }

    /// Every other node's status, where the node's callbacks run: on each change, each
    /// node's current set when this binds, and an empty set when a node goes away. A later
    /// call replaces the handler.
    pub fn on_status(&self, handler: impl for<'a> FnMut(StatusUpdate<'a>) + Send + 'static) -> Result<(), Error> {
        *lock(&self.inner.on_status) = Some(Arc::new(Callback::new(Box::new(handler))));
        if self.inner.status_bound.load(SeqCst) {
            return Ok(());
        }
        bind_builtin(&self.inner, "rant::Node::on_status", ffi::rant_node_status_topic,
                     ffi::rant_node_set_status_queue, "@rant/status", Box::new(deliver), &self.inner.status_bound)
    }
}

fn deliver(inner: &NodeInner, m: MessageView<'_>) {
    let n = m.get_array_count("conditions");
    let conditions: Vec<Condition<'_>> = (0..n)
        .map(|i| {
            let at = |member: &str| format!("conditions[{i}].{member}");
            Condition {
                key: m.get_string(&at("key")),
                level: Level::from_wire(m.get_uint(&at("level"))),
                origin: Origin::from_wire(m.get_uint(&at("origin"))),
                clearable: m.get_bool(&at("clearable")),
                since_us: m.get_uint(&at("since_us")),
                text: m.get_string(&at("text")),
            }
        })
        .collect();
    {
        let mut peers = lock(&inner.status_peers);
        if conditions.is_empty() {
            peers.remove(&m.publisher_id());
        } else {
            peers.insert(m.publisher_id());
        }
    }
    run(inner, StatusUpdate { node: m.publisher_name(), node_id: m.publisher_id(), conditions });
}

/// A node that went away takes its conditions with it: on_status sees its set empty.
pub(crate) fn gone(inner: &NodeInner, peer: u32, name: Option<&str>) {
    if !lock(&inner.status_peers).remove(&peer) {
        return;
    }
    run(inner, StatusUpdate { node: name.unwrap_or(""), node_id: peer, conditions: Vec::new() });
}

fn run(inner: &NodeInner, update: StatusUpdate<'_>) {
    let Some(handler) = lock(&inner.on_status).clone() else { return };
    if let Err(refused) = handler.run(inner.key(), |f| f(update)) {
        inner.report_refused(Some("@rant/status"), refused);
    }
}
