use std::fmt;
use std::ops::Deref;
use std::str::FromStr;

/// A string of at most `N` bytes, the wire's `string<N>`. It lives inline, so a struct of
/// fixed fields stays one fixed message. A longer value is refused, never cut.
#[derive(Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[repr(C)]
pub struct String<const N: usize> {
    len: u16,
    data: [u8; N],
}

/// A value longer than the `N` bytes of a [`String<N>`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StringTooLong {
    message: std::string::String,
}

impl fmt::Display for StringTooLong {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

impl std::error::Error for StringTooLong {}

impl<const N: usize> String<N> {
    const FITS: () = assert!(N <= u16::MAX as usize, "rant::String capacity is at most 65535");

    /// The empty string.
    pub const fn new() -> Self {
        let () = Self::FITS;
        String { len: 0, data: [0; N] }
    }

    /// The string, or an error when it is longer than `N` bytes.
    pub fn from_str_checked(s: &str) -> Result<Self, StringTooLong> {
        let mut out = Self::new();
        if s.len() > N {
            return Err(StringTooLong { message: format!("rant::String<{N}>: \"{s}\" is {} bytes, over {N}", s.len()) });
        }
        out.data[..s.len()].copy_from_slice(s.as_bytes());
        out.len = s.len() as u16;
        Ok(out)
    }

    /// The text.
    pub fn as_str(&self) -> &str {
        // SAFETY: only from_str_checked fills data, with whole UTF-8
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
    type Error = StringTooLong;
    fn try_from(s: &str) -> Result<Self, StringTooLong> {
        Self::from_str_checked(s)
    }
}

impl<const N: usize> FromStr for String<N> {
    type Err = StringTooLong;
    fn from_str(s: &str) -> Result<Self, StringTooLong> {
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
