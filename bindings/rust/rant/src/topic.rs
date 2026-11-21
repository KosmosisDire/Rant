use std::ffi::CString;
use std::marker::PhantomData;
use std::ops::Deref;
use std::ptr::null_mut;
use std::sync::Arc;
use std::time::Duration;

use crate::callback::Callback;
use crate::error::{Error, ErrorKind, SendError};
use crate::ffi;
use crate::message::{Bytes, Codec, Payload};
use crate::node::{NodeInner, lock};
use crate::options::Qos;
use crate::schema::{MessageView, Schema};
use crate::types::Timestamp;

pub(crate) type MsgFn = dyn for<'a> FnMut(&NodeInner, MessageView<'a>) + Send;

/// A subscriber handler, kept by its topic's index.
pub(crate) struct SubHandler {
    pub(crate) name: String,
    pub(crate) cb: Callback<MsgFn>,
}

/// The C topic under every publisher and subscriber. Same name handles are separate C
/// handles of one entity, which the C shares.
pub(crate) struct TopicCore {
    inner: Arc<NodeInner>,
    raw: *mut ffi::RantTopic,
    name: String,
    open: bool,
}

// SAFETY: every C call on a topic is thread safe under the node lock, and holds a pass
unsafe impl Send for TopicCore {}
unsafe impl Sync for TopicCore {}

impl TopicCore {
    pub(crate) const PUB: u8 = 1;
    pub(crate) const SUB: u8 = 2;

    /// Creates a C topic handle of the name.
    pub(crate) fn open(inner: &Arc<NodeInner>, name: &str, bits: u8, codec: Option<&Codec>, qos: &Qos, pull: bool) -> Result<TopicCore, Error> {
        let what = match (bits, pull) {
            (Self::PUB, _) => "rant::Node::publisher",
            (_, true) => "rant::Node::pulled_subscriber",
            _ => "rant::Node::subscriber",
        };
        let fail = |kind, why: &str| Error::new(kind, format!("{what} \"{name}\": {why}"));
        for (other, item) in [(qos.queue.as_ref().map(|q| &q.inner), "queue"), (qos.schema.as_ref().map(|s| &s.inner), "schema")] {
            if other.is_some_and(|o| !Arc::ptr_eq(o, inner)) {
                return Err(fail(ErrorKind::State, &format!("the {item} belongs to another node")));
            }
        }
        let schema = match (codec, &qos.schema) {
            (Some(c), _) => c.schema,
            (None, Some(s)) => s.raw,
            (None, None) => std::ptr::null(),
        };
        let named = qos.queue.as_ref().map_or(null_mut(), |q| q.raw);
        let queue = if pull || !named.is_null() { named } else { inner.node_queue() };
        let cname = CString::new(name).map_err(|_| fail(ErrorKind::BadName, "the name holds a NUL"))?;
        let role = if bits == Self::PUB { ffi::RANT_PUB_ONLY } else { ffi::RANT_SUB_ONLY };

        let _creates = inner.creates.enter().ok_or_else(|| fail(ErrorKind::State, "a handle cannot be made inside a callback"))?;
        let (_pass, raw) = inner.enter_for(what)?;
        let co = ffi::RantTopicOpts { qos: qos.to_c(), reflect_from_mesh: qos.reflect_from_mesh as u8, queue, pull: pull as u8 };
        // SAFETY: the pass keeps the node open, and the schema is node owned
        let t = unsafe { ffi::rant_node_create_topic(raw, cname.as_ptr(), role, schema, &co) };
        if t.is_null() {
            return Err(Error::last(raw, &format!("{what} \"{name}\"")));
        }
        Ok(TopicCore { inner: Arc::clone(inner), raw: t, name: name.to_owned(), open: true })
    }

    /// Runs `f` for every message on this handle until it closes.
    pub(crate) fn set_handler(&mut self, f: Box<MsgFn>) {
        let Ok((_pass, _)) = self.inner.enter() else { return };
        // SAFETY: the pass keeps the topic alive
        let index = unsafe { ffi::rant_topic_index(self.raw) };
        self.inner.set_handler(index, &self.name, f);
    }

    /// Retires this handle. The C retires the entity with its last handle.
    pub(crate) fn close(&mut self) -> Result<(), SendError> {
        if !std::mem::replace(&mut self.open, false) {
            return Ok(());
        }
        let Some(pass) = self.inner.gate.enter() else { return Ok(()) };
        // SAFETY: the pass keeps the topic alive
        let index = unsafe { ffi::rant_topic_index(self.raw) };
        // the handler leaves first, so a refused retire still stops its deliveries
        lock(&self.inner.handlers).remove(&index);
        let Some(creates) = self.inner.creates.enter() else {
            self.inner.report_close_refused(&self.name);
            return Err(SendError::State);
        };
        // SAFETY: the pass keeps the topic alive, and it is freed only on success
        let rc = unsafe { ffi::rant_topic_retire(self.raw) };
        if rc != 0 {
            drop(creates);
            drop(pass);
            self.inner.report_close_refused(&self.name);
            return SendError::check(rc).map(|_| ());
        }
        Ok(())
    }

    pub(crate) fn send(&self, data: &[u8], capture_us: u64) -> Result<(), SendError> {
        let (_pass, _) = self.inner.enter()?;
        let opts = ffi::RantSendOpts { capture_us };
        let bytes = ffi::RantBytes { data: data.as_ptr(), len: data.len() };
        // SAFETY: the pass keeps the topic alive, and the bytes outlive the call
        SendError::check(unsafe { ffi::rant_topic_send(self.raw, bytes, &opts) }).map(|_| ())
    }

    /// The oldest or the newest waiting message of a pulled topic, its views in `msg`.
    fn take<'a>(&'a self, msg: &'a mut ffi::RantMsg, latest: bool, timeout: Duration) -> Result<Option<MessageView<'a>>, Error> {
        let raw = self.raw;
        let rc = self.inner.wait_sliced(timeout, |_, ms| {
            // SAFETY: the slice holds a pass, and msg is ours alone through &mut
            unsafe { if latest { ffi::rant_topic_take_latest(raw, msg, ms) } else { ffi::rant_topic_take(raw, msg, ms) } }
        });
        match rc {
            Ok(0) => Ok(None),
            Ok(_) => {
                // the views live in the topic's ring until the next take, which &mut self
                // holds off, and the pass holds off close
                let pass = self.inner.gate.enter().ok_or_else(|| Error::new(ErrorKind::State, "rant::Subscriber::take: the node closed"))?;
                // SAFETY: msg was just filled by the C, and stays untouched while the view lives
                Ok(Some(unsafe { MessageView::delivered(&self.inner, &*msg) }.hold(pass)))
            }
            Err(SendError::State) => Err(Error::new(ErrorKind::State, format!(
                "rant::Subscriber::take \"{}\": this subscriber has a handler, take needs one made by pulled_subscriber",
                self.name
            ))),
            Err(e) => Err(Error::new(ErrorKind::State, format!("rant::Subscriber::take \"{}\": {e}", self.name))),
        }
    }

    fn with<R: Default>(&self, f: impl FnOnce(*mut ffi::RantTopic) -> R) -> R {
        match self.inner.enter() {
            Ok(_) => f(self.raw),
            Err(_) => R::default(),
        }
    }

    fn schema(&self) -> Option<Schema> {
        // SAFETY: with holds a pass
        let s = self.with(|t| unsafe { ffi::rant_topic_schema(t) } as usize);
        (s != 0).then(|| Schema::new(Arc::clone(&self.inner), s as *const ffi::RantSchema))
    }

    fn counts(&self) -> TopicCounts {
        let mut c = TopicCounts::default();
        // SAFETY: with holds a pass
        self.with(|t| unsafe { ffi::rant_topic_counts(t, &mut c.tx_msgs, &mut c.tx_bytes, &mut c.rx_msgs, &mut c.rx_bytes) });
        c
    }

    fn refresh(&self) -> Result<bool, SendError> {
        let (_pass, _) = self.inner.enter()?;
        // SAFETY: the pass keeps the topic alive
        Ok(SendError::check(unsafe { ffi::rant_topic_refresh(self.raw) })? == 1)
    }
}

impl NodeInner {
    /// Runs `f` for every message on the topic at `index`.
    pub(crate) fn set_handler(&self, index: u16, name: &str, f: Box<MsgFn>) {
        lock(&self.handlers).insert(index, Arc::new(SubHandler { name: name.to_owned(), cb: Callback::new(f) }));
    }
}

impl Drop for TopicCore {
    fn drop(&mut self) {
        let _ = self.close();
    }
}

/// A topic's traffic since open: what this node committed to it and received on it.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[non_exhaustive]
pub struct TopicCounts {
    /// Messages sent.
    pub tx_msgs: u64,
    /// Bytes sent.
    pub tx_bytes: u64,
    /// Messages received.
    pub rx_msgs: u64,
    /// Bytes received.
    pub rx_bytes: u64,
}

/// The publishing side of a topic, from [`Node::publisher`](crate::Node::publisher). Same
/// name handles on a node share one topic, and the last one dropped retires it.
pub struct Publisher<T> {
    core: TopicCore,
    codec: Option<Arc<Codec>>,
    _t: PhantomData<fn() -> T>,
}

impl<T: Payload> Publisher<T> {
    pub(crate) fn new(core: TopicCore, codec: Option<Arc<Codec>>) -> Publisher<T> {
        Publisher { core, codec, _t: PhantomData }
    }

    /// Sends `value` to every matched subscriber. Schema when it does not encode, NoTopic
    /// when nobody matched within the match wait.
    pub fn send(&self, value: &T::Input) -> Result<(), SendError> {
        self.send_at(value, 0)
    }

    /// The same, stating when the data was true as against when it is sent.
    pub fn send_captured(&self, value: &T::Input, capture: Timestamp) -> Result<(), SendError> {
        self.send_at(value, capture.us.max(0) as u64)
    }

    fn send_at(&self, value: &T::Input, capture_us: u64) -> Result<(), SendError> {
        // the encode reads the codec's schema, which lives in the node
        let (_pass, _) = self.core.inner.enter()?;
        T::__with_bytes(self.codec.as_deref(), value, |bytes| self.core.send(bytes, capture_us)).map_err(|_| SendError::Schema)?
    }

    /// The subscribers matched now.
    pub fn match_count(&self) -> usize {
        // SAFETY: with holds a pass
        self.core.with(|t| unsafe { ffi::rant_topic_match_count(t) }).max(0) as usize
    }

    /// True when a send would not wait for a match: a subscriber is matched, or matching
    /// settled with nobody there.
    pub fn ready(&self) -> bool {
        // SAFETY: with holds a pass
        self.core.with(|t| unsafe { ffi::rant_topic_ready(t) } == 1)
    }

    /// Waits until every reliable subscriber has what was sent, the flush before a close.
    /// False when `timeout` passed first.
    pub fn drain(&self, timeout: Duration) -> bool {
        let raw = self.core.raw;
        // SAFETY: each slice holds a pass
        self.core.inner.wait_sliced(timeout, |_, ms| unsafe { ffi::rant_topic_drain(raw, ms) }) == Ok(1)
    }

    /// The topic's traffic counters.
    pub fn counts(&self) -> TopicCounts {
        self.core.counts()
    }

    /// The name as given.
    pub fn name(&self) -> &str {
        &self.core.name
    }

    /// The schema in use, None for untyped bytes.
    pub fn schema(&self) -> Option<Schema> {
        self.core.schema()
    }

    /// Releases the handle now. The last handle on a name retires the topic. State inside
    /// one of the node's inline callbacks, and the topic then stays until the node closes.
    pub fn close(mut self) -> Result<(), SendError> {
        self.core.close()
    }
}

impl Publisher<Bytes> {
    /// A `reflect_from_mesh` publisher: re types in place when the mesh moved. True = re typed.
    pub fn refresh(&self) -> Result<bool, SendError> {
        self.core.refresh()
    }
}

/// The subscribing side of a topic, from [`Node::subscriber`](crate::Node::subscriber) with a
/// handler, or [`Node::pulled_subscriber`](crate::Node::pulled_subscriber) to read with `take`.
pub struct Subscriber<T> {
    core: TopicCore,
    codec: Option<Arc<Codec>>,
    msg: ffi::RantMsg,
    _t: PhantomData<fn() -> T>,
}

// SAFETY: msg holds only views into the C ring, read solely through &mut self
unsafe impl<T> Send for Subscriber<T> {}
unsafe impl<T> Sync for Subscriber<T> {}

impl<T: Payload> Subscriber<T> {
    pub(crate) fn new(core: TopicCore, codec: Option<Arc<Codec>>) -> Subscriber<T> {
        Subscriber { core, codec, msg: ffi::RantMsg::default(), _t: PhantomData }
    }

    fn pull(&mut self, latest: bool, timeout: Duration) -> Result<Option<T::Value<'_>>, Error> {
        let codec = self.codec.clone();
        let name = self.core.name.clone();
        let Some(view) = self.core.take(&mut self.msg, latest, timeout)? else { return Ok(None) };
        T::__taken(codec.as_deref(), view)
            .map(Some)
            .map_err(|why| Error::new(ErrorKind::SchemaMismatch, format!("rant::Subscriber::take \"{name}\": {why}")))
    }

    /// The oldest waiting message of a pulled subscriber, waiting up to `timeout` for one
    /// (`Duration::ZERO` checks, [`FOREVER`](crate::FOREVER) waits with no end).
    pub fn take(&mut self, timeout: Duration) -> Result<Option<T::Value<'_>>, Error> {
        self.pull(false, timeout)
    }

    /// The newest waiting message of a pulled subscriber, dropping the older ones.
    pub fn take_latest(&mut self, timeout: Duration) -> Result<Option<T::Value<'_>>, Error> {
        self.pull(true, timeout)
    }

    /// The topic's traffic counters.
    pub fn counts(&self) -> TopicCounts {
        self.core.counts()
    }

    /// The ring of a queued or pulled subscriber: what waits now and what was dropped.
    /// All zero for an inline one.
    pub fn queue_stats(&self) -> TopicQueueStats {
        let mut s = TopicQueueStats::default();
        // SAFETY: with holds a pass
        self.core.with(|t| unsafe { ffi::rant_topic_queue_stats(t, &mut s.messages, &mut s.bytes, &mut s.capacity, &mut s.dropped) });
        s
    }

    /// The name as given.
    pub fn name(&self) -> &str {
        &self.core.name
    }

    /// The schema in use, None for untyped bytes.
    pub fn schema(&self) -> Option<Schema> {
        self.core.schema()
    }

    /// Releases the handle now. The last handle on a name retires the topic. State inside
    /// one of the node's inline callbacks, and the topic then stays until the node closes.
    pub fn close(mut self) -> Result<(), SendError> {
        self.core.close()
    }
}

/// A queued or pulled subscriber's ring, read now.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[non_exhaustive]
pub struct TopicQueueStats {
    /// Messages waiting.
    pub messages: u32,
    /// Bytes the waiting messages take.
    pub bytes: u32,
    /// The ring's capacity in bytes.
    pub capacity: u32,
    /// Messages dropped since open.
    pub dropped: u32,
}

impl Subscriber<Bytes> {
    /// A `reflect_from_mesh` subscriber: re types in place when the mesh moved. True = re typed.
    pub fn refresh(&self) -> Result<bool, SendError> {
        self.core.refresh()
    }
}

/// One delivered message as a handler sees it: the value, through `Deref`, plus who sent
/// it when. For [`Bytes`] the value is the [`MessageView`] itself.
pub struct Received<'a, T: Payload> {
    value: T::Value<'a>,
    view: MessageView<'a>,
}

impl<'a, T: Payload> Received<'a, T> {
    pub(crate) fn adapt(mut handler: impl for<'b> FnMut(Received<'b, T>) + Send + 'static, codec: Option<Arc<Codec>>, name: &str) -> Box<MsgFn> {
        let name = name.to_owned();
        Box::new(move |inner: &NodeInner, view: MessageView<'_>| match T::__value(codec.as_deref(), &view) {
            Ok(value) => handler(Received { value, view }),
            Err(why) => inner.report_decode(&name, &why),
        })
    }

    /// The value itself.
    pub fn into_inner(self) -> T::Value<'a> {
        self.value
    }

    /// The message: its envelope and its fields by name.
    pub fn view(&self) -> &MessageView<'a> {
        &self.view
    }

    /// The sending node's name.
    pub fn publisher_name(&self) -> &'a str {
        self.view.publisher_name()
    }

    /// The sending node's peer id.
    pub fn publisher_id(&self) -> u32 {
        self.view.publisher_id()
    }

    /// This node's monotonic microseconds at arrival.
    pub fn recv_us(&self) -> u64 {
        self.view.recv_us()
    }

    /// The writer's wall clock in UTC microseconds, 0 when it opted out.
    pub fn written_us(&self) -> u64 {
        self.view.written_us()
    }

    /// When the data was true, 0 when the publisher gave none.
    pub fn capture_us(&self) -> u64 {
        self.view.capture_us()
    }
}

impl<'a, T: Payload> Deref for Received<'a, T> {
    type Target = T::Value<'a>;
    fn deref(&self) -> &T::Value<'a> {
        &self.value
    }
}
