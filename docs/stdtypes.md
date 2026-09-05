# Standard types

A small library of the types applications keep re inventing, shipped with DART so that
two programs that both mean "a 3D point" say so with the same name and the same bytes.

```c
DartSchema *s = dart_schema_compile(alloc, user,
    "Waypoint { at: Pose, when: Timestamp, tag: Color }", NULL);
```

No imports, no registration. Every name below is always in scope in the schema DSL. Strip
the whole module with `DART_NO_STDTYPES`. The names then stop resolving and schemas must
spell their shapes out.

## Why names, not just shapes

Before wire version 8 a schema was purely structural: `{ x: f64, y: f64, z: f64 }` matched
any other schema with those three fields. That is right for a field's layout and wrong for
its meaning. A `Twist` (linear plus angular velocity) and a `Pose` split into two
`Double3`s are the same bytes. A Celsius reading and a Fahrenheit reading are both `f32`.

So a type may carry a NAME. It rides the schema wire, never a message byte, and it narrows
matching:

| reader declares | writer declares | verdict |
|---|---|---|
| `f32` | `Celsius = f32` | matches, unwrapping a name is free |
| `Celsius = f32` | `Celsius = f32` | matches |
| `Celsius = f32` | `f32` | refused, the reader asked for a Celsius |
| `Celsius = f32` | `Fahrenheit = f32` | refused, different names never cross wire |

The shape is still always checked. A name never skips structural verification, and a peer
advertising `Uuid = u8[15]` is refused rather than trusted (`dart_std_recognize` verifies
name and shape). Struct root names stay strict equal in both directions, and enum option
names stay advisory.

Naming costs zero message bytes. `Celsius` is four bytes on the wire the schema is
exchanged on, and nothing per message.

## The roster

Vectors, in the obvious component order:

```
Float2  { x: f32, y: f32 }              Double2 / Double3 / Double4   the same in f64
Float3  { x: f32, y: f32, z: f32 }      Int2    / Int3    / Int4      the same in i32
Float4  { x: f32, y: f32, z: f32, w: f32 }
```

Geometry and time:

```
Quaternion { x: f64, y: f64, z: f64, w: f64 }
Pose       { position: Double3, orientation: Quaternion }
Twist      { linear: Double3, angular: Double3 }        -- m/s and rad/s
GeoPoint   { lat: f64, lon: f64, alt: f64 }             -- degrees, degrees, meters
Matrix3x3 = f32[9]        Matrix4x4 = f32[16]           -- row major
Timestamp = i64           Duration  = i64               -- microseconds
Uuid      = u8[16]                                      -- RFC 4122 byte order
```

Presentation:

```
Color { r: u8, g: u8, b: u8, a: u8 }    -- sRGB, straight alpha (not premultiplied)
Rect  { x: f32, y: f32, w: f32, h: f32 }        RectI  the same in i32
Uri   = string<256>
```

Media:

```
Image { width: u32, height: u32, stride: u32,
        format: enum<u8> { Mono8, Mono16, Rgb8, Rgba8, Bgr8, Yuyv, Nv12,
                           Jpeg = 16, Png = 17 },
        data: u8[] }
VideoFrame { codec: enum<u8> { Unknown, Mjpeg, H264, H265, Av1 },
             width: u32, height: u32,
             keyframe: bool, pts: Timestamp, data: u8[] }
ExternalVideoStream { kind: enum<u8> { Rtsp, WebrtcWhep, Hls, Srt, Rtp, HttpMjpeg,
                                       Other = 15 },
                      codec: enum<u8> { Unknown, Mjpeg, H264, H265, Av1 },
                      width: u32, height: u32,
                      url: Uri, name: string<32> }
```

The `--` lines above are DSL comments.

`Image.stride` 0 means tightly packed rows. A `format` of 16 or more is a compressed
container, so `data` holds the file bytes rather than pixels. `Image` and `VideoFrame`
carry a variable member, which makes them topic or root types: they can nest as a struct
member (the frame is hoisted to the message tail) but they cannot be array elements.

`VideoFrame.width` and `height` are the coded frame size in pixels. Compressed bitstreams
carry it in band (the H.264 SPS, say), which is why some schemas omit it, but a consumer
should not need a bitstream parser to size a canvas before the first decode. 0 means
unstated, and the bitstream is then the only source. `Unknown` plays the same role for a
codec hint.

`ExternalVideoStream` is fully fixed, so it nests anywhere and works as a latched
variable. It is how you hand a viewer a stream URL instead of pushing pixels through the
middleware. Its `codec`, `width` and `height` are hints for pickers and tiling UIs
(`Unknown` or 0 means unstated), never a contract. Once a viewer connects the stream
itself is authoritative (an HLS master may carry several renditions, WebRTC negotiates).

The descriptor carries no transport or session fields (no SDP, no ICE candidates) on
purpose. It describes a STREAM: one latched value, many observers, replayed to late
joiners. SDP offers and ICE candidates describe one SESSION between one viewer and the
source, so they cannot live in a one to many descriptor, and every `kind` in the roster
already makes the `url` a complete rendezvous (RTSP negotiates in protocol, WHEP is
exactly "signal WebRTC through this URL", HLS is plain HTTP). A system that wants to
signal WebRTC through DART itself models it as the interaction it is: a function per
source (offer text in, answer text out) with SDP and candidate lines carried as the opaque
strings every signaling stack passes verbatim.

## Conventions

These are pinned, not suggestions. A number crossing DART in one of these types means
this:

- SI units throughout. Lengths in meters, velocities in m/s, angles in radians.
- Time is `i64` microseconds since the Unix epoch, UTC. It is the same clock
  `DartMsg.written_us` is stamped from, so the two are directly comparable. Cross host
  comparisons are only as good as the hosts' clock sync. Never mix a `Timestamp` with the
  monotonic `DartMsg.recv_us`.
- Quaternions are stored x, y, z, w, in that order.
- `dart_quaternion_mul(a, b)` is the rotation a followed by b, the Hamilton product b times
  a. `dart_color_from_hex` and `dart_color_to_hex` use 0xRRGGBBAA, the CSS order.
- Matrices are row major `f32`.
- Color is RGBA bytes in sRGB, straight alpha.
- Uuid holds the 16 bytes in RFC 4122 order, not a platform GUID's mixed endian layout.
  The C# binding converts explicitly rather than calling `Guid.ToByteArray`.
- A right handed coordinate frame is recommended but not enforced. DART carries the
  numbers. The frame convention is your system's to state.

Deliberately absent for now: civil date and time, unit annotated value types (SI by
convention today, schema level unit annotations are the future mechanism), IP addresses,
the sensor tier (PointCloud, Imu, LaserScan), any TF or frame_id story, and WebRTC
session signaling (SDP and ICE ride as opaque strings through an app level function, see
the media section).

## Using them from C

```c
#include "dart.h"

DartSchema *s = dart_schema_compile(alloc, user, "Track { at: Pose, id: Uuid }", NULL);

uint8_t msg[256];
DartPose p;
p.position    = dart_double3(4.5, -1.25, 9.0);
p.orientation = dart_quaternion_identity();

dart_schema_message_default(s, msg, sizeof msg);
memcpy(msg + /* the Pose field's offset */ 0, &p, sizeof p);   /* layout identical */
```

The C mirror structs (`DartFloat3`, `DartPose`, `DartColor`, `DartUuid`,
`DartMatrix4x4` and the rest) are layout identical to the wire on any little endian
target, which the header static asserts, so a whole value memcpys in and out. Field at a
time access through `dart_get_f64(msg, s, "at.position.x")` works exactly as it does for
any other nested struct.

The operations are deliberately thin: construction, add, sub, scale, dot, cross, length,
normalize, quaternion multiply, conjugate and rotate, `Color` hex conversion, and
timestamp arithmetic. Real linear algebra belongs in Eigen, numpy or your engine's math
library. Convert at the edge. `dart_double3_length` and friends compute their square root
inline rather than pulling in `<math.h>`, so a consumer's build line never grows an `-lm`.

Two values need a platform, so they live in the node runtime rather than beside the type:

```c
DartTimestamp now = dart_timestamp_now();   /* microseconds since the Unix epoch, UTC */
DartUuid id; dart_uuid_new(&id);            /* a random (version 4) Uuid */
```

## Recognizing them in someone else's schema

A tool that renders a schema it has never seen (the explorer, a bridge, a logger) asks:

```c
DartStdType t = dart_std_recognize_field(schema, field, alloc, user);
if (t == DART_STD_COLOR) draw_swatch(...);
```

`dart_std_recognize` looks at the schema's root, `dart_std_recognize_field` at one
field's own type, and `dart_std_recognize_elem` at an array field's element type. All
three verify the shape as well as the name, so a peer that advertises a differently
shaped `Uuid` is reported as `DART_STD_NONE` rather than memcpy'd into a `DartUuid`.
Verifying the shape means compiling the canonical type, hence the allocator hook. The
answer is stable per schema, so cache it.

## Defining your own named types

The same mechanism is yours to use. A statement before the root defines a type the root
(and later definitions) may reference:

```
Celsius = f32
Reading { probe: string<16>, t: Celsius, when: Timestamp }
```

A definition's type may be anything, so an alias (`Celsius = f32`, `Uuid = u8[16]`) is
just as much a named type as a struct. If a text has no root after its definitions, the
LAST definition is the root, which is how a named alias root is spelled:

```
Uuid = u8[16]       -- a topic whose whole payload is a Uuid
```

`dart_schema_compile_env` additionally puts a set of already compiled schemas in scope,
referenceable by their root names, for a program that builds its schemas in layers.

### The names are reserved

Redefining a standard name with a different shape is a compile error. Redefining it
identically is allowed. So `Float3 = { x: f32 }` is refused, and a field written
`p: Float3` always means the standard Float3.

That reservation covers definitions and references, not a plain `Name { ... }` root. A
schema generated by reflection from a class called `Color` or `Image` still compiles (a
Unity `Color` is f32 RGBA, not the standard u8 RGBA, and nothing can reference a root
anyway). If such a schema meets a standard one of the same name, the shapes differ and
the match is refused loudly, like any other mismatch.

## Composing them

Named types nest and array like any other type:

```
Cloud { origin: Float3, points: Float3[], corners: Float3[4], stamp: Timestamp }
```

Two rules bound this, both about keeping an array element's stride static:

- An array ELEMENT must be fixed all the way down. `Image[4]` is refused (Image has a
  variable member), as is an array of arrays, which is why `Uuid[2]` is refused too, a
  `Uuid` being itself a `u8[16]`. Wrap it in a struct if you need one.
- Only one level of struct element array nests, so an element that is itself a struct
  array is refused. Scalar and string arrays inside an element are fine.

Variable members may otherwise sit at any struct depth. Declaration nests, storage does
not: a `string` declared three levels down still claims a flat frame in the message tail,
in depth first declaration order, and its struct's size covers only the fixed part.

Members of a struct array are addressed with an index in the path:

```c
dart_set_f32(msg, cap, s, "corners[2].x", 1.5f);
float z = dart_get_f32(msg, s, "points[0].z");
uint32_t n = dart_get_array_count(msg, s, "points");
dart_set_array_count(msg, cap, s, "points", 64);   /* grow a variable one first */
```

Reflection reaches the same values by flat index with `dart_get_value_at` and
`dart_set_value_at`, taking the element index as an argument. The flat field table holds
one element 0 template per array, and `DartSchemaFieldInfo.arr_parent` points a member
back at the array it belongs to.

## Wire format

Named types are `DART_NAMED` (kind 18) in schema wire version 8:

```
NAMED := [u8 kind=18][u8 namelen >= 1][name][type inner]
```

`inner` is never itself NAMED, and NAMED never appears in root position. A root's name
rides the schema header instead, so there is exactly one encoding for it and the hash
stays canonical. `dart_schema_print` spells a schema back with each named type hoisted to
a leading `Name = type` definition, dependencies first, and that text recompiles to
identical bytes. The full wire grammar is in spec/schema.md.
