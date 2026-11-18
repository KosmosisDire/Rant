use std::time::Duration;

use crate::ffi;
use crate::node::Queue;
use crate::schema::Schema;

/// An options type: private fields, `new()` with every default, and one chained setter per
/// option. Fields under `hand` get their setters written by hand.
macro_rules! options {
    (
        $(#[$meta:meta])*
        pub struct $name:ident {
            $( $(#[$fmeta:meta])* $field:ident : $ty:ty = $default:expr, )*
        }
        $( hand { $( $hfield:ident : $hty:ty = $hdefault:expr, )* } )?
    ) => {
        $(#[$meta])*
        #[derive(Clone, Debug)]
        pub struct $name {
            $( pub(crate) $field: $ty, )*
            $( $( pub(crate) $hfield: $hty, )* )?
        }

        impl $name {
            /// Every option at its default.
            pub fn new() -> Self {
                Self { $( $field: $default, )* $( $( $hfield: $hdefault, )* )? }
            }
            $(
                $(#[$fmeta])*
                pub fn $field(mut self, value: $ty) -> Self {
                    self.$field = value;
                    self
                }
            )*
        }

        impl Default for $name {
            fn default() -> Self {
                Self::new()
            }
        }
    };
}
pub(crate) use options;

options! {
    /// A topic's options: `Qos::new().reliable(true).keep_last(10)`. Every option left out
    /// takes its default, and docs/topics.md explains each.
    pub struct Qos {
        /// Repair gaps and apply backpressure. A reliable subscriber refuses a best effort
        /// publisher. Off by default.
        reliable: bool = false,
        /// Recent messages retained for late joiners and repair. 0 = 1, or 10 when reliable.
        keep_last: u16 = 0,
        /// Recent messages a new subscriber gets at once. 0 = only what is sent later.
        catch_up: u16 = 0,
        /// A size hint that pins the same host shared memory class, never a cap.
        max_message_bytes: u32 = 0,
        /// The idle ping of a reliable publisher. Zero = 250 ms.
        heartbeat: Duration = Duration::ZERO,
        /// The bound on a reliable reader's re ask. Zero = adaptive from the round trip.
        repair_delay: Duration = Duration::ZERO,
        /// How long a reliable send pauses for a slow subscriber. Zero = never.
        backpressure_wait: Duration = Duration::ZERO,
        /// Pin the topic to one shared memory size class. 0 = chosen per message.
        shm_max_bytes: u32 = 0,
        /// The ring cap of a queued or pulled handle. 0 = grows lazily up to 1 MB.
        queue_bytes: u32 = 0,
        /// Best effort subscriber: deliveries kept per second per publisher. 0 = all.
        max_rate_hz: u16 = 0,
        /// Publisher: send no source time, so readers see a written time of 0.
        no_timestamp: bool = false,
        /// A [`Bytes`](crate::Bytes) handle made without a schema takes its type from the
        /// mesh: a reader its provider's, a writer the widest every reader accepts.
        reflect_from_mesh: bool = false,
    }
    hand {
        queue: Option<Queue> = None,
        schema: Option<Schema> = None,
    }
}

impl Qos {
    /// Run this handle's callbacks at [`Queue::dispatch`] of `queue` instead of where the
    /// node's callbacks run. Handles on one name share one slot and must agree.
    pub fn queue(mut self, queue: &Queue) -> Self {
        self.queue = Some(queue.clone());
        self
    }

    /// The schema the bytes of a [`Bytes`](crate::Bytes) handle follow. A typed handle
    /// takes its schema from its type and refuses this.
    pub fn schema(mut self, schema: &Schema) -> Self {
        self.schema = Some(schema.clone());
        self
    }

    pub(crate) fn to_c(&self) -> ffi::RantQos {
        ffi::RantQos {
            reliability: if self.reliable { ffi::RANT_RELIABLE } else { ffi::RANT_BEST_EFFORT },
            keep_last: self.keep_last,
            catch_up: self.catch_up,
            max_message_bytes: self.max_message_bytes,
            heartbeat_us: to_us(self.heartbeat),
            repair_delay_us: to_us(self.repair_delay),
            backpressure_wait_us: to_us(self.backpressure_wait),
            shm_max_bytes: self.shm_max_bytes,
            queue_bytes: self.queue_bytes,
            max_rate_hz: self.max_rate_hz,
            no_timestamp: self.no_timestamp as u8,
        }
    }
}

/// A duration option in C microseconds, saturating.
pub(crate) fn to_us(d: Duration) -> u32 {
    u32::try_from(d.as_micros()).unwrap_or(u32::MAX)
}

/// A timeout in C milliseconds: FOREVER is -1, and a nonzero wait never rounds to a check.
pub(crate) fn to_ms(d: Duration) -> i32 {
    if d == Duration::MAX {
        return -1;
    }
    let ms = d.as_millis();
    if ms == 0 && !d.is_zero() { 1 } else { i32::try_from(ms).unwrap_or(i32::MAX) }
}
