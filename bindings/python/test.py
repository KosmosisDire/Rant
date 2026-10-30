"""The Python binding test: two nodes on one host over reliable typed pub sub, the
patterns and the standard types. `test.py` runs every leg, `test.py name` one, `--list`
lists them. Exit 0 = pass (spec/testing.md)."""
import array
import ctypes
import enum as _enum
import inspect
import os
import struct
import subprocess
import sys
import threading
import time
import weakref
from dataclasses import dataclass, field
from typing import Annotated, Any, cast

# Import the package from the source checkout, which finds the library in dist/native.
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
import rant    # noqa: E402

# ctest hands each program a block of 64 domains through RANT_TEST_DOMAIN, a run by hand
# takes one from the clock, so parallel runs never meet
DOMAIN = int(os.environ.get("RANT_TEST_DOMAIN") or 1000 + time.monotonic_ns() // 1000 % 900 * 64)
IFACE = "127.0.0.1"   # single host: nothing listens on the network


# any annotated class is a schema, @dataclass just gives a handy constructor
@dataclass
class Velocity:
    dx: rant.f32 = 0.0
    dy: rant.f32 = 0.0


@dataclass
class Reading:
    stamp: rant.u64 = 0
    x:     rant.f64 = 0.0
    y:     rant.f64 = 0.0
    uuid:  Annotated[bytes, "u8[4]"] = b""
    frame: Annotated[str, "string<16>"] = ""             # a capped UTF-8 string
    tags:  Annotated[list[str], "string<8>[2]"] = field(default_factory=list)  # a fixed array of capped strings
    vel:   Velocity = field(default_factory=Velocity)


# The v4 variable-length kinds: a variable string, a variable scalar array, a
# variable string array, and a self-describing map.
@dataclass
class Sensor:
    id:      rant.u32 = 0
    name:    Annotated[str, "string<16>"] = ""                 # capped (fixed)
    note:    str = ""                                          # variable string
    samples: list[rant.f32] = field(default_factory=list)       # variable scalar array
    labels:  Annotated[list[str], "string<8>[]"] = field(default_factory=list)  # variable string array
    extras:  dict = field(default_factory=dict)               # self-describing map


# member names in Python style, snake_case on the wire too
@dataclass
class Waypoint:
    frame_id: Annotated[str, "string<16>"] = ""
    max_speed_mps: rant.f32 = 0.0
    gps_fix: bool = False


def round_trip():
    """Encode and decode round trip of the variable kinds, no networking."""
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    node = rant.Node("rt", on_event=lambda e: None, domain=DOMAIN + 9, multicast_interface=IFACE,
                     threading=rant.Threading.MANUAL)
    sch = node.schema(Sensor)
    print("Sensor DSL:\n" + sch.dsl)

    wp = node.schema(Waypoint)
    check("snake_case members spell snake_case on the wire",
          "frame_id: string<16>" in wp.dsl and "max_speed_mps: f32" in wp.dsl and "gps_fix: bool" in wp.dsl)
    check("a text in another spelling is the same schema",
          node.schema("Waypoint { FrameId: string<16>, max_speed_mps: f32, GPSFix: bool }").hash == wp.hash)
    back = wp.decode(wp.encode(Waypoint(frame_id="map", max_speed_mps=1.5, gps_fix=True)))
    check("and decode back into the members",
          back.frame_id == "map" and abs(back.max_speed_mps - 1.5) < 1e-6 and back.gps_fix is True)
    wd = node.schema(wp.dsl).decode(wp.encode(Waypoint(frame_id="odom")))
    check("a dictionary carries the wire names", wd["frame_id"] == "odom")
    any_spelling = wp.decode(wp.encode({"frameId": "x", "MaxSpeedMps": 2.0}))
    check("a dictionary source may use any spelling",
          any_spelling.frame_id == "x" and abs(any_spelling.max_speed_mps - 2.0) < 1e-6)
    src = Sensor(id=42, name="lidar",
                 note="a long unbounded note that exceeds sixteen bytes easily",
                 samples=[1.5, -2.25, 3.75], labels=["front", "left", "rearmost"],
                 extras={"battery": 87, "signed": -5, "mid": 40000, "neg32": -100000,
                         "big": 5_000_000_000, "state": "docked", "ok": True,
                         "temps": [36.2, 34.9, -1.0], "meta": {"fw": "1.2.3", "rev": 7}})
    out = sch.decode(sch.encode(src))
    check("note (vstr)", out.note == src.note)
    check("samples (varr f32)", len(out.samples) == 3 and abs(out.samples[0] - 1.5) < 1e-6)
    check("labels (varr string)", list(out.labels) == ["front", "left", "rearmost"])
    e = out.extras
    check("map scalars", e.get("battery") == 87 and e.get("signed") == -5
          and e.get("mid") == 40000 and e.get("neg32") == -100000
          and e.get("big") == 5_000_000_000 and e.get("state") == "docked" and e.get("ok") is True)
    check("map array", isinstance(e.get("temps"), list) and abs(e["temps"][0] - 36.2) < 1e-9)
    check("map nested", isinstance(e.get("meta"), dict) and e["meta"].get("fw") == "1.2.3"
          and e["meta"].get("rev") == 7)
    # empty variable fields round-trip to empty
    e2 = sch.decode(sch.encode(Sensor(id=1)))
    check("empty defaults", e2.note == "" and list(e2.samples) == []
          and list(e2.labels) == [] and e2.extras == {})
    try:
        sch.encode(Sensor(labels=["toolongforcap"]))
        check("over-cap raises", False)
    except rant.SchemaError:
        check("over-cap raises", True)
    check("SchemaError is a rant.Error", issubclass(rant.SchemaError, rant.Error))
    node.close()
    print("variable-kinds round-trip: " + ("PASS\n" if ok else "FAIL\n"))
    return ok


def _golden():
    """tests/golden/schemas.txt: the hash every binding pins for each schema text."""
    table = {}
    with open(os.path.join(_HERE, "..", "..", "tests", "golden", "schemas.txt")) as f:
        for line in f:
            parts = line.split()
            if parts and not parts[0].startswith("#"):
                table[parts[0]] = int(parts[1], 16)
    return table


GOLDEN = _golden()

# The canonical wire of a bare type is its kind alone, so these hashes are the same in
# every language binding.
HASH_BOOL = GOLDEN["bool"]
HASH_F32ARR = GOLDEN["f32[]"]


class Mode(_enum.IntEnum):
    IDLE = 0
    RUN = 1
    FAULT = 2


# The Float3 schema is the shared cross language golden vector: the same wire and the
# same hash from C, C++, C# and Python.
HASH_FLOAT3 = GOLDEN["Float3"]


@dataclass
class Track:
    """Standard types as ordinary annotations: an alias spells as its name and carries a
        plain value, a composite is a shipped dataclass. Both narrow matching."""
    at: rant.types.Transform = field(default_factory=rant.types.Transform)
    when: rant.types.Timestamp = 0
    tag: rant.types.Color = field(default_factory=rant.types.Color)
    id: rant.types.Uuid = bytes(16)
    velocity: rant.types.Float3 = field(default_factory=rant.types.Float3)


def std_types():
    """The standard type library: golden wire, name-narrowed matching, round trip."""
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    a = rant.Node("SA", on_event=lambda e: None, domain=DOMAIN + 10, multicast_interface=IFACE,
                  threading=rant.Threading.MANUAL)
    b = rant.Node("SB", on_event=lambda e: None, domain=DOMAIN + 10, multicast_interface=IFACE,
                  threading=rant.Threading.MANUAL)
    f3 = a.schema("Float3")
    check("Float3 compiles by name alone, golden hash", f3.hash == HASH_FLOAT3)
    check("Float3 is 12 message bytes", len(f3.encode({})) == 12)
    check("the mirror dataclass IS that type", a.schema(rant.types.Float3).hash == HASH_FLOAT3)

    # every struct has a name, so the same shape under another name never reads a Transform
    # field, and Transform/Twist are distinct names never mistaken for each other.
    # Two shapes of W, so each goes on its own node
    named = a.schema("W { at: Transform }")
    other = b.schema("Frame { pose: Pose, parent: string<30> }\nW { at: Frame }")
    check("the same shape under another name never reads a Transform field",
          not other.can_read(named) and not named.can_read(other))
    check("Transform and Twist never cross-wire",
          not a.schema("Twist").can_read(a.schema("Transform")))
    try:
        a.schema("W { at: Twist }")
        check("a second shape of W on one node is refused", False)
    except rant.SchemaError as e:
        check("a second shape of W on one node is refused: %s" % e, True)

    sch = a.schema(Track)
    text = " ".join(rant.dsl(Track).split())
    check("the reflected schema spells the names, not the shapes",
          text == "Track { at: Transform, when: Timestamp, tag: Color, id: Uuid, velocity: Float3 }")
    check("its message is the sum of the wire shapes (88+8+4+16+12)",
          len(sch.encode(Track())) == 128)

    t = Track(at=rant.types.Transform(rant.types.Pose(position=rant.types.Double3(4.5, -1.25, 9.0))),
              when=rant.types.now(), tag=rant.types.Color(0x11, 0x22, 0x33, 0xFF),
              id=bytes(range(16)), velocity=rant.types.Float3(1.0, 2.0, 3.0))
    back = sch.decode(sch.encode(t))
    check("a Track round-trips whole",
          back.at.pose.position.x == 4.5 and back.at.pose.orientation.w == 1.0
          and back.when == t.when and back.tag.r == 0x11 and back.tag.a == 0xFF
          and bytes(back.id) == bytes(range(16)) and back.velocity.z == 3.0)
    check("types.now is Unix-epoch microseconds", rant.types.now() > 1600000000000000)

    # the shapes: each mirror is its canonical type and size, Capsule and Cylinder never
    # cross wire, and a variable Polygon2D and an Empty round-trip
    T = rant.types
    for cls, size in ((T.Pose, 56), (T.Pose2D, 24), (T.Wrench, 48), (T.AlignedBox, 48),
                      (T.AlignedBox2D, 32), (T.OrientedBox, 80), (T.OrientedBox2D, 40),
                      (T.Plane, 48), (T.Segment, 48), (T.Sphere, 32), (T.Capsule, 56),
                      (T.Cylinder, 56), (T.Cone, 56), (T.Circle, 24), (T.Empty, 0),
                      (T.Transform2D, 56), (T.Twist2D, 24), (T.Wrench2D, 24),
                      (T.Plane2D, 32), (T.Segment2D, 32), (T.Capsule2D, 40)):
        check("%s mirror is %d bytes" % (cls.__name__, size), len(a.schema(cls).encode(cls())) == size)
    # every shipped mirror IS the library type it names, so none drifts unseen
    for cls in vars(T).values():
        std = getattr(cls, "__rant_std__", None)
        if not std:
            continue
        try:
            same = a.schema(cls).hash == a.schema(std).hash
        except rant.SchemaError as e:
            print("       %s: %s" % (std, str(e).splitlines()[0]))
            same = False
        check("the %s mirror IS that type" % std, same)
    check("Capsule and Cylinder never cross wire",
          not a.schema("Capsule").can_read(a.schema("Cylinder")))
    poly = a.schema(T.Polygon2D)
    pb = poly.decode(poly.encode(T.Polygon2D([T.Double2(0, 0), T.Double2(1, 0), T.Double2(1, 2)])))
    check("a Polygon2D round-trips whole", len(pb.points) == 3 and pb.points[2].y == 2)
    empty = a.schema(T.Empty)
    check("an Empty encodes to zero bytes and decodes back",
          len(empty.encode(T.Empty())) == 0 and isinstance(empty.decode(b""), T.Empty))
    a.close()
    b.close()
    return ok


# The video family, the same golden wire in every binding.
HASH_IMAGE = GOLDEN["Image"]
HASH_VIDEO_FRAME = GOLDEN["VideoFrame"]
HASH_EXT_STREAM = GOLDEN["ExternalVideoStream"]


@dataclass
class Clip:
    """The video mirrors used as FIELDS: each spells as its name, so the schema is the
    same as the text form and needs no definition of its own."""
    cover: rant.types.Image = field(default_factory=rant.types.Image)
    live: rant.types.ExternalVideoStream = field(default_factory=rant.types.ExternalVideoStream)


def video_types():
    """The video family: golden wire from both spellings, an Image over two nodes, and
    ExternalVideoStream as the latched stream URL it exists for."""
    print("video leg: golden wire, then two nodes, domain 45, loopback")
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    got = {}
    a = rant.Node("VidA", on_event=on_event("VidA"), domain=DOMAIN + 3, multicast_interface=IFACE,
                  threading=rant.Threading.MANUAL)
    b = rant.Node("VidB", on_event=on_event("VidB"), domain=DOMAIN + 3, multicast_interface=IFACE,
                  threading=rant.Threading.MANUAL)
    for name, cls, golden in (("Image", rant.types.Image, HASH_IMAGE),
                              ("VideoFrame", rant.types.VideoFrame, HASH_VIDEO_FRAME),
                              ("ExternalVideoStream", rant.types.ExternalVideoStream,
                               HASH_EXT_STREAM)):
        check("%s compiles by name alone, golden hash" % name,
              a.schema(name).hash == golden)
        check("the %s mirror IS that type" % name, a.schema(cls).hash == golden)
    check("a video mirror nests as a named field",
          a.schema(Clip).hash
          == a.schema("Clip { cover: Image, live: ExternalVideoStream }").hash)
    try:
        pub = a.publisher("frame", rant.types.Image, reliable=True, keep_last=4)
        b.subscriber("frame", rant.types.Image, lambda i: got.setdefault("img", i),
                     reliable=True, keep_last=4)
        stream = rant.types.ExternalVideoStream(kind=rant.types.VideoStreamKind.Rtsp,
                                          codec=rant.types.VideoCodec.H264,
                                          width=1920, height=1080,
                                          url="rtsp://cam.local/main", name="front door")
        vd = a.variable_definition("stream", rant.types.ExternalVideoStream, initial=stream)
        rv = b.remote_variable("stream", rant.types.ExternalVideoStream)
        check("handles created", all(h is not None for h in (pub, vd, rv)))
        deadline = time.time() + 8.0
        while time.time() < deadline and (pub.match_count == 0 or rv.get() is None):
            a.poll(0.001)
            b.poll(0.001)
        check("image pair matched", pub.match_count == 1)

        pixels = bytes((i * 7) & 0xFF for i in range(384))
        img = rant.types.Image(width=32, height=4, stride=96,
                         format=rant.types.ImageFormat.Rgb8, data=pixels)
        check("image send", pub.send(img) == rant.SendStatus.OK)
        deadline = time.time() + 5.0
        while time.time() < deadline and "img" not in got:
            a.poll(0.001)
            b.poll(0.001)
        r = got.get("img")
        check("an Image crosses whole",
              r is not None and r.width == 32 and r.height == 4 and r.stride == 96
              and r.format == rant.types.ImageFormat.Rgb8 and bytes(r.data) == pixels)

        v = rv.get()
        check("the stream variable replicated",
              v is not None and v.kind == rant.types.VideoStreamKind.Rtsp
              and v.codec == rant.types.VideoCodec.H264
              and v.width == 1920 and v.height == 1080
              and v.url == "rtsp://cam.local/main" and v.name == "front door")
    finally:
        a.close()
        b.close()
    print("video family: " + ("PASS\n" if ok else "FAIL\n"))
    return ok


# A struct array: list[Code] declares it, and the flat table has the element as the row
# after the array, its members after that.
@dataclass
class Code:
    type:  Annotated[str, "string<16>"] = ""
    angle: rant.f32 = 0.0
    pt:    Velocity = field(default_factory=Velocity)     # a nested struct in the element


@dataclass
class Outline:
    corners: Annotated[list[rant.types.Float2], "Float2[4]"] = field(default_factory=list)
    marks:   Annotated[list[Velocity], "Velocity[2]"] = field(default_factory=list)


@dataclass
class Detections:
    stamp: rant.u64 = 0
    codes: list[Code] = field(default_factory=list)
    note:  str = ""                                       # a tail frame after the array


def struct_arrays():
    """Arrays of structs: declared, encoded and decoded through the element rows of the
    flat table."""
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    node = rant.Node("sarr", on_event=lambda e: None, domain=DOMAIN + 11, multicast_interface=IFACE,
                     threading=rant.Threading.MANUAL)
    sch = node.schema(Detections)
    print("Detections DSL:\n" + sch.dsl)
    src = Detections(stamp=9, note="after the array",
                     codes=[Code("C128", 1.0, Velocity(0.5, -0.5)),
                            Code("QR", -2.5, Velocity(1.0, 2.0))])
    out = sch.decode(sch.encode(src))
    check("elements are instances", all(isinstance(c, Code) for c in out.codes))
    check("element scalars", [c.type for c in out.codes] == ["C128", "QR"]
          and abs(out.codes[1].angle + 2.5) < 1e-6)
    check("nested struct in an element", abs(out.codes[0].pt.dx - 0.5) < 1e-6
          and abs(out.codes[1].pt.dy - 2.0) < 1e-6)
    check("the fields around it", out.stamp == 9 and out.note == "after the array")
    check("empty array round-trips empty", sch.decode(sch.encode(Detections())).codes == [])

    # the same wire from DSL text, and the whole of one element's kinds
    text = node.schema("Power = enum<u8> { OFF=0, ON=1 }\n"
                       "Row { a: u8, b: i16, c: f64, d: bool, e: string<4>, f: f32[2], g: Power }\n"
                       "D { rows: Row[], n: u8 }")
    rows = [{"a": 1, "b": -2, "c": 0.5, "d": True, "e": "hi", "f": [1.0, 2.0], "g": 1},
            {"a": 255, "b": -32768, "c": -1.5, "d": False, "e": "", "f": [0.0, 0.0], "g": 0}]
    want = [dict(r, f=array.array("f", r["f"])) for r in rows]   # a numeric array decodes as array.array
    check("every fixed kind inside an element",
          text.decode(text.encode({"rows": rows, "n": 3})) == {"rows": want, "n": 3})
    check("an enum element member takes its option name",
          text.decode(text.encode({"rows": [dict(rows[0], g="OFF")]}))["rows"][0]["g"] == 0)

    fixed = node.schema("P { pts: Float2[3] }")
    short = fixed.decode(fixed.encode({"pts": [{"x": 1.0, "y": 2.0}]}))
    check("a fixed struct array zero fills to its count",
          len(fixed.encode({"pts": []})) == 24 and len(short["pts"]) == 3
          and short["pts"][2] == {"x": 0.0, "y": 0.0})

    text = rant.dsl(Detections)
    check("a nested class is defined once and used by name",
          text.count("Code\n{") == 1 and text.count("Velocity\n{") == 1
          and "codes: Code[]" in text and "pt: Velocity" in text)
    check("the reflected text is the spelled out text",
          node.schema("Velocity { dx: f32, dy: f32 }\n"
                      "Code { type: string<16>, angle: f32, pt: Velocity }\n"
                      "Detections { stamp: u64, codes: Code[], note: string }").hash == sch.hash)
    root = rant.dsl(list[Code])
    check("a struct array root defines its element above it",
          root.startswith("Velocity\n{") and root.endswith("\nCode[]\n"))
    ol = node.schema(Outline)
    check("Annotated text names a fixed struct array",
          "corners: Float2[4]" in rant.dsl(Outline) and "marks: Velocity[2]" in rant.dsl(Outline))
    got = ol.decode(ol.encode(Outline(corners=[rant.types.Float2(1.0, 2.0)],
                                      marks=[Velocity(3.0, 4.0)])))
    check("a fixed struct array fills and zero pads",
          len(got.corners) == 4 and got.corners[0].y == 2.0 and got.corners[3].x == 0.0
          and isinstance(got.marks[1], Velocity) and got.marks[0].dx == 3.0)
    try:
        rant.dsl(type("Bad", (), {"__annotations__": {"a": Annotated[list[Velocity], "Code[2]"]}}))
        check("a struct name that is not the list's class is refused", False)
    except rant.SchemaError:
        check("a struct name that is not the list's class is refused", True)
    outlines = node.schema(list[Outline])
    back = outlines.decode(outlines.encode([Outline(corners=[rant.types.Float2(1.0, 2.0)]),
                                            Outline(marks=[Velocity(0.0, 5.0)])]))
    check("a fixed struct array nests in a struct array's element",
          len(back) == 2 and back[0].corners[0].y == 2.0 and len(back[1].corners) == 4
          and back[1].marks[0].dy == 5.0)
    grid = node.schema("Cell { v: i32 }\nTier { cells: Cell[3], n: u8 }\nG { rows: Tier[2] }")
    got = grid.decode(grid.encode({"rows": [{"cells": [{"v": 1}], "n": 4},
                                            {"cells": [{"v": 7}, {"v": 8}, {"v": 9}]}]}))
    check("nested struct arrays from dicts read back per level",
          got["rows"][0]["cells"][0]["v"] == 1 and got["rows"][0]["cells"][2]["v"] == 0
          and got["rows"][1]["cells"][2]["v"] == 9 and got["rows"][0]["n"] == 4)
    check("the field table counts the enclosing arrays, an element row after each array",
          [(f.name, f.arr_depth) for f in grid.fields]
          == [("rows", 0), ("", 1), ("cells", 1), ("", 2), ("v", 2), ("n", 1)])
    bare = node.schema(list[Code])
    check("a struct array is a bare root too",
          bare.decode(bare.encode([Code("A", 1.0)]))[0].type == "A")
    check("list[Code] and the DSL text agree",
          node.schema(Detections).hash
          == node.schema(rant.dsl(Detections)).hash)
    try:
        sch.encode(Detections(codes=[Code("way-too-long-for-sixteen-bytes")]))
        check("over-cap inside an element raises", False)
    except rant.SchemaError:
        check("over-cap inside an element raises", True)
    try:
        sch.encode({"codes": {"type": "C128"}})
        check("a non-list for a struct array raises", False)
    except rant.SchemaError:
        check("a non-list for a struct array raises", True)
    node.close()
    print("struct arrays: " + ("PASS\n" if ok else "FAIL\n"))
    return ok


# Any type as an element: records with arrays of their own, arrays of arrays, strings,
# enums and maps. The same shapes as the C++ and C# tests, so the hashes agree.
@dataclass
class Stretch:
    speed: rant.f32 = 0.0
    pts:   list[rant.types.Float2] = field(default_factory=list)


@dataclass
class Route:
    id:       rant.u32 = 0
    segments: list[Stretch] = field(default_factory=list)
    name:     str = ""


class Lane(_enum.IntEnum):
    Left = 0
    Right = 1


@dataclass
class Layers:
    grid:  list[list[rant.f32]] = field(default_factory=list)
    rows:  Annotated[list[list[float]], "f32[3][]"] = field(default_factory=list)
    names: list[str] = field(default_factory=list)
    pair:  Annotated[list[str], "string[2]"] = field(default_factory=list)
    legs:  Annotated[list[Stretch], "Stretch[2]"] = field(default_factory=list)
    m:     Annotated[list[list[float]], "f32[3][2]"] = field(default_factory=list)
    areas: list[rant.types.Polygon2D] = field(default_factory=list)
    lanes: list[Lane] = field(default_factory=list)
    words: list[list[str]] = field(default_factory=list)
    tags:  Annotated[list[list[str]], "string<8>[2][]"] = field(default_factory=list)
    flags: list[bool] = field(default_factory=list)
    notes: list[dict] = field(default_factory=list)


def nested():
    """Any type as an array element, nested, typed and as dicts."""
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    def open_node(name):
        return rant.Node(name, on_event=lambda e: None, domain=DOMAIN + 18,
                         multicast_interface=IFACE, threading=rant.Threading.MANUAL)

    F2, D2 = rant.types.Float2, rant.types.Double2
    node, other = open_node("nest"), open_node("nest-w")   # a name has one shape per node
    rs = node.schema(Route)
    same = node.schema("Stretch { speed: f32, pts: Float2[] }\n"
                       "Route { id: u32, segments: Stretch[], name: string }")
    check("a record element spells as C++ and C# do", rs.hash == same.hash)
    wire = rs.encode(Route(7, [Stretch(1.5, [F2(1, 2), F2(3, 4)]), Stretch(2.5, [F2(5, 6)])], "loop"))
    back = rs.decode(wire)
    check("a record element with its own array round trips",
          back.id == 7 and back.name == "loop" and len(back.segments) == 2
          and isinstance(back.segments[0], Stretch) and back.segments[0].pts[1].y == 4
          and back.segments[1].speed == 2.5 and back.segments[1].pts[0].x == 5)
    check("it decodes as nested lists of dicts", same.decode(wire)["segments"][0]["pts"][1]["y"] == 4)
    wide = other.schema("Stretch { speed: f32, pts: Float2[] }\n"
                        "Route { tag: string, id: u32, extra: f64[], segments: Stretch[], name: string }")
    wd = wide.decode(wide.encode({"tag": "first", "id": 9, "extra": [1.0, 2.0, 3.0],
                                  "segments": [{"speed": 4.0}, {"pts": [{}, {"x": 7}]}],
                                  "name": "wide"}))
    check("dicts fill arrays inside the elements of another layout",
          wd["tag"] == "first" and wd["segments"][0]["speed"] == 4.0 and wd["segments"][0]["pts"] == []
          and len(wd["segments"][1]["pts"]) == 2 and wd["segments"][1]["pts"][1]["x"] == 7.0
          and wd["name"] == "wide")

    ls = node.schema(Layers)
    same = node.schema("Stretch { speed: f32, pts: Float2[] }\nLane = enum<u8> { Left=0, Right=1 }\n"
                       "Layers { grid: f32[][], rows: f32[3][], names: string[], pair: string[2], "
                       "legs: Stretch[2], m: f32[3][2], areas: Polygon2D[], "
                       "lanes: Lane[], words: string[][], "
                       "tags: string<8>[2][], flags: bool[], notes: map[] }")
    check("every nesting spells as the DSL does", ls.hash == same.hash)
    src = Layers(grid=[[1.0, 2.0], [], [3.0]], rows=[[1, 2, 3], [4, 5, 6]], names=["abc", "", "hello"],
                 pair=["left", "right"], legs=[Stretch(), Stretch(3.0, [F2(8, 9)])],
                 m=[[0, 2, 0], [0, 0, 6]],
                 areas=[rant.types.Polygon2D(), rant.types.Polygon2D([D2(1, 1), D2(2, 1), D2(2, 2)])],
                 lanes=[Lane.Right, Lane.Left], words=[["a", "bc"], [], ["def"]],
                 tags=[["t1"], ["t2", "t3"]], flags=[True, False, True], notes=[{"k": 1}, {}])
    back = ls.decode(ls.encode(src))
    check("arrays of arrays, strings and records round trip",
          [list(g) for g in back.grid] == [[1.0, 2.0], [], [3.0]] and isinstance(back.grid[0], array.array)
          and [list(r) for r in back.rows] == [[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]]
          and back.names == ["abc", "", "hello"] and back.pair == ["left", "right"]
          and back.legs[0].pts == [] and back.legs[1].pts[0].y == 9.0
          and [list(r) for r in back.m] == [[0.0, 2.0, 0.0], [0.0, 0.0, 6.0]]
          and back.areas[0].points == [] and back.areas[1].points[2].y == 2.0)
    check("enum, capped string, bool and map elements round trip",
          back.lanes == [Lane.Right, Lane.Left] and isinstance(back.lanes[0], Lane)
          and back.words == [["a", "bc"], [], ["def"]] and back.tags == [["t1", ""], ["t2", "t3"]]
          and back.flags == [True, False, True] and back.notes == [{"k": 1}, {}])
    empty = ls.decode(ls.encode(Layers()))
    check("unset nested arrays decode empty and fixed ones at their count",
          empty.grid == [] and empty.words == [] and empty.pair == ["", ""] and len(empty.legs) == 2
          and empty.legs[0].pts == [] and [list(r) for r in empty.m] == [[0.0] * 3] * 2 and empty.tags == [])
    try:
        ls.encode(Layers(tags=[["far too long"]]))
        check("an over cap string deep in an array raises", False)
    except rant.SchemaError as e:
        check("an over cap string deep in an array raises: %s" % e, True)
    check("a fixed array subscripts again for an array of arrays",
          rant.dsl(cast(Any, rant.f32)[3][2]) == "f32[3][2]\n"
          and rant.dsl(list[cast(Any, rant.string(8))[2]]) == "string<8>[2][]\n")
    ws = node.schema(list[list[str]])
    check("a bare array of string arrays is a whole schema", ws.hash == node.schema("string[][]").hash)
    check("and round trips", ws.decode(ws.encode([["x", "yz"], ["w"]])) == [["x", "yz"], ["w"]])
    other.close()
    node.close()
    print("nested: " + ("PASS\n" if ok else "FAIL\n"))
    return ok


def struct_arrays_live():
    """A struct array over a real topic: the reader decodes the writer's layout."""
    print("struct-array live leg: two nodes, domain 45, loopback")
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    seen = []
    a = rant.Node("SA", on_event=lambda e: None, domain=DOMAIN + 3, multicast_interface=IFACE,
                  threading=rant.Threading.MANUAL)
    b = rant.Node("SB", on_event=lambda e: None, domain=DOMAIN + 3, multicast_interface=IFACE,
                  threading=rant.Threading.MANUAL)
    try:
        pub = a.publisher("codes", Detections, reliable=True, keep_last=4)
        b.subscriber("codes", Detections, lambda v: seen.append(v),
                     reliable=True, keep_last=4)
        sent = Detections(stamp=3, note="live",
                          codes=[Code("C128", 1.0, Velocity(0.5, -0.5)),
                                 Code("QR", -2.5, Velocity(1.0, 2.0)),
                                 Code("", 0.0)])
        deadline = time.time() + 8.0
        while time.time() < deadline and not seen:
            pub.send(sent)
            a.poll(0.001)
            b.poll(0.001)
        check("delivered", bool(seen))
        if seen:
            r = seen[0]
            check("three elements arrived whole",
                  len(r.codes) == 3 and [c.type for c in r.codes] == ["C128", "QR", ""]
                  and abs(r.codes[1].pt.dy - 2.0) < 1e-6 and r.note == "live")
    finally:
        a.close()
        b.close()
    print("struct-array live leg: " + ("PASS\n" if ok else "FAIL\n"))
    return ok


def value_roots():
    """Bare types as whole schemas: encode/decode plain values, and the canonical hash."""
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    node = rant.Node("vr", on_event=lambda e: None, domain=DOMAIN + 12, multicast_interface=IFACE,
                     threading=rant.Threading.MANUAL)
    check("bool canonical hash", node.schema(bool).hash == HASH_BOOL)
    check("f32[] canonical hash", node.schema(list[rant.f32]).hash == HASH_F32ARR)
    check("bool dsl", rant.dsl(bool) == "bool\n")
    check("f32[] dsl", rant.dsl(list[rant.f32]) == "f32[]\n")
    check("string(16) dsl", rant.dsl(rant.string(16)) == "string<16>\n")
    check("bare root reflects as one anonymous field",
          len(node.schema(rant.f64).fields) == 1
          and node.schema(rant.f64).name == ""
          and node.schema(rant.f64).fields[0].name == "")
    check("enum variants read back",
          node.schema(Mode).enum_variants(0) == [("Idle", 0), ("Run", 1), ("Fault", 2)])
    for src, value in ((bool, True), (rant.u8, 200), (rant.i32, -7),
                       (rant.f32, 1.5), (rant.f64, -2.25), (int, 5), (float, 0.5),
                       (bool, False), (rant.string(16), "capped"), (str, "unbounded"),
                       (list[rant.f32], [1.5, -2.5]), (dict, {"battery": 87}),
                       ("u8[4]", b"\x01\x02\x03\x04"), (Mode, Mode.FAULT)):
        sch = node.schema(src)
        out = sch.decode(sch.encode(value))
        if isinstance(value, list):
            same = [round(x, 3) for x in out] == [round(x, 3) for x in value]
        elif isinstance(value, float):
            same = abs(out - value) < 1e-6
        elif isinstance(value, bytes):
            same = bytes(out) == value
        else:
            same = out == value
        check("round-trip %s -> %r" % (rant.dsl(src).strip(), out), same)
    # a standard alias root is named after its type and is still a bare value
    T = rant.types
    for alias, value in ((T.Timestamp, 1234567), (T.Duration, -5), (T.Uuid, bytes(range(16))),
                         (T.Matrix3x3, [float(i) for i in range(9)]),
                         (T.Matrix4x4, [float(i) for i in range(16)]),
                         (T.Uri, "rtsp://host/stream")):
        for sch in (node.schema(alias), node.schema(alias.name)):
            out = sch.decode(sch.encode(value))
            got = bytes(out) if isinstance(value, bytes) else list(out) if isinstance(value, list) else out
            check("round-trip %s root -> %r" % (sch.name, out), got == value)
    # text DSL and the bare type agree, and a named bare root is an error
    check("text `bool` == plain bool", node.schema("bool").hash == HASH_BOOL)
    try:
        node.schema("Temperature: f32")
        check("named bare root refused", False)
    except rant.SchemaError:
        check("named bare root refused", True)
    node.close()
    print("bare-type roots: " + ("PASS\n" if ok else "FAIL\n"))
    return ok


def lifecycle():
    """A node runs from construction and closes itself: threaded delivery with no poll, the
    last reference dropped, and interpreter exit, each seen as a bye by a watcher."""
    print("lifecycle leg: domain 47, loopback")
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    seen = []
    changed = threading.Condition()

    def watch(e):
        if e.kind in (rant.EventKind.PEER_UP, rant.EventKind.PEER_DOWN):
            with changed:
                seen.append((e.kind, e.peer_name))
                changed.notify_all()

    def saw(kind, name, timeout):
        with changed:
            return changed.wait_for(lambda: (kind, name) in seen, timeout)

    watcher = rant.Node("watcher", on_event=watch, domain=DOMAIN + 5, multicast_interface=IFACE)
    try:
        check("service thread by default", watcher.threading == rant.Threading.SERVICE_THREAD)
        check("poll refused under the service thread",
              watcher.poll(0) == rant.SendStatus.STATE)

        got = threading.Event()
        watcher.subscriber("twist", Velocity, lambda t: got.set(), reliable=True)

        def open_and_drop():
            n = rant.Node("dropped", on_event=lambda e: None, domain=DOMAIN + 5,
                          multicast_interface=IFACE)
            p = n.publisher("twist", Velocity, reliable=True)
            deadline = time.time() + 8.0
            while time.time() < deadline and p.match_count == 0:
                time.sleep(0.005)
            p.send(Velocity(dx=1.0))
            check("threaded delivery with no poll", got.wait(3.0))
            return weakref.ref(n)
        ref = open_and_drop()
        check("the dropped node is collected", ref() is None)
        check("the dropped node said bye", saw(rant.EventKind.PEER_DOWN, "dropped", 3.0))

        child = ("import sys, time; sys.path.insert(0, %r); import rant; "
                 "n = rant.Node('exiting', on_event=lambda e: None, domain=%d, "
                 "multicast_interface=%r); "
                 "n.subscriber('flag', bool, lambda v: None); time.sleep(1.5)"
                 % (_HERE, DOMAIN + 5, IFACE))
        r = subprocess.run([sys.executable, "-c", child], timeout=30)
        check("exit with a live node returns 0", r.returncode == 0)
        check("the exiting node said bye", saw(rant.EventKind.PEER_DOWN, "exiting", 3.0))
    finally:
        watcher.close()
    print("lifecycle: " + ("PASS\n" if ok else "FAIL\n"))
    return ok


def handles():
    """Same name handles on one node share the topic slot, close releases one hold and the
    last close retires the name. Plus the reflection walks and the log write."""
    print("handles leg: two nodes, domain 48, loopback")
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    got = []
    a = rant.Node("HA", on_event=lambda e: None, domain=DOMAIN + 6, multicast_interface=IFACE,
                  fetch_details=True)
    b = rant.Node("HB", on_event=lambda e: None, domain=DOMAIN + 6, multicast_interface=IFACE)
    try:
        p = a.publisher("shared", Velocity, reliable=True)
        s = a.subscriber("shared", Velocity, lambda t: None, reliable=True)
        vel = a.schema(Velocity).hash
        check("both handles report their schema", p.schema.hash == vel and s.schema.hash == vel)
        try:
            a.publisher("shared", Reading)
            check("a different schema on a live name is refused", False)
        except rant.Error:
            check("a different schema on a live name is refused", True)
        b.subscriber("shared", Velocity, lambda t: got.append(t), reliable=True)
        fn = a.function_definition("double", lambda q: Velocity(dx=q.dx * 2), Velocity, Velocity)
        deadline = time.time() + 8.0
        while time.time() < deadline and p.match_count == 0:
            time.sleep(0.005)
        check("publisher matched the peer's subscriber", p.match_count == 1)
        check("the sibling subscriber closes", s.close())
        check("close is idempotent", s.close())
        check("a closed handle reports no schema", s.schema is None)
        check("the publisher still sends", p.send(Velocity(dx=3.0)) == rant.SendStatus.OK)
        deadline = time.time() + 5.0
        while time.time() < deadline and not got:
            time.sleep(0.005)
        check("and the peer still receives", bool(got) and abs(got[0].dx - 3.0) < 1e-6)
        check("counts show the send", p.counts.tx_msgs >= 1)
        check("the last handle closes", p.close())
        check("a closed handle answers NO_TOPIC", p.send(Velocity()) == rant.SendStatus.NO_TOPIC)
        p2 = a.publisher("shared", Reading)
        check("the name carries another schema after the last close", p2.name == "shared")

        # reflection: the peer, this node's entities, the folded mesh and one lookup
        peers = a.reflection.peers()
        check("the peer is listed active",
              any(x.name == "HB" and x.active for x in peers))
        hb = next((x for x in peers if x.name == "HB" and x.active), None)
        snap = a.reflection.meta(hb.id, rant.MetaSection.NODE, 3.0) if hb else None
        check("meta counts the matched peers",
              snap is not None and snap.valid and snap.name == "HB"
              and 1 <= snap.matched_peers <= snap.max_peers)
        mine = a.reflection.entities()
        check("this node's entities carry the function",
              any(e.kind == rant.EntityKind.FUNCTION and e.name == "double" and e.provides
                  for e in mine))
        found = a.reflection.find(rant.EntityKind.TOPIC, "shared")
        check("find folds the topic", found is not None and found.kind == rant.EntityKind.TOPIC)
        check("the folded schema is an owned copy",
              found is not None and found.schema is not None
              and found.schema.hash == a.schema(Reading).hash)
        check("mesh lists what find found",
              any(e.name == "shared" for e in a.reflection.mesh()))
        check("epoch is a counter", isinstance(a.reflection.epoch, int))
        check("the function definition closes", fn.close())

        check("log writes", a.log(rant.Level.INFO, "hello") == rant.SendStatus.OK)
        handler = lambda line: None
        check("on_log binds and returns the handler", a.on_log(handler) is handler)
        st = a.stats
        check("stats is one snapshot", st.mem_in_use > 0 and st.alloc_calls > 0)
        # the Reading publisher against HB's Velocity subscriber is the mismatch it records, once
        # HB's interest has come back
        deadline = time.time() + 5.0
        while time.time() < deadline and a.last_error.error == rant.ErrorKind.NONE:
            time.sleep(0.005)
        check("last_error is the schema mismatch",
              a.last_error.error == rant.ErrorKind.SCHEMA_MISMATCH)
        # a code near the end of the enum: any drift from the C enum shows as another name
        try:
            a.publisher("bad@name")
        except rant.Error:
            pass
        check("a late error code keeps its name",
              a.last_error.error == rant.ErrorKind.BAD_NAME)
    finally:
        a.close()
        b.close()
    print("handles: " + ("PASS\n" if ok else "FAIL\n"))
    return ok


def value_roots_live():
    """Two nodes, bare-typed topics and a bare-typed variable, over loopback."""
    print("bare-root live leg: two nodes, domain 44, loopback")
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    got = {}
    a = rant.Node("VA", on_event=lambda e: None, domain=DOMAIN + 2, multicast_interface=IFACE,
                  threading=rant.Threading.MANUAL)
    b = rant.Node("VB", on_event=lambda e: None, domain=DOMAIN + 2, multicast_interface=IFACE,
                  threading=rant.Threading.MANUAL)
    try:
        pub_flag = a.publisher("flag", bool, reliable=True, keep_last=4)
        sub_flag = b.subscriber("flag", bool, lambda v, m: got.setdefault(m.topic_name, v),
                                reliable=True, keep_last=4)
        pub_note = a.publisher("note", str, reliable=True, keep_last=4)
        sub_note = b.subscriber("note", str, lambda v, m: got.setdefault(m.topic_name, v),
                                reliable=True, keep_last=4)
        pub_url = a.publisher("url", rant.types.Uri, reliable=True, keep_last=4)
        sub_url = b.subscriber("url", rant.types.Uri, lambda v, m: got.setdefault(m.topic_name, v),
                               reliable=True, keep_last=4)
        vd = a.variable_definition("gain", rant.f64, initial=1.25)
        rv = b.remote_variable("gain", rant.f64)
        check("handles created", all(h is not None for h in
                                    (pub_flag, sub_flag, pub_note, sub_note, pub_url, sub_url,
                                     vd, rv)))
        deadline = time.time() + 8.0
        while time.time() < deadline and (pub_flag.match_count == 0
                                          or pub_note.match_count == 0
                                          or pub_url.match_count == 0
                                          or rv.get() is None):
            a.poll(0.001)
            b.poll(0.001)
        check("bare topics matched",
              pub_flag.match_count == 1 and pub_note.match_count == 1
              and pub_url.match_count == 1)
        check("bare variable replicated the initial", rv.get() == 1.25)
        pub_flag.send(True)
        pub_note.send("a bare unbounded string")
        pub_url.send("rtsp://host/stream")
        deadline = time.time() + 5.0
        while time.time() < deadline and len(got) < 3:
            a.poll(0.001)
            b.poll(0.001)
        check("bool delivered as a plain value", got.get("flag") is True)
        check("string delivered as a plain value", got.get("note") == "a bare unbounded string")
        check("Uri root delivered as a plain value", got.get("url") == "rtsp://host/stream")
        rv.set(2.5)
        deadline = time.time() + 5.0
        while time.time() < deadline and vd.get() != 2.5:
            a.poll(0.001)
            b.poll(0.001)
        check("bare variable set converged", vd.get() == 2.5)
    finally:
        a.close()
        b.close()
    print("bare-root live leg: " + ("PASS\n" if ok else "FAIL\n"))
    return ok


# patterns leg types
@dataclass
class AddReq:
    a: rant.i32 = 0
    b: rant.i32 = 0


@dataclass
class AddRsp:
    sum: rant.i32 = 0


@dataclass
class Level:
    value: rant.i32 = 0


@dataclass
class ProbeRead:
    __rant_name__ = "Probe"   # one wire name in three layouts, each writer puts a and b elsewhere
    a: rant.u32 = 0
    b: rant.u32 = 0


@dataclass
class ProbeFirst:
    __rant_name__ = "Probe"
    b: rant.u32 = 0
    a: rant.u32 = 0


@dataclass
class ProbeSecond:
    __rant_name__ = "Probe"
    a: rant.u32 = 0
    x: rant.u32 = 0
    b: rant.u32 = 0


def rebind():
    """A rebase, then another after the reader's retired slot goes to another name: each
    writer's layout decodes with its own offsets, never those cached for an earlier one."""
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    def until(seconds, done):
        deadline = time.time() + seconds
        while time.time() < deadline and not done():
            time.sleep(0.002)
        return done()

    opts = dict(on_event=lambda e: None, domain=DOMAIN + 14, multicast_interface=IFACE)
    r, w1, w2 = rant.Node("RR", **opts), rant.Node("RW1", **opts), rant.Node("RW2", **opts)
    got = []
    try:
        pub = w1.publisher("rb/first", ProbeFirst, reliable=True)
        sub = r.subscriber("rb/first", ProbeRead, got.append, reliable=True)
        check("first pair matched", until(4, lambda: pub.match_count > 0 and pub.ready))
        pub.send(ProbeFirst(b=2, a=1))
        check("the first layout decodes",
              until(3, lambda: len(got) == 1) and (got[0].a, got[0].b) == (1, 2))
        sub.close()
        pub.close()
        pub = w2.publisher("rb/second", ProbeSecond, reliable=True)
        r.subscriber("rb/second", ProbeRead, got.append, reliable=True)
        check("second pair matched", until(4, lambda: pub.match_count > 0 and pub.ready))
        pub.send(ProbeSecond(a=1, x=99, b=2))
        check("the second layout decodes with its own offsets",
              until(3, lambda: len(got) == 2) and (got[1].a, got[1].b) == (1, 2))
    finally:
        for n in (w2, w1, r):
            n.close()
    return ok


def close_race():
    """Calls on other threads while the node closes answer NO_TOPIC or their usual result and
    never touch the freed node, and a close inside the node's own callback runs once it
    returns. ctypes drops the GIL around every C call, so the threads really race."""
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    def until(seconds, done):
        deadline = time.time() + seconds
        while time.time() < deadline and not done():
            time.sleep(0.002)
        return done()

    opts = dict(on_event=lambda e: None, domain=DOMAIN + 15, multicast_interface=IFACE)
    refused_after = True
    for _ in range(40):
        n = rant.Node("CR", match_wait=None, **opts)   # a send with nobody matched drops at once
        pub = n.publisher("cr/level", Level)
        var = n.variable_definition("cr/var", Level, initial=Level())
        stop = threading.Event()
        calls = [0]

        def sender():
            for i in range(1 << 30):
                if stop.is_set():
                    return
                pub.send(Level(value=i))
                calls[0] += 1

        def writer():
            for i in range(1 << 30):
                if stop.is_set():
                    return
                var.set(Level(value=i))
                calls[0] += 1

        def churn():
            while not stop.is_set():
                try:
                    with n.publisher("cr/churn", Level) as p:
                        p.send(Level(value=1))
                except rant.Error:
                    pass
                calls[0] += 1

        threads = [threading.Thread(target=f) for f in (sender, writer, churn)]
        for t in threads:
            t.start()
        while calls[0] < 300:
            time.sleep(0)
        n.close()
        refused_after = (refused_after and pub.send(Level()) == rant.SendStatus.NO_TOPIC
                         and var.set(Level()) == rant.SendStatus.NO_TOPIC)
        stop.set()
        for t in threads:
            t.join()
    check("after close every call answers NO_TOPIC", refused_after)

    src = rant.Node("CS", **opts)
    me = rant.Node("CE", threading=rant.Threading.MANUAL, **opts)
    try:
        accepted = []
        src_pub = src.publisher("cr/close", Level, reliable=True)
        me.subscriber("cr/close", Level, lambda v: accepted.append(me.close()), reliable=True)
        probe = me.publisher("cr/probe", Level)

        def polled(cond):
            me.poll(0.002)
            return cond()
        check("self closing node matched",
              until(4, lambda: polled(lambda: src_pub.match_count > 0 and src_pub.ready)))
        src_pub.send(Level(value=5))
        check("a close inside its own callback is accepted",
              until(3, lambda: polled(lambda: accepted == [True])))
        me.close()   # waits for the close the callback started
        check("the node closed once the callback returned",
              probe.send(Level()) == rant.SendStatus.NO_TOPIC)
    finally:
        me.close()
        src.close()
    return ok


def handle_gate():
    """A handle close waits for the calls inside it on other threads, is refused inside its
    own call's callback, and a deferred answer after its definition closed is refused."""
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    def until(seconds, done):
        deadline = time.time() + seconds
        while time.time() < deadline and not done():
            time.sleep(0.005)
        return done()

    opts = dict(on_event=lambda e: None, domain=DOMAIN + 16, multicast_interface=IFACE,
                max_topics=32)
    a = rant.Node("ga", **opts)
    b = rant.Node("gb", **opts)
    try:
        def _slow(q):
            time.sleep(0.3)
            return AddRsp(sum=q.a + q.b)
        a.function_definition("g/slow", _slow, AddReq, AddRsp)
        rem = b.remote_function("g/slow", AddReq, AddRsp)
        check("matched", until(8, lambda: rem.match_count > 0))
        got = []
        call_done = threading.Event()

        def caller():
            got.append(rem.call(AddReq(a=1, b=2), timeout=3.0))
            call_done.set()
        t = threading.Thread(target=caller)
        t.start()
        time.sleep(0.1)
        rem.close()
        done_first = call_done.is_set()
        t.join()
        check("a close waits for the blocking call on another thread", done_first)
        check("which completed", got[0].status == rant.CallStatus.OK)
        check("then the remote answers NO_TOPIC",
              rem.call(AddReq(a=1, b=2), timeout=0.1).send_status == rant.SendStatus.NO_TOPIC)

        def _job(q, task):
            task.progress(JobPrg(done=1))
            time.sleep(0.2)
            return JobRsp(total=q.count)
        a.task_definition("g/job", _job, JobReq, JobPrg, JobRsp)
        jr = b.remote_task("g/job", JobReq, JobPrg, JobRsp)
        check("task matched", until(8, lambda: jr.match_count > 0))
        inside = []
        r2 = jr.call(JobReq(count=3), on_progress=lambda v: inside.append(jr.close()),
                     timeout=3.0)
        check("a close inside the call's own callback is refused at once",
              inside and not any(inside))
        check("and the call still completes", r2.status == rant.CallStatus.OK)
        check("a close after it retires the remote", jr.close()
              and jr.call(JobReq(count=1), timeout=0.1).send_status == rant.SendStatus.NO_TOPIC)

        parked = []
        park = a.function_definition("g/park", lambda q, req: parked.append(req.defer()),
                                     AddReq, AddRsp)
        pr = b.remote_function("g/park", AddReq, AddRsp)
        check("park matched", until(8, lambda: pr.match_count > 0))
        pr.call_async(AddReq(a=1, b=1), lambda r: None)
        check("the answer parked", until(8, lambda: len(parked) == 1))
        park.close()
        check("a deferred answer after its definition closed is refused",
              not parked[0].complete(AddRsp(sum=2)))
    finally:
        a.close()
        b.close()
    return ok


def patterns():
    """Functions and variables between two nodes on an isolated domain."""
    print("patterns leg: two nodes, domain 43, loopback")
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    srv = rant.Node("srv", on_event=on_event("srv"),
                    domain=DOMAIN + 1, multicast_interface=IFACE, max_topics=32)
    cli = rant.Node("cli", on_event=on_event("cli"),
                    domain=DOMAIN + 1, multicast_interface=IFACE, max_topics=32)

    # definitions on srv: the simple form, a raiser giving APP_ERROR, and a full form that
    # defers and completes off thread
    add = srv.function_definition("add", lambda q: AddRsp(sum=q.a + q.b), AddReq, AddRsp)

    def _boom(q):
        raise RuntimeError("kaboom")   # noqa: the traceback print is expected
    srv.function_definition("boom", _boom, AddReq, AddRsp)

    def _late(q, request):
        d = request.defer()
        threading.Timer(0.05, lambda: d.complete(AddRsp(sum=q.a + q.b))).start()
    srv.function_definition("late", _late, AddReq, AddRsp)

    lvl_def = srv.variable_definition("level", Level, initial=Level(value=5), allow_force=True)

    # remotes on cli, whose service thread delivers while this thread waits
    add_r = cli.remote_function("add", AddReq, AddRsp)
    boom_r = cli.remote_function("boom", AddReq, AddRsp)
    late_r = cli.remote_function("late", AddReq, AddRsp)
    lvl = cli.remote_variable("level", Level)

    deadline = time.time() + 8.0
    while time.time() < deadline and not (add_r.match_count > 0
                                          and boom_r.match_count > 0
                                          and late_r.match_count > 0):
        time.sleep(0.005)
    check("definitions discovered", add_r.match_count > 0 and boom_r.match_count > 0
          and late_r.match_count > 0)

    # blocking calls
    r = add_r.call(AddReq(a=2, b=3), 3.0)
    check("blocking call ok", r.ok and r.status == rant.CallStatus.OK)
    check("blocking call value", r.ok and r.value.sum == 5)
    check("provider set", r.ok and r.provider != 0)

    rb = boom_r.call(AddReq(a=1, b=1), 3.0)
    check("raising handler -> APP_ERROR", rb.status == rant.CallStatus.APP_ERROR)
    threw = False
    try:
        _ = rb.value
    except rant.CallError:
        threw = True
    check(".value on not-ok raises CallError", threw)

    rl = late_r.call(AddReq(a=20, b=22), 3.0)
    check("deferred completion", rl.ok and rl.value.sum == 42)
    check("caller count seen by definition", add.match_count == 1)

    # variable: catch_up hands the remote the initial value
    check("variable wait", lvl.wait(3.0))
    v = lvl.get()
    check("initial value", v is not None and v.value == 5)
    check("owner matched", lvl.match_count > 0)
    check("remote set accepted", lvl.set(Level(value=9)) == rant.SendStatus.OK)
    deadline = time.time() + 5.0
    while time.time() < deadline and not ((v := lvl.get()) and v.value == 9):
        time.sleep(0.005)
    check("set round-trips to the remote", (v := lvl.get()) is not None and v.value == 9)
    check("definition applied it", (v := lvl_def.get()) is not None and v.value == 9)

    # force overrides with a shadow source, unforce restores the latest set
    check("force", lvl_def.force(Level(value=99)) == rant.SendStatus.OK)
    deadline = time.time() + 5.0
    while time.time() < deadline and not ((v := lvl.get()) and v.value == 99):
        time.sleep(0.005)
    check("forced value visible remotely", (v := lvl.get()) is not None and v.value == 99)
    check("remote sees forced", lvl.forced)
    check("unforce", lvl_def.unforce() == rant.SendStatus.OK)
    deadline = time.time() + 5.0
    while time.time() < deadline and not ((v := lvl.get()) and v.value == 9):
        time.sleep(0.005)
    check("unforce restores the latest set",
          (v := lvl.get()) is not None and v.value == 9 and not lvl.forced)

    # variable events: on_change dedups + replays at registration, on_write counts
    # every applied write
    chg, wr, rchg = [], [], []
    lvl.on_change(lambda v: rchg.append(v.value))
    check("remote on_change replays the cache", bool(rchg) and rchg[0] == 9)
    lvl_def.on_change(lambda v, u: chg.append((v.value, u.forced, u.source)))
    check("on_change replays current at registration", len(chg) == 1 and chg[0][0] == 9)
    lvl_def.on_write(lambda v: wr.append(v.value))
    check("on_write does not replay", len(wr) == 0)
    check("identical re-set accepted", lvl_def.set(Level(value=9)) == rant.SendStatus.OK)
    check("identical re-set is a write, not a change", len(chg) == 1 and len(wr) == 1)
    check("new set accepted", lvl_def.set(Level(value=12)) == rant.SendStatus.OK)
    check("change fires inline with the new value",
          len(chg) == 2 and chg[1][0] == 12 and chg[1][2] == 0 and len(wr) == 2)
    deadline = time.time() + 5.0
    while time.time() < deadline and (not rchg or rchg[-1] != 12):
        time.sleep(0.005)
    check("remote change arrives", bool(rchg) and rchg[-1] == 12)
    lvl_def.on_change(None)
    lvl_def.on_write(None)
    lvl.on_change(None)

    # async form: the response fires on cli's service thread
    done = threading.Event()
    async_rsp = []

    def on_rsp(resp):
        async_rsp.append(resp)
        done.set()
    add_r.call_async(AddReq(a=10, b=5), on_rsp)
    check("call_async answered", done.wait(5.0) and async_rsp[0].ok
          and async_rsp[0].value.sum == 15)

    # a blocking call under the service thread sleeps on its progress and answers
    rr = add_r.call(AddReq(a=1, b=2), 2.0)
    check("blocking call answers under service thread", rr.ok and rr.value.sum == 3)

    # a call still pending at close gets exactly one CANCELLED outcome, never hangs
    never = cli.remote_function("never-served", timeout=60.0)
    cancelled = []
    cdone = threading.Event()

    def on_cancel(resp):
        cancelled.append(resp)
        cdone.set()
    never.call_async(None, on_cancel)
    srv.close()
    cli.close()
    check("pending call_async settles CANCELLED at close",
          cdone.wait(2.0) and cancelled[0].status == rant.CallStatus.CANCELLED)
    print("patterns: " + ("PASS\n" if ok else "FAIL\n"))
    return ok


# tasks leg types
@dataclass
class JobReq:
    count: rant.i32 = 0


@dataclass
class JobPrg:
    done: rant.i32 = 0


@dataclass
class JobRsp:
    total: rant.i32 = 0


def tasks():
    """Tasks between two nodes: progress ordering, cancel, no_cancel, blocking."""
    print("tasks leg: two nodes, domain 46, loopback")
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    srv = rant.Node("tsrv", on_event=on_event("tsrv"),
                    domain=DOMAIN + 4, multicast_interface=IFACE, max_topics=32)
    cli = rant.Node("tcli", on_event=on_event("tcli"),
                    domain=DOMAIN + 4, multicast_interface=IFACE, max_topics=32)
    mcli = rant.Node("tmcli", on_event=on_event("tmcli"), domain=DOMAIN + 4,
                     multicast_interface=IFACE, max_topics=32, threading=rant.Threading.MANUAL)
    try:
        # work: streams progress then returns a result
        def _work(req, task):
            total = 0
            for i in range(req.count):
                total += i + 1
                task.progress(JobPrg(done=i + 1))
            return JobRsp(total=total)
        work = srv.task_definition("work", _work, JobReq, JobPrg, JobRsp)

        # grind: runs until cancelled, honors the cancel
        def _grind(req, task):
            task.progress(JobPrg(done=0))
            if not task.cancel_event.wait(8.0) or not task.cancelled:
                return JobRsp(total=-1)   # cancel never arrived: a visible failure
            raise rant.CancelledError("stopped", partial=JobRsp(total=5))
        srv.task_definition("grind", _grind, JobReq, JobPrg, JobRsp)

        # rigid: declares no_cancel, completes regardless with the one argument form
        def _rigid(req):
            time.sleep(0.3)
            return JobRsp(total=7)
        srv.task_definition("rigid", _rigid, JobReq, JobPrg, JobRsp, no_cancel=True)

        # progress and responses fire on cli's service thread
        work_r = cli.remote_task("work", JobReq, JobPrg, JobRsp)
        grind_r = cli.remote_task("grind", JobReq, JobPrg, JobRsp)
        rigid_r = cli.remote_task("rigid", JobReq, JobPrg, JobRsp)

        deadline = time.time() + 8.0
        while time.time() < deadline and not (work_r.match_count > 0
                                              and grind_r.match_count > 0
                                              and rigid_r.match_count > 0):
            time.sleep(0.005)
        check("definitions discovered", work_r.match_count > 0
              and grind_r.match_count > 0 and rigid_r.match_count > 0)

        # async round trip: RUNNING ack first, then the values in order, terminal OK
        prog, rsps = [], []
        done = threading.Event()

        def on_prog(v):   # 1-arg form: v is the decoded value, None for the ack
            prog.append(None if v is None else v.done)

        def on_rsp(r):
            rsps.append(r)
            done.set()
        call_id = work_r.call_async(JobReq(count=3), on_rsp, on_progress=on_prog)
        check("call id assigned", call_id != 0)
        check("async task answered", done.wait(5.0))
        check("terminal OK with the decoded result",
              rsps and rsps[0].ok and rsps[0].value.total == 6)
        check("RUNNING ack first, then values in order", prog == [None, 1, 2, 3])

        # cancel honored: the handler observes it and raises CancelledError
        ginfo, grsps = [], []
        grunning, gdone = threading.Event(), threading.Event()

        def gprog(v, p):   # 2-arg form: p is a Progress
            ginfo.append((None if v is None else v.done, p.call_id, p.provider))
            grunning.set()

        def grsp(r):
            grsps.append(r)
            gdone.set()
        gid = grind_r.call_async(JobReq(count=1), grsp, on_progress=gprog)
        check("grind running", grunning.wait(5.0))
        check("cancel accepted", grind_r.cancel(gid) == rant.SendStatus.OK)
        check("terminal CANCELLED with the handler's message",
              gdone.wait(5.0) and grsps[0].status == rant.CallStatus.CANCELLED
              and grsps[0].message == "stopped")
        check("a cancelled task carries its partial result",
              bool(grsps) and grsps[0].has_value and grsps[0].value.total == 5)
        check("progress carries the call id and provider",
              bool(ginfo) and all(c == gid and p != 0 for _, c, p in ginfo))

        # no_cancel: cancel refused locally, the task completes anyway
        nrsps = []
        nrunning, ndone = threading.Event(), threading.Event()
        nid = rigid_r.call_async(JobReq(count=1),
                                 lambda r: (nrsps.append(r), ndone.set()),
                                 on_progress=lambda v: nrunning.set())
        check("rigid running", nrunning.wait(5.0))
        check("cancel refused (no_cancel)",
              rigid_r.cancel(nid) == rant.SendStatus.BAD_ROLE)
        check("rigid completes anyway",
              ndone.wait(5.0) and nrsps[0].ok and nrsps[0].value.total == 7)

        # a MANUAL node's blocking call drives its loop, so progress fires on the calling thread
        mwork_r = mcli.remote_task("work", JobReq, JobPrg, JobRsp)
        deadline = time.time() + 8.0
        while time.time() < deadline and mwork_r.match_count == 0:
            mcli.poll(0.005)
        bprog, bthread = [], []

        def bprg(v):
            bprog.append(None if v is None else v.done)
            bthread.append(threading.current_thread() is threading.main_thread())
        br = mwork_r.call(JobReq(count=2), on_progress=bprg, timeout=3.0)
        check("blocking task ok", br.ok and br.value.total == 3)
        check("blocking progress on the calling thread, in order",
              bprog == [None, 1, 2] and all(bthread))

        check("caller count seen by the definition", work.match_count == 2)

        # every handle reports the schemas it uses, a reflect remote what it adopted
        def hash_of(s):
            return s.hash if s is not None else None
        req_h, prg_h = hash_of(work.request_schema), hash_of(work.progress_schema)
        check("a definition reports its schemas",
              req_h is not None and prg_h is not None and work.response_schema is not None)
        check("a typed remote reports the same", hash_of(work_r.request_schema) == req_h)
        work_r.close()   # one remote per name on a node
        mesh_r = cli.remote_task("work", reflect_from_mesh=True)
        deadline = time.time() + 8.0
        while time.time() < deadline and mesh_r.request_schema is None:
            mesh_r.refresh()
            time.sleep(0.02)
        check("a reflect remote reports the schemas it adopted",
              req_h is not None and hash_of(mesh_r.request_schema) == req_h
              and hash_of(mesh_r.progress_schema) == prg_h)

        try:
            srv.function_definition("nohandler", None, JobReq, JobRsp)
            refused = False
        except ValueError:
            refused = True
        check("a definition without a handler is refused", refused)

        # on_event rebinds after open: a node joining shows up at the new handler
        ups = threading.Event()
        cli.on_event(lambda e: ups.set() if e.kind == rant.EventKind.PEER_UP else None)
        late = rant.Node("tlate", domain=DOMAIN + 4, multicast_interface=IFACE)
        check("on_event set after open receives events", ups.wait(5.0))
        late.close()

        # a payload that does not decode is an event naming the topic, never a silent drop
        errs = []
        cli.on_event(lambda e: errs.append(e) if e.is_error else None)
        cli._report_decode("t/undecodable", "a message did not decode")  # pyright: ignore[reportAttributeAccessIssue]
        check("a decode failure is a SCHEMA_MISMATCH event naming the topic",
              bool(errs) and errs[0].error == rant.ErrorKind.SCHEMA_MISMATCH
              and errs[0].topic_name == "t/undecodable")
    finally:
        srv.close()
        cli.close()
        mcli.close()
    print("tasks: " + ("PASS\n" if ok else "FAIL\n"))
    return ok


got = threading.Event()
received = []


def dispatch_leg():
    """Threading.DISPATCH: every handler, response, log line and event runs on the thread
        that calls dispatch(), and a queue of its own keeps a handle's callbacks apart."""
    print("dispatch leg: two nodes, domain 55, loopback")
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    me = threading.get_ident()
    seen = {}

    def mark(what):
        seen.setdefault(what, threading.get_ident())

    srv = rant.Node("dsrv", on_event=lambda e: mark("event"), domain=DOMAIN + 13,
                    multicast_interface=IFACE, max_topics=32, threading=rant.Threading.DISPATCH)
    cli = rant.Node("dcli", on_event=lambda e: None, domain=DOMAIN + 13, multicast_interface=IFACE,
                    max_topics=32, threading=rant.Threading.DISPATCH)

    def drain():
        srv.dispatch()
        cli.dispatch()

    def wait(cond, secs=8.0):
        deadline = time.time() + secs
        while time.time() < deadline and not cond():
            drain()
            time.sleep(0.005)
        return cond()

    def add(q):
        mark("function")
        return AddRsp(sum=q.a + q.b)
    srv.function_definition("dadd", add, AddReq, AddRsp)
    lvl_def = srv.variable_definition("dlevel", Level, initial=Level(value=1))
    got = []
    srv.subscriber("dtopic", Level, lambda v: (mark("subscriber"), got.append(v.value)))

    add_r = cli.remote_function("dadd", AddReq, AddRsp)
    lvl = cli.remote_variable("dlevel", Level)
    pub = cli.publisher("dtopic", Level, reliable=True)
    check("everything matched",
          wait(lambda: add_r.match_count > 0 and lvl.match_count > 0 and pub.match_count > 0))

    rsp = []
    add_r.call_async(AddReq(a=2, b=3), lambda r: (mark("response"), rsp.append(r)))
    check("the dispatched handler answered", wait(lambda: bool(rsp)) and rsp[0].value.sum == 5)

    changes = []
    lvl.on_change(lambda v: (mark("change"), changes.append(v.value)))
    lvl_def.set(Level(value=9))
    check("the variable change arrived", wait(lambda: 9 in changes))

    pub.send(Level(value=4))
    check("the subscriber got the message", wait(lambda: 4 in got))

    lines = []
    cli.on_log(lambda line: (mark("log"), lines.append(line.text)))
    srv.log(rant.Level.INFO, "dispatched line")
    check("the log line arrived", wait(lambda: "dispatched line" in lines))
    check("the event arrived", wait(lambda: "event" in seen))

    stray = sorted(k for k, v in seen.items() if v != me)
    check("every callback ran on the dispatch thread (%s)" % (", ".join(stray) or "all"),
          not stray and len(seen) == 6)

    # a queue of its own: the handler waits for that queue alone
    q = srv.create_queue()
    own = []
    srv.subscriber("down", Level, lambda v: own.append(v.value), queue=q)
    pub2 = cli.publisher("down", Level, reliable=True)
    check("the second topic matched", wait(lambda: pub2.match_count > 0))
    pub2.send(Level(value=7))
    check("the queue holds it until its own dispatch",
          wait(lambda: q.stats[0] == 1) and not own)
    check("the queue's dispatch runs it", q.dispatch() == 1 and own == [7])
    try:
        srv.publisher("down", Level)
        shared = True
    except rant.Error:
        shared = False
    check("a same name handle on another queue is refused", not shared)
    try:
        cli.subscriber("x", Level, lambda v: None, queue=q)
        foreign = True
    except ValueError:
        foreign = False
    check("another node's queue is refused", not foreign)

    # pull: no handler, the app takes when it wants
    pulled = srv.subscriber("pulled", Level, reliable=True, keep_last=8)
    pub3 = cli.publisher("pulled", Level, reliable=True)
    check("the pull topic matched", wait(lambda: pub3.match_count > 0))
    for v in (1, 2, 3):
        pub3.send(Level(value=v))
    first = pulled.take(timeout=2.0)
    check("take returns the oldest", first is not None and first.value == 1)
    # the taken record stays counted until the next take, so 3 means 2 and 3 have arrived
    check("the rest arrived", wait(lambda: pulled.queue_stats.messages >= 3))
    newest = pulled.latest()
    check("latest returns the newest and drops the rest",
          newest is not None and newest.value == 3 and pulled.take() is None)
    try:
        srv.subscriber("down", Level, lambda v: None, queue=q).take()
        refused = False
    except rant.Error:
        refused = True
    check("take on a handler subscriber raises", refused)
    try:
        srv.subscriber("pq", Level, queue=q)
        refused = False
    except ValueError:
        refused = True
    check("a pulled subscriber refuses a queue", refused)

    plain = rant.Node("dplain", on_event=lambda e: None, domain=DOMAIN + 13, multicast_interface=IFACE)
    try:
        plain.dispatch()
        raised = False
    except rant.Error:
        raised = True
    check("dispatch() needs Threading.DISPATCH", raised)
    quiet = rant.Node("dquiet", on_event=lambda e: None, domain=DOMAIN + 13, multicast_interface=IFACE,
                      disable_logs=True)
    try:
        quiet.on_log(lambda line: None)
        raised = False
    except rant.Error:
        raised = True
    check("on_log on a node without logs raises", raised)
    for n in (quiet, plain, cli, srv):
        n.close()
    print("dispatch: PASS\n" if ok else "dispatch: FAIL\n")
    return ok


def on_pose(pose, m):
    received.append(pose)
    print("recv: [%s] from %s -> %r" % (m.topic_name, m.publisher_name, pose))
    got.set()


def on_event(tag):
    return lambda e: print("event(%s):" % tag, e)


def array_buffers():
    """A numeric array field takes any buffer: copied at once when its element type matches,
    converted element by element otherwise. Two nodes, domain +7, loopback."""
    print("array buffers leg: two nodes, loopback")
    import array
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    try:
        import numpy as np
    except ImportError:
        np = None
    dsl = ("Bufs { a_u8: u8[], a_u16: u16[], a_u32: u32[], a_u64: u64[], a_i8: i8[], "
           "a_i16: i16[], a_i32: i32[], a_i64: i64[], a_f32: f32[], a_f64: f64[], "
           "a_bool: bool[], fx: f32[3], img: Image }")
    a = rant.Node("BufA", on_event=on_event("BufA"), domain=DOMAIN + 7, multicast_interface=IFACE,
                  threading=rant.Threading.MANUAL)
    b = rant.Node("BufB", on_event=on_event("BufB"), domain=DOMAIN + 7, multicast_interface=IFACE,
                  threading=rant.Threading.MANUAL)
    got = []
    try:
        pub = a.publisher("bufs", dsl, reliable=True, keep_last=8)
        b.subscriber("bufs", dsl, got.append, reliable=True, keep_last=8)
        deadline = time.time() + 8.0
        while time.time() < deadline and pub.match_count == 0:
            a.poll(0.001)
            b.poll(0.001)
        check("pair matched", pub.match_count == 1)

        def trip(**fields):
            got.clear()
            pub.send(fields)
            deadline = time.time() + 5.0
            while time.time() < deadline and not got:
                a.poll(0.001)
                b.poll(0.001)
            return got[0] if got else None

        def f32(x):
            return struct.unpack("<f", struct.pack("<f", x))[0]

        for code, name in (("B", "a_u8"), ("H", "a_u16"), ("I", "a_u32"), ("Q", "a_u64"),
                           ("b", "a_i8"), ("h", "a_i16"), ("i", "a_i32"), ("q", "a_i64")):
            src = [0, 1, 100]
            r = trip(**{name: array.array(code, src)})
            check("array.array(%r) for %s" % (code, name), r is not None and list(r[name]) == src)
        r = trip(a_f32=array.array("f", [1.5, -2.25]), a_f64=array.array("d", [0.1, -3.5e100]))
        check("array.array f and d", r is not None and list(r["a_f32"]) == [1.5, -2.25]
              and list(r["a_f64"]) == [0.1, -3.5e100])
        r = trip(a_f32=array.array("d", [0.1, 2.5]))
        check("array.array d for an f32 field converts",
              r is not None and list(r["a_f32"]) == [f32(0.1), 2.5])
        r = trip(a_f32=memoryview(array.array("d", [0.1, 2.5])))
        check("a memoryview of doubles for an f32 field is not raw bytes",
              r is not None and list(r["a_f32"]) == [f32(0.1), 2.5])
        r = trip(a_f64=array.array("i", [-7, 9]))
        check("array.array i for an f64 field converts",
              r is not None and list(r["a_f64"]) == [-7.0, 9.0])
        r = trip(a_i32=array.array("q", [-5, 1 << 20]))
        check("array.array q for an i32 field converts",
              r is not None and list(r["a_i32"]) == [-5, 1 << 20])
        r = trip(a_u8=memoryview(b"\x01\x02\x03"), a_f32=memoryview(array.array("f", [4.5])).cast("B").cast("f"))
        check("memoryview of bytes for u8, cast to f for f32",
              r is not None and bytes(r["a_u8"]) == b"\x01\x02\x03" and list(r["a_f32"]) == [4.5])
        r = trip(a_f32=struct.pack("<2f", 1.0, 2.0))
        check("bytes for an f32 field stay raw", r is not None and list(r["a_f32"]) == [1.0, 2.0])
        try:
            trip(a_u8=array.array("b", [-1]))
            check("a signed buffer for a u8 field raises on a negative", False)
        except (struct.error, rant.Error):
            check("a signed buffer for a u8 field raises on a negative", True)
        r = trip(a_u8=array.array("b", [1, 2]))
        check("a signed buffer for a u8 field converts in range",
              r is not None and bytes(r["a_u8"]) == b"\x01\x02")

        if np is None:
            print("  numpy missing, numpy cases skipped")
        else:
            pic = np.arange(4 * 5 * 3, dtype=np.uint8).reshape(4, 5, 3)
            r = trip(img=rant.types.Image(width=5, height=4, stride=15,
                                          format=rant.types.ImageFormat.Rgb8, data=pic))
            check("a 3 dimensional uint8 image packs flat",
                  r is not None and bytes(r["img"]["data"]) == pic.tobytes())
            r = trip(a_f32=np.array([1.5, 2.5], dtype=np.float32))
            check("float32 array for f32", r is not None and list(r["a_f32"]) == [1.5, 2.5])
            r = trip(a_f32=np.array([0.1, 2.5], dtype=np.float64))
            check("float64 array for an f32 field converts",
                  r is not None and list(r["a_f32"]) == [f32(0.1), 2.5])
            r = trip(a_i16=np.arange(10, dtype=np.int16)[::2])
            check("a non contiguous slice", r is not None and list(r["a_i16"]) == [0, 2, 4, 6, 8])
            r = trip(a_f32=np.array([1.5, -2.5, 0.1], dtype=">f4"))
            check("a big endian float32 array converts",
                  r is not None and list(r["a_f32"]) == [1.5, -2.5, f32(0.1)])
            r = trip(a_u32=np.array([1, 0x01020304], dtype=">u4"))
            check("a big endian uint32 array converts",
                  r is not None and list(r["a_u32"]) == [1, 0x01020304])
            r = trip(a_bool=np.array([True, False, True]))
            check("a numpy bool array for a bool field",
                  r is not None and [bool(x) for x in r["a_bool"]] == [True, False, True])
            r = trip(a_i64=np.array([-1, 5], dtype=np.int64), a_u16=np.array([7], dtype=np.uint16))
            check("int64 and uint16 arrays match their fields",
                  r is not None and list(r["a_i64"]) == [-1, 5] and list(r["a_u16"]) == [7])
            r = trip(fx=np.array([1, 2, 3], dtype=np.float64))
            check("a fixed f32 field converts a float64 array",
                  r is not None and list(r["fx"]) == [1.0, 2.0, 3.0])
    finally:
        a.close()
        b.close()
    print("array buffers: " + ("PASS\n" if ok else "FAIL\n"))
    return ok


def log_status():
    """A line logged before the subscribe replays with its level and origin and one below the
    level never arrives. A status raised before anyone listens reaches a late binder whole, a
    peer clear of a clearable condition lands and A's app hears it, and a node that went away
    reads as empty."""
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    def until(seconds, done):
        deadline = time.time() + seconds
        while time.time() < deadline and not done():
            time.sleep(0.005)
        return done()

    cleared = []
    opts = dict(domain=DOMAIN + 17, multicast_interface=IFACE)
    a = rant.Node("lsa", on_event=lambda e: cleared.append(e.detail)
                  if e.kind == rant.EventKind.STATUS_CLEARED else None, **opts)
    b = rant.Node("lsb", on_event=lambda e: None, **opts)
    try:
        a.log(rant.Level.WARN, "before")
        a.log(rant.Level.DEBUG, "hidden")
        lines = []
        b.on_log(lambda line: lines.append(line) if line.node == "lsa" else None)

        def has(text, level):
            return any(x.text == text and x.level == level and x.origin == rant.Origin.APP and x.wall_us
                       for x in lines)
        check("a line logged before the subscribe replays", until(8, lambda: has("before", rant.Level.WARN)))
        a.log(rant.Level.ERROR, "live")
        check("a live line arrives with its level and origin", until(8, lambda: has("live", rant.Level.ERROR)))
        check("a line below the level never arrives", not any(x.text == "hidden" for x in lines))

        check("raised before anyone listens",
              a.set_status("estop", rant.Level.ERROR, "pressed") == rant.SendStatus.OK
              and a.set_status("hot", rant.Level.WARN, "motor hot", clearable=True) == rant.SendStatus.OK)
        check("a long key is refused", a.set_status("k" * 49, rant.Level.INFO, "x") == rant.SendStatus.TOO_BIG)
        seen = []
        b.on_status(lambda u: seen.append(u) if u.node == "lsa" else None)

        def count():
            return len(seen[-1].conditions) if seen else -1
        check("the late binder gets the whole set", until(8, lambda: count() == 2))
        c = seen[-1].conditions if seen else []
        check("the set decodes", len(c) == 2 and c[0].key == "estop" and c[0].level == rant.Level.ERROR
              and not c[0].clearable and c[1].key == "hot" and c[1].text == "motor hot" and c[1].clearable
              and c[1].since_us > 0 and c[1].origin == rant.Origin.APP)
        if len(c) == 2:
            check("a peer clear is sent",
                  b.clear_peer_status(seen[-1].node_id, "hot", c[1].since_us) == rant.SendStatus.OK)
        check("A's app hears which was cleared", until(8, lambda: cleared == ["hot"]))
        check("B sees the smaller set", until(8, lambda: count() == 1))
        check("an own clear", a.clear_status("estop") == rant.SendStatus.OK)
        check("B sees it empty", until(8, lambda: count() == 0))
        a.set_status("estop", rant.Level.ERROR, "pressed again")
        check("raised again", until(8, lambda: count() == 1))
        before = len(seen)
        a.close()
        check("a node that went away reads as empty",
              until(8, lambda: len(seen) > before and count() == 0))

        try:
            rant.Node("lbad", on_event=lambda e: None, log_level=9, **opts)
            raised = False
        except rant.Error:
            raised = True
        check("a bad log_level refuses the open", raised)
        quiet = rant.Node("lnone", on_event=lambda e: None, disable_status=True, **opts)
        try:
            quiet.on_status(lambda u: None)
            raised = False
        except rant.Error:
            raised = True
        check("on_status on a node without status raises", raised)
        check("set_status on a node without status is NOSYS",
              quiet.set_status("k", rant.Level.INFO, "x") == rant.SendStatus.NOSYS)
        quiet.close()
    finally:
        a.close()
        b.close()
    print("log_status: PASS\n" if ok else "log_status: FAIL\n")
    return ok


def version_leg():
    with open(os.path.join(_HERE, "..", "..", "VERSION")) as f:
        want = f.read().strip()
    ok = rant.version() == want
    print("version: " + rant.version() + (" PASS\n" if ok else " FAIL, VERSION says " + want + "\n"))
    return ok


def layout_leg():
    """Every ctypes mirror against the C layout the build writes: a stale mirror crashes
    every node open, so a size or offset that drifted fails here first."""
    path = os.environ.get("RANT_LAYOUT") or os.path.join(_HERE, "..", "..", "build", "tests", "layout.txt")
    if not os.path.exists(path):
        print("FAIL: no C layout at %s, build first" % path)
        return False
    layout = {}
    with open(path) as f:
        for line in f:
            key, value = line.split()
            layout[key] = int(value)
    from rant import _native
    ok = True
    for name, cls in inspect.getmembers(_native, inspect.isclass):
        if not issubclass(cls, ctypes.Structure) or cls is ctypes.Structure:
            continue
        c_name = {"RantStringView": "RantString"}.get(name, name)
        if c_name not in layout:
            print("FAIL: %s has no C layout, add it to tests/support/layout.c" % name)
            ok = False
            continue
        if ctypes.sizeof(cls) != layout[c_name]:
            print("FAIL: %s is %d bytes, C says %d" % (name, ctypes.sizeof(cls), layout[c_name]))
            ok = False
        for field_name, *_ in cls._fields_:
            key = c_name + "." + field_name.rstrip("_")
            offset = getattr(cls, field_name).offset
            if layout.get(key) != offset:
                print("FAIL: %s.%s at %d, C says %s" % (name, field_name, offset, layout.get(key)))
                ok = False
    print("layout: " + ("PASS\n" if ok else "FAIL\n"))
    return ok


def pose_leg():
    print("opening nodes...")
    sub = rant.Node("sub", on_event=on_event("sub"),
                    domain=DOMAIN, multicast_interface=IFACE, threading=rant.Threading.MANUAL)
    pub = rant.Node("pub", on_event=on_event("pub"),
                    domain=DOMAIN, multicast_interface=IFACE, threading=rant.Threading.MANUAL)

    subch = sub.subscriber("pose", Reading, on_pose, reliable=True, keep_last=8)
    pubch = pub.publisher("pose", Reading, reliable=True, keep_last=8)

    sent = Reading(stamp=7, x=1.5, y=-2.5, uuid=b"\x01\x02\x03\x04",
                frame="map", tags=["fast", "ok"], vel=Velocity(dx=0.5, dy=0.25))

    # MANUAL: drive both nodes by polling them in the loop.
    deadline = time.time() + 8.0
    while time.time() < deadline and not got.is_set():
        pubch.send(sent)
        pub.poll(0.001)
        sub.poll(0.001)

    ok = got.is_set()
    if ok:
        r = received[0]
        ok = (isinstance(r, Reading) and r.stamp == 7 and abs(r.x - 1.5) < 1e-9
              and abs(r.y + 2.5) < 1e-9 and bytes(r.uuid) == b"\x01\x02\x03\x04"
              and r.frame == "map" and list(r.tags) == ["fast", "ok"]
              and abs(r.vel.dx - 0.5) < 1e-6 and abs(r.vel.dy - 0.25) < 1e-6)
        print("PASS" if ok else "FAIL: decoded value mismatch: %r" % (r,))
        print("Reading DSL (for C interop):\n" + rant.dsl(Reading))
    else:
        print("FAIL: no message delivered within timeout")

    # An over-cap string must raise, never silently truncate.
    if ok:
        try:
            pubch.send(Reading(frame="way-too-long-for-sixteen-bytes"))
            print("FAIL: over-cap string did not raise")
            ok = False
        except rant.SchemaError:
            print("PASS: over-cap string refused")

    pub.close()
    sub.close()

    # A handle outliving its node answers NO_TOPIC instead of touching freed memory.
    if ok:
        ok = (pubch.send(sent) == rant.SendStatus.NO_TOPIC and pubch.match_count == 0
              and subch.queue_stats.messages == 0 and pubch.name == "pose" and pub.name == "pub")
        print("PASS: handle after close" if ok else "FAIL: handle after close")

    return ok


LEGS = [
    ("version", version_leg),
    ("layout", layout_leg),
    ("round_trip", round_trip),
    ("value_roots", value_roots),
    ("std_types", std_types),
    ("struct_arrays", struct_arrays),
    ("nested", nested),
    ("pose", pose_leg),
    ("lifecycle", lifecycle),
    ("handles", handles),
    ("value_roots_live", value_roots_live),
    ("struct_arrays_live", struct_arrays_live),
    ("video_types", video_types),
    ("patterns", patterns),
    ("tasks", tasks),
    ("dispatch", dispatch_leg),
    ("array_buffers", array_buffers),
    ("log_status", log_status),
    ("rebind", rebind),
    ("close_race", close_race),
    ("handle_gate", handle_gate),
]


def main(argv):
    """Every leg runs, so one failure never hides the rest. A raise fails that leg."""
    names = [name for name, _ in LEGS]
    if argv == ["--list"]:
        print("\n".join(names))
        return 0
    unknown = [a for a in argv if a not in names]
    if unknown:
        print("no leg named %s, --list lists them" % unknown[0], file=sys.stderr)
        return 2
    failed = []
    for name, leg in LEGS:
        if argv and name not in argv:
            continue
        print("-- " + name)
        try:
            ok = leg()
        except Exception as e:   # noqa: BLE001 a crash fails the leg, the rest still run
            print("FAIL: %s raised %r" % (name, e))
            ok = False
        if not ok:
            failed.append(name)
    print("RESULT: " + ("FAIL in " + " ".join(failed) if failed else "PASS"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
