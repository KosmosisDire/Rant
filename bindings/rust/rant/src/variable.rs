use std::borrow::Borrow;
use std::ffi::c_void;
use std::fmt;
use std::marker::PhantomData;
use std::ops::Deref;
use std::sync::Arc;
use std::time::Duration;

use crate::callback::{Callback, in_callback_of};
use crate::error::{Error, ErrorKind, SendError};
use crate::ffi;
use crate::function::c_name;
use crate::message::{Bytes, Codec, Payload};
use crate::node::{Node, NodeInner, Queue};
use crate::options::to_us;
use crate::pattern::{Handle, Marker, channel_schema, queue_for, same_node};
use crate::schema::{Schema, c_bytes, rbytes, rstring_bytes, utf8_prefix};

/// A variable handle's options, a definition or a remote:
/// `VariableOptions::new().initial(1.5).allow_force(true)`. Every option left out takes its
/// default, and docs/patterns.md explains each. initial, read_only and allow_force apply to a
/// definition only.
pub struct VariableOptions<T: Payload> {
    pub(crate) initial: Option<T::Owned>,
    pub(crate) read_only: bool,
    pub(crate) allow_force: bool,
    pub(crate) catch_up: u16,
    pub(crate) keep_last: u16,
    pub(crate) backpressure_wait: Duration,
    pub(crate) reflect_from_mesh: bool,
    pub(crate) queue: Option<Queue>,
    pub(crate) schema: Option<Schema>,
}

impl<T: Payload> VariableOptions<T> {
    /// Every option at its default.
    pub fn new() -> Self {
        VariableOptions {
            initial: None,
            read_only: false,
            allow_force: false,
            catch_up: 0,
            keep_last: 0,
            backpressure_wait: Duration::ZERO,
            reflect_from_mesh: false,
            queue: None,
            schema: None,
        }
    }

    /// The value before any set.
    pub fn initial(mut self, value: T::Owned) -> Self {
        self.initial = Some(value);
        self
    }

    /// No set channel: a remote's set answers BadRole.
    pub fn read_only(mut self, value: bool) -> Self {
        self.read_only = value;
        self
    }

    /// Permit force, here and from remotes. Off by default.
    pub fn allow_force(mut self, value: bool) -> Self {
        self.allow_force = value;
        self
    }

    /// The value channel's catch up. 0 = 1.
    pub fn catch_up(mut self, value: u16) -> Self {
        self.catch_up = value;
        self
    }

    /// Both channels' history depth, the repair window for a burst of writes. 0 = 10.
    pub fn keep_last(mut self, value: u16) -> Self {
        self.keep_last = value;
        self
    }

    /// How long a write pauses for a slow reader. Zero = never, and a slow reader may miss
    /// the writes between but always ends on the newest value.
    pub fn backpressure_wait(mut self, value: Duration) -> Self {
        self.backpressure_wait = value;
        self
    }

    /// A [`Bytes`] handle made without a schema takes the owner's from the mesh.
    pub fn reflect_from_mesh(mut self, value: bool) -> Self {
        self.reflect_from_mesh = value;
        self
    }

    /// Run on_change and on_write at [`Queue::dispatch`] of `queue` instead of where the
    /// node's callbacks run.
    pub fn queue(mut self, queue: &Queue) -> Self {
        self.queue = Some(queue.clone());
        self
    }

    /// The schema the bytes of a [`Bytes`] handle follow.
    pub fn schema(mut self, schema: &Schema) -> Self {
        self.schema = Some(schema.clone());
        self
    }
}

impl<T: Payload> Default for VariableOptions<T> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T: Payload> fmt::Debug for VariableOptions<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("VariableOptions")
            .field("initial", &self.initial.is_some())
            .field("read_only", &self.read_only)
            .field("allow_force", &self.allow_force)
            .field("catch_up", &self.catch_up)
            .field("keep_last", &self.keep_last)
            .field("backpressure_wait", &self.backpressure_wait)
            .field("reflect_from_mesh", &self.reflect_from_mesh)
            .finish()
    }
}

/// The state behind a variable handle.
struct VarShared {
    handle: Handle<ffi::RantVariable>,
    codec: Option<Arc<Codec>>,
}

impl Deref for VarShared {
    type Target = Handle<ffi::RantVariable>;
    fn deref(&self) -> &Handle<ffi::RantVariable> {
        &self.handle
    }
}

/// One applied write as an on_change or on_write handler sees it: the value, through
/// `Deref`, plus who wrote it when.
pub struct VariableUpdate<'a, T: Payload> {
    value: T::Data<'a>,
    raw: &'a ffi::RantVariableUpdate,
}

impl<'a, T: Payload> VariableUpdate<'a, T> {
    /// The value itself.
    pub fn into_inner(self) -> T::Data<'a> {
        self.value
    }

    /// The variable's name on the mesh.
    pub fn name(&self) -> &'a str {
        utf8_prefix(rstring_bytes(self.raw.name))
    }

    /// True when the value is a forced override.
    pub fn forced(&self) -> bool {
        self.raw.forced != 0
    }

    /// The owner's write counter.
    pub fn write_seq(&self) -> u32 {
        self.raw.write_seq
    }

    /// The peer the write came from, 0 for a write on this node.
    pub fn source(&self) -> u32 {
        self.raw.source
    }

    /// When the write applied, on this node's monotonic clock in microseconds.
    pub fn recv_us(&self) -> u64 {
        self.raw.recv_us
    }

    /// The writer's wall clock in UTC microseconds, 0 when it opted out.
    pub fn written_us(&self) -> u64 {
        self.raw.written_us
    }
}

impl<'a, T: Payload> Deref for VariableUpdate<'a, T> {
    type Target = T::Data<'a>;
    fn deref(&self) -> &T::Data<'a> {
        &self.value
    }
}

type UpdateFn = dyn FnMut(&NodeInner, &ffi::RantVariableUpdate) + Send;

struct UpdateBox {
    node: *const NodeInner,
    name: String,
    cb: Callback<UpdateFn>,
}

// SAFETY: node is read only, and the box lives inside that node's registry
unsafe impl Send for UpdateBox {}
unsafe impl Sync for UpdateBox {}

unsafe extern "C" fn on_update(u: *const ffi::RantVariableUpdate, user: *mut c_void) {
    // SAFETY: user is an UpdateBox the node keeps until the handle retires, and the update is
    // valid for this call
    let (b, u) = unsafe { (&*(user as *const UpdateBox), &*u) };
    let inner = unsafe { &*b.node };
    if let Err(refused) = b.cb.run(inner.key(), |f| f(inner, u)) {
        inner.report_refused(Some(&b.name), refused);
    }
}

/// The surface a definition and a remote share: reads, writes, force, the two events.
macro_rules! var_handle {
    ($ty:ident, $what:literal) => {
        impl<T: Payload> $ty<T> {
            /// The name as given.
            pub fn name(&self) -> &str {
                &self.shared.name
            }

            /// The current value, read locally: the owner's own, or the latest a remote
            /// received. None while there is none, or once closed. SchemaMismatch when it does
            /// not decode.
            pub fn get(&self) -> Result<Option<T::Owned>, Error> {
                self.shared.get::<T>()
            }

            /// A write: applied and published by a definition, sent to the owner by a remote.
            /// NoTopic when no owner matched, BadRole when the owner is read only, Schema when
            /// the value does not encode.
            pub fn set(&self, value: &T::Input) -> Result<(), SendError> {
                self.shared.write::<T>(value, ffi::rant_variable_set)
            }

            /// Overrides the value until [`Self::unforce`]. Writes meanwhile are held, and
            /// unforce restores the latest. Needs `allow_force` on the definition: State on a
            /// definition without it, BadRole on a remote whose owner refuses it.
            pub fn force(&self, value: &T::Input) -> Result<(), SendError> {
                self.shared.write::<T>(value, ffi::rant_variable_force)
            }

            /// Ends a force and restores the latest write.
            pub fn unforce(&self) -> Result<(), SendError> {
                let p = self.shared.enter()?;
                // SAFETY: the pass keeps the handle alive
                SendError::check(unsafe { ffi::rant_variable_unforce(p.raw) }).map(|_| ())
            }

            /// True while a force is in effect.
            pub fn forced(&self) -> bool {
                // SAFETY: the pass keeps the handle alive
                self.shared.enter().is_ok_and(|p| unsafe { ffi::rant_variable_forced(p.raw) } == 1)
            }

            /// Waits until a value exists, up to `timeout` ([`FOREVER`](crate::FOREVER) waits
            /// with no end). False when the time ran out, inside an inline callback, or once closed.
            pub fn wait(&self, timeout: Duration) -> bool {
                self.shared.wait(timeout)
            }

            /// The other side matched now: remotes at a definition, the owner at a remote.
            pub fn match_count(&self) -> usize {
                // SAFETY: the pass keeps the handle alive
                self.shared.enter().map_or(0, |p| unsafe { ffi::rant_variable_match_count(p.raw) }.max(0) as usize)
            }

            /// Runs `handler` when the value really changes: a first value, different bytes or a
            /// force flipping. The current value replays at once. It runs where the node's
            /// callbacks run, or at dispatch of the queue in the options. A later call replaces it.
            pub fn on_change(&self, handler: impl for<'a> FnMut(VariableUpdate<'a, T>) + Send + 'static) -> Result<(), SendError> {
                self.shared.observe::<T>(handler, ffi::rant_variable_on_change)
            }

            /// Runs `handler` on every applied write, the same value again included, as
            /// [`Self::on_change`] runs its own. No replay.
            pub fn on_write(&self, handler: impl for<'a> FnMut(VariableUpdate<'a, T>) + Send + 'static) -> Result<(), SendError> {
                self.shared.observe::<T>(handler, ffi::rant_variable_on_write)
            }

            /// The schema in use, None while untyped.
            pub fn schema(&self) -> Option<Schema> {
                let p = self.shared.enter().ok()?;
                // SAFETY: the pass keeps the handle alive, and its schema lives with the node
                let s = unsafe { ffi::rant_variable_schema(p.raw) };
                (!s.is_null()).then(|| Schema::new(Arc::clone(&self.shared.inner), s))
            }

            /// Retires the handle now, freeing the name. State inside one of the node's inline
            /// callbacks, and the handle then stays until the node closes.
            pub fn close(mut self) -> Result<(), SendError> {
                self.open = false;
                self.shared.close()
            }
        }

        impl $ty<Bytes> {
            /// A `reflect_from_mesh` handle: re types in place when the mesh moved. True = re typed.
            pub fn refresh(&self) -> Result<bool, SendError> {
                let p = self.shared.enter()?;
                // SAFETY: the pass keeps the handle alive
                Ok(SendError::check(unsafe { ffi::rant_variable_refresh(p.raw) })? == 1)
            }
        }

        impl<T: Payload> Drop for $ty<T> {
            fn drop(&mut self) {
                if self.open && self.shared.close() == Err(SendError::State) {
                    self.shared.inner.report_close_refused(&self.shared.name);
                }
            }
        }

        impl<T: Payload> fmt::Debug for $ty<T> {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                write!(f, "{}({:?})", $what, self.shared.name)
            }
        }
    };
}

/// A variable this node owns, from [`Node::variable_definition`]: it holds the value,
/// publishes every applied write and takes sets from remotes unless read only.
pub struct VariableDefinition<T: Payload> {
    shared: Arc<VarShared>,
    open: bool,
    _t: Marker<T>,
}

var_handle!(VariableDefinition, "VariableDefinition");

/// A variable another node owns, from [`Node::remote_variable`]: reads see the latest value
/// it published, and writes go to the owner and come back as a change.
pub struct RemoteVariable<T: Payload> {
    shared: Arc<VarShared>,
    open: bool,
    _t: Marker<T>,
}

var_handle!(RemoteVariable, "RemoteVariable");

type WriteFn = unsafe extern "C" fn(*mut ffi::RantVariable, ffi::RantBytes) -> i32;
type ObserveFn = unsafe extern "C" fn(*mut ffi::RantVariable, ffi::RantVariableUpdateFn, *mut c_void) -> i32;

impl VarShared {
    fn get<T: Payload>(&self) -> Result<Option<T::Owned>, Error> {
        let Ok(p) = self.enter() else { return Ok(None) };
        let node = self.inner.raw();
        let mut out = ffi::RantBytes::default();
        // SAFETY: the pass keeps the handle alive, and the node lock keeps the stored value
        // still while it is decoded, since another thread's write may move it
        let value = unsafe {
            ffi::rant_node_lock(node);
            let has = ffi::rant_variable_get(p.raw, &mut out) == 1;
            let value = if has { T::__owned(self.codec.as_deref(), rbytes(out), std::ptr::null()).map(Some) } else { Ok(None) };
            ffi::rant_node_unlock(node);
            value
        };
        value.map_err(|why| Error::new(ErrorKind::SchemaMismatch, format!("rant::Variable::get \"{}\": the value did not decode: {why}", self.name)))
    }

    fn write<T: Payload>(&self, value: &T::Input, op: WriteFn) -> Result<(), SendError> {
        let p = self.enter()?;
        // SAFETY: the pass keeps the handle alive, and the bytes outlive the call
        let rc = T::__with_bytes(self.codec.as_deref(), value, |b| unsafe { op(p.raw, c_bytes(b)) });
        SendError::check(rc.map_err(|_| SendError::Schema)?).map(|_| ())
    }

    fn wait(&self, timeout: Duration) -> bool {
        // the C answers at once from a callback, which would spin the slices below
        if in_callback_of(self.inner.key()) {
            // SAFETY: the pass keeps the handle alive
            return self.enter().is_ok_and(|p| unsafe { ffi::rant_variable_wait(p.raw, 0) } == 1);
        }
        let waited = self.inner.wait_sliced(timeout, |_, ms| match self.enter() {
            // SAFETY: the pass keeps the handle alive
            Ok(p) => unsafe { ffi::rant_variable_wait(p.raw, ms) },
            Err(_) => ffi::RANT_ERR_NO_TOPIC,
        });
        waited == Ok(1)
    }

    fn observe<T: Payload>(&self, mut handler: impl for<'a> FnMut(VariableUpdate<'a, T>) + Send + 'static, op: ObserveFn) -> Result<(), SendError> {
        let p = self.enter()?;
        let codec = self.codec.clone();
        let name = self.name.clone();
        let adapter = move |inner: &NodeInner, u: &ffi::RantVariableUpdate| match T::__data(codec.as_deref(), rbytes(u.value), u.schema) {
            Ok(value) => handler(VariableUpdate { value, raw: u }),
            Err(why) => inner.report_decode(&name, &format!("a variable update did not decode: {why}")),
        };
        let b = Arc::new(UpdateBox { node: Arc::as_ptr(&self.inner), name: self.name.clone(), cb: Callback::new(Box::new(adapter)) });
        // SAFETY: the pass keeps the handle alive, and the box lives until it retires, since
        // the C may still run a replaced handler on another thread
        let rc = unsafe { op(p.raw, Some(on_update), Arc::as_ptr(&b) as *mut c_void) };
        SendError::check(rc)?;
        self.keep(Box::new(b));
        Ok(())
    }
}

impl Node {
    /// A variable this node owns: it holds the value and publishes every applied write.
    pub fn variable_definition<T: Payload>(&self, name: &str, options: VariableOptions<T>) -> Result<VariableDefinition<T>, Error> {
        let what = format!("rant::Node::variable_definition \"{name}\"");
        let (shared, schema, mut co, cname) = self.variable_parts(name, &options, &what)?;
        co.access = if options.read_only { ffi::RANT_VAR_READONLY as u8 } else { ffi::RANT_VAR_READWRITE as u8 };
        co.allow_force = options.allow_force as u8;
        let (_pass, raw) = self.inner.enter_for(&what)?;
        let mut create = |initial: &[u8]| {
            co.initial = c_bytes(initial);
            // SAFETY: the pass keeps the node open, the schema is node owned, and open copies the initial value
            unsafe { ffi::rant_node_create_variable_definition(raw, cname.as_ptr(), schema, &co) }
        };
        let v = match &options.initial {
            Some(v) => T::__with_bytes(shared.codec.as_deref(), v.borrow(), create)
                .map_err(|why| Error::new(ErrorKind::SchemaMismatch, format!("{what}: the initial value did not encode: {why}")))?,
            None => create(&[]),
        };
        if v.is_null() {
            return Err(Error::last(raw, &what));
        }
        shared.set_raw(v);
        Ok(VariableDefinition { shared, open: true, _t: PhantomData })
    }

    /// A variable another node owns: reads see the latest value, writes go to the owner.
    pub fn remote_variable<T: Payload>(&self, name: &str, options: VariableOptions<T>) -> Result<RemoteVariable<T>, Error> {
        let what = format!("rant::Node::remote_variable \"{name}\"");
        let (shared, schema, co, cname) = self.variable_parts(name, &options, &what)?;
        let (_pass, raw) = self.inner.enter_for(&what)?;
        // SAFETY: the pass keeps the node open, and the schema is node owned
        let v = unsafe { ffi::rant_node_create_remote_variable(raw, cname.as_ptr(), schema, &co) };
        if v.is_null() {
            return Err(Error::last(raw, &what));
        }
        shared.set_raw(v);
        Ok(RemoteVariable { shared, open: true, _t: PhantomData })
    }

    /// What both variable factories prepare: the codec, the C schema, the options and the name.
    #[allow(clippy::type_complexity)]
    fn variable_parts<T: Payload>(
        &self,
        name: &str,
        o: &VariableOptions<T>,
        what: &str,
    ) -> Result<(Arc<VarShared>, *const ffi::RantSchema, ffi::RantVariableOpts, std::ffi::CString), Error> {
        same_node(&self.inner, o.queue.as_ref(), &[o.schema.as_ref()], what)?;
        let codec = T::__codec(&self.inner, o.schema.as_ref(), o.reflect_from_mesh, what)?;
        let schema = channel_schema(codec.as_ref(), o.schema.as_ref());
        let co = ffi::RantVariableOpts {
            catch_up: o.catch_up,
            keep_last: o.keep_last,
            backpressure_wait_us: to_us(o.backpressure_wait),
            reflect_from_mesh: o.reflect_from_mesh as u8,
            queue: queue_for(&self.inner, o.queue.as_ref()),
            ..Default::default()
        };
        let cname = c_name(name, what)?;
        Ok((Arc::new(VarShared { handle: Handle::new(&self.inner, name, true), codec }), schema, co, cname))
    }
}
