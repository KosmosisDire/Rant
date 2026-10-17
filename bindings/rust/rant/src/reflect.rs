use std::collections::BTreeMap;
use std::ffi::CString;
use std::future::Future;
use std::sync::Arc;
use std::time::Duration;

use crate::error::{Error, ErrorKind};
use crate::ffi;
use crate::map::MapItem;
use crate::message::Bytes;
use crate::node::{Node, NodeInner};
use crate::pattern::{CallOptions, Codecs, FnShared, PendingCall, Response, async_call, blocking_call};
use crate::schema::{Schema, rbytes, rstring_bytes, utf8_prefix};

/// What a mesh entity is. Reflection shows entities folded, never their channels.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub enum EntityKind {
    /// A topic.
    Topic,
    /// A function.
    Function,
    /// A variable.
    Variable,
    /// A task.
    Task,
    /// A kind this binding does not know yet.
    Unknown,
}

impl EntityKind {
    fn from_c(k: ffi::RantEntityKind) -> EntityKind {
        match k {
            ffi::RANT_ENTITY_TOPIC => EntityKind::Topic,
            ffi::RANT_ENTITY_FUNCTION => EntityKind::Function,
            ffi::RANT_ENTITY_VARIABLE => EntityKind::Variable,
            ffi::RANT_ENTITY_TASK => EntityKind::Task,
            _ => EntityKind::Unknown,
        }
    }

    fn to_c(self) -> ffi::RantEntityKind {
        match self {
            EntityKind::Topic | EntityKind::Unknown => ffi::RANT_ENTITY_TOPIC,
            EntityKind::Function => ffi::RANT_ENTITY_FUNCTION,
            EntityKind::Variable => ffi::RANT_ENTITY_VARIABLE,
            EntityKind::Task => ffi::RANT_ENTITY_TASK,
        }
    }
}

/// A discovered peer, copied out. Dropped peers stay listed, since they may come back under
/// the same uuid, so check `active`.
#[derive(Clone, Debug, PartialEq, Eq)]
#[non_exhaustive]
pub struct Peer {
    /// The peer id events and messages name it by.
    pub id: u32,
    /// The process instance: a restart is a new uuid.
    pub uuid: [u8; 16],
    /// The node's name.
    pub name: String,
    /// Its data address, "ip:port".
    pub address: String,
    /// False once it went silent or said bye.
    pub active: bool,
    /// When it was last heard, on this node's monotonic clock in microseconds.
    pub last_heard_us: u64,
    /// Bumps on every reflected change at this peer.
    pub epoch: u32,
    /// It advertises newer state than this node holds yet.
    pub catching_up: bool,
    /// Its UDP payload bytes per fragment.
    pub fragment_size: u16,
    /// The smoothed round trip in microseconds, meaningful once `rtt_samples` is above 0.
    pub rtt_us: u32,
    /// The round trip's jitter in microseconds.
    pub rtt_jitter_us: u32,
    /// The smallest round trip seen, in microseconds.
    pub rtt_min_us: u32,
    /// Round trips measured.
    pub rtt_samples: u32,
}

/// One mesh entity, copied out with its schemas. `name` reads "0x" and a hash until the
/// peer's details arrive, which a node opened with `fetch_details` asks for.
#[derive(Clone, Debug)]
#[non_exhaustive]
pub struct Entity {
    /// What it is.
    pub kind: EntityKind,
    /// Its name on the mesh.
    pub name: String,
    /// The primary channel's name hash, low 32 bits.
    pub hash: u32,
    /// A publisher, definition or owner is present.
    pub provides: bool,
    /// A subscriber, caller or remote is present.
    pub consumes: bool,
    /// The primary channel is reliable.
    pub reliable: bool,
    /// A variable takes sets.
    pub writable: bool,
    /// A variable's owner permits force.
    pub forceable: bool,
    /// A task's provider honors cancel.
    pub cancellable: bool,
    /// A task declares one call at a time.
    pub exclusive: bool,
    /// Several providers are intended.
    pub multi: bool,
    /// Its details are still arriving.
    pub incomplete: bool,
    /// Live endpoints declare schemas that cannot read each other.
    pub conflict: bool,
    /// Live providing endpoints.
    pub providers: u16,
    /// Live consuming endpoints.
    pub consumers: u16,
    /// The ranked provider's peer id, 0 for this node, meaningful when `providers` is above 0.
    pub provider: u32,
    /// The node the schemas were read from.
    pub from: String,
    /// The value, request or payload schema, None when untyped or not fetched.
    pub schema: Option<Schema>,
    /// A function's or task's response schema.
    pub response_schema: Option<Schema>,
    /// A task's progress schema.
    pub progress_schema: Option<Schema>,
    /// Changes exactly when the provider, a schema or an attribute changed.
    pub generation: u64,
}

/// The sections of a meta snapshot to ask for: `MetaSections::NODE | MetaSections::PEERS`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct MetaSections(u32);

impl MetaSections {
    /// Uptime, memory, backpressure, peer and topic counts, the last error.
    pub const NODE: MetaSections = MetaSections(ffi::RANT_META_NODE);
    /// Process cpu and memory, where the platform measures them.
    pub const PROC: MetaSections = MetaSections(ffi::RANT_META_PROC);
    /// Each topic's name, role, match counts and traffic.
    pub const TOPICS: MetaSections = MetaSections(ffi::RANT_META_TOPICS);
    /// Each peer's id, name, address and match counts.
    pub const PEERS: MetaSections = MetaSections(ffi::RANT_META_PEERS);
    /// Every section.
    pub const ALL: MetaSections = MetaSections(ffi::RANT_META_NODE | ffi::RANT_META_PROC | ffi::RANT_META_TOPICS | ffi::RANT_META_PEERS);
}

impl std::ops::BitOr for MetaSections {
    type Output = MetaSections;
    fn bitor(self, rhs: MetaSections) -> MetaSections {
        MetaSections(self.0 | rhs.0)
    }
}

/// A peer's runtime snapshot from [`Reflection::meta`]. The node and process numbers are
/// pulled out, the whole body stays in `info`. A section not asked for reads zeros or None.
#[derive(Clone, Debug, Default)]
#[non_exhaustive]
pub struct MetaSnapshot {
    /// The peer that answered.
    pub provider: u32,
    /// The whole decoded body, one key per section.
    pub info: BTreeMap<String, MapItem>,
    /// The node section.
    pub node: MetaNode,
    /// The process section, None where the platform does not measure it.
    pub process: Option<MetaProcess>,
}

/// The node section of a [`MetaSnapshot`], named as on the wire.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
#[non_exhaustive]
#[allow(missing_docs)]
pub struct MetaNode {
    pub name: String,
    pub uptime_us: u64,
    pub wall_us: u64,
    pub mem_in_use: u64,
    pub mem_peak: u64,
    pub alloc_calls: u64,
    pub evicted_unsent: u64,
    pub bp_waited_us: u64,
    pub bp_waits: u64,
    pub peers: u64,
    pub max_peers: u64,
    pub topics: u64,
    pub max_topics: u64,
    pub shm_tx: u64,
    pub shm_rx: u64,
    pub last_error: u64,
    pub last_error_text: String,
}

/// The process section of a [`MetaSnapshot`], named as on the wire. `cpu_us` is None where
/// the platform does not measure it.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
#[non_exhaustive]
#[allow(missing_docs)]
pub struct MetaProcess {
    pub pid: u64,
    pub cpu_us: Option<u64>,
    pub rss: u64,
    pub peak_rss: u64,
    pub heap_total: u64,
    pub heap_free: u64,
    pub heap_min_free: u64,
    pub heap_largest_free_block: u64,
}

impl MetaSnapshot {
    fn from_response(r: Response<Bytes>) -> Result<MetaSnapshot, Error> {
        if !r.is_ok() {
            return Err(r.into_result().err().map(Error::from).unwrap_or_else(|| Error::new(ErrorKind::None, "rant: meta came back empty")));
        }
        let info = r.fields().get_map("info").to_map();
        let section = |key: &str| match info.get(key) {
            Some(MapItem::Map(m)) => Some(m.clone()),
            _ => None,
        };
        let uint = |m: &BTreeMap<String, MapItem>, k: &str| match m.get(k) {
            Some(MapItem::UInt(v)) => *v,
            Some(MapItem::Int(v)) => *v as u64,
            _ => 0,
        };
        let text = |m: &BTreeMap<String, MapItem>, k: &str| match m.get(k) {
            Some(MapItem::String(s)) => s.clone(),
            _ => String::new(),
        };
        let node = section("node").map_or_else(MetaNode::default, |m| MetaNode {
            name: text(&m, "name"),
            uptime_us: uint(&m, "uptime_us"),
            wall_us: uint(&m, "wall_us"),
            mem_in_use: uint(&m, "mem_in_use"),
            mem_peak: uint(&m, "mem_peak"),
            alloc_calls: uint(&m, "alloc_calls"),
            evicted_unsent: uint(&m, "evicted_unsent"),
            bp_waited_us: uint(&m, "bp_waited_us"),
            bp_waits: uint(&m, "bp_waits"),
            peers: uint(&m, "peers"),
            max_peers: uint(&m, "max_peers"),
            topics: uint(&m, "topics"),
            max_topics: uint(&m, "max_topics"),
            shm_tx: uint(&m, "shm_tx"),
            shm_rx: uint(&m, "shm_rx"),
            last_error: uint(&m, "last_error"),
            last_error_text: text(&m, "last_error_text"),
        });
        let process = section("proc").map(|m| MetaProcess {
            pid: uint(&m, "pid"),
            cpu_us: m.contains_key("cpu_us").then(|| uint(&m, "cpu_us")),
            rss: uint(&m, "rss"),
            peak_rss: uint(&m, "peak_rss"),
            heap_total: uint(&m, "heap_total"),
            heap_free: uint(&m, "heap_free"),
            heap_min_free: uint(&m, "heap_min_free"),
            heap_largest_free_block: uint(&m, "heap_largest_free_block"),
        });
        Ok(MetaSnapshot { provider: r.provider(), info, node, process })
    }
}

/// The mesh as a node sees it, from [`Node::reflection`]: its peers, what each offers, the
/// folded mesh, and meta snapshots. Every walk copies out, so results outlive the loop.
#[derive(Clone)]
pub struct Reflection {
    inner: Arc<NodeInner>,
}

/// Holds the node lock across a walk, since its views are valid until the next poll.
struct Locked(*mut ffi::RantNode);

impl Locked {
    fn new(raw: *mut ffi::RantNode) -> Locked {
        // SAFETY: the caller holds a pass on the node. From a callback it does nothing
        unsafe { ffi::rant_node_lock(raw) };
        Locked(raw)
    }
}

impl Drop for Locked {
    fn drop(&mut self) {
        // SAFETY: as new
        unsafe { ffi::rant_node_unlock(self.0) };
    }
}

impl Reflection {
    /// Every discovered peer, dropped ones included.
    pub fn peers(&self) -> Vec<Peer> {
        let Ok((_pass, raw)) = self.inner.enter() else { return Vec::new() };
        let _lock = Locked::new(raw);
        let mut it = ffi::RantIter::default();
        let mut p = ffi::RantPeerInfo::default();
        let mut out = Vec::new();
        // SAFETY: the pass and the lock keep every view valid until it is copied
        while unsafe { ffi::rant_node_peers_next(raw, &mut it, &mut p) } != 0 {
            out.push(Peer {
                id: p.id,
                uuid: p.uuid,
                name: utf8_prefix(rstring_bytes(p.name)).to_owned(),
                address: utf8_prefix(rstring_bytes(p.address)).to_owned(),
                active: p.liveness == ffi::RANT_PEER_ACTIVE,
                last_heard_us: p.last_heard_us,
                epoch: p.epoch,
                catching_up: p.catching_up != 0,
                fragment_size: p.fragment_size,
                rtt_us: p.rtt_us,
                rtt_jitter_us: p.rtt_jitter_us,
                rtt_min_us: p.rtt_min_us,
                rtt_samples: p.rtt_samples,
            });
        }
        out
    }

    /// The entities one node offers: a peer by id, or 0 for this node. A dropped peer
    /// serves its last known view.
    pub fn entities(&self, peer: u32) -> Vec<Entity> {
        self.walk(|raw, it, e| unsafe { ffi::rant_node_entities_next(raw, peer, it, e) })
    }

    /// The whole mesh folded: one entity per kind and name across every active peer and this
    /// node, its schemas from the provider.
    pub fn mesh(&self) -> Vec<Entity> {
        self.walk(|raw, it, e| unsafe { ffi::rant_node_mesh_next(raw, it, e) })
    }

    /// One folded entity by kind and name.
    pub fn find(&self, kind: EntityKind, name: &str) -> Option<Entity> {
        let cname = CString::new(name).ok()?;
        let (_pass, raw) = self.inner.enter().ok()?;
        let _lock = Locked::new(raw);
        let mut e = ffi::RantEntityInfo::default();
        // SAFETY: the pass and the lock keep the views valid until they are copied
        let found = unsafe { ffi::rant_node_mesh_find(raw, kind.to_c(), cname.as_ptr(), &mut e) } != 0;
        found.then(|| self.entity(raw, &e))
    }

    /// Moves on every reflected change anywhere in the mesh: walk again only when it moved.
    pub fn epoch(&self) -> u32 {
        // SAFETY: the pass keeps the node open
        self.inner.enter().map_or(0, |(_pass, raw)| unsafe { ffi::rant_node_mesh_epoch(raw) })
    }

    /// A peer's runtime snapshot through its @rant/meta function, waiting up to `timeout`.
    /// Refused inside an inline callback, where [`Reflection::meta_async`] is the tool.
    pub fn meta(&self, peer: u32, sections: MetaSections, timeout: Duration) -> Result<MetaSnapshot, Error> {
        let shared = self.meta_function()?;
        let options = CallOptions::new().provider(peer).timeout(timeout);
        let r = blocking_call::<Bytes, Bytes>(&shared, &sections.0.to_le_bytes(), &options, None)?;
        MetaSnapshot::from_response(r)
    }

    /// The same, returning at once with a Future of the snapshot, which any executor can await.
    pub fn meta_async(&self, peer: u32, sections: MetaSections) -> Result<impl Future<Output = Result<MetaSnapshot, Error>> + Send + 'static, Error> {
        let shared = self.meta_function()?;
        let (state, _) = async_call::<Bytes, Bytes>(&shared, &sections.0.to_le_bytes(), &CallOptions::new().provider(peer), None)?;
        let pending = PendingCall { state };
        Ok(async move { MetaSnapshot::from_response(pending.await) })
    }

    /// The node's own @rant/meta handle, which the node keeps and never retires.
    fn meta_function(&self) -> Result<Arc<FnShared>, Error> {
        const WHAT: &str = "rant::Reflection::meta";
        let (_pass, raw) = self.inner.enter_for(WHAT)?;
        // SAFETY: the pass keeps the node open, and the builtin lives as long as the node
        let f = unsafe { ffi::rant_node_meta_function(raw) };
        if f.is_null() {
            return Err(Error::new(ErrorKind::State, format!("{WHAT}: meta is disabled on this node")));
        }
        let shared = FnShared::new(&self.inner, "@rant/meta", false, Codecs { req: None, rsp: None, prg: None });
        shared.set_raw(f);
        Ok(shared)
    }

    fn walk(&self, mut next: impl FnMut(*mut ffi::RantNode, *mut ffi::RantIter, *mut ffi::RantEntityInfo) -> i32) -> Vec<Entity> {
        let Ok((_pass, raw)) = self.inner.enter() else { return Vec::new() };
        let _lock = Locked::new(raw);
        let mut it = ffi::RantIter::default();
        let mut e = ffi::RantEntityInfo::default();
        let mut out = Vec::new();
        while next(raw, &mut it, &mut e) != 0 {
            out.push(self.entity(raw, &e));
        }
        out
    }

    /// One entity view copied out, its schemas registered in the node so they outlive the walk.
    fn entity(&self, raw: *mut ffi::RantNode, e: &ffi::RantEntityInfo) -> Entity {
        let keep = |view: *const ffi::RantSchema| {
            if view.is_null() {
                return None;
            }
            // SAFETY: the walk's pass and lock keep the view valid, and parse copies its wire
            let s = unsafe {
                let wire = rbytes(ffi::rant_schema_wire(view));
                ffi::rant_node_schema_parse(raw, wire.as_ptr().cast(), wire.len())
            };
            (!s.is_null()).then(|| Schema::new(Arc::clone(&self.inner), s))
        };
        Entity {
            kind: EntityKind::from_c(e.kind),
            name: utf8_prefix(rstring_bytes(e.name)).to_owned(),
            hash: e.hash,
            provides: e.provides != 0,
            consumes: e.consumes != 0,
            reliable: e.reliable != 0,
            writable: e.writable != 0,
            forceable: e.forceable != 0,
            cancellable: e.cancellable != 0,
            exclusive: e.exclusive != 0,
            multi: e.multi != 0,
            incomplete: e.incomplete != 0,
            conflict: e.conflict != 0,
            providers: e.providers,
            consumers: e.consumers,
            provider: e.provider,
            from: utf8_prefix(rstring_bytes(e.from)).to_owned(),
            schema: keep(e.schema),
            response_schema: keep(e.rsp_schema),
            progress_schema: keep(e.progress_schema),
            generation: e.generation,
        }
    }
}

impl Node {
    /// The mesh as this node sees it: peers, entities, the folded mesh and meta snapshots.
    pub fn reflection(&self) -> Reflection {
        Reflection { inner: Arc::clone(&self.inner) }
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn every_c_entity_kind_has_a_variant() {
        for (name, v) in crate::c_enum_values("RantEntityKind") {
            assert_ne!(super::EntityKind::from_c(v as crate::ffi::RantEntityKind), super::EntityKind::Unknown, "{name}");
        }
    }
}
