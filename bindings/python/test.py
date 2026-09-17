"""The Python binding test: two nodes on one host over reliable typed pub sub, the
patterns and the standard types. Exit 0 = pass (spec/testing.md)."""
import enum as _enum
import os
import subprocess
import sys
import threading
import time
import weakref
from dataclasses import dataclass, field
from typing import Annotated

# Import the package from the source checkout, which finds the library in dist/native.
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
import rant    # noqa: E402

DOMAIN = 42
IFACE = "127.0.0.1"   # pin discovery to loopback for a single-host run


# any annotated class is a schema, @dataclass just gives a handy constructor
@dataclass
class Twist:
    dx: rant.f32 = 0.0
    dy: rant.f32 = 0.0


@dataclass
class Pose:
    stamp: rant.u64 = 0
    x:     rant.f64 = 0.0
    y:     rant.f64 = 0.0
    uuid:  Annotated[bytes, "u8[4]"] = b""
    frame: Annotated[str, "string<16>"] = ""             # a capped UTF-8 string
    tags:  Annotated[list[str], "string<8>[2]"] = field(default_factory=list)  # a fixed array of capped strings
    vel:   Twist = field(default_factory=Twist)


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


def round_trip():
    """Encode and decode round trip of the variable kinds, no networking."""
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    sch = rant.Schema(Sensor)
    print("Sensor DSL:\n" + sch.dsl)
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
    print("variable-kinds round-trip: " + ("PASS\n" if ok else "FAIL\n"))
    return ok


# The canonical wire of a bare type is its kind alone, so these hashes are the same in
# every language binding (pinned in C by rant_test's schema-root phase).
HASH_BOOL = 0xEE90234F61D2520B
HASH_F32ARR = 0x314844E3386A1FC4


class Mode(_enum.IntEnum):
    IDLE = 0
    RUN = 1
    FAULT = 2


# The Float3 schema is the shared cross-language golden vector: the same wire and the
# same hash from C, C++, C# and Python (pinned in C by rant_test's stdtypes phase).
HASH_FLOAT3 = 0x04AA9469CD08B1DD


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

    f3 = rant.Schema("Float3")
    check("Float3 compiles by name alone, golden hash", f3.hash == HASH_FLOAT3)
    check("Float3 is 12 message bytes", len(f3.encode({})) == 12)
    check("the mirror dataclass IS that type", rant.Schema(rant.types.Float3).hash == HASH_FLOAT3)

    # a name narrows: an anonymous field of the same shape reads a Transform field, never
    # the reverse, and Transform/Twist are distinct names never mistaken for each other
    named = rant.Schema("W { at: Transform }")
    bare = rant.Schema("W { at: { translation: { x: f64, y: f64, z: f64 },"
                       "         rotation: { x: f64, y: f64, z: f64, w: f64 },"
                       "         parent: string<30> } }")
    check("an anonymous field of the same shape reads a Transform field",
          bare.can_read(named) and not named.can_read(bare))
    check("Transform and Twist never cross-wire",
          not rant.Schema("Twist").can_read(rant.Schema("Transform")))

    sch = rant.Schema(Track)
    text = " ".join(rant.dsl(Track).split())
    check("the reflected schema spells the names, not the shapes",
          text == "Track { at: Transform, when: Timestamp, tag: Color, id: Uuid, velocity: Float3 }")
    check("its message is the sum of the wire shapes (88+8+4+16+12)",
          len(sch.encode(Track())) == 128)

    t = Track(at=rant.types.Transform(translation=rant.types.Double3(4.5, -1.25, 9.0)),
              when=rant.types.now(), tag=rant.types.Color(0x11, 0x22, 0x33, 0xFF),
              id=bytes(range(16)), velocity=rant.types.Float3(1.0, 2.0, 3.0))
    back = sch.decode(sch.encode(t))
    check("a Track round-trips whole",
          back.at.translation.x == 4.5 and back.at.rotation.w == 1.0
          and back.when == t.when and back.tag.r == 0x11 and back.tag.a == 0xFF
          and bytes(back.id) == bytes(range(16)) and back.velocity.z == 3.0)
    check("types.now is Unix-epoch microseconds", rant.types.now() > 1600000000000000)
    return ok


# The video family: golden wire shared with the C, C++ and C# bindings (pinned in
# bindings/cpp/test.cpp and by rant_test's stdtypes phase).
HASH_IMAGE = 0x489841F99F392B85
HASH_VIDEO_FRAME = 0xF677BD147B513FBC
HASH_EXT_STREAM = 0xAAE502077016AC13


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

    for name, cls, golden in (("Image", rant.types.Image, HASH_IMAGE),
                              ("VideoFrame", rant.types.VideoFrame, HASH_VIDEO_FRAME),
                              ("ExternalVideoStream", rant.types.ExternalVideoStream,
                               HASH_EXT_STREAM)):
        check("%s compiles by name alone, golden hash" % name,
              rant.Schema(name).hash == golden)
        check("the %s mirror IS that type" % name, rant.Schema(cls).hash == golden)
    check("a video mirror nests as a named field",
          rant.Schema(Clip).hash
          == rant.Schema("Clip { cover: Image, live: ExternalVideoStream }").hash)

    got = {}
    a = rant.Node("VidA", on_event=on_event("VidA"), domain=45, multicast_interface=IFACE,
                  threading=rant.Threading.MANUAL)
    b = rant.Node("VidB", on_event=on_event("VidB"), domain=45, multicast_interface=IFACE,
                  threading=rant.Threading.MANUAL)
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


def value_roots():
    """Bare types as whole schemas: encode/decode plain values, and the canonical hash."""
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    check("bool canonical hash", rant.Schema(bool).hash == HASH_BOOL)
    check("f32[] canonical hash", rant.Schema(list[rant.f32]).hash == HASH_F32ARR)
    check("bool dsl", rant.dsl(bool) == "bool\n")
    check("f32[] dsl", rant.dsl(list[rant.f32]) == "f32[]\n")
    check("string(16) dsl", rant.dsl(rant.string(16)) == "string<16>\n")
    check("bare root reflects as one anonymous field",
          len(rant.Schema(rant.f64).fields) == 1
          and rant.Schema(rant.f64).name == ""
          and rant.Schema(rant.f64).fields[0].name == "")
    check("enum variants read back",
          rant.Schema(Mode).enum_variants(0) == [("IDLE", 0), ("RUN", 1), ("FAULT", 2)])
    for src, value in ((bool, True), (rant.u8, 200), (rant.i32, -7),
                       (rant.f32, 1.5), (rant.f64, -2.25), (int, 5), (float, 0.5),
                       (bool, False), (rant.string(16), "capped"), (str, "unbounded"),
                       (list[rant.f32], [1.5, -2.5]), (dict, {"battery": 87}),
                       ("u8[4]", b"\x01\x02\x03\x04"), (Mode, Mode.FAULT)):
        sch = rant.Schema(src)
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
    # text DSL and the bare type agree, and a named bare root is an error
    check("text `bool` == plain bool", rant.Schema("bool").hash == HASH_BOOL)
    try:
        rant.Schema("Temperature: f32")
        check("named bare root refused", False)
    except rant.SchemaError:
        check("named bare root refused", True)
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

    watcher = rant.Node("watcher", on_event=watch, domain=47, multicast_interface=IFACE)
    try:
        check("service thread by default", watcher.threading == rant.Threading.SERVICE_THREAD)
        check("poll refused under the service thread",
              watcher.poll(0) == rant.SendStatus.STATE)

        got = threading.Event()
        watcher.subscriber("twist", Twist, lambda t: got.set(), reliable=True)

        def open_and_drop():
            n = rant.Node("dropped", on_event=lambda e: None, domain=47,
                          multicast_interface=IFACE)
            p = n.publisher("twist", Twist, reliable=True)
            deadline = time.time() + 8.0
            while time.time() < deadline and p.match_count == 0:
                time.sleep(0.005)
            p.send(Twist(dx=1.0))
            check("threaded delivery with no poll", got.wait(3.0))
            return weakref.ref(n)
        ref = open_and_drop()
        check("the dropped node is collected", ref() is None)
        check("the dropped node said bye", saw(rant.EventKind.PEER_DOWN, "dropped", 3.0))

        child = ("import sys, time; sys.path.insert(0, %r); import rant; "
                 "n = rant.Node('exiting', on_event=lambda e: None, domain=47, "
                 "multicast_interface=%r); "
                 "n.subscriber('flag', bool, lambda v: None); time.sleep(1.5)"
                 % (_HERE, IFACE))
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
    a = rant.Node("HA", on_event=lambda e: None, domain=48, multicast_interface=IFACE,
                  fetch_details=True)
    b = rant.Node("HB", on_event=lambda e: None, domain=48, multicast_interface=IFACE)
    try:
        p = a.publisher("shared", Twist, reliable=True)
        s = a.subscriber("shared", Twist, lambda t: None, reliable=True)
        try:
            a.publisher("shared", Pose)
            check("a different schema on a live name is refused", False)
        except rant.Error:
            check("a different schema on a live name is refused", True)
        b.subscriber("shared", Twist, lambda t: got.append(t), reliable=True)
        fn = a.function_definition("double", lambda q: Twist(dx=q.dx * 2), Twist, Twist)
        deadline = time.time() + 8.0
        while time.time() < deadline and p.match_count == 0:
            time.sleep(0.005)
        check("publisher matched the peer's subscriber", p.match_count == 1)
        check("the sibling subscriber closes", s.close())
        check("close is idempotent", s.close())
        check("the publisher still sends", p.send(Twist(dx=3.0)) == rant.SendStatus.OK)
        deadline = time.time() + 5.0
        while time.time() < deadline and not got:
            time.sleep(0.005)
        check("and the peer still receives", bool(got) and abs(got[0].dx - 3.0) < 1e-6)
        check("counts show the send", p.counts.tx_msgs >= 1)
        check("the last handle closes", p.close())
        check("a closed handle answers NO_TOPIC", p.send(Twist()) == rant.SendStatus.NO_TOPIC)
        p2 = a.publisher("shared", Pose)
        check("the name carries another schema after the last close", p2.name == "shared")

        # reflection: the peer, this node's entities, the folded mesh and one lookup
        peers = a.reflection.peers()
        check("the peer is listed active",
              any(x.name == "HB" and x.active for x in peers))
        mine = a.reflection.entities()
        check("this node's entities carry the function",
              any(e.kind == rant.EntityKind.FUNCTION and e.name == "double" and e.provides
                  for e in mine))
        found = a.reflection.find(rant.EntityKind.TOPIC, "shared")
        check("find folds the topic", found is not None and found.kind == rant.EntityKind.TOPIC)
        check("the folded schema is an owned copy",
              found is not None and found.schema is not None
              and found.schema.hash == rant.Schema(Pose).hash)
        check("mesh lists what find found",
              any(e.name == "shared" for e in a.reflection.mesh()))
        check("epoch is a counter", isinstance(a.reflection.epoch, int))
        check("the function definition closes", fn.close())

        check("log writes", a.log(rant.LogLevel.INFO, "hello") == rant.SendStatus.OK)
        handler = lambda line: None
        check("on_log binds and returns the handler", a.on_log(handler) is handler)
        st = a.stats
        check("stats is one snapshot", st.mem_in_use > 0 and st.alloc_calls > 0)
        # the Pose publisher against HB's Twist subscriber is the mismatch it records, once
        # HB's interest has come back
        deadline = time.time() + 5.0
        while time.time() < deadline and a.last_error.error == rant.ErrorKind.NONE:
            time.sleep(0.005)
        check("last_error is the schema mismatch",
              a.last_error.error == rant.ErrorKind.SCHEMA_MISMATCH)
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
    a = rant.Node("VA", on_event=lambda e: None, domain=44, multicast_interface=IFACE,
                  threading=rant.Threading.MANUAL)
    b = rant.Node("VB", on_event=lambda e: None, domain=44, multicast_interface=IFACE,
                  threading=rant.Threading.MANUAL)
    try:
        pub_flag = a.publisher("flag", bool, reliable=True, keep_last=4)
        sub_flag = b.subscriber("flag", bool, lambda v, m: got.setdefault(m.topic_name, v),
                                reliable=True, keep_last=4)
        pub_note = a.publisher("note", str, reliable=True, keep_last=4)
        sub_note = b.subscriber("note", str, lambda v, m: got.setdefault(m.topic_name, v),
                                reliable=True, keep_last=4)
        vd = a.variable_definition("gain", rant.f64, initial=1.25)
        rv = b.remote_variable("gain", rant.f64)
        check("handles created", all(h is not None for h in
                                    (pub_flag, sub_flag, pub_note, sub_note, vd, rv)))
        deadline = time.time() + 8.0
        while time.time() < deadline and (pub_flag.match_count == 0
                                          or pub_note.match_count == 0
                                          or rv.get() is None):
            a.poll(0.001)
            b.poll(0.001)
        check("bare topics matched",
              pub_flag.match_count == 1 and pub_note.match_count == 1)
        check("bare variable replicated the initial", rv.get() == 1.25)
        pub_flag.send(True)
        pub_note.send("a bare unbounded string")
        deadline = time.time() + 5.0
        while time.time() < deadline and len(got) < 2:
            a.poll(0.001)
            b.poll(0.001)
        check("bool delivered as a plain value", got.get("flag") is True)
        check("string delivered as a plain value", got.get("note") == "a bare unbounded string")
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


def patterns():
    """Functions and variables between two nodes on an isolated domain."""
    print("patterns leg: two nodes, domain 43, loopback")
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    srv = rant.Node("srv", on_event=on_event("srv"),
                    domain=43, multicast_interface=IFACE, max_topics=32)
    cli = rant.Node("cli", on_event=on_event("cli"),
                    domain=43, multicast_interface=IFACE, max_topics=32)

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
                    domain=46, multicast_interface=IFACE, max_topics=32)
    cli = rant.Node("tcli", on_event=on_event("tcli"),
                    domain=46, multicast_interface=IFACE, max_topics=32)
    mcli = rant.Node("tmcli", on_event=on_event("tmcli"), domain=46,
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
            raise rant.CancelledError("stopped")
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
    finally:
        srv.close()
        cli.close()
        mcli.close()
    print("tasks: " + ("PASS\n" if ok else "FAIL\n"))
    return ok


got = threading.Event()
received = []


def on_pose(pose, m):
    received.append(pose)
    print("recv: [%s] from %s -> %r" % (m.topic_name, m.publisher_name, pose))
    got.set()


def on_event(tag):
    return lambda e: print("event(%s):" % tag, e)


def main():
    if not round_trip():
        return 1
    if not value_roots():
        return 1
    if not std_types():
        return 1
    print("opening nodes...")
    sub = rant.Node("sub", on_event=on_event("sub"),
                    domain=DOMAIN, multicast_interface=IFACE, threading=rant.Threading.MANUAL)
    pub = rant.Node("pub", on_event=on_event("pub"),
                    domain=DOMAIN, multicast_interface=IFACE, threading=rant.Threading.MANUAL)

    subch = sub.subscriber("pose", Pose, on_pose, reliable=True, keep_last=8)
    pubch = pub.publisher("pose", Pose, reliable=True, keep_last=8)

    sent = Pose(stamp=7, x=1.5, y=-2.5, uuid=b"\x01\x02\x03\x04",
                frame="map", tags=["fast", "ok"], vel=Twist(dx=0.5, dy=0.25))

    # MANUAL: drive both nodes by polling them in the loop.
    deadline = time.time() + 8.0
    while time.time() < deadline and not got.is_set():
        pubch.send(sent)
        pub.poll(0.001)
        sub.poll(0.001)

    ok = got.is_set()
    if ok:
        r = received[0]
        ok = (isinstance(r, Pose) and r.stamp == 7 and abs(r.x - 1.5) < 1e-9
              and abs(r.y + 2.5) < 1e-9 and bytes(r.uuid) == b"\x01\x02\x03\x04"
              and r.frame == "map" and list(r.tags) == ["fast", "ok"]
              and abs(r.vel.dx - 0.5) < 1e-6 and abs(r.vel.dy - 0.25) < 1e-6)
        print("PASS" if ok else "FAIL: decoded value mismatch: %r" % (r,))
        print("Pose DSL (for C interop):\n" + rant.dsl(Pose))
    else:
        print("FAIL: no message delivered within timeout")

    # An over-cap string must raise, never silently truncate.
    if ok:
        try:
            pubch.send(Pose(frame="way-too-long-for-sixteen-bytes"))
            print("FAIL: over-cap string did not raise")
            ok = False
        except rant.SchemaError:
            print("PASS: over-cap string refused")

    pub.close()
    sub.close()

    # A handle outliving its node answers NO_TOPIC instead of touching freed memory.
    if ok:
        ok = (pubch.send(sent) == rant.SendStatus.NO_TOPIC and pubch.match_count == 0
              and subch.take() is None and pubch.name == "pose" and pub.name == "pub")
        print("PASS: handle after close" if ok else "FAIL: handle after close")

    if ok:
        ok = lifecycle()
    if ok:
        ok = handles()
    if ok:
        ok = value_roots_live()
    if ok:
        ok = video_types()
    if ok:
        ok = patterns()
    if ok:
        ok = tasks()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
