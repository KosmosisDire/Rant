use std::ffi::CStr;
use std::fmt;

use crate::ffi;

/// The fault an [`Error`] or an error [`Event`](crate::Event) carries, the C `RANT_E_*` list.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub enum ErrorKind {
    /// No C fault: a refusal of this binding, a call that did not end Ok, or a handler that
    /// panicked.
    None,
    /// A peer's topic name hashes to ours but differs, or a live same name entity exists.
    NameCollision,
    /// A reliable subscriber met a best effort publisher.
    QosIncompatible,
    /// The name is another entity kind at a peer.
    KindMismatch,
    /// A peer's schema cannot be read, or a payload did not decode into the handle's type.
    SchemaMismatch,
    /// A peer's index map failed to allocate.
    InterestOverflow,
    /// Our announce overflowed, so peers see no topics.
    MetaTruncatedInterest,
    /// A peer's announce exceeds our buffer.
    PeerMetaTooBig,
    /// A received message could not be buffered.
    MessageTooBig,
    /// The peer table is full of active peers.
    PeerRefused,
    /// A send overwrote history nobody had received yet.
    EvictedUnsent,
    /// A send reached nobody while a match was still resolving.
    UnmatchedSend,
    /// A peer also claims the handler side of an entity.
    DuplicateAuthority,
    /// Out of memory.
    Oom,
    /// The platform network layer failed to start.
    Platform,
    /// Opening a socket failed.
    Socket,
    /// Binding a port failed, it is in use.
    Bind,
    /// Joining the discovery group failed.
    McastJoin,
    /// A send failed hard.
    Send,
    /// A receive failed hard.
    Recv,
    /// The socket wait failed.
    Poll,
    /// No cross thread waker, so wakes come at the next tick.
    Waker,
    /// A configured address could not be parsed.
    BadAddress,
    /// A name is empty, too long or holds '@'.
    BadName,
    /// Refused at this moment, such as a close inside the handle's own callback.
    State,
    /// Schema text or wire that does not compile.
    BadSchema,
    /// RANT_DOMAIN is not a number from 0 to 65535.
    BadDomain,
    /// The prefix holds '@', starts or ends with '/' or is too long.
    BadPrefix,
    /// A fault this binding does not know yet.
    Unknown,
}

impl ErrorKind {
    pub(crate) fn from_c(e: ffi::RantErrorKind) -> ErrorKind {
        use ErrorKind::*;
        match e {
            ffi::RANT_E_NONE => None,
            ffi::RANT_E_NAME_COLLISION => NameCollision,
            ffi::RANT_E_QOS_INCOMPATIBLE => QosIncompatible,
            ffi::RANT_E_KIND_MISMATCH => KindMismatch,
            ffi::RANT_E_SCHEMA_MISMATCH => SchemaMismatch,
            ffi::RANT_E_INTEREST_OVERFLOW => InterestOverflow,
            ffi::RANT_E_META_TRUNCATED_INTEREST => MetaTruncatedInterest,
            ffi::RANT_E_PEER_META_TOO_BIG => PeerMetaTooBig,
            ffi::RANT_E_MSG_TOO_BIG => MessageTooBig,
            ffi::RANT_E_PEER_REFUSED => PeerRefused,
            ffi::RANT_E_EVICTED_UNSENT => EvictedUnsent,
            ffi::RANT_E_UNMATCHED_SEND => UnmatchedSend,
            ffi::RANT_E_DUPLICATE_AUTHORITY => DuplicateAuthority,
            ffi::RANT_E_OOM => Oom,
            ffi::RANT_E_PLATFORM => Platform,
            ffi::RANT_E_SOCKET => Socket,
            ffi::RANT_E_BIND => Bind,
            ffi::RANT_E_MCAST_JOIN => McastJoin,
            ffi::RANT_E_SEND => Send,
            ffi::RANT_E_RECV => Recv,
            ffi::RANT_E_POLL => Poll,
            ffi::RANT_E_WAKER => Waker,
            ffi::RANT_E_BAD_ADDRESS => BadAddress,
            ffi::RANT_E_BAD_NAME => BadName,
            ffi::RANT_E_STATE => State,
            ffi::RANT_E_BAD_SCHEMA => BadSchema,
            ffi::RANT_E_BAD_DOMAIN => BadDomain,
            ffi::RANT_E_BAD_PREFIX => BadPrefix,
            _ => Unknown,
        }
    }

    pub(crate) fn to_c(self) -> ffi::RantErrorKind {
        use ErrorKind::*;
        match self {
            None | Unknown => ffi::RANT_E_NONE,
            NameCollision => ffi::RANT_E_NAME_COLLISION,
            QosIncompatible => ffi::RANT_E_QOS_INCOMPATIBLE,
            KindMismatch => ffi::RANT_E_KIND_MISMATCH,
            SchemaMismatch => ffi::RANT_E_SCHEMA_MISMATCH,
            InterestOverflow => ffi::RANT_E_INTEREST_OVERFLOW,
            MetaTruncatedInterest => ffi::RANT_E_META_TRUNCATED_INTEREST,
            PeerMetaTooBig => ffi::RANT_E_PEER_META_TOO_BIG,
            MessageTooBig => ffi::RANT_E_MSG_TOO_BIG,
            PeerRefused => ffi::RANT_E_PEER_REFUSED,
            EvictedUnsent => ffi::RANT_E_EVICTED_UNSENT,
            UnmatchedSend => ffi::RANT_E_UNMATCHED_SEND,
            DuplicateAuthority => ffi::RANT_E_DUPLICATE_AUTHORITY,
            Oom => ffi::RANT_E_OOM,
            Platform => ffi::RANT_E_PLATFORM,
            Socket => ffi::RANT_E_SOCKET,
            Bind => ffi::RANT_E_BIND,
            McastJoin => ffi::RANT_E_MCAST_JOIN,
            Send => ffi::RANT_E_SEND,
            Recv => ffi::RANT_E_RECV,
            Poll => ffi::RANT_E_POLL,
            Waker => ffi::RANT_E_WAKER,
            BadAddress => ffi::RANT_E_BAD_ADDRESS,
            BadName => ffi::RANT_E_BAD_NAME,
            State => ffi::RANT_E_STATE,
            BadSchema => ffi::RANT_E_BAD_SCHEMA,
            BadDomain => ffi::RANT_E_BAD_DOMAIN,
            BadPrefix => ffi::RANT_E_BAD_PREFIX,
        }
    }
}

/// A failed node open, handle factory or schema compile, or a refused take. The text names
/// the call and the reason.
#[derive(Clone, Debug)]
pub struct Error {
    kind: ErrorKind,
    os_error: i32,
    text: String,
}

impl Error {
    pub(crate) fn new(kind: ErrorKind, text: impl Into<String>) -> Error {
        Error { kind, os_error: 0, text: text.into() }
    }

    /// The node's last error, or the process slot's when node is null, after `what` failed.
    pub(crate) fn last(node: *mut ffi::RantNode, what: &str) -> Error {
        // SAFETY: a null node reads the process slot, a live one is held open by the caller
        let ev = unsafe { ffi::rant_last_error(node) };
        Error { kind: ErrorKind::from_c(ev.error), os_error: ev.os_error, text: format!("{what}: {}", event_text(&ev)) }
    }

    /// The fault.
    pub fn kind(&self) -> ErrorKind {
        self.kind
    }

    /// The OS error code of a socket fault, else 0.
    pub fn os_error(&self) -> i32 {
        self.os_error
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.text)
    }
}

impl std::error::Error for Error {}

impl From<SendError> for Error {
    fn from(e: SendError) -> Error {
        let kind = if e == SendError::Schema { ErrorKind::SchemaMismatch } else { ErrorKind::State };
        Error::new(kind, e.to_string())
    }
}

/// A refused send, set or other data path call: the C `RantResult` codes.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub enum SendError {
    /// A closed handle or node, or nobody matched within the match wait.
    NoTopic,
    /// Over the largest message the wire carries.
    TooBig,
    /// The handle or its peer has no such side, such as a set on a read only variable.
    BadRole,
    /// Out of memory.
    OutOfMemory,
    /// Refused at this moment, such as a blocking call from an inline callback.
    State,
    /// Switched off on this node, such as logs.
    NoSys,
    /// The value does not fit the schema.
    Schema,
}

impl SendError {
    /// Ok for a C return of 0 or more.
    pub(crate) fn check(rc: i32) -> Result<i32, SendError> {
        match rc {
            rc if rc >= 0 => Ok(rc),
            ffi::RANT_ERR_NO_TOPIC => Err(SendError::NoTopic),
            ffi::RANT_ERR_TOO_BIG => Err(SendError::TooBig),
            ffi::RANT_ERR_ROLE => Err(SendError::BadRole),
            ffi::RANT_ERR_OOM => Err(SendError::OutOfMemory),
            ffi::RANT_ERR_NOSYS => Err(SendError::NoSys),
            ffi::RANT_ERR_SCHEMA => Err(SendError::Schema),
            _ => Err(SendError::State),
        }
    }
}

impl fmt::Display for SendError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            SendError::NoTopic => "no topic: the handle or node is closed, or nobody matched in time",
            SendError::TooBig => "the message is too big",
            SendError::BadRole => "the handle or its peer has no such side",
            SendError::OutOfMemory => "out of memory",
            SendError::State => "refused at this moment, such as a blocking call from an inline callback",
            SendError::NoSys => "switched off on this node",
            SendError::Schema => "the value does not fit the schema",
        })
    }
}

impl std::error::Error for SendError {}

/// One event as the C formats it.
pub(crate) fn event_text(ev: &ffi::RantEvent) -> String {
    let mut buf = [0 as std::os::raw::c_char; 256];
    // SAFETY: the formatter always NUL terminates within cap
    unsafe {
        ffi::rant_event_str(ev, buf.as_mut_ptr(), buf.len());
        CStr::from_ptr(buf.as_ptr()).to_string_lossy().into_owned()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_c_error_kind_has_a_variant() {
        let kinds = crate::c_enum_values("RantErrorKind");
        assert!(kinds.len() > 20);
        for (name, v) in kinds {
            assert_ne!(ErrorKind::from_c(v as ffi::RantErrorKind), ErrorKind::Unknown, "{name}");
            assert_eq!(ErrorKind::from_c(v as ffi::RantErrorKind).to_c() as i64, v, "{name}");
        }
    }

    #[test]
    fn every_c_result_code_has_a_variant() {
        for (name, v) in crate::c_enum_values("RantResult") {
            let mapped = SendError::check(v as i32);
            assert_eq!(mapped.is_ok(), v == 0, "{name}");
            assert!(v == 0 || v == ffi::RANT_ERR_STATE as i64 || mapped != Err(SendError::State), "{name} has no variant");
        }
    }
}
