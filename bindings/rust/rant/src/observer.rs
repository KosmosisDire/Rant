use std::ffi::c_void;
use std::fmt;
use std::ptr::null;
use std::sync::Arc;

use crate::callback::Callback;
use crate::error::{Error, SendError};
use crate::ffi;
use crate::function::c_name;
use crate::node::{Node, NodeInner, Queue};
use crate::pattern::{CHandle, CallStatus, Handle, queue_for, same_node};
use crate::schema::{FieldView, Schema, rbytes, rstring_bytes, utf8_prefix};

/// A step of a call, as a call observer sees it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CallEventKind {
    /// A request arrived, the data is the request.
    Call,
    /// A task started.
    Running,
    /// A task reported, the data is the update.
    Progress,
    /// A cancel arrived, the message names the node that asked.
    Cancel,
    /// The definition answered: a status, a message, and the data is the response.
    End,
}

impl CallEventKind {
    fn from_c(k: ffi::RantCallEventKind) -> CallEventKind {
        match k {
            ffi::RANT_CALL_EVENT_CALL => CallEventKind::Call,
            ffi::RANT_CALL_EVENT_RUNNING => CallEventKind::Running,
            ffi::RANT_CALL_EVENT_PROGRESS => CallEventKind::Progress,
            ffi::RANT_CALL_EVENT_CANCEL => CallEventKind::Cancel,
            _ => CallEventKind::End,
        }
    }
}

/// One step of a call, valid for the handler. Its data reads by field name when the schema is
/// known. `caller_lo` and `call_id` together name the call.
pub struct CallEvent<'a> {
    raw: &'a ffi::RantCallEvent,
    inner: &'a NodeInner,
}

impl<'a> CallEvent<'a> {
    /// Which step this is.
    pub fn kind(&self) -> CallEventKind {
        CallEventKind::from_c(self.raw.kind)
    }

    /// The function or task.
    pub fn name(&self) -> &'a str {
        utf8_prefix(rstring_bytes(self.raw.name))
    }

    /// The peer id of the definition reporting it.
    pub fn provider(&self) -> u32 {
        self.raw.provider
    }

    /// The name of the definition's node.
    pub fn provider_name(&self) -> &'a str {
        utf8_prefix(rstring_bytes(self.raw.provider_name))
    }

    /// The caller's uuid low 32.
    pub fn caller_lo(&self) -> u32 {
        self.raw.caller_lo
    }

    /// The caller's node name, empty when the provider no longer knows it.
    pub fn caller_name(&self) -> &'a str {
        utf8_prefix(rstring_bytes(self.raw.caller_name))
    }

    /// The caller's number for this call.
    pub fn call_id(&self) -> u32 {
        self.raw.call_id
    }

    /// The outcome of an End.
    pub fn status(&self) -> CallStatus {
        CallStatus::from_c(self.raw.status)
    }

    /// An End's outcome text, or the node that asked for a Cancel.
    pub fn message(&self) -> &'a str {
        utf8_prefix(rstring_bytes(self.raw.message))
    }

    /// The payload's encoded bytes.
    pub fn data(&self) -> &'a [u8] {
        rbytes(self.raw.data)
    }

    /// The payload by field name, with the fields empty when its layout is unknown here.
    pub fn fields(&self) -> FieldView<'a> {
        FieldView::new(rbytes(self.raw.data), self.raw.schema)
    }

    /// True when the payload's layout is known here.
    pub fn has_schema(&self) -> bool {
        !self.raw.schema.is_null()
    }

    /// The payload's layout, kept by the node past the handler. None when it is unknown here.
    pub fn schema(&self) -> Option<Schema> {
        if self.raw.schema.is_null() {
            return None;
        }
        // SAFETY: the handler runs with the node open, and parse copies the view's wire
        let s = unsafe {
            let wire = rbytes(ffi::rant_schema_wire(self.raw.schema));
            ffi::rant_node_schema_parse(self.inner.raw(), wire.as_ptr().cast(), wire.len())
        };
        // SAFETY: every NodeInner lives in an Arc
        (!s.is_null()).then(|| Schema::new(unsafe { self.inner.arc() }, s))
    }

    /// The provider's wall clock at the report in UTC microseconds, 0 when it opted out.
    pub fn written_us(&self) -> u64 {
        self.raw.written_us
    }

    /// When it arrived, on this node's monotonic clock in microseconds.
    pub fn recv_us(&self) -> u64 {
        self.raw.recv_us
    }
}

/// A call observer's options: `CallObserverOptions::new().reflect_from_mesh(true)`.
#[derive(Default)]
pub struct CallObserverOptions {
    best_effort: bool,
    reflect_from_mesh: bool,
    queue: Option<Queue>,
    queue_bytes: u32,
    request_schema: Option<Schema>,
    progress_schema: Option<Schema>,
    response_schema: Option<Schema>,
}

impl CallObserverOptions {
    /// Every option at its default.
    pub fn new() -> Self {
        Self::default()
    }

    /// No repair. Never slows the provider either way.
    pub fn best_effort(mut self, value: bool) -> Self {
        self.best_effort = value;
        self
    }

    /// Takes the provider's schemas from the mesh, which needs `fetch_details` on the node.
    pub fn reflect_from_mesh(mut self, value: bool) -> Self {
        self.reflect_from_mesh = value;
        self
    }

    /// The queue the handler parks on, run at its dispatch.
    pub fn queue(mut self, queue: &Queue) -> Self {
        self.queue = Some(queue.clone());
        self
    }

    /// Its ring cap with a queue. 0 = 1 MB.
    pub fn queue_bytes(mut self, value: u32) -> Self {
        self.queue_bytes = value;
        self
    }

    /// The schema requests read with.
    pub fn request_schema(mut self, schema: &Schema) -> Self {
        self.request_schema = Some(schema.clone());
        self
    }

    /// The schema a task's progress reads with.
    pub fn progress_schema(mut self, schema: &Schema) -> Self {
        self.progress_schema = Some(schema.clone());
        self
    }

    /// The schema responses read with.
    pub fn response_schema(mut self, schema: &Schema) -> Self {
        self.response_schema = Some(schema.clone());
        self
    }
}

impl CHandle for ffi::RantCallObserver {
    unsafe fn retire(raw: *mut Self) -> i32 {
        // SAFETY: as the trait says
        unsafe { ffi::rant_call_observer_retire(raw) }
    }
}

type EventFn = dyn for<'a> FnMut(CallEvent<'a>) + Send;

struct EventBox {
    node: *const NodeInner,
    name: String,
    cb: Callback<EventFn>,
}

// SAFETY: node is read only, and the box lives inside that node's registry
unsafe impl Send for EventBox {}
unsafe impl Sync for EventBox {}

unsafe extern "C" fn on_event(e: *const ffi::RantCallEvent, user: *mut c_void) {
    // SAFETY: user is an EventBox the node keeps until the handle retires, and the event is
    // valid for this call
    let (b, e) = unsafe { (&*(user as *const EventBox), &*e) };
    let inner = unsafe { &*b.node };
    if let Err(refused) = b.cb.run(inner.key(), |f| f(CallEvent { raw: e, inner })) {
        inner.report_refused(Some(&b.name), refused);
    }
}

/// Watches a function's or task's calls, from [`Node::call_observer`]. It neither calls nor
/// provides, costs the provider nothing unwatched and never slows it. docs/patterns.md has
/// the limits.
pub struct CallObserver {
    handle: Arc<Handle<ffi::RantCallObserver>>,
    open: bool,
}

impl CallObserver {
    /// The name as given.
    pub fn name(&self) -> &str {
        &self.handle.name
    }

    /// The definitions matched now.
    pub fn match_count(&self) -> usize {
        // SAFETY: the pass keeps the handle alive
        self.handle.enter().map_or(0, |p| unsafe { ffi::rant_call_observer_match_count(p.raw) }.max(0) as usize)
    }

    /// Stops watching now, freeing the name. State inside one of the node's inline callbacks,
    /// and the handle then stays until the node closes.
    pub fn close(mut self) -> Result<(), SendError> {
        self.open = false;
        self.handle.close()
    }
}

impl Drop for CallObserver {
    fn drop(&mut self) {
        if self.open && self.handle.close() == Err(SendError::State) {
            self.handle.inner.report_close_refused(&self.handle.name);
        }
    }
}

impl fmt::Debug for CallObserver {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "CallObserver({:?})", self.handle.name)
    }
}

impl Node {
    /// Watches the calls of a function or task by name, on every node that defines it. A node
    /// may not define and observe one name.
    pub fn call_observer(
        &self,
        name: &str,
        options: CallObserverOptions,
        handler: impl for<'a> FnMut(CallEvent<'a>) + Send + 'static,
    ) -> Result<CallObserver, Error> {
        let what = format!("rant::Node::call_observer \"{name}\"");
        let o = &options;
        same_node(
            &self.inner,
            o.queue.as_ref(),
            &[o.request_schema.as_ref(), o.progress_schema.as_ref(), o.response_schema.as_ref()],
            &what,
        )?;
        let schema = |s: &Option<Schema>| s.as_ref().map_or(null(), |s| s.raw as *const ffi::RantSchema);
        let co = ffi::RantCallObserverOpts {
            best_effort: o.best_effort as u8,
            reflect_from_mesh: o.reflect_from_mesh as u8,
            queue: queue_for(&self.inner, o.queue.as_ref()),
            queue_bytes: o.queue_bytes,
        };
        let cname = c_name(name, &what)?;
        let handle = Arc::new(Handle::new(&self.inner, name, true));
        let b = Arc::new(EventBox { node: Arc::as_ptr(&self.inner), name: name.to_owned(), cb: Callback::new(Box::new(handler)) });
        let (_pass, raw) = self.inner.enter_for(&what)?;
        // SAFETY: the pass keeps the node open, the schemas are node owned, and the box lives
        // until the handle retires
        let c = unsafe {
            ffi::rant_node_create_call_observer(
                raw,
                cname.as_ptr(),
                schema(&o.request_schema),
                schema(&o.progress_schema),
                schema(&o.response_schema),
                Some(on_event),
                Arc::as_ptr(&b) as *mut c_void,
                &co,
            )
        };
        if c.is_null() {
            return Err(Error::last(raw, &what));
        }
        handle.set_raw(c);
        handle.keep(Box::new(b));
        Ok(CallObserver { handle, open: true })
    }
}
