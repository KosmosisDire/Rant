use std::cell::RefCell;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Mutex, PoisonError};

static NEXT_TAG: AtomicU64 = AtomicU64::new(1);

thread_local! {
    static TAG: u64 = NEXT_TAG.fetch_add(1, Ordering::Relaxed);
    // the nodes whose callbacks this thread runs now, innermost last
    #[allow(clippy::missing_const_for_thread_local)]
    static RUNNING: RefCell<Vec<usize>> = const { RefCell::new(Vec::new()) };
}

fn thread_tag() -> u64 {
    TAG.with(|t| *t)
}

/// True while this thread runs a callback of the node keyed by `node`.
pub(crate) fn in_callback_of(node: usize) -> bool {
    RUNNING.with(|r| r.borrow().contains(&node))
}

/// Why a callback did not run to its end.
pub(crate) enum Refused {
    /// It was called again from inside itself, which a FnMut cannot allow.
    Reentered,
    /// It panicked, with the panic's text.
    Panicked(String),
}

/// One app callback: never on two threads at once, never inside itself, and a panic never
/// unwinds into C.
pub(crate) struct Callback<F: ?Sized> {
    owner: AtomicU64,
    f: Mutex<Box<F>>,
}

impl<F: ?Sized> Callback<F> {
    pub(crate) fn new(f: Box<F>) -> Callback<F> {
        Callback { owner: AtomicU64::new(0), f: Mutex::new(f) }
    }

    /// Runs the callback through `call`, marking this thread as inside node's callbacks.
    pub(crate) fn run<R>(&self, node: usize, call: impl FnOnce(&mut F) -> R) -> Result<R, Refused> {
        let me = thread_tag();
        if self.owner.load(Ordering::Acquire) == me {
            return Err(Refused::Reentered);
        }
        let mut f = self.f.lock().unwrap_or_else(PoisonError::into_inner);
        self.owner.store(me, Ordering::Release);
        RUNNING.with(|r| r.borrow_mut().push(node));
        let result = catch_unwind(AssertUnwindSafe(|| call(&mut **f)));
        RUNNING.with(|r| r.borrow_mut().pop());
        self.owner.store(0, Ordering::Release);
        result.map_err(|panic| Refused::Panicked(panic_text(panic.as_ref())))
    }
}

/// A registry lock that refuses its own thread instead of deadlocking: a create holds it
/// across a C call, and an event the C fires inline may run a handler that creates again.
pub(crate) struct Registry<T> {
    owner: AtomicU64,
    m: Mutex<T>,
}

pub(crate) struct RegistryGuard<'a, T> {
    owner: &'a AtomicU64,
    guard: std::sync::MutexGuard<'a, T>,
}

impl<T> Registry<T> {
    pub(crate) fn new(value: T) -> Registry<T> {
        Registry { owner: AtomicU64::new(0), m: Mutex::new(value) }
    }

    /// None when this thread already holds it.
    pub(crate) fn enter(&self) -> Option<RegistryGuard<'_, T>> {
        let me = thread_tag();
        if self.owner.load(Ordering::Acquire) == me {
            return None;
        }
        let guard = self.m.lock().unwrap_or_else(PoisonError::into_inner);
        self.owner.store(me, Ordering::Release);
        Some(RegistryGuard { owner: &self.owner, guard })
    }
}

impl<T> std::ops::Deref for RegistryGuard<'_, T> {
    type Target = T;
    fn deref(&self) -> &T {
        &self.guard
    }
}

impl<T> std::ops::DerefMut for RegistryGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        &mut self.guard
    }
}

impl<T> Drop for RegistryGuard<'_, T> {
    fn drop(&mut self) {
        self.owner.store(0, Ordering::Release);
    }
}

pub(crate) fn panic_text(panic: &(dyn std::any::Any + Send)) -> String {
    if let Some(s) = panic.downcast_ref::<String>() {
        s.clone()
    } else if let Some(s) = panic.downcast_ref::<&str>() {
        (*s).to_owned()
    } else {
        "a panic with no text".to_owned()
    }
}
