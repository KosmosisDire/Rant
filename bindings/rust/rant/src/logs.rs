use std::borrow::Cow;
use std::sync::Arc;
use std::sync::atomic::Ordering::SeqCst;

use crate::callback::Callback;
use crate::error::{Error, ErrorKind, SendError};
use crate::ffi;
use crate::node::{Node, NodeInner, lock};
use crate::schema::MessageView;
use crate::topic::MsgFn;

/// The severity of a log line or a status condition, rising.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Level {
    /// Detail for whoever debugs, sent only when the node's `log_level` allows it.
    Debug,
    /// Normal progress.
    Info,
    /// Something is wrong but work goes on.
    Warn,
    /// Something failed.
    Error,
}

impl Level {
    pub(crate) fn to_c(self) -> ffi::RantLevel {
        match self {
            Level::Debug => ffi::RANT_LEVEL_DEBUG,
            Level::Info => ffi::RANT_LEVEL_INFO,
            Level::Warn => ffi::RANT_LEVEL_WARN,
            Level::Error => ffi::RANT_LEVEL_ERROR,
        }
    }

    /// A wire level. A number past the known ones reads as the nearest.
    pub(crate) fn from_wire(v: u64) -> Level {
        match v {
            0 | 1 => Level::Debug,
            2 => Level::Info,
            3 => Level::Warn,
            _ => Level::Error,
        }
    }
}

/// Who wrote a log line or a condition.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Origin {
    /// The app.
    App,
    /// Rant itself.
    Rant,
}

impl Origin {
    pub(crate) fn from_wire(v: u64) -> Origin {
        if v == u64::from(ffi::RANT_ORIGIN_RANT) { Origin::Rant } else { Origin::App }
    }
}

/// One line from another node's log, as a [`Node::on_log`] handler sees it, valid inside it.
#[derive(Clone, Debug)]
#[non_exhaustive]
pub struct LogLine<'a> {
    /// Its severity.
    pub level: Level,
    /// The app wrote it, or Rant itself.
    pub origin: Origin,
    /// The publishing node's name.
    pub node: &'a str,
    /// The publishing node's peer id.
    pub node_id: u32,
    /// The publisher's wall clock in UTC microseconds when it logged.
    pub wall_us: u64,
    /// When it arrived, on this node's monotonic clock in microseconds.
    pub recv_us: u64,
    /// The text. Bytes that are not UTF-8 read as replacement characters.
    pub text: Cow<'a, str>,
}

pub(crate) type LogFn = dyn for<'a> FnMut(LogLine<'a>) + Send;

impl Node {
    /// Publishes one line on the built in log, cut at 512 bytes. A level below the node's
    /// `log_level` sends nothing. It works inside callbacks too. NoSys when the node was
    /// opened with `disable_logs`.
    pub fn log(&self, level: Level, text: &str) -> Result<(), SendError> {
        let (_pass, raw) = self.inner.enter()?;
        let len = i32::try_from(text.len()).unwrap_or(i32::MAX);
        // SAFETY: the pass keeps the node open, and the text outlives the call
        SendError::check(unsafe { ffi::rant_node_log_text(raw, level.to_c(), text.as_ptr().cast(), len) }).map(|_| ())
    }

    /// Every other node's log lines, where the node's callbacks run. A node never receives
    /// its own lines, and a late handler gets each node's last lines. A later call replaces
    /// the handler.
    pub fn on_log(&self, handler: impl for<'a> FnMut(LogLine<'a>) + Send + 'static) -> Result<(), Error> {
        *lock(&self.inner.on_log) = Some(Arc::new(Callback::new(Box::new(handler))));
        if self.inner.log_bound.load(SeqCst) {
            return Ok(());
        }
        bind_builtin(&self.inner, "rant::Node::on_log", ffi::rant_node_log_topic, ffi::rant_node_set_log_queue,
                     "@rant/log", Box::new(deliver), &self.inner.log_bound)
    }
}

/// Widens a builtin topic of this node to subscribe for the node's life. The queue and the
/// handler go first, so the catch up replay parks or lands on the handler.
pub(crate) fn bind_builtin(
    inner: &NodeInner,
    what: &str,
    topic: unsafe extern "C" fn(*mut ffi::RantNode) -> *mut ffi::RantTopic,
    set_queue: unsafe extern "C" fn(*mut ffi::RantNode, *mut ffi::RantQueue) -> i32,
    name: &str,
    deliver: Box<MsgFn>,
    bound: &std::sync::atomic::AtomicBool,
) -> Result<(), Error> {
    let _topics = inner.topics.enter().ok_or_else(|| Error::new(ErrorKind::State, format!("{what}: the first handler cannot be set inside a callback")))?;
    if bound.load(SeqCst) {
        return Ok(());
    }
    let (_pass, raw) = inner.enter_for(what)?;
    // SAFETY: the pass keeps the node open, and a builtin topic lives as long as the node
    let t = unsafe { topic(raw) };
    if t.is_null() {
        return Err(Error::new(ErrorKind::State, format!("{what}: the builtin is disabled on this node")));
    }
    let q = inner.node_queue();
    // SAFETY: as above
    if !q.is_null() && unsafe { set_queue(raw, q) } != 0 {
        return Err(Error::last(raw, what));
    }
    // SAFETY: as above
    let index = unsafe { ffi::rant_topic_index(t) };
    let id = inner.add_handler(index, name, deliver);
    // SAFETY: as above
    if unsafe { ffi::rant_topic_set_role(t, ffi::RANT_PUBSUB) } != 0 {
        inner.remove_handler(index, id);
        return Err(Error::last(raw, what));
    }
    bound.store(true, SeqCst);
    Ok(())
}

fn deliver(inner: &NodeInner, m: MessageView<'_>) {
    let Some(handler) = lock(&inner.on_log).clone() else { return };
    let line = LogLine {
        level: Level::from_wire(m.get_uint("level")),
        origin: Origin::from_wire(m.get_uint("origin")),
        node: m.publisher_name(),
        node_id: m.publisher_id(),
        wall_us: if m.capture_us() != 0 { m.capture_us() } else { m.written_us() },
        recv_us: m.recv_us(),
        text: m.get_string("text"),
    };
    if let Err(refused) = handler.run(inner.key(), |f| f(line)) {
        inner.report_refused(Some("@rant/log"), refused);
    }
}
