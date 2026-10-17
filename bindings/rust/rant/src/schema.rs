use std::borrow::Cow;
use std::ffi::CString;
use std::fmt;
use std::os::raw::c_char;
use std::ptr::null;
use std::sync::Arc;

use crate::error::{Error, ErrorKind};
use crate::ffi;
use crate::gate::Pass;
use crate::map::MapReader;
use crate::node::NodeInner;

/// The valid UTF-8 prefix of bytes, so a hostile name never panics or allocates.
pub(crate) fn utf8_prefix(b: &[u8]) -> &str {
    match std::str::from_utf8(b) {
        Ok(s) => s,
        // SAFETY: from_utf8 vouches for every byte before valid_up_to
        Err(e) => unsafe { std::str::from_utf8_unchecked(&b[..e.valid_up_to()]) },
    }
}

/// A C string view as bytes, empty for a null one.
pub(crate) fn rstring_bytes<'a>(s: ffi::RantString) -> &'a [u8] {
    if s.data.is_null() || s.len == 0 {
        return &[];
    }
    // SAFETY: the C hands data and len describing one live run, valid for 'a by the caller
    unsafe { std::slice::from_raw_parts(s.data.cast(), s.len) }
}

pub(crate) fn rbytes<'a>(b: ffi::RantBytes) -> &'a [u8] {
    if b.data.is_null() || b.len == 0 {
        return &[];
    }
    // SAFETY: as rstring_bytes
    unsafe { std::slice::from_raw_parts(b.data, b.len) }
}

pub(crate) fn c_bytes(b: &[u8]) -> ffi::RantBytes {
    ffi::RantBytes { data: b.as_ptr(), len: b.len() }
}

/// Calls f with a NUL terminated copy of s, on the stack when short. Interior NUL reads as
/// a name no field has.
pub(crate) fn with_c<R>(s: &str, f: impl FnOnce(*const c_char) -> R) -> R {
    let mut buf = [0u8; 128];
    if s.len() < buf.len() && !s.as_bytes().contains(&0) {
        buf[..s.len()].copy_from_slice(s.as_bytes());
        return f(buf.as_ptr().cast());
    }
    let c = CString::new(s).unwrap_or_else(|_| CString::new("\u{1}").unwrap());
    f(c.as_ptr())
}

/// A schema field kind for reflection, the C wire values. Array and String are fixed,
/// VString, VArray and Map ride the message tail and report offset and size 0.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[non_exhaustive]
#[allow(missing_docs)]
pub enum FieldType {
    U8, U16, U32, U64, I8, I16, I32, I64, F32, F64, Bool,
    Array, Struct, String, VString, VArray, Map, Enum, Named,
    /// A kind this binding does not know yet.
    Unknown,
}

impl FieldType {
    pub(crate) fn from_c(k: u8) -> FieldType {
        use FieldType::*;
        const ALL: [FieldType; 19] = [U8, U16, U32, U64, I8, I16, I32, I64, F32, F64, Bool, Array, Struct, String, VString, VArray, Map, Enum, Named];
        ALL.get(k as usize).copied().unwrap_or(Unknown)
    }
}

/// One row of a schema's flat depth first field table, as the C reports it. A struct's
/// members follow it one level deeper.
#[derive(Clone, Debug, PartialEq, Eq)]
#[non_exhaustive]
pub struct Field {
    /// The field's own name, "" for a bare root.
    pub name: String,
    /// The field type's name, None when anonymous.
    pub type_name: Option<String>,
    /// An array element type's name, None when anonymous.
    pub elem_name: Option<String>,
    /// The field's kind. A named type reports what it wraps.
    pub kind: FieldType,
    /// The array element kind, or an enum's backing kind.
    pub elem: FieldType,
    /// A fixed array's element count, or an enum's option count.
    pub count: u16,
    /// 0 at the top level, n for a member n structs down.
    pub depth: u16,
    /// The string capacity of a String field or element.
    pub str_cap: u16,
    /// The flat index of the nearest enclosing struct array, 0xFFFF for none.
    pub arr_parent: u16,
    /// The struct arrays around the field, one index each in an access.
    pub arr_depth: u16,
    /// The byte offset in the fixed section, 0 for a tail kind.
    pub offset: u32,
    /// The byte size, 0 for a tail kind.
    pub size: u32,
    /// The bytes of one array element, else 0.
    pub elem_size: u32,
}

/// One option of an enum field.
#[derive(Clone, Debug, PartialEq, Eq)]
#[non_exhaustive]
pub struct EnumVariant {
    /// The number on the wire.
    pub value: i64,
    /// The option's name.
    pub name: String,
}

/// A compiled message schema from [`Node::schema`](crate::Node::schema) or a handle, cheap to
/// clone. Its node owns it and frees it at close, after which every read answers empty.
#[derive(Clone)]
pub struct Schema {
    pub(crate) inner: Arc<NodeInner>,
    pub(crate) raw: *const ffi::RantSchema,
}

// SAFETY: a compiled schema is immutable, and every read holds a pass
unsafe impl Send for Schema {}
unsafe impl Sync for Schema {}

impl Schema {
    pub(crate) fn new(inner: Arc<NodeInner>, raw: *const ffi::RantSchema) -> Schema {
        Schema { inner, raw }
    }

    fn read<R: Default>(&self, f: impl FnOnce(*const ffi::RantSchema) -> R) -> R {
        match self.inner.gate.enter() {
            Some(_pass) => f(self.raw),
            None => R::default(),
        }
    }

    /// The root type's name, "" for an anonymous or bare type.
    pub fn name(&self) -> String {
        // SAFETY: the pass keeps the schema alive
        self.read(|s| utf8_prefix(rstring_bytes(unsafe { ffi::rant_schema_name(s) })).to_owned())
    }

    /// The shape's identity, the same in every language.
    pub fn hash(&self) -> u64 {
        // SAFETY: as name
        self.read(|s| unsafe { ffi::rant_schema_hash(s) })
    }

    /// The fixed section's bytes, tail kinds excluded.
    pub fn size(&self) -> u32 {
        // SAFETY: as name
        self.read(|s| unsafe { ffi::rant_schema_size(s) })
    }

    /// The canonical bytes a node advertises, which [`Node::schema_from_wire`](crate::Node::schema_from_wire) reads.
    pub fn wire(&self) -> Vec<u8> {
        // SAFETY: as name
        self.read(|s| rbytes(unsafe { ffi::rant_schema_wire(s) }).to_vec())
    }

    /// The flat index of a field by name, a member by dotted path (`velocity.dx`) and a
    /// struct array member by index (`corners[2].x`).
    pub fn field_index(&self, path: &str) -> Option<u16> {
        // SAFETY: as name
        let i = self.read(|s| with_c(path, |p| unsafe { ffi::rant_schema_field_index(s, p) }));
        u16::try_from(i).ok()
    }

    /// The whole flat field table.
    pub fn fields(&self) -> Vec<Field> {
        self.read(|s| {
            // SAFETY: as name
            let n = unsafe { ffi::rant_schema_field_count(s) };
            (0..n).filter_map(|i| field_at(s, i)).collect()
        })
    }

    /// The options of the enum field at flat index `field`, empty for any other field.
    pub fn enum_variants(&self, field: u16) -> Vec<EnumVariant> {
        self.read(|s| {
            // SAFETY: as name
            let n = unsafe { ffi::rant_schema_enum_count(s, field) };
            (0..n)
                .filter_map(|i| {
                    let mut value = 0i64;
                    let mut name = ffi::RantString::default();
                    // SAFETY: as name
                    let ok = unsafe { ffi::rant_schema_enum_variant(s, field, i, &mut value, &mut name) } != 0;
                    ok.then(|| EnumVariant { value, name: utf8_prefix(rstring_bytes(name)).to_owned() })
                })
                .collect()
        })
    }

    /// Can a reader declaring this schema read messages written with `publisher`? A type
    /// name narrows: an anonymous type reads a named one, never the reverse.
    pub fn can_read(&self, publisher: &Schema) -> bool {
        // SAFETY: as name, and publisher's own pass keeps it alive
        self.read(|s| publisher.read(|p| unsafe { ffi::rant_schema_subset(s, p) } != 0))
    }

    /// Why this schema cannot read `publisher`, one line. None when it can.
    pub fn why_not(&self, publisher: &Schema) -> Option<String> {
        self.read(|s| {
            publisher.read(|p| {
                let mut buf = [0 as c_char; 256];
                // SAFETY: as can_read, and the C NUL terminates within cap
                let ok = unsafe { ffi::rant_schema_subset_why(s, p, buf.as_mut_ptr(), buf.len()) } != 0;
                (!ok).then(|| unsafe { std::ffi::CStr::from_ptr(buf.as_ptr()) }.to_string_lossy().into_owned())
            })
        })
    }

    /// Reads `data` as a message of this schema, such as a raw call's response or a raw
    /// variable's value. After the node closed every read answers 0 or empty.
    pub fn view<'a>(&'a self, data: &'a [u8]) -> FieldView<'a> {
        match self.inner.gate.enter() {
            Some(pass) => FieldView { data, schema: self.raw, _pass: Some(pass) },
            None => FieldView::new(data, null()),
        }
    }

    /// The schema spelled back as DSL text, every named type hoisted to a leading definition.
    pub fn to_dsl(&self) -> String {
        self.read(|s| {
            // SAFETY: as name, and a null buffer measures
            let n = unsafe { ffi::rant_schema_print(s, std::ptr::null_mut(), 0) } as usize;
            let mut buf = vec![0u8; n + 1];
            unsafe { ffi::rant_schema_print(s, buf.as_mut_ptr().cast(), buf.len()) };
            buf.truncate(n);
            String::from_utf8_lossy(&buf).into_owned()
        })
    }
}

pub(crate) fn field_at(s: *const ffi::RantSchema, i: u16) -> Option<Field> {
    let mut f = ffi::RantSchemaFieldInfo::default();
    // SAFETY: the caller holds a pass on the schema's node
    if unsafe { ffi::rant_schema_field_at(s, i, &mut f) } == 0 {
        return None;
    }
    let opt = |b: &[u8]| (!b.is_empty()).then(|| utf8_prefix(b).to_owned());
    Some(Field {
        name: utf8_prefix(rstring_bytes(f.name)).to_owned(),
        type_name: opt(rstring_bytes(f.type_name)),
        elem_name: opt(rstring_bytes(f.elem_name)),
        kind: FieldType::from_c(f.kind),
        elem: FieldType::from_c(f.elem),
        count: f.count,
        depth: f.depth,
        str_cap: f.str_cap,
        arr_parent: f.arr_parent,
        arr_depth: f.arr_depth,
        offset: f.offset,
        size: f.size,
        elem_size: f.elem_size,
    })
}

impl PartialEq for Schema {
    /// One handle per shape in a node, so two schemas are equal when they are the same handle.
    fn eq(&self, other: &Schema) -> bool {
        self.raw == other.raw
    }
}

impl Eq for Schema {}

impl fmt::Debug for Schema {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Schema({})", self.to_dsl())
    }
}

/// Field reads by name, dotted path (`at.pose.position.x`) or index (`corners[2].x`). An
/// unknown field or a kind mismatch reads 0 or empty, as the C does.
pub struct FieldView<'a> {
    data: &'a [u8],
    schema: *const ffi::RantSchema,
    // a view outside a callback holds the node open while it lives
    _pass: Option<Pass<'a>>,
}

impl<'a> FieldView<'a> {
    /// A view inside a callback, where the node is open.
    pub(crate) fn new(data: &'a [u8], schema: *const ffi::RantSchema) -> FieldView<'a> {
        FieldView { data, schema, _pass: None }
    }

    pub(crate) fn raw_schema(&self) -> *const ffi::RantSchema {
        self.schema
    }

    fn msg(&self) -> ffi::RantBytes {
        c_bytes(self.data)
    }

    /// The payload bytes.
    pub fn data(&self) -> &'a [u8] {
        self.data
    }

    /// True when the payload came with a schema, which every field read needs.
    pub fn has_schema(&self) -> bool {
        !self.schema.is_null()
    }

    /// An unsigned or bool field, widened.
    pub fn get_uint(&self, field: &str) -> u64 {
        // SAFETY: the view's data and schema are valid for 'a
        with_c(field, |f| unsafe { ffi::rant_get_uint(self.msg(), self.schema, f) })
    }

    /// A signed field, widened. Also an enum field's number.
    pub fn get_int(&self, field: &str) -> i64 {
        // SAFETY: as get_uint
        with_c(field, |f| unsafe { ffi::rant_get_int(self.msg(), self.schema, f) })
    }

    /// An f64 or f32 field.
    pub fn get_f64(&self, field: &str) -> f64 {
        // SAFETY: as get_uint
        with_c(field, |f| unsafe { ffi::rant_get_f64(self.msg(), self.schema, f) })
    }

    /// An f32 field.
    pub fn get_f32(&self, field: &str) -> f32 {
        // SAFETY: as get_uint
        with_c(field, |f| unsafe { ffi::rant_get_f32(self.msg(), self.schema, f) })
    }

    /// A bool field.
    pub fn get_bool(&self, field: &str) -> bool {
        self.get_uint(field) != 0
    }

    /// A fixed array whole, or a variable array's live elements, as little endian bytes.
    pub fn get_array(&self, field: &str) -> &'a [u8] {
        // SAFETY: as get_uint, and the result views the message bytes
        rbytes(with_c(field, |f| unsafe { ffi::rant_get_array(self.msg(), self.schema, f) }))
    }

    /// An array's live element count, a fixed one's count, 0 for any other field.
    pub fn get_array_count(&self, field: &str) -> u32 {
        // SAFETY: as get_uint
        with_c(field, |f| unsafe { ffi::rant_get_array_count(self.msg(), self.schema, f) })
    }

    /// A capped or variable string. Bytes that are not UTF-8 read as replacement characters.
    pub fn get_string(&self, field: &str) -> Cow<'a, str> {
        // SAFETY: as get_array
        String::from_utf8_lossy(rstring_bytes(with_c(field, |f| unsafe { ffi::rant_get_string(self.msg(), self.schema, f) })))
    }

    /// One element of a string array, empty past the live count.
    pub fn get_string_at(&self, field: &str, index: u16) -> Cow<'a, str> {
        // SAFETY: as get_array
        String::from_utf8_lossy(rstring_bytes(with_c(field, |f| unsafe {
            ffi::rant_get_string_at(self.msg(), self.schema, f, index)
        })))
    }

    /// A map field, as a reader over its body.
    pub fn get_map(&self, field: &str) -> MapReader<'a> {
        // SAFETY: as get_array
        MapReader::new(rbytes(with_c(field, |f| unsafe { ffi::rant_get_map(self.msg(), self.schema, f) })))
    }

    /// An enum field's option name, None when the stored number has no option.
    pub fn get_enum_name(&self, field: &str) -> Option<&'a str> {
        // SAFETY: as get_array
        let s = rstring_bytes(with_c(field, |f| unsafe { ffi::rant_get_enum(self.msg(), self.schema, f) }));
        (!s.is_empty()).then(|| utf8_prefix(s))
    }
}

/// A delivered message: its fields through [`FieldView`] and who sent it when. Valid
/// inside the handler, or until the pulled subscriber's next take.
pub struct MessageView<'a> {
    fields: FieldView<'a>,
    inner: &'a NodeInner,
    topic_index: u16,
    publisher_id: u32,
    publisher_name: &'a str,
    topic_name: &'a str,
    recv_us: u64,
    written_us: u64,
    capture_us: u64,
}

impl<'a> MessageView<'a> {
    /// A view of a message the C delivered. Safety: `msg` and every view in it stay valid for 'a.
    pub(crate) unsafe fn delivered(inner: &'a NodeInner, msg: &'a ffi::RantMsg) -> MessageView<'a> {
        MessageView {
            fields: FieldView::new(rbytes(msg.data), msg.schema),
            inner,
            topic_index: msg.topic_index,
            publisher_id: msg.publisher_id,
            publisher_name: utf8_prefix(rstring_bytes(msg.publisher_name)),
            topic_name: utf8_prefix(rstring_bytes(msg.topic_name)),
            recv_us: msg.recv_us,
            written_us: msg.written_us,
            capture_us: msg.capture_us,
        }
    }

    /// A taken view holds the node open while it lives.
    pub(crate) fn hold(mut self, pass: Pass<'a>) -> MessageView<'a> {
        self.fields._pass = Some(pass);
        self
    }

    /// A second view of the same message for a handler. Callback views need no pass.
    pub(crate) fn reborrow(&self) -> MessageView<'a> {
        MessageView {
            fields: FieldView::new(self.fields.data, self.fields.schema),
            inner: self.inner,
            topic_index: self.topic_index,
            publisher_id: self.publisher_id,
            publisher_name: self.publisher_name,
            topic_name: self.topic_name,
            recv_us: self.recv_us,
            written_us: self.written_us,
            capture_us: self.capture_us,
        }
    }

    /// The sending node's name.
    pub fn publisher_name(&self) -> &'a str {
        self.publisher_name
    }

    /// The sending node's peer id.
    pub fn publisher_id(&self) -> u32 {
        self.publisher_id
    }

    /// The topic's name on the mesh.
    pub fn topic_name(&self) -> &'a str {
        self.topic_name
    }

    /// This node's monotonic microseconds at arrival, even when read later from a queue.
    pub fn recv_us(&self) -> u64 {
        self.recv_us
    }

    /// The writer's wall clock in UTC microseconds when it wrote the message. 0 = the
    /// publisher opted out. Never mix it with the monotonic recv_us.
    pub fn written_us(&self) -> u64 {
        self.written_us
    }

    /// When the data was true, as against when it was sent. 0 = none given.
    pub fn capture_us(&self) -> u64 {
        self.capture_us
    }

    /// The schema of the fields: the subscriber's own when it has one, else the
    /// publisher's. None for untyped bytes.
    pub fn schema(&self) -> Option<Schema> {
        if !self.fields.has_schema() {
            return None;
        }
        // a typed delivery may be a rebased view a retire frees, so the stable handle with the
        // same fields is the topic's own. An untyped topic gets the publisher's, kept till close
        let node = self.inner.raw();
        // SAFETY: the view is valid while the node is, and a topic index stays valid with it
        let own = unsafe {
            let topic = if node.is_null() { std::ptr::null_mut() } else { ffi::rant_node_topic(node, self.topic_index) };
            if topic.is_null() { null() } else { ffi::rant_topic_schema(topic) }
        };
        let raw = if own.is_null() { self.fields.raw_schema() } else { own };
        // SAFETY: every NodeInner lives in an Arc
        Some(Schema::new(unsafe { self.inner.arc() }, raw))
    }
}

impl<'a> std::ops::Deref for MessageView<'a> {
    type Target = FieldView<'a>;
    fn deref(&self) -> &FieldView<'a> {
        &self.fields
    }
}

/// A message built field by field for a [`Bytes`](crate::Bytes) handle: set fields by
/// name or path, then send `builder.bytes()`. Each set fails loudly, naming the field.
pub struct MessageBuilder {
    schema: Schema,
    buf: Vec<u8>,
}

impl MessageBuilder {
    /// The schema's default message: zeros and empty tails.
    pub fn new(schema: &Schema) -> MessageBuilder {
        let mut b = MessageBuilder { schema: schema.clone(), buf: Vec::new() };
        if let Some(_pass) = schema.inner.gate.enter() {
            // SAFETY: the pass keeps the schema alive
            let min = unsafe { ffi::rant_schema_msg_min(schema.raw) } as usize;
            b.buf = vec![0u8; min];
            unsafe { ffi::rant_schema_message_default(schema.raw, b.buf.as_mut_ptr().cast(), b.buf.len()) };
        }
        b
    }

    fn set(&mut self, field: &str, extra: usize, op: impl FnOnce(*mut u8, usize, *const ffi::RantSchema, *const c_char) -> i32) -> Result<&mut Self, Error> {
        let what = || format!("rant::MessageBuilder: the field \"{field}\" refused the value");
        let pass = self.schema.inner.gate.enter().ok_or_else(|| Error::new(ErrorKind::State, "rant::MessageBuilder: the node is closed"))?;
        if extra > 0 {
            // a tail grows by at most its new content past the live length
            // SAFETY: the pass keeps the schema alive
            let used = unsafe { ffi::rant_schema_msg_len(self.schema.raw, self.buf.as_ptr().cast(), self.buf.len()) } as usize;
            if self.buf.len() < used + extra {
                self.buf.resize(used + extra, 0);
            }
        }
        let (buf, cap, s) = (self.buf.as_mut_ptr(), self.buf.len(), self.schema.raw);
        let ok = with_c(field, |f| op(buf, cap, s, f)) != 0;
        drop(pass);
        if ok { Ok(self) } else { Err(Error::new(ErrorKind::SchemaMismatch, what())) }
    }

    /// An unsigned or bool field. The value narrows like a C cast.
    pub fn set_uint(&mut self, field: &str, v: u64) -> Result<&mut Self, Error> {
        // SAFETY: set hands a live buffer, its length and a schema held by a pass
        self.set(field, 0, |b, c, s, f| unsafe { ffi::rant_set_uint(b.cast(), c, s, f, v) })
    }

    /// A signed field.
    pub fn set_int(&mut self, field: &str, v: i64) -> Result<&mut Self, Error> {
        // SAFETY: as set_uint
        self.set(field, 0, |b, c, s, f| unsafe { ffi::rant_set_int(b.cast(), c, s, f, v) })
    }

    /// An f64 or f32 field.
    pub fn set_f64(&mut self, field: &str, v: f64) -> Result<&mut Self, Error> {
        // SAFETY: as set_uint
        self.set(field, 0, |b, c, s, f| unsafe { ffi::rant_set_f64(b.cast(), c, s, f, v) })
    }

    /// An f32 field.
    pub fn set_f32(&mut self, field: &str, v: f32) -> Result<&mut Self, Error> {
        // SAFETY: as set_uint
        self.set(field, 0, |b, c, s, f| unsafe { ffi::rant_set_f32(b.cast(), c, s, f, v) })
    }

    /// A bool field.
    pub fn set_bool(&mut self, field: &str, v: bool) -> Result<&mut Self, Error> {
        self.set_uint(field, v as u64)
    }

    /// A capped or variable string. Over a capped string's capacity is refused, never cut.
    pub fn set_string(&mut self, field: &str, v: &str) -> Result<&mut Self, Error> {
        let s = ffi::RantString { data: v.as_ptr().cast(), len: v.len() };
        // SAFETY: as set_uint
        self.set(field, v.len(), |b, c, sc, f| unsafe { ffi::rant_set_string(b.cast(), c, sc, f, s) })
    }

    /// One element of a string array. Size a variable one first with set_array_count.
    pub fn set_string_at(&mut self, field: &str, index: u16, v: &str) -> Result<&mut Self, Error> {
        let s = ffi::RantString { data: v.as_ptr().cast(), len: v.len() };
        // SAFETY: as set_uint
        self.set(field, v.len() + 2, |b, c, sc, f| unsafe { ffi::rant_set_string_at(b.cast(), c, sc, f, index, s) })
    }

    /// Sizes a variable array to `count` zeroed elements, so a struct array's members can
    /// be set by path such as `pts[2].x`.
    pub fn set_array_count(&mut self, field: &str, count: u32) -> Result<&mut Self, Error> {
        let elem = self.schema.field_index(field).and_then(|i| self.schema.read(|s| field_at(s, i))).map_or(0, |f| f.elem_size);
        // SAFETY: as set_uint
        self.set(field, count as usize * elem as usize, |b, c, s, f| unsafe { ffi::rant_set_array_count(b.cast(), c, s, f, count) })
    }

    /// A fixed or variable array from little endian element bytes, whole elements only.
    pub fn set_array(&mut self, field: &str, elems: &[u8]) -> Result<&mut Self, Error> {
        let e = c_bytes(elems);
        // SAFETY: as set_uint
        self.set(field, elems.len(), |b, c, s, f| unsafe { ffi::rant_set_array(b.cast(), c, s, f, e) })
    }

    /// A map field from a map body, such as [`MapWriter::finish`](crate::MapWriter::finish)'s. Validated first.
    pub fn set_map(&mut self, field: &str, body: &[u8]) -> Result<&mut Self, Error> {
        let e = c_bytes(body);
        // SAFETY: as set_uint
        self.set(field, body.len(), |b, c, s, f| unsafe { ffi::rant_set_map(b.cast(), c, s, f, e) })
    }

    /// An enum field by option name, in any spelling. An unknown name is refused.
    pub fn set_enum(&mut self, field: &str, option: &str) -> Result<&mut Self, Error> {
        let name = CString::new(option).map_err(|_| Error::new(ErrorKind::SchemaMismatch, "rant::MessageBuilder: the option holds a NUL"))?;
        // SAFETY: as set_uint
        self.set(field, 0, |b, c, s, f| unsafe { ffi::rant_set_enum(b.cast(), c, s, f, name.as_ptr()) })
    }

    /// The live message bytes, its fixed section and tail.
    pub fn bytes(&self) -> &[u8] {
        let n = self.schema.read(|s| {
            // SAFETY: the read holds a pass
            unsafe { ffi::rant_schema_msg_len(s, self.buf.as_ptr().cast(), self.buf.len()) as usize }
        });
        &self.buf[..n.min(self.buf.len())]
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn every_c_field_kind_has_a_variant() {
        for (name, v) in crate::c_enum_values("RantSchemaTypeKind") {
            assert_ne!(super::FieldType::from_c(v as u8), super::FieldType::Unknown, "{name}");
        }
    }
}
