use std::ffi::{CString, c_void};
use std::marker::PhantomData;
use std::ops::Deref;
use std::sync::Arc;
use std::time::Duration;

use crate::error::{Error, ErrorKind, SendError};
use crate::ffi;
use crate::message::{Bytes, Payload};
use crate::node::{Node, Queue};
use crate::options::{options, to_us};
use crate::pattern::{
    CallOptions, Codecs, FnShared, IntoReply, Marker, PendingCall, RequestBox, RequestFn, Response, answer_request, async_call, blocking_call,
    channel_schema, fail_request, on_request, queue_for, request_accessors, same_node,
};
use crate::schema::{Schema, rbytes, rstring_bytes, utf8_prefix};

options! {
    /// A function handle's options, a definition or a remote: `FunctionOptions::new().keep_last(20)`.
    /// Every option left out takes its default, and docs/patterns.md explains each.
    pub struct FunctionOptions {
        /// A remote's call timeout. Zero = 5 s.
        timeout: Duration = Duration::ZERO,
        /// How long a request or a reply pauses for a slow peer. Zero = 1 s.
        backpressure_wait: Duration = Duration::ZERO,
        /// Request and response history depth. 0 = 10. Raise it above the most requests one
        /// loop pass delivers, on both sides, or the replies past it are lost.
        keep_last: u16 = 0,
        /// Many definitions are intended, so the duplicate authority warning is off. A call
        /// with no provider reaches every one and the first answer wins.
        multi: bool = false,
        /// A [`Bytes`] handle made without schemas takes them from the mesh.
        reflect_from_mesh: bool = false,
    }
    hand {
        queue: Option<Queue> = None,
        request_schema: Option<Schema> = None,
        response_schema: Option<Schema> = None,
    }
}

impl FunctionOptions {
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

    /// The schema the response bytes of a [`Bytes`] handle follow.
    pub fn response_schema(mut self, schema: &Schema) -> Self {
        self.response_schema = Some(schema.clone());
        self
    }

    fn to_c(&self, node: &Node) -> ffi::RantFunctionOpts {
        ffi::RantFunctionOpts {
            backpressure_wait_us: to_us(self.backpressure_wait),
            timeout_us: to_us(self.timeout),
            keep_last: self.keep_last,
            reflect_from_mesh: self.reflect_from_mesh as u8,
            multi: self.multi as u8,
            queue: queue_for(&node.inner, self.queue.as_ref()),
        }
    }
}

/// The parts of a function or task handle every kind shares: its name, its match count, its
/// close, and the retire at drop.
macro_rules! fn_handle {
    ($ty:ident < $($p:ident),+ >, $what:literal) => {
        impl<$($p: Payload),+> $ty<$($p),+> {
            /// The name as given.
            pub fn name(&self) -> &str {
                &self.shared.name
            }

            /// The other side matched now: callers at a definition, providers at a remote.
            pub fn match_count(&self) -> usize {
                self.shared.match_count()
            }

            /// The request schema in use, None while untyped.
            pub fn request_schema(&self) -> Option<Schema> {
                self.shared.schema(ffi::rant_function_request_schema)
            }

            /// The response schema in use, None while untyped.
            pub fn response_schema(&self) -> Option<Schema> {
                self.shared.schema(ffi::rant_function_response_schema)
            }

            /// Retires the handle now, freeing the name, and answers every outstanding call
            /// Cancelled. State inside one of the node's inline callbacks, and the handle
            /// then stays until the node closes.
            pub fn close(mut self) -> Result<(), SendError> {
                self.open = false;
                self.shared.close()
            }
        }

        impl<$($p: Payload),+> Drop for $ty<$($p),+> {
            fn drop(&mut self) {
                if self.open && self.shared.close() == Err(SendError::State) {
                    self.shared.inner.report_close_refused(&self.shared.name);
                }
            }
        }

        impl<$($p: Payload),+> std::fmt::Debug for $ty<$($p),+> {
            fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                write!(f, "{}({:?})", $what, self.shared.name)
            }
        }
    };
}
pub(crate) use fn_handle;

/// The implementation side of a function, from [`Node::function_definition`]: the handler
/// answers every call. One definition per name on the mesh unless `multi`.
pub struct FunctionDefinition<Req: Payload, Rsp: Payload> {
    pub(crate) shared: Arc<FnShared>,
    pub(crate) open: bool,
    _t: Marker<(Req, Rsp)>,
}

fn_handle!(FunctionDefinition<Req, Rsp>, "FunctionDefinition");

impl FunctionDefinition<Bytes, Bytes> {
    /// A `reflect_from_mesh` handle: re types in place when the mesh moved. True = re typed.
    pub fn refresh(&self) -> Result<bool, SendError> {
        self.shared.refresh()
    }
}

/// The caller side of a function another node defines, from [`Node::remote_function`].
pub struct RemoteFunction<Req: Payload, Rsp: Payload> {
    pub(crate) shared: Arc<FnShared>,
    pub(crate) open: bool,
    _t: Marker<(Req, Rsp)>,
}

fn_handle!(RemoteFunction<Req, Rsp>, "RemoteFunction");

impl<Req: Payload, Rsp: Payload> RemoteFunction<Req, Rsp> {
    /// Calls and waits for the outcome, up to the options' timeout, else the handle's. It
    /// waits on the service thread or drives a Manual node's loop. Err when the request never
    /// left: NoTopic once closed, Schema when it did not encode, State from an inline
    /// callback, whose loop it would stall, where `call_async` is the tool.
    pub fn call(&self, request: &Req::Input, options: CallOptions) -> Result<Response<Rsp>, SendError> {
        blocking_call::<Req, Rsp>(&self.shared, request, &options, None)
    }

    /// Calls and returns at once with a Future of the outcome, which any executor can await.
    /// The outcome lands where the node's callbacks run, or at dispatch of the handle's queue.
    pub fn call_async(&self, request: &Req::Input, options: CallOptions) -> Result<PendingCall<Rsp>, SendError> {
        async_call::<Req, Rsp>(&self.shared, request, &options, None).map(|(state, _)| PendingCall { state })
    }
}

impl RemoteFunction<Bytes, Bytes> {
    /// A `reflect_from_mesh` handle: re types in place when the mesh moved, answering the
    /// outstanding calls Cancelled first. True = re typed.
    pub fn refresh(&self) -> Result<bool, SendError> {
        self.shared.refresh()
    }
}

/// One request as a function handler sees it, valid inside the handler: the value, through
/// `Deref`, plus who called when. Return the answer, or [`Request::defer`] it.
pub struct Request<'a, Req: Payload, Rsp: Payload> {
    value: Req::Data<'a>,
    raw: *mut ffi::RantRequest,
    shared: &'a Arc<FnShared>,
    _rsp: Marker<Rsp>,
}

impl<'a, Req: Payload, Rsp: Payload> Request<'a, Req, Rsp> {
    request_accessors!();

    /// The function's name on the mesh.
    pub fn function_name(&self) -> &'a str {
        utf8_prefix(rstring_bytes(self.raw().function_name))
    }

    /// The value itself.
    pub fn into_inner(self) -> Req::Data<'a> {
        self.value
    }

    /// Answers later: the returned [`Deferred`] completes the call from any thread, and the
    /// handler returns `()`.
    pub fn defer(self) -> Deferred<Rsp> {
        // SAFETY: the request is live for the handler
        let token = unsafe { ffi::rant_request_defer(self.raw) };
        Deferred { shared: Arc::clone(self.shared), token, _rsp: PhantomData }
    }
}

impl<'a, Req: Payload, Rsp: Payload> Deref for Request<'a, Req, Rsp> {
    type Target = Req::Data<'a>;
    fn deref(&self) -> &Req::Data<'a> {
        &self.value
    }
}

/// A function answer parked by [`Request::defer`], completed once from any thread. Dropping
/// it unanswered answers AppError, so the caller never waits for its timeout.
#[must_use = "dropping a deferred answer answers AppError"]
pub struct Deferred<Rsp: Payload> {
    shared: Arc<FnShared>,
    token: u64,
    _rsp: Marker<Rsp>,
}

impl<Rsp: Payload> Deferred<Rsp> {
    /// Answers Ok with `value`. NoTopic once the definition or its node closed, State when
    /// the call is gone, Schema when the value did not encode, which answers AppError.
    pub fn complete(mut self, value: &Rsp::Input) -> Result<(), SendError> {
        let token = std::mem::take(&mut self.token);
        if token == 0 {
            return Err(SendError::State);
        }
        self.shared.complete_with::<Rsp>(self.shared.rsp.as_deref(), token, ffi::RANT_CALL_OK, None, value)
    }

    /// Answers AppError with `message`.
    pub fn fail(mut self, message: &str) -> Result<(), SendError> {
        let token = std::mem::take(&mut self.token);
        if token == 0 {
            return Err(SendError::State);
        }
        self.shared.complete(token, ffi::RANT_CALL_APP_ERROR, Some(message), &[])
    }
}

impl<Rsp: Payload> Drop for Deferred<Rsp> {
    fn drop(&mut self) {
        if self.token != 0 {
            let _ = self.shared.complete(self.token, ffi::RANT_CALL_APP_ERROR, Some("the deferred answer was dropped unanswered"), &[]);
        }
    }
}

/// A typed request handler as the trampoline runs it.
fn adapt<Req: Payload, Rsp: Payload, R: IntoReply<Rsp>>(
    mut handler: impl for<'a> FnMut(Request<'a, Req, Rsp>) -> R + Send + 'static,
) -> Box<RequestFn> {
    Box::new(move |shared: &Arc<FnShared>, rq: *mut ffi::RantRequest| {
        // SAFETY: the C hands the request valid for this call
        let r = unsafe { &*rq };
        match Req::__data(shared.req.as_deref(), rbytes(r.data), r.schema) {
            Ok(value) => {
                let answer = handler(Request { value, raw: rq, shared, _rsp: PhantomData }).__reply();
                answer_request::<Rsp>(shared, rq, answer);
            }
            Err(why) => {
                fail_request(rq, &format!("the request did not decode: {why}"));
                shared.inner.report_decode(&shared.name, &why);
            }
        }
    })
}

/// The name as a C string, or BadName naming the call.
pub(crate) fn c_name(name: &str, what: &str) -> Result<CString, Error> {
    CString::new(name).map_err(|_| Error::new(ErrorKind::BadName, format!("{what}: the name holds a NUL")))
}

impl Node {
    /// The implementation side of a function. The handler gets each [`Request`] where the
    /// node's callbacks run, or at dispatch of the queue in the options, and its return
    /// value is the answer: a value answers Ok, an `Err` AppError with its text, and `()`
    /// leaves it to [`Request::defer`]. A panic answers AppError with its text.
    pub fn function_definition<Req: Payload, Rsp: Payload, R: IntoReply<Rsp>>(
        &self,
        name: &str,
        options: FunctionOptions,
        handler: impl for<'a> FnMut(Request<'a, Req, Rsp>) -> R + Send + 'static,
    ) -> Result<FunctionDefinition<Req, Rsp>, Error> {
        let what = format!("rant::Node::function_definition \"{name}\"");
        let o = &options;
        same_node(&self.inner, o.queue.as_ref(), &[o.request_schema.as_ref(), o.response_schema.as_ref()], &what)?;
        let req = Req::__codec(&self.inner, o.request_schema.as_ref(), o.reflect_from_mesh, &what)?;
        let rsp = Rsp::__codec(&self.inner, o.response_schema.as_ref(), o.reflect_from_mesh, &what)?;
        let cname = c_name(name, &what)?;
        let (req_s, rsp_s) = (channel_schema(req.as_ref(), o.request_schema.as_ref()), channel_schema(rsp.as_ref(), o.response_schema.as_ref()));
        let shared = FnShared::new(&self.inner, name, true, Codecs { req, rsp, prg: None });
        let b = RequestBox::new(&shared, adapt::<Req, Rsp, R>(handler));
        let co = o.to_c(self);
        let (_pass, raw) = self.inner.enter_for(&what)?;
        // SAFETY: the pass keeps the node open, the schemas are node owned, and the box lives
        // in the node's registry from here until the handle retires or the node closes
        let f = unsafe {
            ffi::rant_node_create_function_definition(raw, cname.as_ptr(), req_s, rsp_s, Some(on_request), Arc::as_ptr(&b) as *mut c_void, &co)
        };
        if f.is_null() {
            return Err(Error::last(raw, &what));
        }
        shared.keep(Box::new(b));
        shared.set_raw(f);
        Ok(FunctionDefinition { shared, open: true, _t: PhantomData })
    }

    /// The caller side of a function another node defines.
    pub fn remote_function<Req: Payload, Rsp: Payload>(&self, name: &str, options: FunctionOptions) -> Result<RemoteFunction<Req, Rsp>, Error> {
        let what = format!("rant::Node::remote_function \"{name}\"");
        let o = &options;
        same_node(&self.inner, o.queue.as_ref(), &[o.request_schema.as_ref(), o.response_schema.as_ref()], &what)?;
        let req = Req::__codec(&self.inner, o.request_schema.as_ref(), o.reflect_from_mesh, &what)?;
        let rsp = Rsp::__codec(&self.inner, o.response_schema.as_ref(), o.reflect_from_mesh, &what)?;
        let cname = c_name(name, &what)?;
        let (req_s, rsp_s) = (channel_schema(req.as_ref(), o.request_schema.as_ref()), channel_schema(rsp.as_ref(), o.response_schema.as_ref()));
        let shared = FnShared::new(&self.inner, name, true, Codecs { req, rsp, prg: None });
        let co = o.to_c(self);
        let (_pass, raw) = self.inner.enter_for(&what)?;
        // SAFETY: the pass keeps the node open, and the schemas are node owned
        let f = unsafe { ffi::rant_node_create_remote_function(raw, cname.as_ptr(), req_s, rsp_s, &co) };
        if f.is_null() {
            return Err(Error::last(raw, &what));
        }
        shared.set_raw(f);
        Ok(RemoteFunction { shared, open: true, _t: PhantomData })
    }
}
