use std::any::{Any, TypeId};
use std::collections::HashMap;
use std::ffi::{CStr, CString, c_void};
use std::fmt;
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr, SocketAddrV4};
use std::os::raw::c_char;
use std::ptr::{null, null_mut};
use std::sync::atomic::{AtomicPtr, AtomicU64, Ordering::SeqCst};
use std::sync::{Arc, Mutex, PoisonError};
use std::time::{Duration, Instant};

use crate::callback::{Callback, Refused, Registry, in_callback_of};
use crate::error::{Error, ErrorKind, SendError, event_text};
use crate::ffi;
use crate::gate::{Gate, Pass};
use crate::message::{Codec, Payload};
use crate::options::{Qos, options, to_ms, to_us};
use crate::schema::{MessageView, Schema};
use crate::topic::{Publisher, Received, SubHandler, Subscriber, TopicCore, TopicRec};

/// Who runs the node loop and where callbacks fire, chosen at open.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Default)]
pub enum Threading {
    /// The node's own thread runs the loop from open, and callbacks fire on it.
    #[default]
    ServiceThread,
    /// Your thread calls [`Node::poll`], and callbacks fire inside it.
    Manual,
    /// The service thread runs the loop, and callbacks wait for [`Node::dispatch`].
    Dispatch,
}

options! {
    /// A node's options: `NodeOptions::new().domain(7)`. Every option left out takes its
    /// default, and docs/node.md and docs/discovery.md explain each.
    pub struct NodeOptions {
        /// Who runs the loop and where callbacks fire.
        threading: Threading = Threading::ServiceThread,
        /// Nodes see only their own domain. 0 takes RANT_DOMAIN, else 0.
        domain: u16 = 0,
        /// Topic slots. A function takes 2 and a task 3. 0 = 8.
        max_topics: u16 = 0,
        /// Same host peers use UDP too, never shared memory.
        disable_shm: bool = false,
        /// Fetch and cache every topic every peer advertises, for observer tools.
        fetch_details: bool = false,
        /// No `@rant/log` topics.
        disable_logs: bool = false,
        /// No built in `@rant/meta` function.
        disable_meta: bool = false,
        /// Do not mirror this node's errors onto `@rant/log/error`.
        disable_error_logs: bool = false,
        /// The unicast data port. 0 = chosen by the OS.
        data_port: u16 = 0,
        /// The discovery port. 0 = 7400.
        discovery_port: u16 = 0,
        /// Hops an announce may travel. 0 = 1.
        multicast_ttl: u8 = 0,
        /// No group join: seeds and relays only, also behind an outbound only NAT.
        unicast_only: bool = false,
        /// Advertise this data port instead of the bound one.
        advertise_port: u16 = 0,
        /// UDP payload bytes per fragment. 0 = 1350.
        fragment_size: u16 = 0,
        /// The data socket's receive buffer. 0 = the OS default, too small for big messages.
        recv_buffer_bytes: u32 = 0,
        /// The data socket's send buffer. 0 = the OS default.
        send_buffer_bytes: u32 = 0,
        /// How often the node announces itself. Zero = 3 s.
        announce_interval: Duration = Duration::ZERO,
        /// Drop a peer after this silence. Zero = 4 announce intervals.
        peer_timeout: Duration = Duration::ZERO,
        /// Peers the table holds. 0 = 16.
        max_peers: u16 = 0,
    }
    hand {
        prefix: Option<String> = None,
        discovery_group: Option<String> = None,
        multicast_interface: Option<String> = None,
        self_ip: Option<String> = None,
        seed_peers: Vec<String> = Vec::new(),
        match_wait: Option<Duration> = None,
    }
}

impl NodeOptions {
    /// Goes with '/' in front of every name this node creates, inside RANT_PREFIX.
    pub fn prefix(mut self, prefix: impl Into<String>) -> Self {
        self.prefix = Some(prefix.into());
        self
    }

    /// The discovery multicast group. The default is 239.255.0.7.
    pub fn discovery_group(mut self, group: impl Into<String>) -> Self {
        self.discovery_group = Some(group.into());
        self
    }

    /// Pin discovery to the interface with this IP. "127.0.0.1" keeps the node on one host.
    pub fn multicast_interface(mut self, ip: impl Into<String>) -> Self {
        self.multicast_interface = Some(ip.into());
        self
    }

    /// Advertise this IPv4 address to every peer, for a static mapping such as a cloud IP.
    pub fn self_ip(mut self, ip: impl Into<String>) -> Self {
        self.self_ip = Some(ip.into());
        self
    }

    /// Announce by unicast to these peers too, each "ip" or "ip:port".
    pub fn seed_peers<S: Into<String>>(mut self, peers: impl IntoIterator<Item = S>) -> Self {
        self.seed_peers = peers.into_iter().map(Into::into).collect();
        self
    }

    /// How long a send that would reach nobody waits for a match still resolving. Zero
    /// never waits and raises an UnmatchedSend error instead. The default is 1 s.
    pub fn match_wait(mut self, wait: Duration) -> Self {
        self.match_wait = Some(wait);
        self
    }
}

/// What an [`Event`] reports. Every failure arrives as `Error` with [`Event::error`] set.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub enum EventKind {
    /// A peer was discovered or came back.
    PeerUp,
    /// A peer went silent or said bye.
    PeerDown,
    /// What we publish to or receive from a peer changed.
    PeerInterest,
    /// A best effort gap: `lost_first` and `lost_count` say which. Not an error.
    MessageLost,
    /// A fault, named by `error`.
    Error,
    /// A kind this binding does not know yet.
    Unknown,
}

/// A peer, loss or error notification. Read the fields its kind names, the rest are empty.
#[derive(Clone, Debug)]
#[non_exhaustive]
pub struct Event {
    /// What happened.
    pub kind: EventKind,
    /// The fault of an Error event, `ErrorKind::None` otherwise.
    pub error: ErrorKind,
    /// The peer id of a peer scoped event, 0 otherwise.
    pub peer: u32,
    /// The peer's node name. Prefer it over `peer` in messages.
    pub peer_name: Option<String>,
    /// The peer's address, for PeerUp and PeerRefused.
    pub address: Option<SocketAddr>,
    /// The local topic of a topic scoped event.
    pub topic: u16,
    /// Our name of the topic of a topic scoped event.
    pub topic_name: Option<String>,
    /// The OS error code of a socket fault.
    pub os_error: i32,
    /// MessageLost and EvictedUnsent: the first sequence number.
    pub lost_first: u64,
    /// MessageLost and EvictedUnsent: how many.
    pub lost_count: u64,
    /// MessageTooBig, PeerMetaTooBig, Oom and Send: the byte count.
    pub too_big_bytes: u64,
    /// NameCollision: the colliding identity.
    pub identity: u64,
    /// PeerInterest: the topics we now publish to the peer.
    pub publish_topics: u16,
    /// PeerInterest: the topics we now receive from the peer.
    pub receive_topics: u16,
    /// SchemaMismatch: what was incompatible. For a handler that panicked: its text.
    pub detail: Option<String>,
    text: String,
}

impl Event {
    /// True for an Error event.
    pub fn is_error(&self) -> bool {
        self.kind == EventKind::Error
    }

    fn from_c(ev: &ffi::RantEvent) -> Event {
        // SAFETY: the C strings of an event are valid NUL terminated text for its callback
        let text = |p: *const c_char| (!p.is_null()).then(|| unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned());
        Event {
            kind: match ev.kind {
                ffi::RANT_PEER_UP => EventKind::PeerUp,
                ffi::RANT_PEER_DOWN => EventKind::PeerDown,
                ffi::RANT_PEER_INTEREST => EventKind::PeerInterest,
                ffi::RANT_MSG_LOST => EventKind::MessageLost,
                ffi::RANT_ERROR => EventKind::Error,
                _ => EventKind::Unknown,
            },
            error: ErrorKind::from_c(ev.error),
            peer: ev.peer,
            peer_name: text(ev.peer_name),
            address: address_of(&ev.ip, ev.ip_len, ev.port),
            topic: ev.topic,
            topic_name: text(ev.topic_name),
            os_error: ev.os_error,
            lost_first: ev.lost_first,
            lost_count: ev.lost_count,
            too_big_bytes: ev.too_big_bytes,
            identity: ev.identity,
            publish_topics: ev.publish_topics,
            receive_topics: ev.receive_topics,
            detail: text(ev.schema_detail),
            text: event_text(ev),
        }
    }

    /// An Error event this binding raises, formatted by the C like any other.
    fn binding_error(error: ErrorKind, topic_name: Option<&str>, detail: Option<&str>) -> Event {
        let topic_c = topic_name.and_then(|s| CString::new(s).ok());
        let detail_c = detail.and_then(|s| CString::new(s).ok());
        let ev = ffi::RantEvent {
            kind: ffi::RANT_ERROR,
            error: error.to_c(),
            topic_name: topic_c.as_ref().map_or(null(), |c| c.as_ptr()),
            schema_detail: detail_c.as_ref().map_or(null(), |c| c.as_ptr()),
            ..Default::default()
        };
        Event::from_c(&ev)
    }
}

impl fmt::Display for Event {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.text)
    }
}

fn address_of(ip: &[u8; 16], len: u8, port: u16) -> Option<SocketAddr> {
    let ip = match len {
        4 => IpAddr::V4(Ipv4Addr::new(ip[0], ip[1], ip[2], ip[3])),
        16 => IpAddr::V6(Ipv6Addr::from(*ip)),
        _ => return None,
    };
    Some(SocketAddr::new(ip, port))
}

/// A node's counters, read now.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[non_exhaustive]
pub struct NodeStats {
    /// Bytes the node holds now.
    pub memory_in_use: usize,
    /// The most it ever held.
    pub memory_peak: usize,
    /// Heap allocations since open. A flat count proves the hot path allocates nothing.
    pub alloc_calls: u64,
    /// Time reliable sends spent waiting on slow subscribers.
    pub backpressure_waited: Duration,
    /// Sends that waited.
    pub backpressure_waited_sends: u32,
    /// Sends that evicted history nobody had received yet, a sign of bursts.
    pub evicted_unsent: u32,
}

/// A queue's counters, read now.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[non_exhaustive]
pub struct QueueStats {
    /// Callbacks parked now.
    pub waiting: u32,
    /// Best effort callbacks overwritten since open.
    pub dropped: u32,
}

/// A callback queue for handles that need a thread of their own: a handle whose options name
/// it parks its callbacks until [`Queue::dispatch`] runs them on the calling thread.
#[derive(Clone)]
pub struct Queue {
    pub(crate) inner: Arc<NodeInner>,
    pub(crate) raw: *mut ffi::RantQueue,
}

// SAFETY: every C call on a queue is thread safe under the node lock, and the gate keeps it alive
unsafe impl Send for Queue {}
unsafe impl Sync for Queue {}

impl Queue {
    /// Runs up to `max_callbacks` parked callbacks (0 = all), oldest first, waiting up to `timeout`
    /// for the first. The count run. State while another thread dispatches this queue.
    pub fn dispatch(&self, max_callbacks: usize, timeout: Duration) -> Result<usize, SendError> {
        let max = i32::try_from(max_callbacks).unwrap_or(i32::MAX);
        let raw = self.raw;
        // SAFETY: the queue lives while the node does, and the wait holds a pass
        let run = self.inner.wait_sliced(timeout, |_, ms| unsafe { ffi::rant_queue_dispatch(raw, max, ms) })?;
        Ok(run as usize)
    }

    /// The queue's counters.
    pub fn stats(&self) -> QueueStats {
        let mut s = QueueStats::default();
        if let Some(_pass) = self.inner.gate.enter() {
            // SAFETY: the pass keeps the queue alive
            unsafe { ffi::rant_queue_stats(self.raw, &mut s.waiting, &mut s.dropped) };
        }
        s
    }
}

impl fmt::Debug for Queue {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Queue({:p})", self.raw)
    }
}

type EventFn = dyn FnMut(&Event) + Send;

/// The strings and seeds the C node keeps pointing at, owned for its life.
struct Net {
    prefix: Option<CString>,
    group: Option<CString>,
    interface: Option<CString>,
    self_ip: Option<CString>,
    seeds: Vec<ffi::RantAddr>,
}

impl Net {
    fn new(o: &NodeOptions) -> Result<Net, Error> {
        let c = |s: &Option<String>, kind: ErrorKind, what: &str| -> Result<Option<CString>, Error> {
            s.as_deref()
                .map(|s| CString::new(s).map_err(|_| Error::new(kind, format!("rant::Node::new: the {what} holds a NUL"))))
                .transpose()
        };
        let mut seeds = Vec::with_capacity(o.seed_peers.len());
        for s in &o.seed_peers {
            let addr = s.parse::<SocketAddrV4>().or_else(|_| s.parse::<Ipv4Addr>().map(|ip| SocketAddrV4::new(ip, 0)));
            let addr = addr.map_err(|_| {
                Error::new(ErrorKind::BadAddress, format!("rant::Node::new: the seed peer \"{s}\" is not ip or ip:port"))
            })?;
            let mut a = ffi::RantAddr::default();
            a.ip[..4].copy_from_slice(&addr.ip().octets());
            a.ip_len = 4;
            a.port = addr.port();
            seeds.push(a);
        }
        Ok(Net {
            prefix: c(&o.prefix, ErrorKind::BadPrefix, "prefix")?,
            group: c(&o.discovery_group, ErrorKind::BadAddress, "discovery group")?,
            interface: c(&o.multicast_interface, ErrorKind::BadAddress, "multicast interface")?,
            self_ip: c(&o.self_ip, ErrorKind::BadAddress, "self ip")?,
            seeds,
        })
    }
}

fn c_ptr(s: &Option<CString>) -> *const c_char {
    s.as_ref().map_or(null(), |c| c.as_ptr())
}

/// The state behind a [`Node`], shared by every handle. The C node's user pointer points
/// here, so it lives at a fixed address inside its Arc until the C node is closed.
pub(crate) struct NodeInner {
    raw: AtomicPtr<ffi::RantNode>,
    pub(crate) gate: Gate,
    threading: Threading,
    queue: AtomicPtr<ffi::RantQueue>,
    net: Net,
    on_event: Mutex<Option<Arc<Callback<EventFn>>>>,
    /// name to shared topic slot, also the lock that serializes creates and closes
    pub(crate) topics: Registry<HashMap<String, TopicRec>>,
    /// subscriber handlers per topic index, read by the message trampoline
    pub(crate) handlers: Mutex<HashMap<u16, Arc<[SubHandler]>>>,
    next_id: AtomicU64,
    /// the typed codecs, one per type
    pub(crate) codecs: Registry<HashMap<TypeId, Arc<Codec>>>,
    /// the pattern handler boxes the C points at, by id, freed at retire or close
    kept: Mutex<HashMap<u64, Box<dyn Any + Send + Sync>>>,
}

// SAFETY: the C node is thread safe under its own lock, and the gate orders close after every call
unsafe impl Send for NodeInner {}
unsafe impl Sync for NodeInner {}

/// How long one slice of an unbounded wait lasts, which bounds how long close waits for it.
const WAIT_SLICE: Duration = Duration::from_millis(50);

pub(crate) fn lock<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(PoisonError::into_inner)
}

impl NodeInner {
    /// The C node. Only meaningful while the caller holds a pass.
    pub(crate) fn raw(&self) -> *mut ffi::RantNode {
        self.raw.load(SeqCst)
    }

    /// The key this node's callbacks are marked with.
    pub(crate) fn key(&self) -> usize {
        self as *const NodeInner as usize
    }

    /// A pass and the C node, or NoTopic once the node closed.
    pub(crate) fn enter(&self) -> Result<(Pass<'_>, *mut ffi::RantNode), SendError> {
        let pass = self.gate.enter().ok_or(SendError::NoTopic)?;
        Ok((pass, self.raw()))
    }

    /// The same for a creation, as an Error naming the call.
    pub(crate) fn enter_for(&self, what: &str) -> Result<(Pass<'_>, *mut ffi::RantNode), Error> {
        self.enter().map_err(|_| Error::new(ErrorKind::State, format!("{what}: the node is closed")))
    }

    /// The node queue under Dispatch threading, else null.
    pub(crate) fn node_queue(&self) -> *mut ffi::RantQueue {
        self.queue.load(SeqCst)
    }

    pub(crate) fn next_id(&self) -> u64 {
        self.next_id.fetch_add(1, SeqCst)
    }

    /// Holds `value` until [`NodeInner::release`] or close, for a box the C points at. Its id.
    pub(crate) fn keep(&self, value: Box<dyn Any + Send + Sync>) -> u64 {
        let id = self.next_id();
        lock(&self.kept).insert(id, value);
        id
    }

    /// Frees a kept box, outside the lock since its drop may run app code.
    pub(crate) fn release(&self, id: u64) {
        let value = lock(&self.kept).remove(&id);
        drop(value);
    }

    /// An owned Arc of this node state. Safety: `self` lives inside an Arc, as every
    /// NodeInner does.
    pub(crate) unsafe fn arc(&self) -> Arc<NodeInner> {
        // SAFETY: the caller guarantees self came from Arc::new
        unsafe {
            Arc::increment_strong_count(self as *const NodeInner);
            Arc::from_raw(self as *const NodeInner)
        }
    }

    /// Runs `step` with C waits of at most one slice until it answers nonzero or `timeout`
    /// passes, so close never waits behind a wait with no end. 0 = timed out.
    pub(crate) fn wait_sliced(&self, timeout: Duration, mut step: impl FnMut(*mut ffi::RantNode, i32) -> i32) -> Result<i32, SendError> {
        let deadline = if timeout == Duration::MAX { None } else { Instant::now().checked_add(timeout) };
        loop {
            let (pass, raw) = self.enter()?;
            let left = deadline.map_or(WAIT_SLICE, |d| d.saturating_duration_since(Instant::now()));
            let rc = step(raw, to_ms(left.min(WAIT_SLICE)));
            drop(pass);
            if rc != 0 {
                return SendError::check(rc);
            }
            if deadline.is_some_and(|d| Instant::now() >= d) {
                return Ok(0);
            }
        }
    }

    /// The app's event handler, else the default that prints errors to stderr.
    pub(crate) fn emit(&self, ev: &Event) {
        let handler = lock(&self.on_event).clone();
        let Some(handler) = handler else {
            if ev.is_error() {
                eprintln!("rant: {ev}");
            }
            return;
        };
        match handler.run(self.key(), |f| f(ev)) {
            Ok(()) => {}
            Err(Refused::Reentered) => eprintln!("rant: {ev}"),
            Err(Refused::Panicked(text)) => eprintln!("rant: the event handler panicked: {text}"),
        }
    }

    /// A handler that panicked or was re entered: an Error event, never an unwind into C.
    pub(crate) fn report_refused(&self, topic_name: Option<&str>, refused: Refused) {
        match refused {
            Refused::Reentered => {
                eprintln!("rant: a handler was called from inside itself, the inner call was skipped");
                self.emit(&Event::binding_error(ErrorKind::State, topic_name, Some("a handler was called from inside itself")));
            }
            Refused::Panicked(text) => self.emit(&Event::binding_error(ErrorKind::None, topic_name, Some(&text))),
        }
    }

    /// A payload that did not decode into the handle's type: an Error event, never a silent drop.
    pub(crate) fn report_decode(&self, topic_name: &str, why: &str) {
        self.emit(&Event::binding_error(ErrorKind::SchemaMismatch, Some(topic_name), Some(why)));
    }

    /// A handle dropped inside its own callback: its entity stays until the node closes.
    pub(crate) fn report_close_refused(&self, topic_name: &str) {
        self.emit(&Event::binding_error(ErrorKind::State, Some(topic_name), Some("a handle cannot close inside its own callback")));
    }

    /// Closes the C node once. Refused from inside one of its callbacks.
    fn close(self: &Arc<Self>) -> Result<(), Error> {
        if in_callback_of(self.key()) {
            return Err(Error::new(ErrorKind::State, "rant::Node::close: a node cannot close inside its own callback"));
        }
        if !self.gate.close() {
            return Ok(());
        }
        let raw = self.raw.swap(null_mut(), SeqCst);
        if raw.is_null() {
            return Ok(());
        }
        // SAFETY: the gate holds every other call out, and no callback of ours runs here
        let rc = unsafe { ffi::rant_node_close(raw, 1) };
        if rc != 0 {
            // the C node lives on and points here, so this state must never be freed
            std::mem::forget(Arc::clone(self));
            return Err(Error::new(ErrorKind::State, format!("rant::Node::close: the C refused ({rc}), the node is leaked")));
        }
        self.queue.store(null_mut(), SeqCst);
        // a handler holding a handle of this node is a cycle through this state, so the
        // handlers go now, dropped outside their locks
        let handlers = std::mem::take(&mut *lock(&self.handlers));
        let kept = std::mem::take(&mut *lock(&self.kept));
        let on_event = lock(&self.on_event).take();
        drop((handlers, kept, on_event));
        Ok(())
    }
}

impl Drop for NodeInner {
    fn drop(&mut self) {
        // reached only with a node already closed, or never opened, since Node closes first
        debug_assert!(self.raw.load(SeqCst).is_null(), "rant: node state freed while the C node is open");
    }
}

unsafe extern "C" fn on_message(msg: *const ffi::RantMsg) {
    // SAFETY: the C hands a message valid for this call, and user is our NodeInner
    let msg = unsafe { &*msg };
    let inner = unsafe { &*(msg.user as *const NodeInner) };
    let list = lock(&inner.handlers).get(&msg.topic_index).cloned();
    let Some(list) = list else { return };
    for h in list.iter() {
        // SAFETY: the message is valid for the whole trampoline
        let view = unsafe { MessageView::delivered(inner, msg) };
        if let Err(refused) = h.cb.run(inner.key(), |f| f(inner, view)) {
            inner.report_refused(Some(&h.name), refused);
        }
    }
}

unsafe extern "C" fn on_event(ev: *const ffi::RantEvent) {
    // SAFETY: the C hands an event valid for this call, and user is our NodeInner
    let ev = unsafe { &*ev };
    if ev.user.is_null() {
        return;
    }
    let inner = unsafe { &*(ev.user as *const NodeInner) };
    inner.emit(&Event::from_c(ev));
}

/// One participant on the mesh. Its factories make every handle, it runs the loop as
/// [`NodeOptions`] says, and it closes when dropped. Every method is thread safe.
pub struct Node {
    pub(crate) inner: Arc<NodeInner>,
}

impl Node {
    /// Opens a node and joins the mesh. An empty name is generated. The service thread runs
    /// from here unless the threading is Manual.
    pub fn new(name: &str, options: NodeOptions) -> Result<Node, Error> {
        let o = options;
        let cname = if name.is_empty() {
            None
        } else {
            Some(CString::new(name).map_err(|_| Error::new(ErrorKind::BadName, "rant::Node::new: the name holds a NUL"))?)
        };
        let inner = Arc::new(NodeInner {
            raw: AtomicPtr::new(null_mut()),
            gate: Gate::new(),
            threading: o.threading,
            queue: AtomicPtr::new(null_mut()),
            net: Net::new(&o)?,
            on_event: Mutex::new(None),
            topics: Registry::new(HashMap::new()),
            handlers: Mutex::new(HashMap::new()),
            next_id: AtomicU64::new(1),
            codecs: Registry::new(HashMap::new()),
            kept: Mutex::new(HashMap::new()),
        });

        let co = ffi::RantNodeOpts {
            domain: o.domain,
            max_topics: o.max_topics,
            user_data: Arc::as_ptr(&inner) as *mut c_void,
            disable_shm: o.disable_shm as u8,
            fetch_details: o.fetch_details as u8,
            match_wait_ms: match o.match_wait {
                None => 0,
                Some(d) if d.is_zero() => -1,
                Some(d) => to_ms(d),
            },
            disable_logs: o.disable_logs as u8,
            disable_meta: o.disable_meta as u8,
            disable_error_logs: o.disable_error_logs as u8,
            prefix: c_ptr(&inner.net.prefix),
            net: ffi::RantNodeNet {
                data_port: o.data_port,
                discovery_group: c_ptr(&inner.net.group),
                discovery_port: o.discovery_port,
                multicast_interface: c_ptr(&inner.net.interface),
                multicast_ttl: o.multicast_ttl,
                seed_peers: if inner.net.seeds.is_empty() { null() } else { inner.net.seeds.as_ptr() },
                n_seed_peers: inner.net.seeds.len() as u16,
                unicast_only: o.unicast_only as u8,
                recv_buffer_bytes: o.recv_buffer_bytes,
                send_buffer_bytes: o.send_buffer_bytes,
                fragment_size: o.fragment_size,
                self_ip: c_ptr(&inner.net.self_ip),
                advertise_port: o.advertise_port,
            },
            discovery: ffi::RantNodeDiscovery {
                announce_interval_us: to_us(o.announce_interval),
                peer_timeout_us: to_us(o.peer_timeout),
                max_peers: o.max_peers,
            },
            ..Default::default()
        };

        // SAFETY: open copies the allocator and the options, and keeps only the net strings,
        // which live in inner for the node's life
        let raw = unsafe {
            let mut alloc = ffi::rant_allocator_heap(0);
            ffi::rant_node_open(&mut alloc, cname.as_ref().map_or(null(), |c| c.as_ptr()), Some(on_message), Some(on_event), &co)
        };
        if raw.is_null() {
            return Err(Error::last(null_mut(), "rant::Node::new"));
        }
        inner.raw.store(raw, SeqCst);
        // from here a failure returns through Drop, which closes the C node
        let node = Node { inner };
        if o.threading == Threading::Dispatch {
            // SAFETY: the node is open and no other thread knows it yet
            let q = unsafe { ffi::rant_node_create_queue(raw) };
            if q.is_null() || unsafe { ffi::rant_node_set_event_queue(raw, q) } != 0 {
                return Err(Error::last(raw, "rant::Node::new: the dispatch queue"));
            }
            node.inner.queue.store(q, SeqCst);
        }
        // SAFETY: as above
        if o.threading != Threading::Manual && unsafe { ffi::rant_node_start(raw) } != 0 {
            return Err(Error::last(raw, "rant::Node::new: the service thread"));
        }
        Ok(node)
    }

    /// Leaves the mesh with a bye and frees the node, after which every handle answers NoTopic.
    /// Inside one of its own callbacks the close runs on another thread once the callback returns.
    pub fn close(self) -> Result<(), Error> {
        if in_callback_of(self.inner.key()) {
            drop(self);
            return Ok(());
        }
        self.inner.close()
    }

    /// Who runs the loop, as opened.
    pub fn threading(&self) -> Threading {
        self.inner.threading
    }

    /// Manual threading: one loop tick of discovery, receive, timers and queued sends,
    /// where callbacks fire. Waits up to `timeout` for work. State on another threading.
    pub fn poll(&self, timeout: Duration) -> Result<(), SendError> {
        let (_pass, raw) = self.inner.enter()?;
        // SAFETY: the pass keeps the node open
        SendError::check(unsafe { ffi::rant_node_poll(raw, to_ms(timeout)) }).map(|_| ())
    }

    /// Dispatch threading: runs up to `max_callbacks` parked callbacks (0 = all) on this thread,
    /// waiting up to `timeout` for the first. The count run. State on another threading.
    pub fn dispatch(&self, max_callbacks: usize, timeout: Duration) -> Result<usize, SendError> {
        let q = self.inner.node_queue();
        if q.is_null() {
            return Err(SendError::State);
        }
        Queue { inner: Arc::clone(&self.inner), raw: q }.dispatch(max_callbacks, timeout)
    }

    /// Peer, loss and error events, where the node's callbacks run. With none set, errors
    /// print to stderr. A later call replaces the handler.
    pub fn on_event(&self, handler: impl FnMut(&Event) + Send + 'static) {
        *lock(&self.inner.on_event) = Some(Arc::new(Callback::new(Box::new(handler))));
    }

    /// Blocks until discovery and matching settle, so a send now reaches everyone on the
    /// network. Call it after creating the handles. None = 3 announce intervals.
    pub fn settle(&self, timeout: Option<Duration>) -> bool {
        let Ok((_pass, raw)) = self.inner.enter() else { return false };
        // SAFETY: the pass keeps the node open
        unsafe { ffi::rant_node_settle(raw, timeout.map_or(-1, to_ms)) == 1 }
    }

    /// The node's counters.
    pub fn stats(&self) -> NodeStats {
        let mut s = NodeStats::default();
        let Ok((_pass, raw)) = self.inner.enter() else { return s };
        let mut waited_us = 0u64;
        // SAFETY: the pass keeps the node open
        unsafe {
            ffi::rant_node_mem_stats(raw, &mut s.memory_in_use, &mut s.memory_peak, &mut s.alloc_calls);
            ffi::rant_node_backpressure_stats(raw, &mut waited_us, &mut s.backpressure_waited_sends);
            s.evicted_unsent = ffi::rant_node_evicted_unsent(raw);
        }
        s.backpressure_waited = Duration::from_micros(waited_us);
        s
    }

    /// The most recent error this node reported, whether or not a handler saw it. None
    /// while there is none.
    pub fn last_error(&self) -> Option<Event> {
        let Ok((_pass, raw)) = self.inner.enter() else { return None };
        // SAFETY: the pass keeps the node open, and the event's strings are copied at once
        let ev = unsafe { ffi::rant_last_error(raw) };
        (ev.error != ffi::RANT_E_NONE).then(|| Event::from_c(&ev))
    }

    /// Compiles schema text in this node: every definition stays in scope for later
    /// compiles, and a name alone is a schema. The node owns the result for its life.
    pub fn schema(&self, text: &str) -> Result<Schema, Error> {
        const WHAT: &str = "rant::Node::schema";
        let ctext = CString::new(text).map_err(|_| Error::new(ErrorKind::BadSchema, format!("{WHAT}: the text holds a NUL")))?;
        let (_pass, raw) = self.inner.enter_for(WHAT)?;
        // SAFETY: the pass keeps the node open
        let s = unsafe { ffi::rant_node_schema(raw, ctext.as_ptr()) };
        if s.is_null() {
            return Err(Error::last(raw, WHAT));
        }
        Ok(Schema::new(Arc::clone(&self.inner), s))
    }

    /// The same from a schema's wire bytes, such as a peer's.
    pub fn schema_from_wire(&self, wire: &[u8]) -> Result<Schema, Error> {
        const WHAT: &str = "rant::Node::schema_from_wire";
        let (_pass, raw) = self.inner.enter_for(WHAT)?;
        // SAFETY: the pass keeps the node open, and the C bounds checks the bytes
        let s = unsafe { ffi::rant_node_schema_parse(raw, wire.as_ptr().cast(), wire.len()) };
        if s.is_null() {
            return Err(Error::last(raw, WHAT));
        }
        Ok(Schema::new(Arc::clone(&self.inner), s))
    }

    /// A callback queue of this node, for handles that want their callbacks on a thread of
    /// their own: name it in their options. At most 8 per node.
    pub fn create_queue(&self) -> Result<Queue, Error> {
        const WHAT: &str = "rant::Node::create_queue";
        let (_pass, raw) = self.inner.enter_for(WHAT)?;
        // SAFETY: the pass keeps the node open
        let q = unsafe { ffi::rant_node_create_queue(raw) };
        if q.is_null() {
            return Err(Error::last(raw, WHAT));
        }
        Ok(Queue { inner: Arc::clone(&self.inner), raw: q })
    }

    /// The publishing side of a topic. `T` is a [`Message`](crate::Message) type, or
    /// [`Bytes`](crate::Bytes) for raw bytes, which alone takes a schema in its options.
    pub fn publisher<T: Payload>(&self, name: &str, qos: Qos) -> Result<Publisher<T>, Error> {
        let codec = T::__codec(&self.inner, qos.schema.as_ref(), qos.reflect_from_mesh, "rant::Node::publisher")?;
        let core = TopicCore::open(&self.inner, name, TopicCore::PUB, codec.as_deref(), &qos, false)?;
        Ok(Publisher::new(core, codec))
    }

    /// The subscribing side of a topic: `handler` gets every message where the node's
    /// callbacks run, or at dispatch of the queue in the options.
    pub fn subscriber<T: Payload>(
        &self,
        name: &str,
        qos: Qos,
        handler: impl for<'a> FnMut(Received<'a, T>) + Send + 'static,
    ) -> Result<Subscriber<T>, Error> {
        let codec = T::__codec(&self.inner, qos.schema.as_ref(), qos.reflect_from_mesh, "rant::Node::subscriber")?;
        let mut core = TopicCore::open(&self.inner, name, TopicCore::SUB, codec.as_deref(), &qos, false)?;
        core.add_handler(Received::<T>::adapt(handler, codec.clone(), name));
        Ok(Subscriber::new(core, codec))
    }

    /// A pulled subscriber: no handler, messages wait until [`Subscriber::take`] reads them.
    /// A name has one pulled subscriber per node.
    pub fn pulled_subscriber<T: Payload>(&self, name: &str, qos: Qos) -> Result<Subscriber<T>, Error> {
        let codec = T::__codec(&self.inner, qos.schema.as_ref(), qos.reflect_from_mesh, "rant::Node::pulled_subscriber")?;
        let core = TopicCore::open(&self.inner, name, TopicCore::SUB, codec.as_deref(), &qos, true)?;
        Ok(Subscriber::new(core, codec))
    }
}

impl Drop for Node {
    fn drop(&mut self) {
        if in_callback_of(self.inner.key()) {
            // the C refuses to close here, so close on another thread once the callback returns
            let inner = Arc::clone(&self.inner);
            std::thread::spawn(move || {
                if let Err(e) = inner.close() {
                    eprintln!("rant: {e}");
                }
            });
            return;
        }
        if let Err(e) = self.inner.close() {
            eprintln!("rant: {e}");
        }
    }
}

impl fmt::Debug for Node {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Node").field("threading", &self.inner.threading).field("closed", &self.inner.gate.is_closing()).finish()
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn every_c_event_kind_has_a_variant() {
        for (name, v) in crate::c_enum_values("RantEventKind") {
            let ev = crate::ffi::RantEvent { kind: v as crate::ffi::RantEventKind, ..Default::default() };
            assert_ne!(super::Event::from_c(&ev).kind, super::EventKind::Unknown, "{name}");
        }
    }
}
