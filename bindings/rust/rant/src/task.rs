use std::ffi::c_void;
use std::marker::PhantomData;
use std::ops::Deref;
use std::sync::Arc;
use std::time::Duration;

use crate::callback::Callback;
use crate::error::{Error, SendError};
use crate::ffi;
use crate::function::{c_name, fn_handle};
use crate::message::{Bytes, Payload};
use crate::node::{Node, NodeInner, Queue};
use crate::options::{options, to_us};
use crate::pattern::{
    CallOptions, Codecs, FnShared, IntoReply, Marker, Progress, RequestBox, RequestFn, Response, TaskCall, answer_request, async_call, blocking_call,
    cancel_call, channel_schema, fail_request, on_request, progress_adapter, queue_for, request_accessors, same_node,
};
use crate::schema::{Schema, rbytes, rstring_bytes, utf8_prefix};

options! {
    /// A task handle's options, a definition or a remote: `TaskOptions::new().no_cancel(true)`.
    /// Every option left out takes its default, and docs/tasks.md explains each.
    pub struct TaskOptions {
        /// Progress best effort, not reliable. The definition offers, a remote requests.
        progress_best_effort: bool = false,
        /// The progress history depth. 0 = the pattern default.
        progress_keep_last: u16 = 0,
        /// Definition: cancels are not honored, so a remote's cancel answers BadRole.
        no_cancel: bool = false,
        /// Definition: one call at a time, declared for tools. The handler enforces it.
        exclusive: bool = false,
        /// Redundant providers are intended. Each request still runs on one.
        multi: bool = false,
        /// A remote's bound on the wait for the first response. Zero = 5 s.
        timeout: Duration = Duration::ZERO,
        /// How long a request, a progress update or a reply pauses for a slow peer. Zero = 1 s.
        backpressure_wait: Duration = Duration::ZERO,
        /// Request and response history depth. 0 = 10.
        keep_last: u16 = 0,
        /// A [`Bytes`] handle made without schemas takes them from the mesh.
        reflect_from_mesh: bool = false,
    }
    hand {
        queue: Option<Queue> = None,
        request_schema: Option<Schema> = None,
        progress_schema: Option<Schema> = None,
        response_schema: Option<Schema> = None,
    }
}

impl TaskOptions {
    /// Run this handle's callbacks at [`Queue::dispatch`] of `queue` instead of where the
    /// node's callbacks run.
    pub fn queue(mut self, queue: &Queue) -> Self {
        self.queue = Some(queue.clone());
        self
    }

    /// The schema the request bytes of a [`Bytes`] handle follow.
    pub fn request_schema(mut self, schema: &Schema) -> Self {
        self.request_schema = Some(schema.clone());
        self
    }

    /// The schema the progress bytes of a [`Bytes`] handle follow.
    pub fn progress_schema(mut self, schema: &Schema) -> Self {
        self.progress_schema = Some(schema.clone());
        self
    }

    /// The schema the response bytes of a [`Bytes`] handle follow.
    pub fn response_schema(mut self, schema: &Schema) -> Self {
        self.response_schema = Some(schema.clone());
        self
    }

    fn to_c(&self, node: &Node) -> ffi::RantTaskOpts {
        ffi::RantTaskOpts {
            progress_best_effort: self.progress_best_effort as u8,
            progress_keep_last: self.progress_keep_last,
            no_cancel: self.no_cancel as u8,
            exclusive: self.exclusive as u8,
            multi: self.multi as u8,
            timeout_us: to_us(self.timeout),
            backpressure_wait_us: to_us(self.backpressure_wait),
            keep_last: self.keep_last,
            reflect_from_mesh: self.reflect_from_mesh as u8,
            queue: queue_for(&node.inner, self.queue.as_ref()),
        }
    }

    fn schemas(&self) -> [Option<&Schema>; 3] {
        [self.request_schema.as_ref(), self.progress_schema.as_ref(), self.response_schema.as_ref()]
    }
}

/// The implementation side of a task, from [`Node::task_definition`]: a long running call
/// that streams progress and can be cancelled. One per name on the mesh unless `multi`.
pub struct TaskDefinition<Req: Payload, Prg: Payload, Rsp: Payload> {
    shared: Arc<FnShared>,
    open: bool,
    _t: Marker<(Req, Prg, Rsp)>,
}

fn_handle!(TaskDefinition<Req, Prg, Rsp>, "TaskDefinition");

impl<Req: Payload, Prg: Payload, Rsp: Payload> TaskDefinition<Req, Prg, Rsp> {
    /// The progress schema in use, None while untyped.
    pub fn progress_schema(&self) -> Option<Schema> {
        self.shared.schema(ffi::rant_function_progress_schema)
    }

    /// Told the moment a cancel arrives, with the call's [`PendingTask::token`], where the
    /// node's callbacks run. Optional, since [`PendingTask::cancelled`] polls the same. A
    /// later call replaces the handler.
    pub fn on_cancel(&self, handler: impl FnMut(u64) + Send + 'static) -> Result<(), SendError> {
        let p = self.shared.enter()?;
        let b = Arc::new(CancelBox {
            node: Arc::as_ptr(&self.shared.inner),
            name: self.shared.name.clone(),
            cb: Callback::new(Box::new(handler)),
        });
        // SAFETY: the pass keeps the handle alive, and the box lives until it retires, since
        // the C may still run a replaced handler on another thread
        let rc = unsafe { ffi::rant_function_on_cancel(p.raw, Some(on_cancel), Arc::as_ptr(&b) as *mut c_void) };
        SendError::check(rc)?;
        self.shared.keep(Box::new(b));
        Ok(())
    }
}

impl TaskDefinition<Bytes, Bytes, Bytes> {
    /// A `reflect_from_mesh` handle: re types in place when the mesh moved. True = re typed.
    pub fn refresh(&self) -> Result<bool, SendError> {
        self.shared.refresh()
    }
}

struct CancelBox {
    node: *const NodeInner,
    name: String,
    cb: Callback<dyn FnMut(u64) + Send>,
}

// SAFETY: node is read only, and the box lives inside that node's registry
unsafe impl Send for CancelBox {}
unsafe impl Sync for CancelBox {}

unsafe extern "C" fn on_cancel(token: u64, user: *mut c_void) {
    // SAFETY: user is a CancelBox the node keeps until the handle retires
    let b = unsafe { &*(user as *const CancelBox) };
    let inner = unsafe { &*b.node };
    if let Err(refused) = b.cb.run(inner.key(), |f| f(token)) {
        inner.report_refused(Some(&b.name), refused);
    }
}

/// The caller side of a task another node defines, from [`Node::remote_task`]. Every request
/// goes to one provider, and the timeout bounds only the wait for the first response.
pub struct RemoteTask<Req: Payload, Prg: Payload, Rsp: Payload> {
    shared: Arc<FnShared>,
    open: bool,
    _t: Marker<(Req, Prg, Rsp)>,
}

fn_handle!(RemoteTask<Req, Prg, Rsp>, "RemoteTask");

impl<Req: Payload, Prg: Payload, Rsp: Payload> RemoteTask<Req, Prg, Rsp> {
    /// Runs the task and waits for its outcome. `on_progress` gets each update where the
    /// node's callbacks run, on this thread only under Manual, the first being the start of
    /// the work with the call id for a [`RemoteTask::cancel`] from another thread. Err as
    /// [`RemoteFunction::call`](crate::RemoteFunction::call).
    pub fn call(
        &self,
        request: &Req::Input,
        options: CallOptions,
        on_progress: impl for<'a> FnMut(Progress<'a, Prg>) + Send,
    ) -> Result<Response<Rsp>, SendError> {
        let mut adapter = progress_adapter::<Prg>(self.shared.prg.clone(), self.shared.name.clone(), on_progress);
        blocking_call::<Req, Rsp>(&self.shared, request, &options, Some(&mut adapter))
    }

    /// Starts the task and returns at once with a Future of its outcome, which any executor
    /// can await. `on_progress` gets each update where the node's callbacks run, or at
    /// dispatch of the handle's queue. Dropping the [`TaskCall`] early cancels the task.
    pub fn call_async(
        &self,
        request: &Req::Input,
        options: CallOptions,
        on_progress: impl for<'a> FnMut(Progress<'a, Prg>) + Send + 'static,
    ) -> Result<TaskCall<Rsp>, SendError> {
        let adapter = progress_adapter::<Prg>(self.shared.prg.clone(), self.shared.name.clone(), on_progress);
        let (state, id) = async_call::<Req, Rsp>(&self.shared, request, &options, Some(Box::new(adapter)))?;
        Ok(TaskCall { state, shared: Arc::clone(&self.shared), id, detached: false })
    }

    /// Asks the provider to stop a call. Cooperative and never acknowledged: the outcome is
    /// the answer. BadRole when the provider declared no_cancel, State once the call ended.
    pub fn cancel(&self, call_id: u32) -> Result<(), SendError> {
        cancel_call(&self.shared, call_id)
    }

    /// The progress schema in use, None while untyped.
    pub fn progress_schema(&self) -> Option<Schema> {
        self.shared.schema(ffi::rant_function_progress_schema)
    }
}

impl RemoteTask<Bytes, Bytes, Bytes> {
    /// A `reflect_from_mesh` handle: re types in place when the mesh moved, answering the
    /// outstanding calls Cancelled first. True = re typed.
    pub fn refresh(&self) -> Result<bool, SendError> {
        self.shared.refresh()
    }
}

/// One request as a task handler sees it, valid inside the handler: the value, through
/// `Deref`, plus who called when. Return the answer at once, or [`TaskRequest::defer`] the
/// work to a thread of your own. Returning `()` without a defer answers AppError.
pub struct TaskRequest<'a, Req: Payload, Prg: Payload, Rsp: Payload> {
    value: Req::Data<'a>,
    raw: *mut ffi::RantRequest,
    shared: &'a Arc<FnShared>,
    _t: Marker<(Prg, Rsp)>,
}

impl<'a, Req: Payload, Prg: Payload, Rsp: Payload> TaskRequest<'a, Req, Prg, Rsp> {
    request_accessors!();

    /// The task's name on the mesh.
    pub fn task_name(&self) -> &'a str {
        utf8_prefix(rstring_bytes(self.raw().function_name))
    }

    /// The value itself.
    pub fn into_inner(self) -> Req::Data<'a> {
        self.value
    }

    /// Tells the caller the work started, before a slow check. Idempotent, and defer
    /// implies it.
    pub fn start(&self) -> Result<(), SendError> {
        // SAFETY: the request is live for the handler
        SendError::check(unsafe { ffi::rant_request_start(self.raw) }).map(|_| ())
    }

    /// Carries the call to a thread of your own: the returned [`PendingTask`] sends progress
    /// and the one answer, and the handler returns `()`.
    pub fn defer(self) -> PendingTask<Prg, Rsp> {
        // SAFETY: the request is live for the handler
        let token = unsafe { ffi::rant_request_defer(self.raw) };
        PendingTask { shared: Arc::clone(self.shared), token, _t: PhantomData }
    }
}

impl<'a, Req: Payload, Prg: Payload, Rsp: Payload> Deref for TaskRequest<'a, Req, Prg, Rsp> {
    type Target = Req::Data<'a>;
    fn deref(&self) -> &Req::Data<'a> {
        &self.value
    }
}

/// A task call parked by [`TaskRequest::defer`], carried to any thread. Send progress any
/// number of times, then one answer: complete, fail or complete_cancelled consume it.
/// Dropping it unanswered answers AppError, so the caller never hangs.
#[must_use = "dropping a pending task answers AppError"]
pub struct PendingTask<Prg: Payload, Rsp: Payload> {
    shared: Arc<FnShared>,
    token: u64,
    _t: Marker<(Prg, Rsp)>,
}

impl<Prg: Payload, Rsp: Payload> PendingTask<Prg, Rsp> {
    /// One progress update to the caller and any observer. NoTopic once the definition or its
    /// node closed, State when the call is gone, Schema when the value did not encode.
    pub fn progress(&self, value: &Prg::Input) -> Result<(), SendError> {
        let p = self.shared.enter()?;
        // SAFETY: the pass keeps the handle alive, and a stale token answers State
        let rc = Prg::__with_bytes(self.shared.prg.as_deref(), value, |b| unsafe {
            ffi::rant_function_progress(p.raw, self.token, crate::schema::c_bytes(b))
        });
        SendError::check(rc.map_err(|_| SendError::Schema)?).map(|_| ())
    }

    /// True once the caller asked to cancel. Honor it with
    /// [`PendingTask::complete_cancelled`], or finish anyway.
    pub fn cancelled(&self) -> bool {
        match self.shared.enter() {
            // SAFETY: the pass keeps the handle alive
            Ok(p) => unsafe { ffi::rant_function_cancelled(p.raw, self.token) == 1 },
            Err(_) => false,
        }
    }

    /// The call's token, as [`TaskDefinition::on_cancel`] names it.
    pub fn token(&self) -> u64 {
        self.token
    }

    /// Answers Ok with `value`. A value that does not encode answers AppError and returns Schema.
    pub fn complete(mut self, value: &Rsp::Input) -> Result<(), SendError> {
        let token = std::mem::take(&mut self.token);
        self.shared.complete_with::<Rsp>(self.shared.rsp.as_deref(), token, ffi::RANT_CALL_OK, None, value)
    }

    /// Answers AppError with `message`.
    pub fn fail(mut self, message: &str) -> Result<(), SendError> {
        let token = std::mem::take(&mut self.token);
        self.shared.complete(token, ffi::RANT_CALL_APP_ERROR, Some(message), &[])
    }

    /// Honors a cancel: the caller's outcome is Cancelled with `message`, and `partial`, what
    /// got done, is the value it reads.
    pub fn complete_cancelled(mut self, message: &str, partial: Option<&Rsp::Input>) -> Result<(), SendError> {
        let token = std::mem::take(&mut self.token);
        match partial {
            Some(v) => self.shared.complete_with::<Rsp>(self.shared.rsp.as_deref(), token, ffi::RANT_CALL_CANCELLED, Some(message), v),
            None => self.shared.complete(token, ffi::RANT_CALL_CANCELLED, Some(message), &[]),
        }
    }
}

impl<Prg: Payload, Rsp: Payload> Drop for PendingTask<Prg, Rsp> {
    fn drop(&mut self) {
        if self.token != 0 {
            let _ = self.shared.complete(self.token, ffi::RANT_CALL_APP_ERROR, Some("the task was dropped unanswered"), &[]);
        }
    }
}

/// A typed task handler as the trampoline runs it.
fn adapt<Req: Payload, Prg: Payload, Rsp: Payload, R: IntoReply<Rsp>>(
    mut handler: impl for<'a> FnMut(TaskRequest<'a, Req, Prg, Rsp>) -> R + Send + 'static,
) -> Box<RequestFn> {
    Box::new(move |shared: &Arc<FnShared>, rq: *mut ffi::RantRequest| {
        // SAFETY: the C hands the request valid for this call
        let r = unsafe { &*rq };
        match Req::__data(shared.req.as_deref(), rbytes(r.data), r.schema) {
            Ok(value) => {
                let answer = handler(TaskRequest { value, raw: rq, shared, _t: PhantomData }).__reply();
                answer_request::<Rsp>(shared, rq, answer);
            }
            Err(why) => {
                fail_request(rq, &format!("the request did not decode: {why}"));
                shared.inner.report_decode(&shared.name, &why);
            }
        }
    })
}

impl Node {
    /// The implementation side of a task. The handler gets each [`TaskRequest`] where the
    /// node's callbacks run and must be quick: return the answer, or defer the work to a
    /// thread of your own. Under a queue in the options it runs at dispatch and may work in
    /// place. A panic answers AppError with its text.
    pub fn task_definition<Req: Payload, Prg: Payload, Rsp: Payload, R: IntoReply<Rsp>>(
        &self,
        name: &str,
        options: TaskOptions,
        handler: impl for<'a> FnMut(TaskRequest<'a, Req, Prg, Rsp>) -> R + Send + 'static,
    ) -> Result<TaskDefinition<Req, Prg, Rsp>, Error> {
        let what = format!("rant::Node::task_definition \"{name}\"");
        let (shared, schemas, co, cname) = self.task_parts::<Req, Prg, Rsp>(name, &options, &what)?;
        let b = RequestBox::new(&shared, adapt::<Req, Prg, Rsp, R>(handler));
        let (_pass, raw) = self.inner.enter_for(&what)?;
        // SAFETY: the pass keeps the node open, the schemas are node owned, and the box lives
        // in the node's registry from here until the handle retires or the node closes
        let f = unsafe {
            ffi::rant_node_create_task_definition(
                raw,
                cname.as_ptr(),
                schemas[0],
                schemas[1],
                schemas[2],
                Some(on_request),
                Arc::as_ptr(&b) as *mut c_void,
                &co,
            )
        };
        if f.is_null() {
            return Err(Error::last(raw, &what));
        }
        shared.keep(Box::new(b));
        shared.set_raw(f);
        Ok(TaskDefinition { shared, open: true, _t: PhantomData })
    }

    /// The caller side of a task another node defines.
    pub fn remote_task<Req: Payload, Prg: Payload, Rsp: Payload>(&self, name: &str, options: TaskOptions) -> Result<RemoteTask<Req, Prg, Rsp>, Error> {
        let what = format!("rant::Node::remote_task \"{name}\"");
        let (shared, schemas, co, cname) = self.task_parts::<Req, Prg, Rsp>(name, &options, &what)?;
        let (_pass, raw) = self.inner.enter_for(&what)?;
        // SAFETY: the pass keeps the node open, and the schemas are node owned
        let f = unsafe { ffi::rant_node_create_remote_task(raw, cname.as_ptr(), schemas[0], schemas[1], schemas[2], &co) };
        if f.is_null() {
            return Err(Error::last(raw, &what));
        }
        shared.set_raw(f);
        Ok(RemoteTask { shared, open: true, _t: PhantomData })
    }

    /// What both task factories prepare: the codecs, the C schemas, the options and the name.
    #[allow(clippy::type_complexity)]
    fn task_parts<Req: Payload, Prg: Payload, Rsp: Payload>(
        &self,
        name: &str,
        o: &TaskOptions,
        what: &str,
    ) -> Result<(Arc<FnShared>, [*const ffi::RantSchema; 3], ffi::RantTaskOpts, std::ffi::CString), Error> {
        let [rq, pg, rs] = o.schemas();
        same_node(&self.inner, o.queue.as_ref(), &[rq, pg, rs], what)?;
        let req = Req::__codec(&self.inner, rq, o.reflect_from_mesh, what)?;
        let prg = Prg::__codec(&self.inner, pg, o.reflect_from_mesh, what)?;
        let rsp = Rsp::__codec(&self.inner, rs, o.reflect_from_mesh, what)?;
        let schemas = [channel_schema(req.as_ref(), rq), channel_schema(prg.as_ref(), pg), channel_schema(rsp.as_ref(), rs)];
        let cname = c_name(name, what)?;
        Ok((FnShared::new(&self.inner, name, true, Codecs { req, rsp, prg }), schemas, o.to_c(self), cname))
    }
}
