use std::borrow::Cow;
use std::sync::Arc;
use std::sync::atomic::Ordering::SeqCst;

use crate::callback::Callback;
use crate::error::{Error, ErrorKind, SendError};
use crate::ffi;
use crate::node::{Node, NodeInner, lock};
use crate::schema::MessageView;

/// The severity of a line on the built in log topics.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum LogLevel {
    /// `@rant/log/error`, where the node also mirrors its own error events.
    Error,
    /// `@rant/log/warn`.
    Warn,
    /// `@rant/log/info`.
    Info,
}

impl LogLevel {
    const ALL: [LogLevel; 3] = [LogLevel::Error, LogLevel::Warn, LogLevel::Info];

    fn to_c(self) -> ffi::RantLogLevel {
        match self {
            LogLevel::Error => ffi::RANT_LOG_ERROR,
            LogLevel::Warn => ffi::RANT_LOG_WARN,
            LogLevel::Info => ffi::RANT_LOG_INFO,
        }
    }
}

/// One line from another node's log, as a [`Node::on_log`] handler sees it, valid inside it.
#[derive(Clone, Debug)]
#[non_exhaustive]
pub struct LogLine<'a> {
    /// Its severity.
    pub level: LogLevel,
    /// The publishing node's name.
    pub node: &'a str,
    /// The publishing node's peer id.
    pub node_id: u32,
    /// The publisher's wall clock in UTC microseconds when it logged.
    pub wall_us: u64,
    /// The publisher's monotonic clock in microseconds when it logged.
    pub mono_us: u64,
    /// When it arrived, on this node's monotonic clock in microseconds.
    pub recv_us: u64,
    /// The publisher's wall clock in UTC microseconds when the line was sent.
    pub written_us: u64,
    /// The text. Bytes that are not UTF-8 read as replacement characters.
    pub text: Cow<'a, str>,
}

pub(crate) type LogFn = dyn for<'a> FnMut(LogLine<'a>) + Send;

impl Node {
    /// Publishes one line on the level's built in log topic, cut at 512 bytes. It works
    /// inside callbacks too. NoSys when the node was opened with `disable_logs`.
    pub fn log(&self, level: LogLevel, text: &str) -> Result<(), SendError> {
        let (_pass, raw) = self.inner.enter()?;
        let len = i32::try_from(text.len()).unwrap_or(i32::MAX);
        // SAFETY: the pass keeps the node open, and the text outlives the call
        SendError::check(unsafe { ffi::rant_node_log_text(raw, level.to_c(), text.as_ptr().cast(), len) }).map(|_| ())
    }

    /// Every other node's log lines at every level, where the node's callbacks run. A node
    /// never receives its own lines, and a late handler gets each node's last lines per
    /// level. A later call replaces the handler.
    pub fn on_log(&self, handler: impl for<'a> FnMut(LogLine<'a>) + Send + 'static) -> Result<(), Error> {
        const WHAT: &str = "rant::Node::on_log";
        *lock(&self.inner.on_log) = Some(Arc::new(Callback::new(Box::new(handler))));
        if self.inner.log_bound.load(SeqCst) {
            return Ok(());
        }
        let _topics = self.inner.topics.enter().ok_or_else(|| Error::new(ErrorKind::State, format!("{WHAT}: the first handler cannot be set inside a callback")))?;
        if self.inner.log_bound.load(SeqCst) {
            return Ok(());
        }
        let (_pass, raw) = self.inner.enter_for(WHAT)?;
        let q = self.inner.node_queue();
        // SAFETY: the pass keeps the node open. The queue goes first so the catch up parks too
        if !q.is_null() && unsafe { ffi::rant_node_set_log_queue(raw, q) } != 0 {
            return Err(Error::last(raw, WHAT));
        }
        for level in LogLevel::ALL {
            // SAFETY: as above, and a builtin topic lives as long as the node
            let t = unsafe { ffi::rant_node_log_topic(raw, level.to_c()) };
            if t.is_null() {
                return Err(Error::new(ErrorKind::State, format!("{WHAT}: logs are disabled on this node")));
            }
            // SAFETY: as above
            if unsafe { ffi::rant_topic_set_role(t, ffi::RANT_PUBSUB) } != 0 {
                return Err(Error::last(raw, WHAT));
            }
            // SAFETY: as above
            let index = unsafe { ffi::rant_topic_index(t) };
            self.inner.add_handler(index, "@rant/log", Box::new(move |inner: &NodeInner, m: MessageView<'_>| deliver(inner, level, m)));
        }
        self.inner.log_bound.store(true, SeqCst);
        Ok(())
    }
}

fn deliver(inner: &NodeInner, level: LogLevel, m: MessageView<'_>) {
    let Some(handler) = lock(&inner.on_log).clone() else { return };
    let line = LogLine {
        level,
        node: m.publisher_name(),
        node_id: m.publisher_id(),
        wall_us: m.get_uint("wall_us"),
        mono_us: m.get_uint("mono_us"),
        recv_us: m.recv_us(),
        written_us: m.written_us(),
        text: m.get_string("text"),
    };
    if let Err(refused) = handler.run(inner.key(), |f| f(line)) {
        inner.report_refused(Some("@rant/log"), refused);
    }
}
