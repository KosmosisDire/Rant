use std::any::{TypeId, type_name};
use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::ffi::CString;
use std::sync::{Arc, Mutex};

use crate::error::{Error, ErrorKind, SendError};
use crate::ffi;
use crate::node::{NodeInner, lock};
use crate::options::Qos;
use crate::schema::{MessageView, c_bytes, rbytes, rstring_bytes};
use crate::topic::TopicCore;

/// The type argument of a raw handle, which sends any bytes and reads fields by name through
/// a [`MessageView`]. Only a raw handle takes a schema or `reflect_from_mesh` in its options.
pub enum Bytes {}

/// A type that travels as a message: a `#[derive(rant::Message)]` struct or `#[derive(rant::Enum)]` enum,
/// a number, `bool`, `String`, [`rant::String<N>`](crate::String), `Vec<T>`, `[T; N]` or a [`types`](crate::types) one.
pub trait Message: Sized + Send + 'static {
    #[doc(hidden)]
    fn __spell(s: &mut Spell);
    #[doc(hidden)]
    fn __encode(&self, e: &mut Encoder<'_>) -> Result<(), String>;
    #[doc(hidden)]
    fn __decode(d: &mut Decoder<'_>) -> Result<Self, String>;
}

mod sealed {
    pub trait Sealed {}
    impl<T: super::Message> Sealed for T {}
    impl Sealed for super::Bytes {}
}

/// What a handle carries: a [`Message`] type, or [`Bytes`]. Sealed.
pub trait Payload: sealed::Sealed + Send + 'static {
    /// What a send takes: the value, or for [`Bytes`] a byte slice.
    type Input: ?Sized;
    /// What a handler and a take see: the value, or for [`Bytes`] a [`MessageView`].
    type Value<'a>;

    #[doc(hidden)]
    #[allow(private_interfaces)]
    fn __codec(inner: &Arc<NodeInner>, qos: &Qos, what: &str) -> Result<Option<Arc<Codec>>, Error>;
    #[doc(hidden)]
    #[allow(private_interfaces)]
    fn __send(core: &TopicCore, codec: Option<&Codec>, value: &Self::Input, capture_us: u64) -> Result<(), SendError>;
    #[doc(hidden)]
    #[allow(private_interfaces)]
    fn __value<'a>(codec: Option<&Codec>, view: &MessageView<'a>) -> Result<Self::Value<'a>, String>;
    #[doc(hidden)]
    #[allow(private_interfaces)]
    fn __taken<'a>(codec: Option<&Codec>, view: MessageView<'a>) -> Result<Self::Value<'a>, String>;
}

impl<T: Message> Payload for T {
    type Input = T;
    type Value<'a> = T;

    #[allow(private_interfaces)]
    fn __codec(inner: &Arc<NodeInner>, qos: &Qos, what: &str) -> Result<Option<Arc<Codec>>, Error> {
        if qos.schema.is_some() || qos.reflect_from_mesh {
            return Err(Error::new(
                ErrorKind::State,
                format!("{what}: a typed handle takes its schema from its type, use rant::Bytes for a schema or reflect_from_mesh"),
            ));
        }
        Codec::of::<T>(inner).map(Some)
    }

    #[allow(private_interfaces)]
    fn __send(core: &TopicCore, codec: Option<&Codec>, value: &T, capture_us: u64) -> Result<(), SendError> {
        let codec = codec.ok_or(SendError::Schema)?;
        codec.encode(value, |bytes| core.send(bytes, capture_us)).map_err(|_| SendError::Schema)?
    }

    #[allow(private_interfaces)]
    fn __value<'a>(codec: Option<&Codec>, view: &MessageView<'a>) -> Result<T, String> {
        codec.ok_or("no codec")?.decode(view.data(), view.raw_schema())
    }

    #[allow(private_interfaces)]
    fn __taken<'a>(codec: Option<&Codec>, view: MessageView<'a>) -> Result<T, String> {
        Self::__value(codec, &view)
    }
}

impl Payload for Bytes {
    type Input = [u8];
    type Value<'a> = MessageView<'a>;

    #[allow(private_interfaces)]
    fn __codec(_: &Arc<NodeInner>, _: &Qos, _: &str) -> Result<Option<Arc<Codec>>, Error> {
        Ok(None)
    }

    #[allow(private_interfaces)]
    fn __send(core: &TopicCore, _: Option<&Codec>, value: &[u8], capture_us: u64) -> Result<(), SendError> {
        core.send(value, capture_us)
    }

    #[allow(private_interfaces)]
    fn __value<'a>(_: Option<&Codec>, view: &MessageView<'a>) -> Result<MessageView<'a>, String> {
        Ok(view.reborrow())
    }

    #[allow(private_interfaces)]
    fn __taken<'a>(_: Option<&Codec>, view: MessageView<'a>) -> Result<MessageView<'a>, String> {
        Ok(view)
    }
}

/// The wire kinds the speller emits, the C `RANT_*` kind bytes.
#[doc(hidden)]
pub mod kind {
    use crate::ffi;
    pub const U8: u8 = ffi::RANT_U8 as u8;
    pub const U16: u8 = ffi::RANT_U16 as u8;
    pub const U32: u8 = ffi::RANT_U32 as u8;
    pub const U64: u8 = ffi::RANT_U64 as u8;
    pub const I8: u8 = ffi::RANT_I8 as u8;
    pub const I16: u8 = ffi::RANT_I16 as u8;
    pub const I32: u8 = ffi::RANT_I32 as u8;
    pub const I64: u8 = ffi::RANT_I64 as u8;
    pub const F32: u8 = ffi::RANT_F32 as u8;
    pub const F64: u8 = ffi::RANT_F64 as u8;
    pub const BOOL: u8 = ffi::RANT_BOOL as u8;
    pub(crate) const ARR: u8 = ffi::RANT_ARR as u8;
    pub(crate) const STRUCT: u8 = ffi::RANT_STRUCT as u8;
    pub(crate) const STR: u8 = ffi::RANT_STR as u8;
    pub(crate) const VSTR: u8 = ffi::RANT_VSTR as u8;
    pub(crate) const VARR: u8 = ffi::RANT_VARR as u8;
    pub(crate) const ENUM: u8 = ffi::RANT_ENUM as u8;

    pub(crate) fn name(k: u8) -> &'static str {
        ["u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "f32", "f64", "bool"].get(k as usize).copied().unwrap_or("?")
    }

    pub(crate) fn size(k: u8) -> usize {
        // SAFETY: a pure lookup
        unsafe { ffi::rant_schema_scalar_size(k as ffi::RantSchemaTypeKind) as usize }
    }
}

/// One leaf or tail of the flattened type, in the order the type's encode visits it.
pub(crate) struct Op {
    path: CString,
    /// its index in each enclosing fixed struct array, outermost first
    elems: Vec<u16>,
    shape: Shape,
}

enum Shape {
    /// a scalar, enum or capped string slot, or a fixed array of them, stride bytes apart
    Fixed { kind: u8, count: u16, cap: u16, is_enum: bool, is_array: bool, stride: usize },
    VarString,
    VarArray { elem: u8, cap: u16 },
    /// a variable array of structs: the element's ops, relative to one element
    VecStruct { sub: Vec<Op> },
}

impl Shape {
    fn fixed(kind: u8, count: u16, cap: u16, is_enum: bool, is_array: bool) -> Shape {
        let stride = if kind == kind::STR { 2 + cap as usize } else { kind::size(kind) };
        Shape::Fixed { kind, count, cap, is_enum, is_array, stride }
    }

    fn stride(&self) -> usize {
        match *self {
            Shape::Fixed { stride, .. } => stride,
            _ => 0,
        }
    }
}

/// An array wrapper waiting for its element's spelling.
#[derive(Clone, Copy)]
enum ArrayCtx {
    Fixed(u16),
    Var,
}

struct TextCtx {
    buf: String,
    first: bool,
    quiet: u32,
}

/// Spells a Rust type as schema text and its flattened ops, the way `RANT_SCHEMA` does in
/// C++, so both compile to the same schema. Only the derive and the built in types call it.
#[doc(hidden)]
pub struct Spell {
    text: Vec<TextCtx>,
    defs: String,
    defined: HashSet<TypeId>,
    mute_ops: u32,
    at_root: bool,
    path: String,
    prefix: String,
    elems: Vec<u16>,
    array: Option<ArrayCtx>,
    ops: Vec<Op>,
    error: Option<String>,
}

impl Spell {
    fn new() -> Spell {
        Spell {
            text: vec![TextCtx { buf: String::new(), first: true, quiet: 0 }],
            defs: String::new(),
            defined: HashSet::new(),
            mute_ops: 0,
            at_root: true,
            path: String::new(),
            prefix: String::new(),
            elems: Vec::new(),
            array: None,
            ops: Vec::new(),
            error: None,
        }
    }

    fn put(&mut self, s: &str) {
        let t = self.text.last_mut().unwrap();
        if t.quiet == 0 {
            t.buf.push_str(s);
        }
    }

    fn quiet(&mut self, by: i32) {
        let t = self.text.last_mut().unwrap();
        t.quiet = t.quiet.wrapping_add_signed(by);
    }

    fn refuse(&mut self, why: String) {
        self.error.get_or_insert(why);
    }

    fn push_op(&mut self, shape: Shape) {
        if self.mute_ops == 0 {
            let path = CString::new(self.path.as_str()).unwrap_or_default();
            self.ops.push(Op { path, elems: self.elems.clone(), shape });
        }
    }

    fn array_suffix(&mut self, a: ArrayCtx) {
        match a {
            ArrayCtx::Fixed(n) => self.put(&format!("[{n}]")),
            ArrayCtx::Var => self.put("[]"),
        }
    }

    /// A member of the struct being spelled: `name: Type`, with M's ops under its path.
    pub fn member<M: Message>(&mut self, name: &str) {
        let name = name.strip_prefix("r#").unwrap_or(name);
        let t = self.text.last_mut().unwrap();
        if t.quiet == 0 {
            if !t.first {
                t.buf.push_str(", ");
            }
            t.first = false;
            t.buf.push_str(name);
            t.buf.push_str(": ");
        }
        let saved = std::mem::replace(&mut self.path, format!("{}{name}", self.prefix));
        M::__spell(self);
        self.path = saved;
    }

    /// A fixed array `E[n]`.
    pub fn fixed_array<E: Message>(&mut self, n: usize) {
        if self.array.is_some() {
            return self.refuse("an array of arrays is not a wire type".to_owned());
        }
        let Ok(n) = u16::try_from(n) else {
            return self.refuse(format!("a fixed array of {n} elements is over 65535"));
        };
        self.array = Some(ArrayCtx::Fixed(n));
        E::__spell(self);
        self.at_root = false;
    }

    /// A variable array `E[]`.
    pub fn var_array<E: Message>(&mut self) {
        if self.array.is_some() {
            return self.refuse("an array of arrays is not a wire type".to_owned());
        }
        self.array = Some(ArrayCtx::Var);
        E::__spell(self);
        self.at_root = false;
    }

    /// A number or bool, alone or as an array element.
    pub fn scalar(&mut self, k: u8) {
        self.put(kind::name(k));
        match self.array.take() {
            None => self.push_op(Shape::fixed(k, 1, 0, false, false)),
            Some(a @ ArrayCtx::Fixed(n)) => {
                self.array_suffix(a);
                self.push_op(Shape::fixed(k, n, 0, false, true));
            }
            Some(a @ ArrayCtx::Var) => {
                self.array_suffix(a);
                self.push_op(Shape::VarArray { elem: k, cap: 0 });
            }
        }
        self.at_root = false;
    }

    /// A capped string `string<cap>`, alone or as an array element.
    pub fn fixed_string(&mut self, cap: usize) {
        let Ok(cap) = u16::try_from(cap) else {
            return self.refuse(format!("a string capacity of {cap} is over 65535"));
        };
        self.put(&format!("string<{cap}>"));
        match self.array.take() {
            None => self.push_op(Shape::fixed(kind::STR, 1, cap, false, false)),
            Some(a @ ArrayCtx::Fixed(n)) => {
                self.array_suffix(a);
                self.push_op(Shape::fixed(kind::STR, n, cap, false, true));
            }
            Some(a @ ArrayCtx::Var) => {
                self.array_suffix(a);
                self.push_op(Shape::VarArray { elem: kind::STR, cap });
            }
        }
        self.at_root = false;
    }

    /// A variable string `string`.
    pub fn var_string(&mut self) {
        if self.array.take().is_some() {
            return self.refuse("an array of String is not a wire type, use rant::String<N>".to_owned());
        }
        self.put("string");
        self.push_op(Shape::VarString);
        self.at_root = false;
    }

    /// A named integer `enum<k> { A=0, B=1 }`, spelled inline as C++ does.
    pub fn enumeration(&mut self, backing: u8, options: &[(&str, i64)]) {
        if self.array.take().is_some() {
            return self.refuse("an array of enums is not a wire type".to_owned());
        }
        let body: Vec<String> = options.iter().map(|(n, v)| format!("{n}={v}")).collect();
        self.put(&format!("enum<{}> {{ {} }}", kind::name(backing), body.join(", ")));
        self.push_op(Shape::fixed(backing, 1, 0, true, false));
        self.at_root = false;
    }

    /// A struct: at the root its own definition, as a member its name, defined once above.
    pub fn structure(&mut self, id: TypeId, name: &str, members: fn(&mut Spell)) {
        if let Some(a) = self.array.take() {
            self.put(name);
            self.array_suffix(a);
            self.define(id, name, members);
            return self.array_members(a, members);
        }
        if std::mem::replace(&mut self.at_root, false) {
            self.put(name);
            self.put(" { ");
            let t = self.text.last_mut().unwrap();
            t.first = true;
            members(self);
            self.put(" }");
            return;
        }
        self.put(name);
        self.define(id, name, members);
        self.members_under(members);
    }

    /// A standard type: always in scope by name, so it is never defined.
    pub fn standard(&mut self, name: &str, members: fn(&mut Spell)) {
        self.put(name);
        if let Some(a) = self.array.take() {
            self.array_suffix(a);
            return self.array_members(a, members);
        }
        if std::mem::replace(&mut self.at_root, false) {
            self.quiet(1);
            members(self);
            self.quiet(-1);
            return;
        }
        self.members_under(members);
    }

    /// A standard alias such as `Timestamp`, spelled by name and carried as its repr.
    pub fn standard_alias(&mut self, name: &str, repr: fn(&mut Spell)) {
        if self.array.take().is_some() {
            return self.refuse(format!("an array of {name} is not a wire type"));
        }
        self.put(name);
        self.quiet(1);
        repr(self);
        self.quiet(-1);
        self.at_root = false;
    }

    /// The members' ops one level down, under this member's path, spelling nothing.
    fn members_under(&mut self, members: fn(&mut Spell)) {
        let saved = std::mem::replace(&mut self.prefix, format!("{}.", self.path));
        self.quiet(1);
        members(self);
        self.quiet(-1);
        self.prefix = saved;
    }

    /// Ops for every element of a fixed array of structs, or one element template for a
    /// variable one, as C++ flattens them.
    fn array_members(&mut self, a: ArrayCtx, members: fn(&mut Spell)) {
        let path = self.path.clone();
        match a {
            ArrayCtx::Fixed(n) => {
                for i in 0..n {
                    self.path = format!("{path}[{i}]");
                    self.elems.push(i);
                    self.members_under(members);
                    self.elems.pop();
                }
                self.path = path;
            }
            ArrayCtx::Var => {
                let ops = std::mem::take(&mut self.ops);
                let elems = std::mem::take(&mut self.elems);
                self.members_under(members);
                let sub = std::mem::replace(&mut self.ops, ops);
                self.elems = elems;
                if sub.iter().any(|op| !matches!(op.shape, Shape::Fixed { .. })) {
                    return self.refuse("a variable array element must be fixed: no String, Vec or nested Vec of structs".to_owned());
                }
                self.push_op(Shape::VecStruct { sub });
            }
        }
        self.at_root = false;
    }

    /// The definition `Name { ... }` once, after the definitions it uses.
    fn define(&mut self, id: TypeId, name: &str, members: fn(&mut Spell)) {
        if !self.defined.insert(id) {
            return;
        }
        let saved = (std::mem::take(&mut self.prefix), std::mem::take(&mut self.path), std::mem::take(&mut self.elems));
        self.text.push(TextCtx { buf: format!("{name} {{ "), first: true, quiet: 0 });
        self.mute_ops += 1;
        members(self);
        self.mute_ops -= 1;
        let def = self.text.pop().unwrap().buf;
        self.defs.push_str(&def);
        self.defs.push_str(" }\n");
        (self.prefix, self.path, self.elems) = saved;
    }

    fn finish(mut self) -> Result<(String, Vec<Op>), String> {
        if let Some(e) = self.error.take() {
            return Err(e);
        }
        let root = self.text.pop().unwrap().buf;
        Ok((self.defs + &root, self.ops))
    }
}

/// Where each op lives in one compiled schema.
enum Place {
    Fixed(u32),
    Tail,
    VecStruct { elem_wire: u32, sub: Vec<Place> },
}

/// The ops resolved against one schema: a decode plan.
pub(crate) struct Plan {
    places: Vec<Place>,
    size: u32,
}

const NO_PARENT: u16 = 0xFFFF;

fn info(s: *const ffi::RantSchema, i: u16) -> Option<ffi::RantSchemaFieldInfo> {
    let mut f = ffi::RantSchemaFieldInfo::default();
    // SAFETY: the caller holds the schema's node open
    (unsafe { ffi::rant_schema_field_at(s, i, &mut f) } != 0).then_some(f)
}

/// The wire offset of a field reached through its enclosing fixed struct arrays by elems,
/// stopping at the variable array `stop` whose element is the base.
fn static_offset(s: *const ffi::RantSchema, mut fi: ffi::RantSchemaFieldInfo, elems: &[u16], stop: u16) -> Option<u32> {
    let mut off = fi.offset as u64;
    let mut k = elems.len();
    if k + usize::from(stop != NO_PARENT) != fi.arr_depth as usize {
        return None;
    }
    while fi.arr_parent != NO_PARENT && fi.arr_parent != stop {
        let pa = info(s, fi.arr_parent)?;
        k = k.checked_sub(1)?;
        let e = elems[k];
        if pa.kind != kind::ARR || e >= pa.count {
            return None;
        }
        off += e as u64 * pa.elem_size as u64;
        fi = pa;
    }
    u32::try_from(off).ok()
}

fn resolve(ops: &[Op], s: *const ffi::RantSchema, stop: u16) -> Option<Vec<Place>> {
    let mut places = Vec::with_capacity(ops.len());
    for op in ops {
        // SAFETY: the caller holds the schema's node open
        let idx = u16::try_from(unsafe { ffi::rant_schema_field_index(s, op.path.as_ptr()) }).ok()?;
        let fi = info(s, idx)?;
        let place = match &op.shape {
            &Shape::Fixed { kind: k, count, cap, is_enum, is_array, .. } => {
                let fits = if is_enum {
                    fi.kind == kind::ENUM && fi.elem == k
                } else if is_array {
                    fi.kind == kind::ARR && fi.elem == k && fi.count == count && (k != kind::STR || fi.str_cap == cap)
                } else if k == kind::STR {
                    fi.kind == kind::STR && fi.str_cap == cap
                } else {
                    fi.kind == k
                };
                if !fits {
                    return None;
                }
                Place::Fixed(static_offset(s, fi, &op.elems, stop)?)
            }
            Shape::VarString if fi.kind == kind::VSTR => Place::Tail,
            &Shape::VarArray { elem, cap } if fi.kind == kind::VARR && fi.elem == elem && (elem != kind::STR || fi.str_cap == cap) => {
                Place::Tail
            }
            Shape::VecStruct { sub } if fi.kind == kind::VARR && fi.elem == kind::STRUCT => {
                let sub_places = resolve(sub, s, idx)?;
                for (op, place) in sub.iter().zip(&sub_places) {
                    let (Place::Fixed(off), Shape::Fixed { count, .. }) = (place, &op.shape) else { return None };
                    if *off as usize + op.shape.stride() * *count as usize > fi.elem_size as usize {
                        return None;
                    }
                }
                Place::VecStruct { elem_wire: fi.elem_size, sub: sub_places }
            }
            _ => return None,
        };
        places.push(place);
    }
    Some(places)
}

/// One type's codec in one node: its compiled schema, its ops and a decode plan per
/// incoming schema.
pub(crate) struct Codec {
    pub(crate) schema: *const ffi::RantSchema,
    pub(crate) hash: u64,
    name: &'static str,
    ops: Vec<Op>,
    own: Arc<Plan>,
    msg_min: usize,
    /// plans per delivered schema address, cleared on every retire
    plans: Mutex<HashMap<usize, Arc<Plan>>>,
}

// SAFETY: the schema is immutable and node owned, and every use holds the node open
unsafe impl Send for Codec {}
unsafe impl Sync for Codec {}

thread_local! {
    #[allow(clippy::missing_const_for_thread_local)]
    static SCRATCH: RefCell<Vec<u8>> = const { RefCell::new(Vec::new()) };
}

impl Codec {
    /// T's codec in this node, compiled on first use.
    pub(crate) fn of<T: Message>(inner: &Arc<NodeInner>) -> Result<Arc<Codec>, Error> {
        let what = format!("rant: the schema of {}", type_name::<T>());
        let mut codecs = inner.codecs.enter().ok_or_else(|| Error::new(ErrorKind::State, format!("{what}: a handle cannot be made inside a callback")))?;
        if let Some(c) = codecs.get(&TypeId::of::<T>()) {
            return Ok(Arc::clone(c));
        }
        let mut spell = Spell::new();
        T::__spell(&mut spell);
        let (text, ops) = spell.finish().map_err(|e| Error::new(ErrorKind::BadSchema, format!("{what}: {e}")))?;
        let ctext = CString::new(text).map_err(|_| Error::new(ErrorKind::BadSchema, format!("{what}: a name holds a NUL")))?;
        let (_pass, raw) = inner.enter_for(&what)?;
        // SAFETY: the pass keeps the node open
        let schema = unsafe { ffi::rant_node_schema(raw, ctext.as_ptr()) };
        if schema.is_null() {
            return Err(Error::last(raw, &what));
        }
        let places = resolve(&ops, schema, NO_PARENT)
            .ok_or_else(|| Error::new(ErrorKind::BadSchema, format!("{what}: the type does not fit its own schema")))?;
        // SAFETY: as above
        let (hash, size, msg_min) =
            unsafe { (ffi::rant_schema_hash(schema), ffi::rant_schema_size(schema), ffi::rant_schema_msg_min(schema)) };
        let codec = Arc::new(Codec {
            schema,
            hash,
            name: type_name::<T>(),
            ops,
            own: Arc::new(Plan { places, size }),
            msg_min: msg_min as usize,
            plans: Mutex::new(HashMap::new()),
        });
        codecs.insert(TypeId::of::<T>(), Arc::clone(&codec));
        Ok(codec)
    }

    pub(crate) fn clear_plans(&self) {
        lock(&self.plans).clear();
    }

    fn plan(&self, s: *const ffi::RantSchema) -> Result<Arc<Plan>, String> {
        if s.is_null() || s == self.schema {
            return Ok(Arc::clone(&self.own));
        }
        let mut plans = lock(&self.plans);
        if let Some(p) = plans.get(&(s as usize)) {
            return Ok(Arc::clone(p));
        }
        let places = resolve(&self.ops, s, NO_PARENT).ok_or_else(|| format!("the publisher's schema does not fit {}", self.name))?;
        // SAFETY: the caller holds the delivered schema valid
        let plan = Arc::new(Plan { places, size: unsafe { ffi::rant_schema_size(s) } });
        plans.insert(s as usize, Arc::clone(&plan));
        Ok(plan)
    }

    /// Encodes v into a reused per thread buffer and hands f the message bytes.
    pub(crate) fn encode<T: Message, R>(&self, v: &T, f: impl FnOnce(&[u8]) -> R) -> Result<R, String> {
        SCRATCH.with(|scratch| {
            // a nested encode on this thread, such as a send inside a codec, takes its own buffer
            let mut own = Vec::new();
            let mut borrowed = scratch.try_borrow_mut();
            let buf = match borrowed.as_deref_mut() {
                Ok(b) => b,
                Err(_) => &mut own,
            };
            buf.clear();
            buf.resize(self.msg_min, 0);
            // SAFETY: the buffer holds msg_min bytes, and the caller holds the node open
            if unsafe { ffi::rant_schema_message_default(self.schema, buf.as_mut_ptr().cast(), buf.len()) } == 0 {
                return Err(format!("{}: no default message", self.name));
            }
            let mut e = Encoder { buf, mode: EncodeMode::Ops { schema: self.schema, ops: &self.ops, places: &self.own.places, base: 0, i: 0, elem: 0 } };
            v.__encode(&mut e)?;
            // SAFETY: as above
            let n = unsafe { ffi::rant_schema_msg_len(self.schema, e.buf.as_ptr().cast(), e.buf.len()) } as usize;
            if n == 0 {
                return Err(format!("{}: the encoded message is malformed", self.name));
            }
            Ok(f(&e.buf[..n]))
        })
    }

    /// Decodes a delivered payload with the schema it came with, null for our own.
    pub(crate) fn decode<T: Message>(&self, data: &[u8], schema: *const ffi::RantSchema) -> Result<T, String> {
        let plan = self.plan(schema)?;
        if data.len() < plan.size as usize {
            return Err(format!("{} bytes is short of {}'s {}", data.len(), self.name, plan.size));
        }
        let s = if schema.is_null() { self.schema } else { schema };
        let mut d = Decoder { data, mode: DecodeMode::Ops { schema: s, ops: &self.ops, places: &plan.places, base: 0, i: 0, elem: 0 } };
        T::__decode(&mut d)
    }
}

enum EncodeMode<'a> {
    Ops { schema: *const ffi::RantSchema, ops: &'a [Op], places: &'a [Place], base: usize, i: usize, elem: u16 },
    /// a variable array's packed elements, appended in order
    Append,
}

/// Writes a value's leaves and tails in the order its type's spelling flattened them.
#[doc(hidden)]
pub struct Encoder<'a> {
    buf: &'a mut Vec<u8>,
    mode: EncodeMode<'a>,
}

impl<'a> Encoder<'a> {
    /// The next fixed leaf: one scalar or enum, or one element of a fixed array.
    pub fn leaf(&mut self, bytes: &[u8]) -> Result<(), String> {
        match &mut self.mode {
            EncodeMode::Append => {
                self.buf.extend_from_slice(bytes);
                Ok(())
            }
            EncodeMode::Ops { ops, places, base, i, elem, .. } => {
                let (Some(op), Some(&Place::Fixed(off))) = (ops.get(*i), places.get(*i)) else {
                    return Err("the value has more fields than its schema".to_owned());
                };
                let Shape::Fixed { count, .. } = op.shape else { return Err("a fixed field met a tail".to_owned()) };
                let at = *base + off as usize + *elem as usize * op.shape.stride();
                let slot = self.buf.get_mut(at..at + bytes.len()).ok_or("a field lies past the message")?;
                slot.copy_from_slice(bytes);
                *elem += 1;
                if *elem >= count {
                    *elem = 0;
                    *i += 1;
                }
                Ok(())
            }
        }
    }

    /// The next capped string slot: `[u16 length][cap bytes]`.
    pub fn fixed_str(&mut self, live: &[u8], cap: usize) -> Result<(), String> {
        if live.len() > cap {
            return Err(format!("{} bytes do not fit a string<{cap}>", live.len()));
        }
        let mut slot = vec![0u8; 2 + cap];
        slot[..2].copy_from_slice(&(live.len() as u16).to_le_bytes());
        slot[2..2 + live.len()].copy_from_slice(live);
        self.leaf(&slot)
    }

    fn tail(&mut self) -> Result<(*const ffi::RantSchema, &'a Op, &'a Place), String> {
        match &mut self.mode {
            EncodeMode::Ops { schema, ops, places, i, .. } => {
                let (ops, places): (&'a [Op], &'a [Place]) = (ops, places);
                let (op, place) = ops.get(*i).zip(places.get(*i)).ok_or("the value has more fields than its schema")?;
                *i += 1;
                Ok((*schema, op, place))
            }
            EncodeMode::Append => Err("a tail inside an array element".to_owned()),
        }
    }

    /// Room for a tail to grow by `extra` past the live message.
    fn grow(buf: &mut Vec<u8>, schema: *const ffi::RantSchema, extra: usize) {
        // SAFETY: the buffer is a live message of the schema, held open by the caller
        let used = unsafe { ffi::rant_schema_msg_len(schema, buf.as_ptr().cast(), buf.len()) } as usize;
        if buf.len() < used + extra {
            buf.resize(used + extra, 0);
        }
    }

    /// The next variable string.
    pub fn var_string(&mut self, v: &str) -> Result<(), String> {
        let (schema, op, _) = self.tail()?;
        let path = op.path.as_ptr();
        Encoder::grow(self.buf, schema, v.len());
        let s = ffi::RantString { data: v.as_ptr().cast(), len: v.len() };
        // SAFETY: the buffer and its length are live, and the path is the op's own
        let ok = unsafe { ffi::rant_set_string(self.buf.as_mut_ptr().cast(), self.buf.len(), schema, path, s) } != 0;
        if ok { Ok(()) } else { Err("a String did not fit its field".to_owned()) }
    }

    /// The next variable array, from its elements in order.
    pub fn sequence<'x, E: Message + 'x>(&mut self, items: impl ExactSizeIterator<Item = &'x E>) -> Result<(), String> {
        let (schema, op, place) = self.tail()?;
        let path = op.path.as_ptr();
        match (&op.shape, place) {
            (Shape::VarArray { .. }, _) => {
                let mut packed = Vec::new();
                let mut sub = Encoder { buf: &mut packed, mode: EncodeMode::Append };
                for item in items {
                    item.__encode(&mut sub)?;
                }
                Encoder::grow(self.buf, schema, packed.len());
                // SAFETY: as var_string
                let ok = unsafe { ffi::rant_set_array(self.buf.as_mut_ptr().cast(), self.buf.len(), schema, path, c_bytes(&packed)) } != 0;
                if ok { Ok(()) } else { Err("a Vec did not fit its field".to_owned()) }
            }
            (Shape::VecStruct { sub }, Place::VecStruct { elem_wire, sub: sub_places }) => {
                let n = items.len();
                Encoder::grow(self.buf, schema, n * *elem_wire as usize);
                // SAFETY: as var_string
                if unsafe { ffi::rant_set_array_count(self.buf.as_mut_ptr().cast(), self.buf.len(), schema, path, n as u32) } == 0 {
                    return Err("a Vec of structs did not fit its field".to_owned());
                }
                // SAFETY: the frame is a view into the buffer
                let frame = unsafe { ffi::rant_get_array(c_bytes(self.buf), schema, path) };
                if n > 0 && (frame.data.is_null() || frame.len < n * *elem_wire as usize) {
                    return Err("a Vec of structs found no frame".to_owned());
                }
                let at = frame.data as usize - self.buf.as_ptr() as usize;
                for (k, item) in items.enumerate() {
                    let base = at + k * *elem_wire as usize;
                    let mut e = Encoder { buf: &mut *self.buf, mode: EncodeMode::Ops { schema, ops: sub, places: sub_places, base, i: 0, elem: 0 } };
                    item.__encode(&mut e)?;
                }
                Ok(())
            }
            _ => Err("a Vec met a field that is not a variable array".to_owned()),
        }
    }
}

enum DecodeMode<'a> {
    Ops { schema: *const ffi::RantSchema, ops: &'a [Op], places: &'a [Place], base: usize, i: usize, elem: u16 },
    /// a variable array's packed elements, read in order
    Packed { pos: usize },
}

/// Reads a value's leaves and tails in the order its type's spelling flattened them.
#[doc(hidden)]
pub struct Decoder<'a> {
    data: &'a [u8],
    mode: DecodeMode<'a>,
}

impl<'a> Decoder<'a> {
    /// The next fixed leaf's `n` bytes.
    pub fn leaf(&mut self, n: usize) -> Result<&'a [u8], String> {
        let data = self.data;
        let at = match &mut self.mode {
            DecodeMode::Packed { pos } => {
                let at = *pos;
                *pos += n;
                at
            }
            DecodeMode::Ops { ops, places, base, i, elem, .. } => {
                let (Some(op), Some(&Place::Fixed(off))) = (ops.get(*i), places.get(*i)) else {
                    return Err("the type has more fields than the message".to_owned());
                };
                let Shape::Fixed { count, .. } = op.shape else { return Err("a fixed field met a tail".to_owned()) };
                let at = *base + off as usize + *elem as usize * op.shape.stride();
                *elem += 1;
                if *elem >= count {
                    *elem = 0;
                    *i += 1;
                }
                at
            }
        };
        data.get(at..at + n).ok_or_else(|| "a field lies past the message".to_owned())
    }

    /// The next capped string slot's live bytes, a hostile length clamped to the capacity.
    pub fn fixed_str(&mut self, cap: usize) -> Result<&'a [u8], String> {
        let slot = self.leaf(2 + cap)?;
        let len = (u16::from_le_bytes([slot[0], slot[1]]) as usize).min(cap);
        Ok(&slot[2..2 + len])
    }

    fn tail(&mut self) -> Result<(*const ffi::RantSchema, &'a Op, &'a Place), String> {
        match &mut self.mode {
            DecodeMode::Ops { schema, ops, places, i, .. } => {
                let (op, place) = ops.get(*i).zip(places.get(*i)).ok_or("the type has more fields than the message")?;
                *i += 1;
                Ok((*schema, op, place))
            }
            DecodeMode::Packed { .. } => Err("a tail inside an array element".to_owned()),
        }
    }

    /// The next variable string's bytes.
    pub fn var_string(&mut self) -> Result<&'a [u8], String> {
        let (schema, op, _) = self.tail()?;
        // SAFETY: the data is a delivered message of the schema
        let s = unsafe { ffi::rant_get_string(c_bytes(self.data), schema, op.path.as_ptr()) };
        if s.data.is_null() {
            return Err("a string field is missing".to_owned());
        }
        Ok(rstring_bytes(s))
    }

    /// The next variable array's elements.
    pub fn sequence<E: Message>(&mut self) -> Result<Vec<E>, String> {
        let (schema, op, place) = self.tail()?;
        // SAFETY: as var_string
        let frame = rbytes(unsafe { ffi::rant_get_array(c_bytes(self.data), schema, op.path.as_ptr()) });
        match (&op.shape, place) {
            (&Shape::VarArray { elem, cap }, _) => {
                let size = if elem == kind::STR { 2 + cap as usize } else { kind::size(elem) };
                let n = frame.len().checked_div(size).unwrap_or(0);
                let mut sub = Decoder { data: frame, mode: DecodeMode::Packed { pos: 0 } };
                (0..n).map(|_| E::__decode(&mut sub)).collect()
            }
            (Shape::VecStruct { sub }, Place::VecStruct { elem_wire, sub: sub_places }) => {
                let n = frame.len().checked_div(*elem_wire as usize).unwrap_or(0);
                (0..n)
                    .map(|k| {
                        let base = k * *elem_wire as usize;
                        let mut d = Decoder { data: frame, mode: DecodeMode::Ops { schema, ops: sub, places: sub_places, base, i: 0, elem: 0 } };
                        E::__decode(&mut d)
                    })
                    .collect()
            }
            _ => Err("a Vec met a field that is not a variable array".to_owned()),
        }
    }
}

macro_rules! scalar {
    ($($t:ty => $k:ident),* $(,)?) => {$(
        impl Message for $t {
            fn __spell(s: &mut Spell) {
                s.scalar(kind::$k)
            }
            fn __encode(&self, e: &mut Encoder<'_>) -> Result<(), String> {
                e.leaf(&self.to_le_bytes())
            }
            fn __decode(d: &mut Decoder<'_>) -> Result<Self, String> {
                let b = d.leaf(std::mem::size_of::<$t>())?;
                Ok(<$t>::from_le_bytes(b.try_into().map_err(|_| "a short number")?))
            }
        }
    )*};
}

scalar!(u8 => U8, u16 => U16, u32 => U32, u64 => U64, i8 => I8, i16 => I16, i32 => I32, i64 => I64, f32 => F32, f64 => F64);

impl Message for bool {
    fn __spell(s: &mut Spell) {
        s.scalar(kind::BOOL)
    }
    fn __encode(&self, e: &mut Encoder<'_>) -> Result<(), String> {
        e.leaf(&[*self as u8])
    }
    fn __decode(d: &mut Decoder<'_>) -> Result<Self, String> {
        Ok(d.leaf(1)?[0] != 0)
    }
}

impl Message for String {
    fn __spell(s: &mut Spell) {
        s.var_string()
    }
    fn __encode(&self, e: &mut Encoder<'_>) -> Result<(), String> {
        e.var_string(self)
    }
    fn __decode(d: &mut Decoder<'_>) -> Result<Self, String> {
        String::from_utf8(d.var_string()?.to_vec()).map_err(|_| "a string field is not UTF-8".to_owned())
    }
}

impl<E: Message> Message for Vec<E> {
    fn __spell(s: &mut Spell) {
        s.var_array::<E>()
    }
    fn __encode(&self, e: &mut Encoder<'_>) -> Result<(), String> {
        e.sequence(self.iter())
    }
    fn __decode(d: &mut Decoder<'_>) -> Result<Self, String> {
        d.sequence::<E>()
    }
}

impl<E: Message, const N: usize> Message for [E; N] {
    fn __spell(s: &mut Spell) {
        s.fixed_array::<E>(N)
    }
    fn __encode(&self, e: &mut Encoder<'_>) -> Result<(), String> {
        self.iter().try_for_each(|x| x.__encode(e))
    }
    fn __decode(d: &mut Decoder<'_>) -> Result<Self, String> {
        let v = (0..N).map(|_| E::__decode(d)).collect::<Result<Vec<E>, String>>()?;
        v.try_into().map_err(|_| "a fixed array came back short".to_owned())
    }
}
