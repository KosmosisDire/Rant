use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering::SeqCst};
use std::sync::{Condvar, Mutex, PoisonError};
use std::time::Duration;

/// Keeps a C node, or one of its pattern handles, alive while any call is inside it. A close
/// frees it and assumes no other thread is in a call, so every call holds a Pass.
pub(crate) struct Gate {
    active: AtomicUsize,
    closing: AtomicBool,
    lock: Mutex<()>,
    idle: Condvar,
}

/// One call's hold on the node, released at drop.
pub(crate) struct Pass<'a>(&'a Gate);

impl Gate {
    pub(crate) const fn new() -> Gate {
        Gate { active: AtomicUsize::new(0), closing: AtomicBool::new(false), lock: Mutex::new(()), idle: Condvar::new() }
    }

    /// None once close began. Never blocks.
    pub(crate) fn enter(&self) -> Option<Pass<'_>> {
        // count first, then check, so close never misses a call that slipped in
        self.active.fetch_add(1, SeqCst);
        if self.closing.load(SeqCst) {
            self.leave();
            return None;
        }
        Some(Pass(self))
    }

    pub(crate) fn is_closing(&self) -> bool {
        self.closing.load(SeqCst)
    }

    /// Refuses new passes and waits until none is out. False when close already began.
    pub(crate) fn close(&self) -> bool {
        if self.closing.swap(true, SeqCst) {
            return false;
        }
        let mut guard = self.lock.lock().unwrap_or_else(PoisonError::into_inner);
        while self.active.load(SeqCst) != 0 {
            guard = self.idle.wait_timeout(guard, Duration::from_millis(50)).unwrap_or_else(PoisonError::into_inner).0;
        }
        true
    }

    /// Takes passes again after a close the C refused.
    pub(crate) fn reopen(&self) {
        self.closing.store(false, SeqCst);
    }

    fn leave(&self) {
        if self.active.fetch_sub(1, SeqCst) == 1 && self.closing.load(SeqCst) {
            let _guard = self.lock.lock().unwrap_or_else(PoisonError::into_inner);
            self.idle.notify_all();
        }
    }
}

impl Drop for Pass<'_> {
    fn drop(&mut self) {
        self.0.leave();
    }
}
