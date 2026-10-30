//! The standard types, the same names and wire in every language: the `rant-types` crate, which
//! holds no middleware. docs/stdtypes.md has the roster.

use crate::message::{Decoder, Encoder, Message, Spell, kind};

pub use rant_types::*;

/// A random version 4 uuid.
pub fn new_uuid() -> Uuid {
    let mut u = crate::ffi::RantUuid::default();
    // SAFETY: the C fills the 16 bytes
    unsafe { crate::ffi::rant_uuid_new(&mut u) };
    Uuid { bytes: u.bytes }
}

impl<const N: usize> Message for rant_types::String<N> {
    fn __spell(s: &mut Spell) {
        s.fixed_string(N)
    }
    fn __encode(&self, e: &mut Encoder<'_>) -> Result<(), std::string::String> {
        e.fixed_str(self.as_str().as_bytes(), N)
    }
    fn __decode(d: &mut Decoder<'_>) -> Result<Self, std::string::String> {
        let live = d.fixed_str(N)?;
        let text = std::str::from_utf8(live).map_err(|_| format!("a string<{N}> field is not UTF-8"))?;
        Self::from_str_checked(text).map_err(|e| e.to_string())
    }
}

macro_rules! standard_struct {
    ($name:ident { $( $f:ident $wire:literal : $t:ty ),* $(,)? }) => {
        impl Message for rant_types::$name {
            fn __spell(_s: &mut Spell) {
                _s.standard(stringify!($name), |_s| { $( _s.member::<$t>($wire); )* })
            }
            fn __encode(&self, _e: &mut Encoder<'_>) -> Result<(), std::string::String> {
                $( Message::__encode(&self.$f, _e)?; )*
                Ok(())
            }
            fn __decode(_d: &mut Decoder<'_>) -> Result<Self, std::string::String> {
                Ok(Self { $( $f: <$t as Message>::__decode(_d)?, )* })
            }
        }
    };
}

/// An alias type: spelled by its standard name, carried as its one field.
macro_rules! standard_alias {
    ($name:ident { $f:ident : $t:ty }) => {
        impl Message for rant_types::$name {
            fn __spell(s: &mut Spell) {
                s.standard_alias(stringify!($name), <$t as Message>::__spell)
            }
            fn __encode(&self, e: &mut Encoder<'_>) -> Result<(), std::string::String> {
                self.$f.__encode(e)
            }
            fn __decode(d: &mut Decoder<'_>) -> Result<Self, std::string::String> {
                Ok(Self { $f: <$t as Message>::__decode(d)? })
            }
        }
    };
}

macro_rules! standard_enum {
    ($name:ident : $backing:ident { $( $option:ident = $value:literal ),* $(,)? }) => {
        impl Message for rant_types::$name {
            fn __spell(s: &mut Spell) {
                s.standard_enum(stringify!($name), standard_enum!(@kind $backing))
            }
            fn __encode(&self, e: &mut Encoder<'_>) -> Result<(), std::string::String> {
                let v: $backing = match self { $( Self::$option => $value, )* };
                e.leaf(&v.to_le_bytes())
            }
            fn __decode(d: &mut Decoder<'_>) -> Result<Self, std::string::String> {
                let b = d.leaf(std::mem::size_of::<$backing>())?;
                let v = $backing::from_le_bytes(b.try_into().map_err(|_| "a short enum")?);
                $( if v == $value { return Ok(Self::$option); } )*
                Err(format!("{} is no option of {}", v, stringify!($name)))
            }
        }
    };
    (@kind u8) => { kind::U8 };
    (@kind u16) => { kind::U16 };
    (@kind u32) => { kind::U32 };
    (@kind u64) => { kind::U64 };
    (@kind i8) => { kind::I8 };
    (@kind i16) => { kind::I16 };
    (@kind i32) => { kind::I32 };
    (@kind i64) => { kind::I64 };
}

mod standard;

#[cfg(test)]
mod tests {
    use super::Timestamp;

    /// The types crate reads the clock with no middleware, and it must be the C library's clock.
    #[test]
    fn timestamp_now_is_the_clock_the_c_reads() {
        // SAFETY: a plain clock read
        let c = unsafe { crate::ffi::rant_timestamp_now() };
        assert!((Timestamp::now().us - c).abs() < 5_000_000);
    }
}
