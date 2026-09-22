# Schema

`src/serialize/` is an independent schema layer over the transport. Both ends share the
schema, so the wire carries no tags or keys: a value's meaning is its byte position. It
depends only on `common/` and works standalone. The wire version is 8. The user facing
DSL and the standard type roster are in docs/stdtypes.md.

## Layout

Fixed kinds pack first at static offsets, so reads are O(1) and zero copy. Every variable
kind rides a `[u32 len][payload]` frame in the message tail. Frames sit in schema order
whatever the declaration position, and a type agnostic length hop walk of the field's
`var_ord` locates them. Framing is the u32 alone, so skipping never needs the type.
Content typing is the schema, except MAP, whose payload is a tagged value tree that
reuses the same kind bytes.

- `rant_schema_size` is the fixed size.
- `rant_schema_msg_min` is fixed plus 4 bytes per variable field. `message_default`
  writes that.
- `rant_schema_msg_len` is the live length to send. 0 means malformed.

Setters resize frames in place (they memmove the tail), refuse an over cap write and never
truncate. Readers clamp a hostile live length to the cap. A short string write zero fills
the slot tail so it stays canonical. `RantValue.count` is u16 and saturates at 65535 for
VARR and MAP, where `bytes.len` is authoritative.

## Kinds and wire

```
schema := [u8 version=8][u8 root_namelen][root_name][type]
type :=
  scalar U8..BOOL (0..10)   nothing follows
  ARR    (11)  [u16 count][type elem]
  STRUCT (12)  [u8 nfields]([u8 namelen][name][type])*
  STR    (13)  [u16 cap]                 message slot [u16 live_len][cap bytes]
  VSTR   (14)  nothing
  VARR   (15)  [type elem]
  MAP    (16)  nothing
  ENUM   (17)  [u8 backing][u16 n]([value:backing][u8 namelen][name])*
  NAMED  (18)  [u8 namelen >= 1][name][type inner]     inner is never NAMED
```

A root name with a non STRUCT type is a named alias root. The root name never encodes as
NAMED, so there is exactly one spelling and the hash stays canonical. The bounds checked
parser enforces every rule against hostile wire: NAMED never double wraps, array elements
must be fixed, an array of arrays is refused, VSTR, VARR and MAP are legal at any struct
depth except inside array elements, and `RANT_SCHEMA_MAX_DEPTH` caps recursion. An older
wire version is refused at parse.

Settled calls: distinct kind bytes rather than a cap of 0 as a sentinel, so a C switch
fails loudly. VSTR and VARR share the frame mechanism but stay distinct so text intent
survives. `string[]` is refused, since ragged data is the map's job. VARR elements are
scalars or capped strings only. ENUM is refused as a direct array element (an enum inside
a struct element is fine). Roots may be any kind: a bool topic's schema is just `bool`.
Non struct roots are anonymous unless named, so the same bare type in every language
gives byte identical wire and the same hash.

## Enum

An enum is a fixed field carrying only its backing scalar on the wire. The value to name
table is schema only. The subset rule compares the backing width only, so an unknown
newer value stays readable as a number. Option order is declaration order in every
language or hashes diverge (C# must use `GetFields`, never `Enum.GetNames`). The option
count is u16. When two peers define an identical enum the hashes match and no wire is
exchanged. A differing large enum inlines its full wire in the detail exchange, which
pages only at entry boundaries, so it can IP fragment. Enum arrays and flag enums are
deferred.

## Named types

A name rides the schema wire, never a message byte, and narrows matching. An unnamed
reader reads a named writer of the same shape. A named reader demands the identical
writer name. Two different names never cross wire. The shape is always verified as well.
Struct root names stay strict equal both ways. A name has one shape: defining it again
with another is a compile error, in any statement, and the standard names count as
defined. So a type reflected from a class called `Color` must match the standard `Color`
or take another name. Aliases wrap any type but a struct, so `Uuid = u8[16]` is a named
alias, not a wrapper struct.

Array elements must be fixed all the way down, with one level of struct array nesting so
`arr_parent` stays unique. A variable member inside a nested struct declares nested but
stores flat: it claims a top level tail frame in depth first declaration order.

## Reader model

A reader declares the subset of fields it needs. A match means the same topic, the root
name rule, and the reader's fields a subset of the writer's fields by name with the kind
rules (`rant_schema_subset`). Subset applies at the top level only. Nested structs compare
exactly and recursively. Delivery uses the writer's layout through a rebased schema
(`i_rant_schema_rebase`: the reader's fields, order and indices with the writer's offsets
and size), surfaced as `RantMsg.schema`, so reader code keeps its own indices. Rebase walks
both subtrees in lockstep, which handles nested frames and struct arrays.

A raw reader accepts anything. A typed reader refuses an untyped or unverifiable writer.
An identical hash skips the parse. A peer schema is interned once per hash, and the claimed hash must equal the parsed wire's hash, or a lying peer could poison the intern for every honest one. Both directions advertise and run the same gate, so a
refused pair forms no proxy on either side. The node validates the message length against
the sender's schema before delivery and fires `RANT_E_SCHEMA_MISMATCH` with a per field
`schema_detail` from `rant_schema_subset_why`. There is deliberately no boot time
validator API.

## DSL

`rant_node_schema` takes name first IDL text: `Pose { stamp: u64, velocity: { dx: f32 } }`.
Commas are optional and `--` comments run to the end of the line. The text is the cross
language interchange: reflecting languages generate it, others paste it, and identical
text gives identical wire and hash. `rant_schema_print` is the exact inverse. It always
emits a definition for every named type in dependency order, so its text is self
contained and recompiles to identical bytes under `RANT_NO_STDTYPES` too. Only
`rant_schema_print` spells DSL. The C#, Python and explorer emitters call it.

Statements compile into their own scratch arena first, so resolving a reference mid
definition (which may pull in a standard type) never interleaves bytes. The final wire is
assembled and handed to the node's registry, which parses it once per shape.
`rant_std_recognize*` take the node, whose registry holds the canonical shape to compare
against, so recognizing never compiles.

The registry (`i_RantRegistry`, serialize/internal.h) belongs to the node: one arena of
names and type bytes, an index over it, and every schema handed out, one per shape by a
wire compare. Both arrays grow through the node's allocator. A failed text rolls the
arena and index back to where the compile started, so nothing is half kept.
`rant_node_schema_parse` registers wire without defining a name, which is how a create
and the bindings keep a peer's schema. The selftest compiles without a node through a
throwaway registry on its own allocator.

Gotcha: a `--` comment inside concatenated C string literals eats the rest of the schema
unless the literal ends with a newline, and a definition's last token needs a trailing
newline for the same reason.

## Access

Access is by name, nested members by dotted path, array elements by index
(`corners[2].x`). The compiled schema flattens every field at every depth into one depth
first table (`rant_schema_field_count`, `rant_schema_field_at`, `.depth`). Reflection
tools use `rant_get_value` and `rant_set_value` over the flat index with a tagged
`RantValue`. Standard mirrors are layout identical to the wire on a little endian target,
which the header asserts.

## Map

MAP is a tagged value tree: `[u16 n]([u8 klen][key][u8 kind]payload)*`. Only scalars,
VSTR, VARR and MAP are legal inside. Integers store as the smallest kind that fits. An
empty frame is an empty map. `rant_set_map` runs `rant_map_valid` first so malformed
bytes never enter a message. C and C++ wrap the real `RantMapWriter`. C# and Python build
and parse the map body in managed code on purpose, since struct mirrors are the standing
footgun (spec/bindings.md).

## Allocation and ownership

Allocation is a `RantAllocFn` hook (realloc style: a NULL pointer allocates, size 0
frees), the same hook the transport core takes. The read path allocates nothing. A
compiled `RantSchema` is one block: wire bytes, FNV-1a hash, size and offset table.
`rant_node_create_topic` parses its own copy into node memory, so the caller may free its
schema at once. Build a schema from the same allocator the node opens with and do not free
it explicitly, because the node copies the pool by value and close frees the page.

Canonical hashes pinned across languages: `bool` ee90234f61d2520b, `f32[]`
314844e3386a1fc4, `Float3` 04aa9469cd08b1dd, `Image` 489841f99f392b85, `VideoFrame`
f677bd147b513fbc, `ExternalVideoStream` aae502077016ac13.

## Not done

A standalone `dist/rant_serialize.h`. Eigen, GLM, numpy and Unity converters. In Python a
standard type cannot be an array element and user named types are unsupported, since its
emitter does not hoist a definition yet. C, C++, C# and DSL text handle both.

## DSL grammar

```
schema := def* type?              the text compiles to its last statement
def    := IDENT '{' fields '}'    a struct
        | IDENT '=' type          any other type, never a struct and never another name
field  := name ':' type (',')?    fields self delimit, commas are optional
type   := base | base '[' count ']' | base '[' ']' | '{' fields '}' ('[' count? ']')?
base   := scalar | 'string' ('<' cap '>')? | 'map' | 'enum' '<' scalar '>' '{' options '}' | NAME
```

A type word that is not a built in resolves against the node's registry, then the
standard library, which is expanded into the registry on first use, and emits as a NAMED
type. Text is the
one way to make a schema. The parser writes wire type bytes through an internal type
writer, which holds the limits it can see while writing (name lengths, 255 fields per
struct, nesting depth). An array suffix follows its element in the text, so the parser
writes the element first and then splices the array head in front of it. Every other rule
is checked once, when the finished bytes go through `rant_schema_parse`, the same check a
peer's bytes get. A standard type expands at most 8 levels deep. Enum values may be omitted and then count up
from the previous one, starting at 0. Each definition sees the ones before it. A
definition as the last statement gives a root that carries its name with no NAMED wrapper,
and a bare reference as the last statement (`Uuid`, `Pose`) gives the same root, so a
definition taken by name hashes like its struct written out. A type on its own ends the
text. `Name = { ... }` and `A = W` for a name `W` are refused at the type, since a struct
has one spelling and a name never wraps a name. The parser keeps the first refusal as a
reason and a position, and the node prints both in `schema_detail`.

## The flat table

The compiled schema flattens every field at every depth into one depth first table. A
struct array flattens its element once, as an element 0 template under the array field:
its members' offsets are element 0's (message absolute for a fixed array, frame relative
for a variable one) and `arr_parent` names the array, so element i sits at that offset
plus i times the element size. The name based accessors spell the index in the path and
the index based ones take it as an argument. A field's type encoding is kept as a wire
offset and length, so two fields have the same type exactly when the bytes agree.

The compiled block is the wire bytes, an 8 aligned handle and the field table. Statements
compile into their own scratch writer first and are appended to one definition arena,
so resolving a reference mid definition never interleaves bytes. `rant_schema_print`
hoists at most 48 named type definitions, beyond that the tail spells by reference.
