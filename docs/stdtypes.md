# Standard types

A small library of the types applications keep re inventing, shipped with Rant so that
two programs that both mean "a 3D point" say so with the same name and the same bytes.

```c
const RantSchema *s = rant_node_schema(node,
    "Waypoint { at: Transform, when: Timestamp, tag: Color }");
```

No imports, no registration. Every name below is always in scope in the schema DSL. Strip
the whole module with `RANT_NO_STDTYPES`. The names then stop resolving and schemas must
spell their shapes out.

## Why names, not just shapes

Before wire version 8 a schema was purely structural: `{ x: f64, y: f64, z: f64 }` matched
any other schema with those three fields. That is right for a field's layout and wrong for
its meaning. A `Twist` (linear plus angular velocity) and two `Double3`s are the same
bytes. A Celsius reading and a Fahrenheit reading are both `f32`.

So a type may carry a NAME. It rides the schema wire, never a message byte, and it narrows
matching:

| reader declares | writer declares | verdict |
|---|---|---|
| `f32` | `Celsius = f32` | matches, unwrapping a name is free |
| `Celsius = f32` | `Celsius = f32` | matches |
| `Celsius = f32` | `f32` | refused, the reader asked for a Celsius |
| `Celsius = f32` | `Fahrenheit = f32` | refused, different names never cross wire |

The shape is still always checked. A name never skips structural verification, and a peer
advertising `Uuid = u8[15]` is refused rather than trusted (`rant_std_recognize` verifies
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

Placement, motion and time:

```
Quaternion { x: f64, y: f64, z: f64, w: f64 }
Pose       { position: Double3, orientation: Quaternion }
Pose2D     { position: Double2, angle: f64 }            -- radians, from +x toward +y
Transform  { pose: Pose, parent: string<30> }
Twist      { linear: Double3, angular: Double3 }        -- m/s and rad/s
Wrench     { force: Double3, torque: Double3 }          -- N and N m, torque about the frame origin
GeoPoint   { lat: f64, lon: f64, alt: f64 }             -- degrees, degrees, meters
Matrix3x3 = f32[9]        Matrix4x4 = f32[16]           -- row major
Timestamp = i64           Duration  = i64               -- microseconds
Uuid      = u8[16]                                      -- RFC 4122 byte order
```

Shapes, in the units of their frame:

```
AlignedBox    { min: Double3, max: Double3 }            -- min <= max on every axis
OrientedBox   { pose: Pose, size: Double3 }             -- full edge lengths, centered on pose
Plane         { position: Double3, normal: Double3 }    -- any point on the plane, unit normal
Segment       { a: Double3, b: Double3 }
Sphere        { center: Double3, radius: f64 }
Capsule       { axis: Segment, radius: f64 }
Cylinder      { axis: Segment, radius: f64 }            -- axis ends are the cap centers
Cone          { base: Double3, tip: Double3, radius: f64 }   -- radius at the base
Polygon       { points: Double3[] }                     -- closed, planar

AlignedBox2D  { min: Double2, max: Double2 }
OrientedBox2D { pose: Pose2D, size: Double2 }
Circle        { center: Double2, radius: f64 }
Polygon2D     { points: Double2[] }                     -- closed
```

A pixel region is an `AlignedBox2D` in image coordinates. `Polygon` and `Polygon2D` carry a
variable member, so they cannot be array elements. A scene of mixed shapes is a struct of
per kind arrays: `Scene { boxes: OrientedBox[], spheres: Sphere[] }`.

Sensing and robots:

```
CameraIntrinsics { width: u32, height: u32,             -- the resolution these hold for
                   fx: f64, fy: f64, cx: f64, cy: f64,
                   model: enum<u8> { NoDistortion, BrownConrady, Fisheye, Rational },
                   coeffs: f64[8] }                     -- zero filled past the model's count
JointState { position: f64[], velocity: f64[], effort: f64[] }
JointNames { name: string<32>[] }
```

Presentation and other:

```
Color { r: u8, g: u8, b: u8, a: u8 }    -- sRGB, straight alpha (not premultiplied)
Uri   = string<256>
Empty { }                               -- zero bytes, for a signal that carries no data
```

Media:

```
Image { width: u32, height: u32, stride: u32,
        format: enum<u8> { Mono8, Mono16, Rgb8, Rgba8, Bgr8, Yuyv, Nv12, Monof32,
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

Raw `Image.data` starts with the top row, as the image frame's origin is its top left
corner. `Image.stride` 0 means tightly packed rows. A `format` of 16 or more is a compressed
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
signal WebRTC through Rant itself models it as the interaction it is: a function per
source (offer text in, answer text out) with SDP and candidate lines carried as the opaque
strings every signaling stack passes verbatim.

## Conventions

These are pinned, not suggestions. A number crossing Rant in one of these types means
this:

- Physical quantities are SI: velocities in m/s, forces in N, angles in radians.
- Positions and shapes are in the units of their frame: meters in a physical frame,
  pixels in an image frame.
- Every frame is right handed, and an angle turns from +x toward +y.
- A physical 3D frame has z up.
- A physical 2D frame is the top view of that 3D frame: x right, y up, z toward the viewer.
  Its angles turn counterclockwise seen from above.
- An image or screen frame has its origin at the top left corner: x right, y down, z away
  from the viewer. Its angles turn clockwise as seen on screen. Raw image rows start at the
  top, the row at y = 0.
- Time is `i64` microseconds since the Unix epoch, UTC. It is the same clock
  `RantMsg.written_us` is stamped from, so the two are directly comparable. Cross host
  comparisons are only as good as the hosts' clock sync. Never mix a `Timestamp` with the
  monotonic `RantMsg.recv_us`.
- Quaternions are stored x, y, z, w, in that order.
- `rant_quaternion_mul(a, b)` is the rotation a followed by b, the Hamilton product b times
  a. `rant_color_from_hex` and `rant_color_to_hex` use 0xRRGGBBAA, the CSS order.
- Matrices are row major `f32`.
- Color is RGBA bytes in sRGB, straight alpha.
- Uuid holds the 16 bytes in RFC 4122 order, not a platform GUID's mixed endian layout.
  The C# binding converts explicitly rather than calling `Guid.ToByteArray`.
- `Transform.parent` names the frame this one is measured in, `""` when unstated. The cap
  is 30 rather than 32 so the packed 88 bytes stay 8 aligned and the C mirror still matches.
- A `JointState` array is positional. The names ride a `JointNames` variable published
  once, never a field of every sample. `velocity` and `effort` may be empty.
- A new fixed type lists its widest member first, so the packed wire size matches the
  natural C size and the type can keep a mirror. The transport internals follow the same
  rule.
- A name has one spelling on the wire: fields are camelCase, types and enum options
  PascalCase, and the parser makes them so from whatever was written. `frame_id`,
  `FrameId` and `frameId` are one field, `robot_pose` is `RobotPose`, `IDLE` is `Idle`.
  An underscore, a case change and the last capital of a run split words (`HTTPServer`
  is `httpServer`), a digit stays with its word (`Imu9Dof` is `imu9Dof`). Two fields of
  one struct with the same wire name are refused. Every binding derives a member's wire
  name by the same rule, `rant_field_name` in C, so a C# `FrameId`, a Python `frame_id`
  and a C `frameId` meet with no attribute, and a path such as `"frame_id"` given to
  `rant_get_f32` finds `frameId`.

Deliberately absent for now: meshes, polylines, civil date and time, unit annotated value types (SI by
convention today, schema level unit annotations are the future mechanism), IP addresses,
the rest of the sensor tier (PointCloud, Imu, LaserScan), a TF tree, and WebRTC
session signaling (SDP and ICE ride as opaque strings through an app level function, see
the media section).

## Using them from C

```c
#include "rant.h"

const RantSchema *s = rant_node_schema(node, "Track { at: Transform, id: Uuid }");

uint8_t msg[256];
RantTransform p = rant_transform_identity();
p.pose.position = rant_double3(4.5, -1.25, 9.0);

rant_schema_message_default(s, msg, sizeof msg);
memcpy(msg + /* the Transform field's offset */ 0, &p, sizeof p);  /* layout identical */
```

The C mirror structs (`RantFloat3`, `RantTransform`, `RantOrientedBox`, `RantColor`,
`RantUuid`, `RantMatrix4x4` and the rest) are layout identical to the wire on any little endian
target, which the header static asserts, so a whole value memcpys in and out. Field at a
time access through `rant_get_f64(msg, s, "at.pose.position.x")` works exactly as it does
for any other nested struct.

A type only gets a C mirror when its packed wire size already equals its natural C size.
`Image`, `VideoFrame`, `ExternalVideoStream`, `CameraIntrinsics`, `Polygon`, `Polygon2D`
and `Empty` have none: they carry a variable member, would gain padding, or are empty. The other bindings reflect field by field, so
they mirror every type either way.

The operations are deliberately thin: construction, add, sub, scale, dot, cross, length,
normalize, quaternion multiply, conjugate and rotate, `Color` hex conversion, and
timestamp arithmetic. Real linear algebra belongs in Eigen, numpy or your engine's math
library. Convert at the edge. `rant_double3_length` and friends compute their square root
inline rather than pulling in `<math.h>`, so a consumer's build line never grows an `-lm`.

Two values need a platform, so they live in the node runtime rather than beside the type:

```c
RantTimestamp now = rant_timestamp_now();       /* microseconds since the Unix epoch, UTC */
RantUuid id; rant_uuid_new(&id);                /* a random (version 4) Uuid */
```

## Recognizing them in someone else's schema

A tool that renders a schema it has never seen (the explorer, a bridge, a logger) asks:

```c
RantStdType t = rant_std_recognize_field(schema, field, alloc, user);
if (t == RANT_STD_COLOR) draw_swatch(...);
```

`rant_std_recognize` looks at the schema's root, `rant_std_recognize_field` at one
field's own type, and `rant_std_recognize_elem` at an array field's element type. All
three verify the shape as well as the name, so a peer that advertises a differently
shaped `Uuid` is reported as `RANT_STD_NONE` rather than memcpy'd into a `RantUuid`.
Verifying the shape means compiling the canonical type, hence the allocator hook. The
answer is stable per schema, so cache it.

## Defining your own named types

The same mechanism is yours to use. A schema text is a run of statements. Each statement
defines a name, every later statement may use it, and the text compiles to its last
statement:

```
Celsius = f32
Probe   { id: string<16>, where: Float3 }
Reading { probe: Probe, t: Celsius, when: Timestamp }
```

`Name { fields }` defines a struct. `Name = type` names any other type, so an alias
(`Celsius = f32`, `Uuid = u8[16]`) is just as much a named type as a struct. A struct is
never written `Name = { ... }`, and an alias of another name (`A = Probe`) is refused,
since a name never wraps a name on the wire. A text of one statement is the usual case:

```
Uuid = u8[16]       -- a topic whose whole payload is a Uuid
```

The last statement may also be a type on its own: a name defined above or a standard
name (`Probe`, `Transform`), a bare type (`bool`, `f32[3]`) or an anonymous struct
(`{ x: f32 }`). A definition taken by its name is the same schema, with the same hash, as
its struct written out.

Every compile goes through a node (`rant_node_schema`, C# `node.Schema`, C++
`node.schema`, Python `node.schema`) and the node keeps every definition: a name defined
in one text is in scope for every later text on that node, so a shared type is written
once and later handles take just its name. A failed text leaves no definition behind. The
node owns each schema it hands out, one handle per shape, and frees them at close, so
nothing is ever freed by hand. A refused text fires `RANT_E_BAD_SCHEMA` and
`rant_last_error(node).schema_detail` says why and where: `unknown type near: Twist {`,
`the name is already defined with another shape near: Color {`, `expected } at the end`.

### The names are reserved

Defining a name twice with two shapes is a compile error, and the standard names count as
defined. Defining a name again identically is allowed. So `Float3 { x: f32 }` is refused,
in any statement of a text, and a field written `p: Float3` always means the standard
Float3. A type of your own that shares a standard name must match it or take another
name: a Unity `Color` is f32 RGBA, not the standard u8 RGBA, so convert it to the
standard `Color` or call the type something else.

## Composing them

Named types nest and array like any other type:

```
Cloud { origin: Float3, points: Float3[], corners: Float3[4], stamp: Timestamp }
```

Two rules bound this, both about keeping an array element's stride static:

- An array ELEMENT must be fixed all the way down. `Image[4]` is refused (Image has a
  variable member), as is an array of arrays, which is why `Uuid[2]` is refused too, a
  `Uuid` being itself a `u8[16]`. Wrap it in a struct if you need one.
- Inside an element everything has a fixed size, so a struct array nests in another's
  element only as a fixed one: `{ corners: Float2[4] }[]` works, `{ pts: Float2[] }[]`
  is refused. The outermost array may be variable.

Variable members may otherwise sit at any struct depth. Declaration nests, storage does
not: a `string` declared three levels down still claims a flat frame in the message tail,
in depth first declaration order, and its struct's size covers only the fixed part.

Members of a struct array are addressed with an index in the path:

```c
rant_set_f32(msg, cap, s, "corners[2].x", 1.5f);
rant_set_f32(msg, cap, s, "shapes[1].corners[2].x", 0.5f);   /* one index per level */
float z = rant_get_f32(msg, s, "points[0].z");
uint32_t n = rant_get_array_count(msg, s, "points");
rant_set_array_count(msg, cap, s, "points", 64);   /* grow a variable one first */
```

Reflection reaches the same values by flat index with `rant_get_value_at` and
`rant_set_value_at`, taking one index per enclosing struct array, outermost first. The
flat field table holds one element 0 template per array, `RantSchemaFieldInfo.arr_parent`
points a member back at the nearest array it belongs to, and `arr_depth` says how many
indices it takes.

## Wire format

Named types are `RANT_NAMED` (kind 18) in schema wire version 8:

```
NAMED := [u8 kind=18][u8 namelen >= 1][name][type inner]
```

`inner` is never itself NAMED, and NAMED never appears in root position. A root's name
rides the schema header instead, so there is exactly one encoding for it and the hash
stays canonical. `rant_schema_print` spells a schema back with each named type hoisted to
a leading definition, `Name { }` for a struct and `Name = type` for the rest, dependencies
first, and that text recompiles to identical bytes. The full wire grammar is in spec/schema.md.
