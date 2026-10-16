use std::borrow::Borrow;
use std::collections::BTreeMap;
use std::ffi::{CString, c_void};
use std::fmt;
use std::future::Future;
use std::pin::Pin;
use std::ptr::{null, null_mut};
use std::sync::atomic::{AtomicPtr, AtomicUsize, Ordering::SeqCst};
use std::sync::{Arc, Condvar, Mutex, PoisonError};
use std::task::{Context, Poll, Waker};
use std::time::Duration;

use crate::callback::{Callback, Refused};
use crate::error::{Error, ErrorKind, SendError};
use crate::ffi;
use crate::gate::{Gate, Pass};
use crate::message::{Bytes, Codec, Message, Payload};
use crate::node::{NodeInner, Queue, lock};
use crate::options::{options, to_ms};
use crate::schema::{FieldView, Schema, c_bytes, rbytes, rstring_bytes};

/// How a call ended. Timeout, PeerLost and NoProvider are decided on the caller.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub enum CallStatus {
    /// The handler answered.
    Ok,
    /// The handler failed, returned an `Err` or panicked: the message holds its text.
    AppError,
    /// The definition has no handler.
    NoHandler,
    /// No answer within the timeout.
    Timeout,
    /// The provider dropped mid call.
    PeerLost,
    /// A task honored a cancel, or the handle or a node closed mid call.
    Cancelled,
    /// A task started. The one status that is not an outcome, seen only as the first progress.
    Running,
    /// Nobody defines the name: the request never left this node.
    NoProvider,
    /// A status this binding does not know yet.
    Unknown,
}

impl CallStatus {
    pub(crate) fn from_c(s: ffi::RantCallStatus) -> CallStatus {
        match s {
            ffi::RANT_CALL_OK => CallStatus::Ok,
            ffi::RANT_CALL_APP_ERROR => CallStatus::AppError,
            ffi::RANT_CALL_NO_HANDLER => CallStatus::NoHandler,
            ffi::RANT_CALL_TIMEOUT => CallStatus::Timeout,
            ffi::RANT_CALL_PEER_LOST => CallStatus::PeerLost,
            ffi::RANT_CALL_CANCELLED => CallStatus::Cancelled,
            ffi::RANT_CALL_RUNNING => CallStatus::Running,
            ffi::RANT_CALL_NO_PROVIDER => CallStatus::NoProvider,
            _ => CallStatus::Unknown,
        }
    }
}

options! {
    /// One call's options: `CallOptions::new().provider(peer)`. Every option left out takes
    /// its default.
    pub struct CallOptions {
        /// Direct the call at this peer only. 0 = every definition, and the first answer
        /// wins. A task always goes to one provider, 0 = the oldest.
        provider: u32 = 0,
    }
    hand {
        timeout: Option<Duration> = None,
    }
}

impl CallOptions {
    /// How long a blocking call waits for its answer, else the handle's timeout. A task's
    /// bounds only the wait for its first response. An async call takes the handle's.
    pub fn timeout(mut self, timeout: Duration) -> Self {
        self.timeout = Some(timeout);
        self
    }
}

/// A call that did not end Ok with a value, from [`Response::into_result`].
#[derive(Clone, Debug)]
pub struct CallError {
    status: CallStatus,
    message: String,
}

impl CallError {
    /// How the call ended.
    pub fn status(&self) -> CallStatus {
        self.status
    }

    /// The text for a person: the provider's, else the status's own.
    pub fn message(&self) -> &str {
        &self.message
    }
}

impl fmt::Display for CallError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "the call ended {:?}: {}", self.status, self.message)
    }
}

impl std::error::Error for CallError {}

impl From<CallError> for Error {
    fn from(e: CallError) -> Error {
        Error::new(ErrorKind::None, e.to_string())
    }
}

/// A call's outcome, owned. A call outcome is a value, never a fault: [`Response::value`]
/// holds whatever payload came back, a cancelled task's partial result or failure data too.
pub struct Response<Rsp: Payload> {
    status: CallStatus,
    provider: u32,
    written_us: u64,
    message: String,
    value: Option<Rsp::Owned>,
    schema: Option<Schema>,
}

impl<Rsp: Payload> Response<Rsp> {
    /// The outcome as the C hands it over, decoded while its views are valid.
    pub(crate) fn from_c(inner: &NodeInner, codec: Option<&Codec>, name: &str, r: &ffi::RantResponse) -> Response<Rsp> {
        let status = CallStatus::from_c(r.status);
        let data = rbytes(r.data);
        // an empty Ok decodes, since a type such as Empty is zero bytes on the wire
        let value = if data.is_empty() && status != CallStatus::Ok {
            None
        } else {
            decoded::<Rsp>(inner, codec, name, data, r.schema, "a response")
        };
        Response {
            status,
            provider: r.provider,
            written_us: r.written_us,
            message: String::from_utf8_lossy(rstring_bytes(r.message)).into_owned(),
            value,
            // SAFETY: every NodeInner lives in an Arc
            schema: (!r.schema.is_null()).then(|| Schema::new(unsafe { inner.arc() }, r.schema)),
        }
    }

    /// How the call ended.
    pub fn status(&self) -> CallStatus {
        self.status
    }

    /// True when the call ended Ok.
    pub fn is_ok(&self) -> bool {
        self.status == CallStatus::Ok
    }

    /// The peer that answered, 0 when the outcome was decided here.
    pub fn provider(&self) -> u32 {
        self.provider
    }

    /// The provider's wall clock in UTC microseconds, 0 when decided here.
    pub fn written_us(&self) -> u64 {
        self.written_us
    }

    /// The text for a person: the provider's message, else the status's own. Empty only on
    /// Ok with no message.
    pub fn message(&self) -> &str {
        &self.message
    }

    /// The payload that came back, None when none did or it did not decode, which an error
    /// event reports.
    pub fn value(&self) -> Option<&Rsp::Owned> {
        self.value.as_ref()
    }

    /// The payload, owned.
    pub fn into_value(self) -> Option<Rsp::Owned> {
        self.value
    }

    /// The payload of an Ok call, else the status and the message, for `?`.
    pub fn into_result(self) -> Result<Rsp::Owned, CallError> {
        match (self.status, self.value) {
            (CallStatus::Ok, Some(v)) => Ok(v),
            (CallStatus::Ok, None) => Err(CallError { status: CallStatus::Ok, message: "the answer carried no value".to_owned() }),
            (status, _) => Err(CallError { status, message: self.message }),
        }
    }

    /// The schema the payload came with, None when untyped or none came back.
    pub fn schema(&self) -> Option<&Schema> {
        self.schema.as_ref()
    }
}

impl Response<Bytes> {
    /// The payload's fields by name, empty when no payload or schema came back.
    pub fn fields(&self) -> FieldView<'_> {
        let data = self.value.as_deref().unwrap_or(&[]);
        match &self.schema {
            Some(s) => s.view(data),
            None => FieldView::new(&[], null()),
        }
    }
}

impl<Rsp: Payload> fmt::Debug for Response<Rsp>
where
    Rsp::Owned: fmt::Debug,
{
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Response")
            .field("status", &self.status)
            .field("provider", &self.provider)
            .field("message", &self.message)
            .field("value", &self.value)
            .finish()
    }
}

/// Decodes a payload, reporting one that does not fit as an error event. None either way.
pub(crate) fn decoded<P: Payload>(
    inner: &NodeInner,
    codec: Option<&Codec>,
    name: &str,
    data: &[u8],
    schema: *const ffi::RantSchema,
    what: &str,
) -> Option<P::Owned> {
    match P::__owned(codec, data, schema) {
        Ok(v) => Some(v),
        Err(why) => {
            if !data.is_empty() {
                inner.report_decode(name, &format!("{what} did not decode: {why}"));
            }
            None
        }
    }
}

/// What a function or task handler returns. A value answers Ok, an `Err` answers AppError
/// with its text, and `()` leaves the answer to the handler's defer, else a function answers
/// Ok with nothing and a task AppError.
pub trait IntoReply<Rsp: Payload> {
    #[doc(hidden)]
    #[allow(private_interfaces)]
    fn __reply(self) -> Answer<Rsp>;
}

/// A handler's answer, as [`IntoReply`] spells it.
pub(crate) enum Answer<Rsp: Payload> {
    Value(Rsp::Owned),
    Fail(String),
    Nothing,
}

impl<T: Message> IntoReply<T> for T {
    #[allow(private_interfaces)]
    fn __reply(self) -> Answer<T> {
        Answer::Value(self)
    }
}

impl<T: Message, E: fmt::Display> IntoReply<T> for Result<T, E> {
    #[allow(private_interfaces)]
    fn __reply(self) -> Answer<T> {
        match self {
            Ok(v) => Answer::Value(v),
            Err(e) => Answer::Fail(e.to_string()),
        }
    }
}

impl<Rsp: Payload> IntoReply<Rsp> for () {
    #[allow(private_interfaces)]
    fn __reply(self) -> Answer<Rsp> {
        Answer::Nothing
    }
}

impl IntoReply<Bytes> for Vec<u8> {
    #[allow(private_interfaces)]
    fn __reply(self) -> Answer<Bytes> {
        Answer::Value(self)
    }
}

impl<E: fmt::Display> IntoReply<Bytes> for Result<Vec<u8>, E> {
    #[allow(private_interfaces)]
    fn __reply(self) -> Answer<Bytes> {
        match self {
            Ok(v) => Answer::Value(v),
            Err(e) => Answer::Fail(e.to_string()),
        }
    }
}

/// A C string for a message, NUL bytes made spaces so the text is never refused.
pub(crate) fn c_text(text: &str) -> CString {
    CString::new(text.replace('\0', " ")).unwrap_or_default()
}

/// Answers AppError with `text`. A request already answered or deferred ignores it.
pub(crate) fn fail_request(rq: *mut ffi::RantRequest, text: &str) {
    let c = c_text(text);
    // SAFETY: rq is the live request of the running handler
    unsafe { ffi::rant_request_fail(rq, c.as_ptr(), ffi::RantBytes::default()) };
}

/// Sends a handler's answer.
pub(crate) fn answer_request<Rsp: Payload>(shared: &FnShared, rq: *mut ffi::RantRequest, answer: Answer<Rsp>) {
    match answer {
        Answer::Value(v) => {
            // SAFETY: rq is the live request of the running handler
            let sent = Rsp::__with_bytes(shared.rsp.as_deref(), v.borrow(), |b| unsafe { ffi::rant_request_reply(rq, c_bytes(b)) });
            if let Err(why) = sent {
                fail_request(rq, &format!("the reply did not encode: {why}"));
            }
        }
        Answer::Fail(text) => fail_request(rq, &text),
        Answer::Nothing => {}
    }
}

/// A C pattern handle a retire frees.
pub(crate) trait CHandle {
    /// Safety: raw is a live handle no other call is inside.
    unsafe fn retire(raw: *mut Self) -> i32;
}

impl CHandle for ffi::RantFunction {
    unsafe fn retire(raw: *mut Self) -> i32 {
        // SAFETY: as the trait says
        unsafe { ffi::rant_function_retire(raw) }
    }
}

impl CHandle for ffi::RantVariable {
    unsafe fn retire(raw: *mut Self) -> i32 {
        // SAFETY: as the trait says
        unsafe { ffi::rant_variable_retire(raw) }
    }
}

/// A function, task or variable handle's life: the C handle, a gate of its own that keeps it
/// alive under the calls of the handle and of its deferred answers, and the handler boxes
/// the C points at, freed when it retires.
pub(crate) struct Handle<C: CHandle> {
    pub(crate) inner: Arc<NodeInner>,
    raw: AtomicPtr<C>,
    gate: Gate,
    pub(crate) name: String,
    /// false for the node's own @rant/meta handle, which is never retired
    owned: bool,
    boxes: Mutex<Vec<u64>>,
}

// SAFETY: every C call on the handle holds a pass on the node and on the handle
unsafe impl<C: CHandle> Send for Handle<C> {}
unsafe impl<C: CHandle> Sync for Handle<C> {}

/// A call's hold on a pattern handle and its node, and the C handle while it lasts.
pub(crate) struct HandlePass<'a, C> {
    _node: Pass<'a>,
    _handle: Pass<'a>,
    pub(crate) raw: *mut C,
}

impl<C: CHandle> Handle<C> {
    pub(crate) fn new(inner: &Arc<NodeInner>, name: &str, owned: bool) -> Handle<C> {
        Handle {
            inner: Arc::clone(inner),
            raw: AtomicPtr::new(null_mut()),
            gate: Gate::new(),
            name: name.to_owned(),
            owned,
            boxes: Mutex::new(Vec::new()),
        }
    }

    pub(crate) fn set_raw(&self, raw: *mut C) {
        self.raw.store(raw, SeqCst);
    }

    /// The C handle, waiting out the moment between its create and its store, when a
    /// callback may already arrive. Only called from a callback of a created handle.
    pub(crate) fn raw_when_set(&self) -> *mut C {
        loop {
            let raw = self.raw.load(SeqCst);
            if !raw.is_null() {
                return raw;
            }
            std::thread::yield_now();
        }
    }

    /// Keeps a handler box in the node until this handle retires.
    pub(crate) fn keep(&self, value: Box<dyn std::any::Any + Send + Sync>) {
        let id = self.inner.keep(value);
        lock(&self.boxes).push(id);
    }

    /// NoTopic once the handle or its node closed.
    pub(crate) fn enter(&self) -> Result<HandlePass<'_, C>, SendError> {
        let node = self.inner.gate.enter().ok_or(SendError::NoTopic)?;
        let handle = self.gate.enter().ok_or(SendError::NoTopic)?;
        let raw = self.raw.load(SeqCst);
        if raw.is_null() {
            return Err(SendError::NoTopic);
        }
        Ok(HandlePass { _node: node, _handle: handle, raw })
    }

    /// Retires the C handle once its calls are out. State from inside one of its callbacks,
    /// and the handle then stays until the node closes.
    pub(crate) fn close(&self) -> Result<(), SendError> {
        let Some(_node) = self.inner.gate.enter() else { return Ok(()) };
        if !self.owned || !self.gate.close() {
            return Ok(());
        }
        let raw = self.raw.load(SeqCst);
        if raw.is_null() {
            return Ok(());
        }
        // SAFETY: the node pass keeps the node open and the closed gate holds every other call out
        let rc = unsafe { C::retire(raw) };
        if rc != 0 {
            self.gate.reopen();
            return SendError::check(rc).map(|_| ());
        }
        self.raw.store(null_mut(), SeqCst);
        let boxes = std::mem::take(&mut *lock(&self.boxes));
        for id in boxes {
            self.inner.release(id);
        }
        Ok(())
    }
}

/// The state behind a function or task handle, shared with its deferred answers.
pub(crate) struct FnShared {
    handle: Handle<ffi::RantFunction>,
    pub(crate) req: Option<Arc<Codec>>,
    pub(crate) rsp: Option<Arc<Codec>>,
    pub(crate) prg: Option<Arc<Codec>>,
}

impl std::ops::Deref for FnShared {
    type Target = Handle<ffi::RantFunction>;
    fn deref(&self) -> &Handle<ffi::RantFunction> {
        &self.handle
    }
}

/// The codecs of a function or task, one per channel.
pub(crate) struct Codecs {
    pub(crate) req: Option<Arc<Codec>>,
    pub(crate) rsp: Option<Arc<Codec>>,
    pub(crate) prg: Option<Arc<Codec>>,
}

impl FnShared {
    pub(crate) fn new(inner: &Arc<NodeInner>, name: &str, owned: bool, codecs: Codecs) -> Arc<FnShared> {
        Arc::new(FnShared { handle: Handle::new(inner, name, owned), req: codecs.req, rsp: codecs.rsp, prg: codecs.prg })
    }

    pub(crate) fn match_count(&self) -> usize {
        match self.enter() {
            // SAFETY: the pass keeps the handle alive
            Ok(p) => unsafe { ffi::rant_function_match_count(p.raw) }.max(0) as usize,
            Err(_) => 0,
        }
    }

    pub(crate) fn refresh(&self) -> Result<bool, SendError> {
        let p = self.enter()?;
        // SAFETY: the pass keeps the handle alive
        Ok(SendError::check(unsafe { ffi::rant_function_refresh(p.raw) })? == 1)
    }

    /// A channel's schema now, through one of the rant_function_*_schema getters.
    pub(crate) fn schema(&self, get: unsafe extern "C" fn(*const ffi::RantFunction) -> *const ffi::RantSchema) -> Option<Schema> {
        let p = self.enter().ok()?;
        // SAFETY: the pass keeps the handle alive, and its schemas live with the node
        let s = unsafe { get(p.raw) };
        (!s.is_null()).then(|| Schema::new(Arc::clone(&self.inner), s))
    }

    /// Answers a deferred call: the status, an optional message, and the payload bytes.
    pub(crate) fn complete(&self, token: u64, status: ffi::RantCallStatus, message: Option<&str>, payload: &[u8]) -> Result<(), SendError> {
        let p = self.enter()?;
        let msg = message.map(c_text);
        // SAFETY: the pass keeps the handle alive, and a stale token answers State
        let rc = unsafe { ffi::rant_function_complete(p.raw, token, status, msg.as_ref().map_or(null(), |m| m.as_ptr()), c_bytes(payload)) };
        SendError::check(rc).map(|_| ())
    }

    /// Answers a deferred call with a typed or raw payload. One that does not encode answers
    /// AppError, so the caller is not left waiting, and returns Schema.
    pub(crate) fn complete_with<P: Payload>(
        &self,
        codec: Option<&Codec>,
        token: u64,
        status: ffi::RantCallStatus,
        message: Option<&str>,
        value: &P::Input,
    ) -> Result<(), SendError> {
        // the encode reads the codec's schema, which lives in the node
        let p = self.enter()?;
        match P::__with_bytes(codec, value, |b| self.complete(token, status, message, b)) {
            Ok(r) => r,
            Err(why) => {
                drop(p);
                self.complete(token, ffi::RANT_CALL_APP_ERROR, Some(&format!("the answer did not encode: {why}")), &[])?;
                Err(SendError::Schema)
            }
        }
    }
}

/// A request handler as the trampoline runs it.
pub(crate) type RequestFn = dyn FnMut(&Arc<FnShared>, *mut ffi::RantRequest) + Send;

/// A definition's handler, kept by the node and pointed at by the C.
pub(crate) struct RequestBox {
    node: *const NodeInner,
    shared: Arc<FnShared>,
    cb: Callback<RequestFn>,
}

// SAFETY: node is read only, and the box lives inside that node's registry
unsafe impl Send for RequestBox {}
unsafe impl Sync for RequestBox {}

impl RequestBox {
    pub(crate) fn new(shared: &Arc<FnShared>, f: Box<RequestFn>) -> Arc<RequestBox> {
        Arc::new(RequestBox { node: Arc::as_ptr(&shared.inner), shared: Arc::clone(shared), cb: Callback::new(f) })
    }
}

pub(crate) unsafe extern "C" fn on_request(rq: *mut ffi::RantRequest, user: *mut c_void) {
    // SAFETY: user is a RequestBox the node keeps until the handle retires
    let b = unsafe { &*(user as *const RequestBox) };
    let inner = unsafe { &*b.node };
    b.shared.raw_when_set();
    match b.cb.run(inner.key(), |f| f(&b.shared, rq)) {
        Ok(()) => {}
        Err(Refused::Panicked(text)) => fail_request(rq, &format!("the handler panicked: {text}")),
        Err(Refused::Reentered) => fail_request(rq, "the handler was called from inside itself"),
    }
}

/// The accessors every request type shares, over its `raw` request.
macro_rules! request_accessors {
    () => {
        /// The calling node's peer id.
        pub fn caller(&self) -> u32 {
            self.raw().caller
        }

        /// The calling node's name.
        pub fn caller_name(&self) -> &'a str {
            crate::schema::utf8_prefix(crate::schema::rstring_bytes(self.raw().caller_name))
        }

        /// When it arrived, on this node's monotonic clock in microseconds.
        pub fn recv_us(&self) -> u64 {
            self.raw().recv_us
        }

        /// The caller's wall clock in UTC microseconds, 0 when it opted out.
        pub fn written_us(&self) -> u64 {
            self.raw().written_us
        }

        /// The request's payload bytes, which the value was decoded from.
        pub fn data(&self) -> &'a [u8] {
            crate::schema::rbytes(self.raw().data)
        }

        fn raw(&self) -> &'a crate::ffi::RantRequest {
            // SAFETY: the C hands the request valid for the handler, which 'a spans
            unsafe { &*self.raw }
        }
    };
}
pub(crate) use request_accessors;

/// One in flight call's callbacks, found by the id the C carries as its user pointer, so
/// a callback racing the call's end finds nothing instead of freed memory.
struct CallSlot {
    node: *const NodeInner,
    name: String,
    progress: Option<Callback<ProgressFn>>,
    on_response: Mutex<Option<Box<ResponseFn>>>,
    inflight: Mutex<u32>,
    idle: Condvar,
}

// SAFETY: node is read only and outlives every call of its node
unsafe impl Send for CallSlot {}
unsafe impl Sync for CallSlot {}

pub(crate) type ProgressFn = dyn FnMut(&NodeInner, &ffi::RantProgress) + Send;
/// A progress handler a blocking call borrows for its length.
pub(crate) type BorrowedProgress<'b> = dyn FnMut(&NodeInner, &ffi::RantProgress) + Send + 'b;
type ResponseFn = dyn FnOnce(&NodeInner, &ffi::RantResponse) + Send;

static CALLS: Mutex<BTreeMap<usize, Arc<CallSlot>>> = Mutex::new(BTreeMap::new());
static NEXT_CALL: AtomicUsize = AtomicUsize::new(1);

fn calls() -> std::sync::MutexGuard<'static, BTreeMap<usize, Arc<CallSlot>>> {
    CALLS.lock().unwrap_or_else(PoisonError::into_inner)
}

fn register(slot: CallSlot) -> usize {
    let id = NEXT_CALL.fetch_add(1, SeqCst);
    calls().insert(id, Arc::new(slot));
    id
}

/// Ends a call's registration, then waits for a progress callback still running elsewhere.
fn settle(id: usize) {
    let slot = calls().remove(&id);
    if let Some(s) = slot {
        let mut n = lock(&s.inflight);
        while *n > 0 {
            n = s.idle.wait(n).unwrap_or_else(PoisonError::into_inner);
        }
    }
}

unsafe extern "C" fn on_progress(p: *const ffi::RantProgress) {
    // SAFETY: the C hands an update valid for this call
    let p = unsafe { &*p };
    let slot = {
        let calls = calls();
        let Some(s) = calls.get(&(p.user as usize)) else { return };
        *lock(&s.inflight) += 1;
        Arc::clone(s)
    };
    // SAFETY: a call's node outlives its callbacks
    let inner = unsafe { &*slot.node };
    if let Some(cb) = &slot.progress {
        if let Err(refused) = cb.run(inner.key(), |f| f(inner, p)) {
            inner.report_refused(Some(&slot.name), refused);
        }
    }
    let mut n = lock(&slot.inflight);
    *n -= 1;
    if *n == 0 {
        slot.idle.notify_all();
    }
}

unsafe extern "C" fn on_response(r: *const ffi::RantResponse) {
    // SAFETY: the C hands the outcome valid for this call, once per call
    let r = unsafe { &*r };
    let Some(slot) = calls().remove(&(r.user as usize)) else { return };
    // SAFETY: a call's node outlives its callbacks
    let inner = unsafe { &*slot.node };
    let done = lock(&slot.on_response).take();
    if let Some(done) = done {
        done(inner, r);
    }
}

/// One progress update of a task call. The start of the work, the RUNNING answer, arrives
/// first with no value and carries the call id, the handle for a cancel from another thread.
pub struct Progress<'a, Prg: Payload> {
    value: Option<Prg::Data<'a>>,
    call_id: u32,
    provider: u32,
    written_us: u64,
    recv_us: u64,
}

impl<'a, Prg: Payload> Progress<'a, Prg> {
    /// The update, None for the start of the work.
    pub fn value(&self) -> Option<&Prg::Data<'a>> {
        self.value.as_ref()
    }

    /// The update, owned by the caller.
    pub fn into_value(self) -> Option<Prg::Data<'a>> {
        self.value
    }

    /// The call this update belongs to.
    pub fn call_id(&self) -> u32 {
        self.call_id
    }

    /// The peer working the call.
    pub fn provider(&self) -> u32 {
        self.provider
    }

    /// The provider's wall clock in UTC microseconds, 0 when unstamped.
    pub fn written_us(&self) -> u64 {
        self.written_us
    }

    /// When it arrived, on this node's monotonic clock in microseconds.
    pub fn recv_us(&self) -> u64 {
        self.recv_us
    }
}

/// A typed progress handler as the trampoline runs it.
pub(crate) fn progress_adapter<Prg: Payload>(
    codec: Option<Arc<Codec>>,
    name: String,
    mut f: impl for<'a> FnMut(Progress<'a, Prg>) + Send,
) -> impl FnMut(&NodeInner, &ffi::RantProgress) + Send {
    move |inner: &NodeInner, p: &ffi::RantProgress| {
        let data = rbytes(p.data);
        let value = if data.is_empty() {
            None
        } else {
            match Prg::__data(codec.as_deref(), data, p.schema) {
                Ok(v) => Some(v),
                Err(why) => return inner.report_decode(&name, &format!("a progress update did not decode: {why}")),
            }
        };
        f(Progress { value, call_id: p.call_id, provider: p.provider, written_us: p.written_us, recv_us: p.recv_us })
    }
}

/// A progress handler borrowed from a blocking call's stack. The call settles its slot
/// before it returns, so the borrow never outlives the frame.
struct Borrowed(*mut ProgressFn);

// SAFETY: the target is Send, and is called only while its call is in flight
unsafe impl Send for Borrowed {}

impl Borrowed {
    fn call(&self, inner: &NodeInner, p: &ffi::RantProgress) {
        // SAFETY: as the type says
        unsafe { (*self.0)(inner, p) }
    }
}

/// A blocking call: the request out, a wait on the service thread or on this one's loop,
/// then the decoded outcome. NoTopic once closed, Schema when the request did not encode,
/// State from an inline callback, whose loop it would stall.
pub(crate) fn blocking_call<Req: Payload, Rsp: Payload>(
    shared: &FnShared,
    req: &Req::Input,
    options: &CallOptions,
    progress: Option<&mut BorrowedProgress<'_>>,
) -> Result<Response<Rsp>, SendError> {
    let p = shared.enter()?;
    let mut co = ffi::RantCallOpts { provider: options.provider, ..Default::default() };
    let slot = progress.map(|f| {
        let raw: *mut BorrowedProgress<'_> = f;
        // SAFETY: only the lifetime is erased, and settle ends every use before the frame does
        let raw: *mut ProgressFn = unsafe { std::mem::transmute(raw) };
        let target = Borrowed(raw);
        register(CallSlot {
            node: Arc::as_ptr(&shared.inner),
            name: shared.name.clone(),
            progress: Some(Callback::new(Box::new(move |i: &NodeInner, p: &ffi::RantProgress| target.call(i, p)))),
            on_response: Mutex::new(None),
            inflight: Mutex::new(0),
            idle: Condvar::new(),
        })
    });
    if let Some(id) = slot {
        co.on_progress = Some(on_progress);
        co.progress_user = id as *mut c_void;
    }
    let mut out = ffi::RantResponse::default();
    let timeout = options.timeout.map_or(-1, to_ms);
    // SAFETY: the pass keeps the handle alive, and out and co outlive the call
    let rc = Req::__with_bytes(shared.req.as_deref(), req, |b| unsafe { ffi::rant_function_call(p.raw, c_bytes(b), &mut out, timeout, &co) });
    if let Some(id) = slot {
        settle(id);
    }
    let rc = rc.map_err(|_| SendError::Schema)?;
    if rc < 0 {
        return Err(SendError::check(rc).err().unwrap_or(SendError::State));
    }
    // 1 is an outcome and 0 the local timeout, and out reads either way until the next call
    Ok(Response::from_c(&shared.inner, shared.rsp.as_deref(), &shared.name, &out))
}

/// The outcome an async call delivers, and the waker of whoever waits for it.
pub(crate) struct CallState<Rsp: Payload> {
    m: Mutex<(Option<Response<Rsp>>, Option<Waker>, bool)>,
}

impl<Rsp: Payload> CallState<Rsp> {
    fn finish(&self, r: Response<Rsp>) {
        let waker = {
            let mut s = lock(&self.m);
            s.0 = Some(r);
            s.2 = true;
            s.1.take()
        };
        if let Some(w) = waker {
            w.wake();
        }
    }

    fn finished(&self) -> bool {
        lock(&self.m).2
    }

    fn poll(&self, cx: &mut Context<'_>) -> Poll<Response<Rsp>> {
        let mut s = lock(&self.m);
        match s.0.take() {
            Some(r) => Poll::Ready(r),
            None => {
                s.1 = Some(cx.waker().clone());
                Poll::Pending
            }
        }
    }
}

/// Sends a request and returns at once with the state its outcome lands in and its call id.
pub(crate) fn async_call<Req: Payload, Rsp: Payload>(
    shared: &FnShared,
    req: &Req::Input,
    options: &CallOptions,
    progress: Option<Box<ProgressFn>>,
) -> Result<(Arc<CallState<Rsp>>, u32), SendError> {
    let p = shared.enter()?;
    let state = Arc::new(CallState { m: Mutex::new((None, None, false)) });
    let done: Box<ResponseFn> = {
        let state = Arc::clone(&state);
        let codec = shared.rsp.clone();
        let name = shared.name.clone();
        Box::new(move |inner: &NodeInner, r: &ffi::RantResponse| state.finish(Response::from_c(inner, codec.as_deref(), &name, r)))
    };
    let has_progress = progress.is_some();
    let id = register(CallSlot {
        node: Arc::as_ptr(&shared.inner),
        name: shared.name.clone(),
        progress: progress.map(Callback::new),
        on_response: Mutex::new(Some(done)),
        inflight: Mutex::new(0),
        idle: Condvar::new(),
    });
    let mut call_id = 0u32;
    let co = ffi::RantCallOpts {
        provider: options.provider,
        on_progress: if has_progress { Some(on_progress) } else { None },
        progress_user: id as *mut c_void,
        id_out: &mut call_id,
    };
    // SAFETY: the pass keeps the handle alive, and the slot outlives the call until its outcome
    let rc = Req::__with_bytes(shared.req.as_deref(), req, |b| unsafe {
        ffi::rant_function_call_async(p.raw, c_bytes(b), Some(on_response), id as *mut c_void, &co)
    });
    match rc.map_err(|_| SendError::Schema).and_then(SendError::check) {
        Ok(_) => Ok((state, call_id)),
        Err(e) => {
            // the C fires nothing for a request it refused
            calls().remove(&id);
            Err(e)
        }
    }
}

/// A function call in flight, from [`RemoteFunction::call_async`](crate::RemoteFunction::call_async):
/// a Future that resolves to the outcome on any executor, with no runtime crate. Dropping it
/// only stops listening, since a function call cannot be stopped.
#[must_use = "a pending call does nothing unless awaited"]
pub struct PendingCall<Rsp: Payload> {
    pub(crate) state: Arc<CallState<Rsp>>,
}

impl<Rsp: Payload> Future for PendingCall<Rsp> {
    type Output = Response<Rsp>;
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Response<Rsp>> {
        self.state.poll(cx)
    }
}

/// A task call in flight, from [`RemoteTask::call_async`](crate::RemoteTask::call_async): a
/// Future that resolves to the outcome on any executor. Dropping it before the outcome
/// cancels the task, as dropping a future stops its work. [`TaskCall::detach`] keeps it going.
#[must_use = "a dropped task call cancels the task, detach() keeps it running"]
pub struct TaskCall<Rsp: Payload> {
    pub(crate) state: Arc<CallState<Rsp>>,
    pub(crate) shared: Arc<FnShared>,
    pub(crate) id: u32,
    pub(crate) detached: bool,
}

impl<Rsp: Payload> TaskCall<Rsp> {
    /// The call id, as each progress update carries it.
    pub fn id(&self) -> u32 {
        self.id
    }

    /// Asks the provider to stop. Cooperative, and the outcome is the answer: BadRole when
    /// the provider declared no_cancel, State once the call ended.
    pub fn cancel(&self) -> Result<(), SendError> {
        cancel_call(&self.shared, self.id)
    }

    /// Lets the task run to its end unwatched. Its outcome is dropped.
    pub fn detach(mut self) {
        self.detached = true;
    }
}

impl<Rsp: Payload> Future for TaskCall<Rsp> {
    type Output = Response<Rsp>;
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Response<Rsp>> {
        self.state.poll(cx)
    }
}

impl<Rsp: Payload> Drop for TaskCall<Rsp> {
    fn drop(&mut self) {
        if !self.detached && !self.state.finished() {
            let _ = cancel_call(&self.shared, self.id);
        }
    }
}

pub(crate) fn cancel_call(shared: &FnShared, call_id: u32) -> Result<(), SendError> {
    let p = shared.enter()?;
    // SAFETY: the pass keeps the handle alive
    SendError::check(unsafe { ffi::rant_function_cancel(p.raw, call_id) }).map(|_| ())
}

/// The type parameters a handle carries for its methods alone.
pub(crate) type Marker<T> = std::marker::PhantomData<fn() -> T>;

/// Checks that a queue and the schemas named in a handle's options belong to this node.
pub(crate) fn same_node(inner: &Arc<NodeInner>, queue: Option<&Queue>, schemas: &[Option<&Schema>], what: &str) -> Result<(), Error> {
    if queue.is_some_and(|q| !Arc::ptr_eq(&q.inner, inner)) {
        return Err(Error::new(ErrorKind::State, format!("{what}: the queue belongs to another node")));
    }
    if schemas.iter().flatten().any(|s| !Arc::ptr_eq(&s.inner, inner)) {
        return Err(Error::new(ErrorKind::State, format!("{what}: the schema belongs to another node")));
    }
    Ok(())
}

/// The C schema a channel declares: the codec's, else the one given for raw bytes, else none.
pub(crate) fn channel_schema(codec: Option<&Arc<Codec>>, given: Option<&Schema>) -> *const ffi::RantSchema {
    match (codec, given) {
        (Some(c), _) => c.schema,
        (None, Some(s)) => s.raw,
        (None, None) => null(),
    }
}

/// The queue a pattern handle's callbacks park on: the one named, else the node's under
/// Dispatch threading, else none.
pub(crate) fn queue_for(inner: &NodeInner, queue: Option<&Queue>) -> *mut ffi::RantQueue {
    queue.map_or_else(|| inner.node_queue(), |q| q.raw)
}

#[cfg(test)]
mod tests {
    #[test]
    fn every_c_call_status_has_a_variant() {
        for (name, v) in crate::c_enum_values("RantCallStatus") {
            assert_ne!(super::CallStatus::from_c(v as crate::ffi::RantCallStatus), super::CallStatus::Unknown, "{name}");
        }
    }
}
