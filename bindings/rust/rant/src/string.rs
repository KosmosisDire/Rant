use std::fmt;
use std::ops::Deref;
use std::str::FromStr;

use crate::error::{Error, ErrorKind};
use crate::message::{Decoder, Encoder, Message, Spell};

/// A string of at most `N` bytes, the wire's `string<N>`. It lives inline, so a struct of
/// fixed fields stays one fixed message. A longer value is refused, never cut.
#[derive(Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct String<const N: usize> {
    len: u16,
    data: [u8; N],
}

impl<const N: usize> String<N> {
    const FITS: () = assert!(N <= u16::MAX as usize, "rant::String capacity is at most 65535");

    /// The empty string.
    pub const fn new() -> Self {
        let () = Self::FITS;
        String { len: 0, data: [0; N] }
    }

    /// The string, or an error when it is longer than `N` bytes.
    pub fn from_str_checked(s: &str) -> Result<Self, Error> {
        let mut out = Self::new();
        if s.len() > N {
            return Err(Error::new(ErrorKind::None, format!("rant::String<{N}>: \"{s}\" is {} bytes, over {N}", s.len())));
        }
        out.data[..s.len()].copy_from_slice(s.as_bytes());
        out.len = s.len() as u16;
        Ok(out)
    }

    /// The text.
    pub fn as_str(&self) -> &str {
        // SAFETY: only from_str_checked and a checked decode fill data, both with whole UTF-8
        unsafe { std::str::from_utf8_unchecked(&self.data[..self.len as usize]) }
    }

    /// The capacity N.
    pub const fn capacity(&self) -> usize {
        N
    }
}

impl<const N: usize> Default for String<N> {
    fn default() -> Self {
        Self::new()
    }
}

impl<const N: usize> Deref for String<N> {
    type Target = str;
    fn deref(&self) -> &str {
        self.as_str()
    }
}

impl<const N: usize> TryFrom<&str> for String<N> {
    type Error = Error;
    fn try_from(s: &str) -> Result<Self, Error> {
        Self::from_str_checked(s)
    }
}

impl<const N: usize> FromStr for String<N> {
    type Err = Error;
    fn from_str(s: &str) -> Result<Self, Error> {
        Self::from_str_checked(s)
    }
}

impl<const N: usize> PartialEq<str> for String<N> {
    fn eq(&self, other: &str) -> bool {
        self.as_str() == other
    }
}

impl<const N: usize> PartialEq<&str> for String<N> {
    fn eq(&self, other: &&str) -> bool {
        self.as_str() == *other
    }
}

impl<const N: usize> fmt::Display for String<N> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

impl<const N: usize> fmt::Debug for String<N> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Debug::fmt(self.as_str(), f)
    }
}

impl<const N: usize> Message for String<N> {
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
