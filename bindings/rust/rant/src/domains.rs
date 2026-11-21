use std::ffi::CString;
use std::ptr::null;

use crate::error::{Error, ErrorKind};
use crate::ffi;
use crate::options::options;

options! {
    /// The census watch's options: `DomainsOptions::new().multicast_interface("127.0.0.1")`.
    pub struct DomainsOptions {
        /// The nodes' discovery port. 0 = 6730.
        discovery_port: u16 = 0,
    }
    hand {
        multicast_interface: Option<String> = None,
    }
}

impl DomainsOptions {
    /// Hear on the interface with this IP only. "127.0.0.1" hears this host alone.
    pub fn multicast_interface(mut self, ip: impl Into<String>) -> Self {
        self.multicast_interface = Some(ip.into());
        self
    }
}

/// A domain with nodes on it, from [`Domains::list`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DomainInfo {
    /// The domain number.
    pub domain: u16,
    /// Nodes heard on it within the peer timeout.
    pub nodes: u16,
}

/// Hears Rant's census: which domains have nodes on this network. It is no node. One thread at
/// a time, so it is Send but not Sync.
pub struct Domains {
    raw: *mut ffi::RantDomains,
}

// SAFETY: the watch has no thread affinity, and &mut self on list keeps it to one thread at a time
unsafe impl Send for Domains {}

impl Domains {
    /// Starts hearing the census.
    pub fn new(options: DomainsOptions) -> Result<Domains, Error> {
        let mcast_if = options
            .multicast_interface
            .as_deref()
            .map(CString::new)
            .transpose()
            .map_err(|_| Error::new(ErrorKind::BadAddress, "rant::Domains::new: the interface holds a NUL"))?;
        let co = ffi::RantDomainsOpts {
            discovery_port: options.discovery_port,
            multicast_interface: mcast_if.as_ref().map_or(null(), |c| c.as_ptr()),
        };
        // SAFETY: open copies the allocator and reads the options only while it runs
        let raw = unsafe {
            let mut alloc = ffi::rant_allocator_heap(0);
            ffi::rant_domains_open(&mut alloc, &co)
        };
        if raw.is_null() {
            return Err(Error::last(std::ptr::null_mut(), "rant::Domains::new"));
        }
        Ok(Domains { raw })
    }

    /// Every domain heard within the peer timeout, ascending. Call it at least every 3 s, and
    /// expect the first call after open to be partial.
    pub fn list(&mut self) -> Vec<DomainInfo> {
        let mut it = ffi::RantIter::default();
        let mut d = ffi::RantDomainInfo::default();
        let mut out = Vec::new();
        // SAFETY: raw lives until drop, and &mut self keeps the walk to one thread
        while unsafe { ffi::rant_domains_next(self.raw, &mut it, &mut d) } != 0 {
            out.push(DomainInfo { domain: d.domain, nodes: d.nodes });
        }
        out
    }
}

impl Drop for Domains {
    fn drop(&mut self) {
        // SAFETY: raw came from open and is closed once
        unsafe { ffi::rant_domains_close(self.raw) };
    }
}
