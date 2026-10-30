use std::any::{TypeId, type_name};
use std::borrow::{Borrow, Cow};
use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::ffi::CString;
use std::sync::{Arc, Mutex};

use crate::error::{Error, ErrorKind};
use crate::ffi;
use crate::node::{NodeInner, lock};
use crate::schema::{FieldView, MessageView, Schema, c_bytes, rbytes};

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
    /// A variable array's elements as the wire packs them, one after another. A number's
    /// slice goes at once, since walking 6 MB of pixels one element at a time is slow.
    #[doc(hidden)]
    fn __pack(items: &[Self]) -> Result<Cow<'_, [u8]>, String> {
        pack_each(items)
    }
    /// `n` elements from a variable array's packed bytes.
    #[doc(hidden)]
    fn __unpack(packed: &[u8], n: usize) -> Result<Vec<Self>, String> {
        unpack_each(packed, n)
    }
}

fn pack_each<E: Message>(items: &[E]) -> Result<Cow<'_, [u8]>, String> {
    let mut packed = Vec::new();
    let mut e = Encoder { buf: &mut packed, mode: EncodeMode::Append };
    items.iter().try_for_each(|x| x.__encode(&mut e))?;
    Ok(Cow::Owned(packed))
}

fn unpack_each<E: Message>(packed: &[u8], n: usize) -> Result<Vec<E>, String> {
    let mut d = Decoder { data: packed, mode: DecodeMode::Packed { pos: 0 } };
    (0..n).map(|_| E::__decode(&mut d)).collect()
}

/// A primitive number: no padding, and every bit pattern is a value.
///
/// # Safety
/// Only for the integer and float primitives, whose memory may be read and written as bytes.
unsafe trait Number: Message + Copy {}

/// On a little endian target a slice of numbers is already its wire packing.
fn pack_numbers<T: Number>(items: &[T]) -> Result<Cow<'_, [u8]>, String> {
    if cfg!(target_endian = "big") {
        return pack_each(items);
    }
    // SAFETY: a Number's memory is initialised bytes with no padding, little endian here
    Ok(Cow::Borrowed(unsafe { std::slice::from_raw_parts(items.as_ptr().cast::<u8>(), std::mem::size_of_val(items)) }))
}

fn unpack_numbers<T: Number>(packed: &[u8], n: usize) -> Result<Vec<T>, String> {
    if cfg!(target_endian = "big") {
        return unpack_each(packed, n);
    }
    let len = n.checked_mul(std::mem::size_of::<T>()).filter(|&len| len <= packed.len()).ok_or("an array lies past the message")?;
    let mut v = Vec::<T>::with_capacity(n);
    // SAFETY: the copy fills all n elements, and any bytes are a Number's value
    unsafe {
        std::ptr::copy_nonoverlapping(packed.as_ptr(), v.as_mut_ptr().cast::<u8>(), len);
        v.set_len(n);
    }
    Ok(v)
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
    /// An owned value, as a reply, a call's response or a variable read holds it: the value,
    /// or for [`Bytes`] the bytes.
    type Owned: Borrow<Self::Input> + Send + 'static;
    /// What a subscriber handler and a take see: the value, or for [`Bytes`] a [`MessageView`].
    type Value<'a>;
    /// What a request, a progress update or a variable update holds inside its callback:
    /// the value, or for [`Bytes`] a [`FieldView`].
    type Data<'a>;

    #[doc(hidden)]
    #[allow(private_interfaces)]
    fn __codec(inner: &Arc<NodeInner>, schema: Option<&Schema>, reflect: bool, what: &str) -> Result<Option<Arc<Codec>>, Error>;
    #[doc(hidden)]
    #[allow(private_interfaces)]
    fn __with_bytes<R>(codec: Option<&Codec>, value: &Self::Input, f: impl FnOnce(&[u8]) -> R) -> Result<R, String>;
    #[doc(hidden)]
    #[allow(private_interfaces)]
    fn __value<'a>(codec: Option<&Codec>, view: &MessageView<'a>) -> Result<Self::Value<'a>, String>;
    #[doc(hidden)]
    #[allow(private_interfaces)]
    fn __taken<'a>(codec: Option<&Codec>, view: MessageView<'a>) -> Result<Self::Value<'a>, String>;
    #[doc(hidden)]
    #[allow(private_interfaces)]
    fn __data<'a>(codec: Option<&Codec>, data: &'a [u8], schema: *const ffi::RantSchema) -> Result<Self::Data<'a>, String>;
    #[doc(hidden)]
    #[allow(private_interfaces)]
    fn __owned(codec: Option<&Codec>, data: &[u8], schema: *const ffi::RantSchema) -> Result<Self::Owned, String>;
}

impl<T: Message> Payload for T {
    type Input = T;
    type Owned = T;
    type Value<'a> = T;
    type Data<'a> = T;

    #[allow(private_interfaces)]
    fn __codec(inner: &Arc<NodeInner>, schema: Option<&Schema>, reflect: bool, what: &str) -> Result<Option<Arc<Codec>>, Error> {
        if schema.is_some() || reflect {
            return Err(Error::new(
                ErrorKind::State,
                format!("{what}: a typed handle takes its schema from its type, use rant::Bytes for a schema or reflect_from_mesh"),
            ));
        }
        Codec::of::<T>(inner).map(Some)
    }

    #[allow(private_interfaces)]
    fn __with_bytes<R>(codec: Option<&Codec>, value: &T, f: impl FnOnce(&[u8]) -> R) -> Result<R, String> {
        codec.ok_or("no codec")?.encode(value, f)
    }

    #[allow(private_interfaces)]
    fn __value<'a>(codec: Option<&Codec>, view: &MessageView<'a>) -> Result<T, String> {
        Self::__owned(codec, view.data(), view.raw_schema())
    }

    #[allow(private_interfaces)]
    fn __taken<'a>(codec: Option<&Codec>, view: MessageView<'a>) -> Result<T, String> {
        Self::__value(codec, &view)
    }

    #[allow(private_interfaces)]
    fn __data(codec: Option<&Codec>, data: &[u8], schema: *const ffi::RantSchema) -> Result<T, String> {
        Self::__owned(codec, data, schema)
    }

    #[allow(private_interfaces)]
    fn __owned(codec: Option<&Codec>, data: &[u8], schema: *const ffi::RantSchema) -> Result<T, String> {
        codec.ok_or("no codec")?.decode(data, schema)
    }
}

impl Payload for Bytes {
    type Input = [u8];
    type Owned = Vec<u8>;
    type Value<'a> = MessageView<'a>;
    type Data<'a> = FieldView<'a>;

    #[allow(private_interfaces)]
    fn __codec(_: &Arc<NodeInner>, _: Option<&Schema>, _: bool, _: &str) -> Result<Option<Arc<Codec>>, Error> {
        Ok(None)
    }

    #[allow(private_interfaces)]
    fn __with_bytes<R>(_: Option<&Codec>, value: &[u8], f: impl FnOnce(&[u8]) -> R) -> Result<R, String> {
        Ok(f(value))
    }

    #[allow(private_interfaces)]
    fn __value<'a>(_: Option<&Codec>, view: &MessageView<'a>) -> Result<MessageView<'a>, String> {
        Ok(view.reborrow())
    }

    #[allow(private_interfaces)]
    fn __taken<'a>(_: Option<&Codec>, view: MessageView<'a>) -> Result<MessageView<'a>, String> {
        Ok(view)
    }

    #[allow(private_interfaces)]
    fn __data<'a>(_: Option<&Codec>, data: &'a [u8], schema: *const ffi::RantSchema) -> Result<FieldView<'a>, String> {
        Ok(FieldView::new(data, schema))
    }

    #[allow(private_interfaces)]
    fn __owned(_: Option<&Codec>, data: &[u8], _: *const ffi::RantSchema) -> Result<Vec<u8>, String> {
        Ok(data.to_vec())
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

/// One leaf or variable value of the flattened type, in the order the type's encode visits it.
#[derive(Clone)]
pub(crate) struct Op {
    /// its row's path within its record
    path: String,
    /// its index in each enclosing fixed array within the record, outermost first
    elems: Vec<u16>,
    shape: Shape,
}

#[derive(Clone)]
enum Shape {
    /// a scalar, enum or capped string slot, or a fixed array of them, stride bytes apart
    Fixed { kind: u8, count: u16, cap: u16, is_enum: bool, is_array: bool, stride: usize },
    VarString,
    /// a variable array of scalars, enums or capped strings, one packed value
    Packed { kind: u8, cap: u16, is_enum: bool },
    /// an array whose elements go one at a time with the element's ops: a variable array of
    /// anything else, or a fixed array of variable elements
    Elements { fixed_n: u16, sub: Vec<Op> },
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

/// A lone scalar, enum or capped string at the element's own row: an element that packs.
fn lone_leaf(sub: &[Op]) -> Option<(u8, u16, bool)> {
    match sub {
        [Op { path, shape: Shape::Fixed { kind, cap, is_enum, is_array: false, .. }, .. }] if path.is_empty() => Some((*kind, *cap, *is_enum)),
        _ => None,
    }
}

/// A row's path under base. An index joins with no dot.
fn join(base: &str, rel: &str) -> String {
    if rel.is_empty() {
        base.to_owned()
    } else if base.is_empty() || rel.starts_with('[') {
        format!("{base}{rel}")
    } else {
        format!("{base}.{rel}")
    }
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
    /// the suffixes of the arrays waiting for their element's spelling, outermost first
    suffixes: Vec<String>,
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
            suffixes: Vec::new(),
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

    /// A type's name or token, then the suffixes of the arrays around it, innermost first.
    fn base(&mut self, s: &str) {
        self.put(s);
        while let Some(suffix) = self.suffixes.pop() {
            self.put(&suffix);
        }
        self.at_root = false;
    }

    fn emit(&mut self, op: Op) {
        if self.mute_ops == 0 {
            self.ops.push(op);
        }
    }

    fn push_op(&mut self, shape: Shape) {
        let op = Op { path: self.path.clone(), elems: self.elems.clone(), shape };
        self.emit(op);
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

    /// One element's spelling, its ops relative to the element.
    fn element<E: Message>(&mut self) -> Vec<Op> {
        self.at_root = false;
        let saved = (std::mem::take(&mut self.ops), std::mem::take(&mut self.path), std::mem::take(&mut self.prefix), std::mem::take(&mut self.elems));
        E::__spell(self);
        let sub = std::mem::replace(&mut self.ops, saved.0);
        (self.path, self.prefix, self.elems) = (saved.1, saved.2, saved.3);
        sub
    }

    /// A fixed array `E[n]`. Fixed elements flatten into leaves, variable ones go one at a time.
    pub fn fixed_array<E: Message>(&mut self, n: usize) {
        let Ok(n) = u16::try_from(n) else {
            return self.refuse(format!("a fixed array of {n} elements is over 65535"));
        };
        self.suffixes.push(format!("[{n}]"));
        let sub = self.element::<E>();
        if let Some((kind, cap, is_enum)) = lone_leaf(&sub) {
            self.push_op(Shape::fixed(kind, n, cap, is_enum, true));
        } else if sub.iter().all(|op| matches!(op.shape, Shape::Fixed { .. })) {
            for i in 0..n {
                let at = format!("{}[{i}]", self.path);
                for op in &sub {
                    let mut elems = self.elems.clone();
                    elems.push(i);
                    elems.extend_from_slice(&op.elems);
                    self.emit(Op { path: join(&at, &op.path), elems, shape: op.shape.clone() });
                }
            }
        } else {
            self.push_op(Shape::Elements { fixed_n: n, sub });
        }
    }

    /// A variable array `E[]`.
    pub fn var_array<E: Message>(&mut self) {
        self.suffixes.push("[]".to_owned());
        let sub = self.element::<E>();
        let shape = match lone_leaf(&sub) {
            Some((kind, cap, is_enum)) => Shape::Packed { kind, cap, is_enum },
            None => Shape::Elements { fixed_n: 0, sub },
        };
        self.push_op(shape);
    }

    /// A number or bool.
    pub fn scalar(&mut self, k: u8) {
        self.base(kind::name(k));
        self.push_op(Shape::fixed(k, 1, 0, false, false));
    }

    /// A capped string `string<cap>`.
    pub fn fixed_string(&mut self, cap: usize) {
        let Ok(cap) = u16::try_from(cap) else {
            return self.refuse(format!("a string capacity of {cap} is over 65535"));
        };
        self.base(&format!("string<{cap}>"));
        self.push_op(Shape::fixed(kind::STR, 1, cap, false, false));
    }

    /// A variable string `string`.
    pub fn var_string(&mut self) {
        self.base("string");
        self.push_op(Shape::VarString);
    }

    /// A named integer `enum<k> { A=0, B=1 }`, spelled inline as C++ does.
    pub fn enumeration(&mut self, backing: u8, options: &[(&str, i64)]) {
        let body: Vec<String> = options.iter().map(|(n, v)| format!("{n}={v}")).collect();
        self.base(&format!("enum<{}> {{ {} }}", kind::name(backing), body.join(", ")));
        self.push_op(Shape::fixed(backing, 1, 0, true, false));
    }

    /// A struct: at the root its own definition, elsewhere its name, defined once above.
    pub fn structure(&mut self, id: TypeId, name: &str, members: fn(&mut Spell)) {
        if std::mem::replace(&mut self.at_root, false) {
            self.put(name);
            self.put(" { ");
            let t = self.text.last_mut().unwrap();
            t.first = true;
            members(self);
            self.put(" }");
            return;
        }
        self.base(name);
        self.define(id, name, members);
        self.members_under(members);
    }

    /// A standard type: always in scope by name, so it is never defined.
    pub fn standard(&mut self, name: &str, members: fn(&mut Spell)) {
        if std::mem::replace(&mut self.at_root, false) {
            self.put(name);
            self.quiet(1);
            members(self);
            self.quiet(-1);
            return;
        }
        self.base(name);
        self.members_under(members);
    }

    /// A standard alias such as `Timestamp`, spelled by name and carried as its repr.
    pub fn standard_alias(&mut self, name: &str, repr: fn(&mut Spell)) {
        self.base(name);
        self.quiet(1);
        repr(self);
        self.quiet(-1);
    }

    /// The members' ops one level down, under this member's path, spelling nothing.
    fn members_under(&mut self, members: fn(&mut Spell)) {
        let prefix = if self.path.is_empty() { String::new() } else { format!("{}.", self.path) };
        let saved = std::mem::replace(&mut self.prefix, prefix);
        self.quiet(1);
        members(self);
        self.quiet(-1);
        self.prefix = saved;
    }

    /// The definition `Name { ... }` once, after the definitions it uses.
    fn define(&mut self, id: TypeId, name: &str, members: fn(&mut Spell)) {
        if !self.defined.insert(id) {
            return;
        }
        let saved = (std::mem::take(&mut self.prefix), std::mem::take(&mut self.path), std::mem::take(&mut self.elems), std::mem::take(&mut self.suffixes));
        self.text.push(TextCtx { buf: format!("{name} {{ "), first: true, quiet: 0 });
        self.mute_ops += 1;
        members(self);
        self.mute_ops -= 1;
        let def = self.text.pop().unwrap().buf;
        self.defs.push_str(&def);
        self.defs.push_str(" }\n");
        (self.prefix, self.path, self.elems, self.suffixes) = saved;
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
    /// a leaf's offset in its record's fixed part
    Fixed(u32),
    /// a variable value's row
    Var(u16),
    /// an array's row and its element row's sizes, and where the element's ops live in it
    Elements { row: u16, elem_wire: u32, elem_min: u32, fixed: bool, leaves: bool, sub: Vec<Place> },
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

/// The wire offset of row fi within its record or element, reached through the fixed arrays
/// around it by elems up to row stop. None when an index is out of range or a variable array
/// lies between.
fn static_offset(s: *const ffi::RantSchema, mut fi: ffi::RantSchemaFieldInfo, elems: &[u16], stop: u16) -> Option<u32> {
    let mut off = fi.offset as u64;
    let mut k = elems.len();
    while fi.arr_parent != stop {
        if fi.arr_parent == NO_PARENT {
            return None;
        }
        let (pa, el) = (info(s, fi.arr_parent)?, info(s, fi.arr_parent + 1)?);
        k = k.checked_sub(1)?;
        let e = elems[k];
        if pa.kind != kind::ARR || pa.size == 0 || e >= pa.count {
            return None;
        }
        off += e as u64 * el.size as u64 + pa.offset as u64;
        fi = pa;
    }
    if k != 0 {
        return None;
    }
    u32::try_from(off).ok()
}

/// The row holds this leaf: the scalar kind, the enum backing or the capped string.
fn leaf_fits(fi: &ffi::RantSchemaFieldInfo, k: u8, cap: u16, is_enum: bool) -> bool {
    if is_enum {
        fi.kind == kind::ENUM && fi.backing == k
    } else if k == kind::STR {
        fi.kind == kind::STR && fi.str_cap == cap
    } else {
        fi.kind == k
    }
}

/// The ops' rows and offsets in schema s, every kind verified. base is the path of the ops'
/// record, stop the array it is an element of, room its fixed bytes.
fn resolve(ops: &[Op], s: *const ffi::RantSchema, base: &str, stop: u16, room: u32) -> Option<Vec<Place>> {
    let mut places = Vec::with_capacity(ops.len());
    for op in ops {
        let path = join(base, &op.path);
        let cpath = CString::new(path.as_str()).ok()?;
        // SAFETY: the caller holds the schema's node open
        let idx = u16::try_from(unsafe { ffi::rant_schema_field_index(s, cpath.as_ptr()) }).ok()?;
        let fi = info(s, idx)?;
        let place = match &op.shape {
            &Shape::Fixed { kind: k, count, cap, is_enum, is_array, stride } => {
                let fits = if is_array {
                    fi.kind == kind::ARR && fi.count == count && leaf_fits(&info(s, idx + 1)?, k, cap, is_enum)
                } else {
                    leaf_fits(&fi, k, cap, is_enum)
                };
                let off = static_offset(s, fi, &op.elems, stop).filter(|_| fits)?;
                if off as u64 + stride as u64 * count as u64 > room as u64 {
                    return None;
                }
                Place::Fixed(off)
            }
            Shape::VarString if fi.kind == kind::VSTR => Place::Var(idx),
            &Shape::Packed { kind: k, cap, is_enum } if fi.kind == kind::VARR => {
                let el = info(s, idx + 1)?;
                if el.size == 0 || el.min_len != el.size || !leaf_fits(&el, k, cap, is_enum) {
                    return None;
                }
                Place::Var(idx)
            }
            &Shape::Elements { fixed_n, ref sub } if fi.kind == if fixed_n > 0 { kind::ARR } else { kind::VARR } && (fixed_n == 0 || fi.count == fixed_n) => {
                let el = info(s, idx + 1)?;
                let sub_places = resolve(sub, s, &join(&path, "[0]"), idx, el.size)?;
                let leaves = sub.iter().any(|o| matches!(o.shape, Shape::Fixed { .. }));
                Place::Elements { row: idx, elem_wire: el.size, elem_min: el.min_len, fixed: el.size != 0 && el.min_len == el.size, leaves, sub: sub_places }
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
    /// plans per delivered schema address, which names one schema until the node closes
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
        // SAFETY: as above
        let (hash, size, msg_min) =
            unsafe { (ffi::rant_schema_hash(schema), ffi::rant_schema_size(schema), ffi::rant_schema_msg_min(schema)) };
        let places = resolve(&ops, schema, "", NO_PARENT, size)
            .ok_or_else(|| Error::new(ErrorKind::BadSchema, format!("{what}: the type does not fit its own schema")))?;
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

    fn plan(&self, s: *const ffi::RantSchema) -> Result<Arc<Plan>, String> {
        if s.is_null() || s == self.schema {
            return Ok(Arc::clone(&self.own));
        }
        let mut plans = lock(&self.plans);
        if let Some(p) = plans.get(&(s as usize)) {
            return Ok(Arc::clone(p));
        }
        // SAFETY: the caller holds the delivered schema valid
        let size = unsafe { ffi::rant_schema_size(s) };
        let places = resolve(&self.ops, s, "", NO_PARENT, size).ok_or_else(|| format!("the publisher's schema does not fit {}", self.name))?;
        let plan = Arc::new(Plan { places, size });
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
            let mut e = Encoder { buf, mode: EncodeMode::Ops { schema: self.schema, ix: &[], ops: &self.ops, places: &self.own.places, base: 0, i: 0, elem: 0 } };
            v.__encode(&mut e)?;
            // SAFETY: as above
            let n = unsafe { ffi::rant_schema_msg_len(self.schema, e.buf.as_ptr().cast(), e.buf.len()) } as usize;
            if n == 0 && self.msg_min > 0 {
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
        let mut d = Decoder { data, mode: DecodeMode::Ops { schema: s, ix: &[], ops: &self.ops, places: &plan.places, base: 0, i: 0, elem: 0 } };
        T::__decode(&mut d)
    }
}

/// The next op and its place, for a variable value.
type Next<'a> = (*const ffi::RantSchema, &'a [u32], &'a Op, &'a Place);

enum EncodeMode<'a> {
    /// ix holds the index in each enclosing array, base the record's fixed part in the buffer
    Ops { schema: *const ffi::RantSchema, ix: &'a [u32], ops: &'a [Op], places: &'a [Place], base: usize, i: usize, elem: u16 },
    /// a variable array's packed elements, appended in order
    Append,
}

/// Writes a value's leaves and variable values in the order its type's spelling flattened them.
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
                let Shape::Fixed { count, .. } = op.shape else { return Err("a fixed field met a variable one".to_owned()) };
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

    fn next(&mut self) -> Result<Next<'a>, String> {
        match &mut self.mode {
            EncodeMode::Ops { schema, ix, ops, places, i, .. } => {
                let (ix, ops, places): (&'a [u32], &'a [Op], &'a [Place]) = (ix, ops, places);
                let (op, place) = ops.get(*i).zip(places.get(*i)).ok_or("the value has more fields than its schema")?;
                *i += 1;
                Ok((*schema, ix, op, place))
            }
            EncodeMode::Append => Err("a variable value inside a packed element".to_owned()),
        }
    }

    /// True when the next op is an array whose elements go one at a time.
    fn next_is_elements(&self) -> bool {
        matches!(&self.mode, EncodeMode::Ops { ops, i, .. } if matches!(ops.get(*i), Some(Op { shape: Shape::Elements { .. }, .. })))
    }

    /// Room for a variable value to grow by `extra` past the live message.
    fn grow(buf: &mut Vec<u8>, schema: *const ffi::RantSchema, extra: usize) {
        // SAFETY: the buffer is a live message of the schema, held open by the caller
        let used = unsafe { ffi::rant_schema_msg_len(schema, buf.as_ptr().cast(), buf.len()) } as usize;
        if let Some(add) = (used + extra).checked_sub(buf.len()).filter(|&add| add > 0) {
            buf.reserve(add);
            // SAFETY: the reserve made room, and the zeros land before the length covers them. A
            // memset, where resize writes byte by byte in an unoptimised build
            unsafe {
                std::ptr::write_bytes(buf.as_mut_ptr().add(buf.len()), 0, add);
                buf.set_len(buf.len() + add);
            }
        }
    }

    /// Sets the value at row under ix through the C value accessor.
    fn set(&mut self, schema: *const ffi::RantSchema, row: u16, ix: &[u32], val: &ffi::RantValue) -> bool {
        // SAFETY: the buffer and its length are live, and the caller holds the node open
        unsafe { ffi::rant_set_value_at(self.buf.as_mut_ptr().cast(), self.buf.len(), schema, row, ix.as_ptr(), ix.len() as u16, val) != 0 }
    }

    /// The offset in the buffer of row's bytes under ix: an element's fixed part.
    fn view(&self, schema: *const ffi::RantSchema, row: u16, ix: &[u32], need: u32) -> Result<usize, String> {
        let mut got = ffi::RantValue::default();
        // SAFETY: the buffer is a live message of the schema, held open by the caller
        let ok = unsafe { ffi::rant_get_value_at(c_bytes(self.buf), schema, row, ix.as_ptr(), ix.len() as u16, &mut got) } != 0;
        if !ok || got.bytes.data.is_null() || got.bytes.len < need as usize {
            return Err("an array element found no room".to_owned());
        }
        Ok(got.bytes.data as usize - self.buf.as_ptr() as usize)
    }

    /// The next variable string.
    pub fn var_string(&mut self, v: &str) -> Result<(), String> {
        let (schema, ix, _, place) = self.next()?;
        let &Place::Var(row) = place else { return Err("a String met a field that is not a string".to_owned()) };
        Encoder::grow(self.buf, schema, v.len());
        let val = ffi::RantValue { bytes: c_bytes(v.as_bytes()), ..Default::default() };
        if self.set(schema, row, ix, &val) { Ok(()) } else { Err("a String did not fit its field".to_owned()) }
    }

    /// The next variable array, from its elements in order.
    pub fn sequence<E: Message>(&mut self, items: &[E]) -> Result<(), String> {
        let (schema, ix, op, place) = self.next()?;
        match (&op.shape, place) {
            (Shape::Packed { .. }, &Place::Var(row)) => {
                let packed = E::__pack(items)?;
                Encoder::grow(self.buf, schema, packed.len());
                let val = ffi::RantValue { bytes: c_bytes(&packed), ..Default::default() };
                if self.set(schema, row, ix, &val) { Ok(()) } else { Err("a Vec did not fit its field".to_owned()) }
            }
            (Shape::Elements { sub, .. }, &Place::Elements { row, elem_wire, elem_min, fixed, leaves, sub: ref sub_places }) => {
                let count = u32::try_from(items.len()).map_err(|_| "a Vec is over 4294967295 elements".to_owned())?;
                // each element adds its end and its default body, or its fixed bytes
                Encoder::grow(self.buf, schema, items.len() * if fixed { elem_wire as usize } else { 4 + elem_min as usize });
                // SAFETY: as set
                let sized = unsafe { ffi::rant_set_array_count_at(self.buf.as_mut_ptr().cast(), self.buf.len(), schema, row, ix.as_ptr(), ix.len() as u16, count) } != 0;
                if !sized {
                    return Err("a Vec did not fit its field".to_owned());
                }
                self.elements(schema, ix, items, row, elem_wire, leaves, sub, sub_places)
            }
            _ => Err("a Vec met a field that is not a variable array".to_owned()),
        }
    }

    /// A fixed array: its leaves in order, or each variable element on its own.
    pub fn array<E: Message>(&mut self, items: &[E]) -> Result<(), String> {
        if !self.next_is_elements() {
            return items.iter().try_for_each(|x| x.__encode(self));
        }
        let (schema, ix, op, place) = self.next()?;
        let (Shape::Elements { sub, .. }, &Place::Elements { row, elem_wire, leaves, sub: ref sub_places, .. }) = (&op.shape, place) else {
            return Err("a fixed array met a field that is not an array".to_owned());
        };
        self.elements(schema, ix, items, row, elem_wire, leaves, sub, sub_places)
    }

    /// Each element through the element row after row, its fixed part a view of the buffer.
    #[allow(clippy::too_many_arguments)]
    fn elements<E: Message>(&mut self, schema: *const ffi::RantSchema, ix: &[u32], items: &[E], row: u16, elem_wire: u32, leaves: bool, sub: &[Op], sub_places: &[Place]) -> Result<(), String> {
        let mut at = ix.to_vec();
        at.push(0);
        for (k, item) in items.iter().enumerate() {
            *at.last_mut().unwrap() = k as u32;
            let base = if leaves { self.view(schema, row + 1, &at, elem_wire)? } else { 0 };
            let mut e = Encoder { buf: &mut *self.buf, mode: EncodeMode::Ops { schema, ix: &at, ops: sub, places: sub_places, base, i: 0, elem: 0 } };
            item.__encode(&mut e)?;
        }
        Ok(())
    }
}

enum DecodeMode<'a> {
    /// as EncodeMode::Ops, base the record's fixed part in the message
    Ops { schema: *const ffi::RantSchema, ix: &'a [u32], ops: &'a [Op], places: &'a [Place], base: usize, i: usize, elem: u16 },
    /// a variable array's packed elements, read in order
    Packed { pos: usize },
}

/// Reads a value's leaves and variable values in the order its type's spelling flattened them.
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
                let Shape::Fixed { count, .. } = op.shape else { return Err("a fixed field met a variable one".to_owned()) };
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

    fn next(&mut self) -> Result<Next<'a>, String> {
        match &mut self.mode {
            DecodeMode::Ops { schema, ix, ops, places, i, .. } => {
                let (ix, ops, places): (&'a [u32], &'a [Op], &'a [Place]) = (ix, ops, places);
                let (op, place) = ops.get(*i).zip(places.get(*i)).ok_or("the type has more fields than the message")?;
                *i += 1;
                Ok((*schema, ix, op, place))
            }
            DecodeMode::Packed { .. } => Err("a variable value inside a packed element".to_owned()),
        }
    }

    fn next_is_elements(&self) -> bool {
        matches!(&self.mode, DecodeMode::Ops { ops, i, .. } if matches!(ops.get(*i), Some(Op { shape: Shape::Elements { .. }, .. })))
    }

    /// The value at row under ix through the C value accessor.
    fn get(&self, schema: *const ffi::RantSchema, row: u16, ix: &[u32]) -> Result<ffi::RantValue, String> {
        let mut v = ffi::RantValue::default();
        // SAFETY: the data is a delivered message of the schema
        let ok = unsafe { ffi::rant_get_value_at(c_bytes(self.data), schema, row, ix.as_ptr(), ix.len() as u16, &mut v) } != 0;
        if ok { Ok(v) } else { Err("a field is missing from the message".to_owned()) }
    }

    /// The next variable string's bytes.
    pub fn var_string(&mut self) -> Result<&'a [u8], String> {
        let (schema, ix, _, place) = self.next()?;
        let &Place::Var(row) = place else { return Err("a String met a field that is not a string".to_owned()) };
        Ok(rbytes(self.get(schema, row, ix)?.bytes))
    }

    /// The next variable array's elements.
    pub fn sequence<E: Message>(&mut self) -> Result<Vec<E>, String> {
        let (schema, ix, op, place) = self.next()?;
        match (&op.shape, place) {
            (Shape::Packed { .. }, &Place::Var(row)) => {
                let v = self.get(schema, row, ix)?;
                E::__unpack(rbytes(v.bytes), v.count as usize)
            }
            (Shape::Elements { sub, .. }, &Place::Elements { row, elem_wire, leaves, sub: ref sub_places, .. }) => {
                let n = self.get(schema, row, ix)?.count as usize;
                self.elements(schema, ix, n, row, elem_wire, leaves, sub, sub_places)
            }
            _ => Err("a Vec met a field that is not a variable array".to_owned()),
        }
    }

    /// A fixed array's n elements: its leaves in order, or each variable element on its own.
    pub fn array<E: Message>(&mut self, n: usize) -> Result<Vec<E>, String> {
        if !self.next_is_elements() {
            return (0..n).map(|_| E::__decode(self)).collect();
        }
        let (schema, ix, op, place) = self.next()?;
        let (Shape::Elements { sub, .. }, &Place::Elements { row, elem_wire, leaves, sub: ref sub_places, .. }) = (&op.shape, place) else {
            return Err("a fixed array met a field that is not an array".to_owned());
        };
        self.elements(schema, ix, n, row, elem_wire, leaves, sub, sub_places)
    }

    /// n elements through the element row after row, each fixed part a view of the message.
    #[allow(clippy::too_many_arguments)]
    fn elements<E: Message>(&self, schema: *const ffi::RantSchema, ix: &[u32], n: usize, row: u16, elem_wire: u32, leaves: bool, sub: &[Op], sub_places: &[Place]) -> Result<Vec<E>, String> {
        let mut at = ix.to_vec();
        at.push(0);
        let mut out = Vec::with_capacity(n.min(self.data.len()));
        for k in 0..n {
            *at.last_mut().unwrap() = k as u32;
            let base = if leaves {
                let v = self.get(schema, row + 1, &at)?;
                if v.bytes.data.is_null() || v.bytes.len < elem_wire as usize {
                    return Err("an array element lies past the message".to_owned());
                }
                v.bytes.data as usize - self.data.as_ptr() as usize
            } else {
                0
            };
            let mut d = Decoder { data: self.data, mode: DecodeMode::Ops { schema, ix: &at, ops: sub, places: sub_places, base, i: 0, elem: 0 } };
            out.push(E::__decode(&mut d)?);
        }
        Ok(out)
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
            fn __pack(items: &[Self]) -> Result<Cow<'_, [u8]>, String> {
                pack_numbers(items)
            }
            fn __unpack(packed: &[u8], n: usize) -> Result<Vec<Self>, String> {
                unpack_numbers(packed, n)
            }
        }

        // SAFETY: an integer or float primitive
        unsafe impl Number for $t {}
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
        e.sequence(self)
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
        e.array(self)
    }
    fn __decode(d: &mut Decoder<'_>) -> Result<Self, String> {
        let v = d.array::<E>(N)?;
        v.try_into().map_err(|_| "a fixed array came back short".to_owned())
    }
}
