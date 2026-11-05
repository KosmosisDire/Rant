use std::borrow::Cow;
use std::collections::BTreeMap;
use std::ffi::CString;
use std::marker::PhantomData;
use std::ptr::null;

use crate::error::{Error, ErrorKind};
use crate::ffi;
use crate::schema::{FieldType, c_bytes, rbytes, rstring_bytes, utf8_prefix};

/// Builds a map field's body, the self describing value tree. Open a nested map or array,
/// write its members, then close it. Inside an array the key is ignored.
pub struct MapWriter {
    buf: Vec<u8>,
    w: ffi::RantMapWriter,
    finished: usize,
}

// SAFETY: the writer's pointer is into its own buffer, owned with it
unsafe impl Send for MapWriter {}

impl MapWriter {
    /// A writer with 512 bytes of room.
    pub fn new() -> MapWriter {
        MapWriter::with_capacity(512)
    }

    /// A writer with `capacity` bytes of room. A put past it fails [`MapWriter::finish`].
    pub fn with_capacity(capacity: usize) -> MapWriter {
        let mut buf = vec![0u8; capacity.max(1)];
        // SAFETY: the buffer's heap block stays put when the writer moves
        let w = unsafe { ffi::rant_map_begin(buf.as_mut_ptr().cast(), buf.len()) };
        MapWriter { buf, w, finished: 0 }
    }

    fn put(&mut self, key: &str, f: impl FnOnce(*mut ffi::RantMapWriter, *const std::os::raw::c_char)) -> &mut Self {
        let in_array = self.w.depth > 0 && self.w.is_arr[(self.w.depth - 1) as usize] != 0;
        match CString::new(key) {
            _ if in_array => f(&mut self.w, null()),
            Ok(k) => f(&mut self.w, k.as_ptr()),
            Err(_) => self.w.err = 1,
        }
        self
    }

    /// An unsigned integer.
    pub fn put_uint(&mut self, key: &str, v: u64) -> &mut Self {
        // SAFETY: the writer and its buffer are live, and put hands a valid key or null
        self.put(key, |w, k| unsafe { ffi::rant_map_put_uint(w, k, v); })
    }

    /// A signed integer.
    pub fn put_int(&mut self, key: &str, v: i64) -> &mut Self {
        // SAFETY: as put_uint
        self.put(key, |w, k| unsafe { ffi::rant_map_put_int(w, k, v); })
    }

    /// An f64.
    pub fn put_f64(&mut self, key: &str, v: f64) -> &mut Self {
        // SAFETY: as put_uint
        self.put(key, |w, k| unsafe { ffi::rant_map_put_f64(w, k, v); })
    }

    /// An f32.
    pub fn put_f32(&mut self, key: &str, v: f32) -> &mut Self {
        // SAFETY: as put_uint
        self.put(key, |w, k| unsafe { ffi::rant_map_put_f32(w, k, v); })
    }

    /// A bool.
    pub fn put_bool(&mut self, key: &str, v: bool) -> &mut Self {
        // SAFETY: as put_uint
        self.put(key, |w, k| unsafe { ffi::rant_map_put_bool(w, k, v as i32); })
    }

    /// A string.
    pub fn put_string(&mut self, key: &str, v: &str) -> &mut Self {
        let s = ffi::RantString { data: v.as_ptr().cast(), len: v.len() };
        // SAFETY: as put_uint
        self.put(key, |w, k| unsafe { ffi::rant_map_put_string(w, k, s); })
    }

    /// Opens a nested map. Close it with [`MapWriter::close`].
    pub fn open_map(&mut self, key: &str) -> &mut Self {
        // SAFETY: as put_uint
        self.put(key, |w, k| unsafe { ffi::rant_map_open_map(w, k); })
    }

    /// Opens a nested array. Close it with [`MapWriter::close`].
    pub fn open_array(&mut self, key: &str) -> &mut Self {
        // SAFETY: as put_uint
        self.put(key, |w, k| unsafe { ffi::rant_map_open_array(w, k); })
    }

    /// Closes the innermost open map or array.
    pub fn close(&mut self) -> &mut Self {
        // SAFETY: the writer is live
        unsafe { ffi::rant_map_close(&mut self.w) };
        self
    }

    /// The finished body, for [`MessageBuilder::set_map`](crate::MessageBuilder::set_map).
    /// Fails when a put overflowed the room or a body was left open. Nothing can be added after.
    pub fn finish(&mut self) -> Result<&[u8], Error> {
        if self.finished == 0 {
            // SAFETY: the writer is live
            self.finished = unsafe { ffi::rant_map_finish(&mut self.w) } as usize;
        }
        if self.finished == 0 {
            return Err(Error::new(ErrorKind::None, "rant::MapWriter: the map overflowed its capacity or was left open"));
        }
        Ok(&self.buf[..self.finished])
    }
}

impl Default for MapWriter {
    fn default() -> MapWriter {
        MapWriter::new()
    }
}

/// Reads a map body: by key, or every entry in order. Every walk is bounds checked in C.
#[derive(Clone, Copy)]
pub struct MapReader<'a> {
    body: &'a [u8],
}

impl<'a> MapReader<'a> {
    /// A reader over a map body, such as a finished [`MapWriter`]'s.
    pub fn new(body: &'a [u8]) -> MapReader<'a> {
        MapReader { body }
    }

    /// The body bytes.
    pub fn body(&self) -> &'a [u8] {
        self.body
    }

    /// The entries at this level.
    pub fn len(&self) -> usize {
        // SAFETY: the C bounds checks the body
        unsafe { ffi::rant_map_count(c_bytes(self.body)) as usize }
    }

    /// True with no entries.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// The entry named key.
    pub fn get(&self, key: &str) -> Option<MapValue<'a>> {
        let k = CString::new(key).ok()?;
        let mut v = ffi::RantValue::default();
        // SAFETY: as len
        (unsafe { ffi::rant_map_get(c_bytes(self.body), k.as_ptr(), &mut v) } != 0).then(|| MapValue::new(v))
    }

    /// Every entry with its key, in body order.
    pub fn iter(&self) -> impl Iterator<Item = (&'a str, MapValue<'a>)> + 'a {
        let body = self.body;
        (0..self.len()).map_while(move |i| {
            let mut k = ffi::RantString::default();
            let mut v = ffi::RantValue::default();
            // SAFETY: as len
            let ok = unsafe { ffi::rant_map_at(c_bytes(body), i as u16, &mut k, &mut v) } != 0;
            ok.then(|| (utf8_prefix(rstring_bytes(k)), MapValue::new(v)))
        })
    }

    /// The whole map decoded into owned values that outlive the message.
    pub fn to_map(&self) -> BTreeMap<String, MapItem> {
        self.iter().map(|(k, v)| (k.to_owned(), v.to_item())).collect()
    }
}

/// One value read from a map: a scalar, a string, a nested map or an array.
#[derive(Clone, Copy)]
pub struct MapValue<'a> {
    v: ffi::RantValue,
    _body: PhantomData<&'a [u8]>,
}

impl<'a> MapValue<'a> {
    fn new(v: ffi::RantValue) -> MapValue<'a> {
        MapValue { v, _body: PhantomData }
    }

    /// Which reader applies: a scalar kind, VString, VArray or Map.
    pub fn kind(&self) -> FieldType {
        FieldType::from_c(self.v.kind)
    }

    /// An unsigned integer.
    pub fn as_uint(&self) -> u64 {
        // SAFETY: every member of the scalar union is plain bits
        unsafe { self.v.v.u }
    }

    /// A signed integer.
    pub fn as_int(&self) -> i64 {
        // SAFETY: as as_uint
        unsafe { self.v.v.i }
    }

    /// A float.
    pub fn as_f64(&self) -> f64 {
        // SAFETY: as as_uint
        unsafe { self.v.v.f }
    }

    /// A bool.
    pub fn as_bool(&self) -> bool {
        self.as_uint() != 0
    }

    /// A string. Bytes that are not UTF-8 read as replacement characters.
    pub fn as_str(&self) -> Cow<'a, str> {
        String::from_utf8_lossy(rbytes(self.v.bytes))
    }

    /// A nested map.
    pub fn as_map(&self) -> MapReader<'a> {
        MapReader::new(rbytes(self.v.bytes))
    }

    /// An array's element count.
    pub fn array_len(&self) -> usize {
        // SAFETY: the C bounds checks the body
        unsafe { ffi::rant_map_array_count(self.v.bytes) as usize }
    }

    /// An array's element by index.
    pub fn array_at(&self, index: usize) -> Option<MapValue<'a>> {
        let mut v = ffi::RantValue::default();
        // SAFETY: as array_len
        (unsafe { ffi::rant_map_array_at(self.v.bytes, index as u16, &mut v) } != 0).then(|| MapValue::new(v))
    }

    /// The value copied into an owned tree.
    pub fn to_item(&self) -> MapItem {
        use FieldType::*;
        match self.kind() {
            Bool => MapItem::Bool(self.as_bool()),
            U8 | U16 | U32 | U64 => MapItem::UInt(self.as_uint()),
            I8 | I16 | I32 | I64 => MapItem::Int(self.as_int()),
            F32 | F64 => MapItem::Float(self.as_f64()),
            VString => MapItem::String(self.as_str().into_owned()),
            VArray => MapItem::List((0..self.array_len()).filter_map(|i| self.array_at(i)).map(|v| v.to_item()).collect()),
            Map => MapItem::Map(self.as_map().to_map()),
            _ => MapItem::Null,
        }
    }
}

/// One owned node of a decoded map.
#[derive(Clone, Debug, PartialEq)]
#[allow(missing_docs)]
pub enum MapItem {
    Null,
    Bool(bool),
    UInt(u64),
    Int(i64),
    Float(f64),
    String(String),
    List(Vec<MapItem>),
    Map(BTreeMap<String, MapItem>),
}
