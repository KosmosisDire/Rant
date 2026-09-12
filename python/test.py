"""The Python binding test: two nodes on one host over reliable typed pub sub, the
patterns and the standard types. Exit 0 = pass (spec/testing.md)."""
import enum as _enum
import os
import sys
import threading
import time
from dataclasses import dataclass, field

# Import the package from the source checkout, which finds the library in dist/native.
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
import dart  # noqa: E402

DOMAIN = 42
IFACE = "127.0.0.1"   # pin discovery to loopback for a single-host run


# any annotated class is a schema, @dataclass just gives a handy constructor
@dataclass
class Twist:
    dx: dart.f32 = 0.0
    dy: dart.f32 = 0.0


@dataclass
class Pose:
    stamp: dart.u64 = 0
    x:     dart.f64 = 0.0
    y:     dart.f64 = 0.0
    uuid:  dart.u8[4] = b""
    frame: dart.string(16) = ""             # a capped UTF-8 string
    tags:  dart.string(8)[2] = ()           # a fixed array of capped strings
    vel:   Twist = field(default_factory=Twist)


# The v4 variable-length kinds: a variable string, a variable scalar array, a
# variable string array, and a self-describing map.
@dataclass
class Sensor:
    id:      dart.u32 = 0
    name:    dart.string(16) = ""                              # capped (fixed)
    note:    str = ""                                          # variable string
    samples: list[dart.f32] = field(default_factory=list)     # variable scalar array
    labels:  list[dart.string(8)] = field(default_factory=list)  # variable string array
    extras:  dict = field(default_factory=dict)               # self-describing map


def round_trip():
    """Encode and decode round trip of the variable kinds, no networking."""
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    sch = dart.Schema(Sensor)
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
    except dart.SchemaError:
        check("over-cap raises", True)
    print("variable-kinds round-trip: " + ("PASS\n" if ok else "FAIL\n"))
    return ok


# The canonical wire of a bare type is its kind alone, so these hashes are the same in
# every language binding (pinned in C by dart_test's schema-root phase).
HASH_BOOL = 0xEE90234F61D2520B
HASH_F32ARR = 0x314844E3386A1FC4


class Mode(_enum.IntEnum):
    IDLE = 0
    RUN = 1
    FAULT = 2


# The Float3 schema is the shared cross-language golden vector: the same wire and the
# same hash from C, C++, C# and Python (pinned in C by dart_test's stdtypes phase).
HASH_FLOAT3 = 0x04AA9469CD08B1DD


@dataclass
class Track:
    """Standard types as ordinary annotations: an alias spells as its name and carries a
        plain value, a composite is a shipped dataclass. Both narrow matching."""
    at: dart.types.Transform = field(default_factory=dart.types.Transform)
    when: dart.types.Timestamp = 0
    tag: dart.types.Color = field(default_factory=dart.types.Color)
    id: dart.types.Uuid = bytes(16)
    velocity: dart.types.Float3 = field(default_factory=dart.types.Float3)


def std_types():
    """The standard type library: golden wire, name-narrowed matching, round trip."""
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    f3 = dart.Schema("Float3")
    check("Float3 compiles by name alone, golden hash", f3.hash == HASH_FLOAT3)
    check("Float3 is 12 message bytes", f3.size == 12)
    check("the mirror dataclass IS that type", dart.Schema(dart.types.Float3).hash == HASH_FLOAT3)

    # a name narrows: an anonymous field of the same shape reads a Transform field, never
    # the reverse, and Transform/Twist are distinct names never mistaken for each other
    named = dart.Schema("W { at: Transform }")
    bare = dart.Schema("W { at: { translation: { x: f64, y: f64, z: f64 },"
                       "         rotation: { x: f64, y: f64, z: f64, w: f64 },"
                       "         parent: string<30> } }")
    check("an anonymous field of the same shape reads a Transform field",
          bare.can_read(named) and not named.can_read(bare))
    check("Transform and Twist never cross-wire",
          not dart.Schema("Twist").can_read(dart.Schema("Transform")))

    sch = dart.Schema(Track)
    text = " ".join(dart.dsl(Track).split())
    check("the reflected schema spells the names, not the shapes",
          text == "Track { at: Transform, when: Timestamp, tag: Color, id: Uuid, velocity: Float3 }")
    check("its message is the sum of the wire shapes (88+8+4+16+12)", sch.size == 128)

    t = Track(at=dart.types.Transform(translation=dart.types.Double3(4.5, -1.25, 9.0)),
              when=dart.types.now(), tag=dart.types.Color(0x11, 0x22, 0x33, 0xFF),
              id=bytes(range(16)), velocity=dart.types.Float3(1.0, 2.0, 3.0))
    back = sch.decode(sch.encode(t))
    check("a Track round-trips whole",
          back.at.translation.x == 4.5 and back.at.rotation.w == 1.0
          and back.when == t.when and back.tag.r == 0x11 and back.tag.a == 0xFF
          and bytes(back.id) == bytes(range(16)) and back.velocity.z == 3.0)
    check("types.now is Unix-epoch microseconds", dart.types.now() > 1600000000000000)
    return ok


# The video family: golden wire shared with the C, C++ and C# bindings (pinned in
# cpp/test.cpp and by dart_test's stdtypes phase).
HASH_IMAGE = 0x489841F99F392B85
HASH_VIDEO_FRAME = 0xF677BD147B513FBC
HASH_EXT_STREAM = 0xAAE502077016AC13


@dataclass
class Clip:
    """The video mirrors used as FIELDS: each spells as its name, so the schema is the
    same as the text form and needs no definition of its own."""
    cover: dart.types.Image = field(default_factory=dart.types.Image)
    live: dart.types.ExternalVideoStream = field(default_factory=dart.types.ExternalVideoStream)


def video_types():
    """The video family: golden wire from both spellings, an Image over two nodes, and
    ExternalVideoStream as the latched stream URL it exists for."""
    print("video leg: golden wire, then two nodes, domain 45, loopback")
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    for name, cls, golden in (("Image", dart.types.Image, HASH_IMAGE),
                              ("VideoFrame", dart.types.VideoFrame, HASH_VIDEO_FRAME),
                              ("ExternalVideoStream", dart.types.ExternalVideoStream,
                               HASH_EXT_STREAM)):
        check("%s compiles by name alone, golden hash" % name,
              dart.Schema(name).hash == golden)
        check("the %s mirror IS that type" % name, dart.Schema(cls).hash == golden)
    check("a video mirror nests as a named field",
          dart.Schema(Clip).hash
          == dart.Schema("Clip { cover: Image, live: ExternalVideoStream }").hash)

    got = {}
    a = dart.Node("VidA", None, on_event("VidA"), domain=45, multicast_interface=IFACE)
    b = dart.Node("VidB", None, on_event("VidB"), domain=45, multicast_interface=IFACE)
    try:
        qos = dart.Qos(reliability=dart.Reliability.RELIABLE, keep_last=4)
        pub = dart.Publisher[dart.types.Image](a, "frame", qos=qos)
        dart.Subscriber[dart.types.Image](b, "frame", lambda i: got.setdefault("img", i), qos=qos)
        stream = dart.types.ExternalVideoStream(kind=dart.types.VideoStreamKind.Rtsp,
                                          codec=dart.types.VideoCodec.H264,
                                          width=1920, height=1080,
                                          url="rtsp://cam.local/main", name="front door")
        vd = dart.VariableDefinition[dart.types.ExternalVideoStream](a, "stream", initial=stream)
        rv = dart.RemoteVariable[dart.types.ExternalVideoStream](b, "stream")
        check("handles created", all(h is not None for h in (pub, vd, rv)))
        deadline = time.time() + 8.0
        while time.time() < deadline and (pub.match_count() == 0 or rv.get() is None):
            a.poll(1)
            b.poll(1)
        check("image pair matched", pub.match_count() == 1)

        pixels = bytes((i * 7) & 0xFF for i in range(384))
        img = dart.types.Image(width=32, height=4, stride=96,
                         format=dart.types.ImageFormat.Rgb8, data=pixels)
        check("image send", pub.send(img) == dart.SendStatus.OK)
        deadline = time.time() + 5.0
        while time.time() < deadline and "img" not in got:
            a.poll(1)
            b.poll(1)
        r = got.get("img")
        check("an Image crosses whole",
              r is not None and r.width == 32 and r.height == 4 and r.stride == 96
              and r.format == dart.types.ImageFormat.Rgb8 and bytes(r.data) == pixels)

        v = rv.get()
        check("the stream variable replicated",
              v is not None and v.kind == dart.types.VideoStreamKind.Rtsp
              and v.codec == dart.types.VideoCodec.H264
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

    check("bool canonical hash", dart.Schema(bool).hash == HASH_BOOL)
    check("f32[] canonical hash", dart.Schema(list[dart.f32]).hash == HASH_F32ARR)
    check("bool dsl", dart.dsl(bool) == "bool\n")
    check("f32[] dsl", dart.dsl(list[dart.f32]) == "f32[]\n")
    check("string(16) dsl", dart.dsl(dart.string(16)) == "string<16>\n")
    check("bare root reflects as one anonymous field",
          dart.Schema(dart.f64).field_count == 1
          and dart.Schema(dart.f64).name == ""
          and dart.Schema(dart.f64).fields()[0].name == "")
    for src, value in ((bool, True), (dart.u8, 200), (dart.i32, -7),
                       (dart.f32, 1.5), (dart.f64, -2.25), (int, 5), (float, 0.5),
                       (bool, False), (dart.string(16), "capped"), (str, "unbounded"),
                       (list[dart.f32], [1.5, -2.5]), (dict, {"battery": 87}),
                       (dart.u8[4], b"\x01\x02\x03\x04"), (Mode, Mode.FAULT)):
        sch = dart.Schema(src)
        out = sch.decode(sch.encode(value))
        if isinstance(value, list):
            same = [round(x, 3) for x in out] == [round(x, 3) for x in value]
        elif isinstance(value, float):
            same = abs(out - value) < 1e-6
        elif isinstance(value, bytes):
            same = bytes(out) == value
        else:
            same = out == value
        check("round-trip %s -> %r" % (dart.dsl(src).strip(), out), same)
    # text DSL and the bare type agree, and a named bare root is an error
    check("text `bool` == plain bool", dart.Schema("bool").hash == HASH_BOOL)
    try:
        dart.Schema("Temperature: f32")
        check("named bare root refused", False)
    except dart.SchemaError:
        check("named bare root refused", True)
    print("bare-type roots: " + ("PASS\n" if ok else "FAIL\n"))
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
    a = dart.Node("VA", None, lambda e: None, domain=44, multicast_interface=IFACE)
    b = dart.Node("VB", None, lambda e: None, domain=44, multicast_interface=IFACE)
    try:
        qos = dart.Qos(reliability=dart.Reliability.RELIABLE, keep_last=4)
        pub_flag = dart.Topic[bool](a, "flag", dart.Role.PUB_ONLY, qos)
        sub_flag = dart.Topic[bool](b, "flag", dart.Role.SUB_ONLY, qos)
        pub_note = dart.Topic[str](a, "note", dart.Role.PUB_ONLY, qos)
        sub_note = dart.Topic[str](b, "note", dart.Role.SUB_ONLY, qos)
        b.on_message(lambda m: got.setdefault(m.topic_name, m.value))
        vd = dart.VariableDefinition[dart.f64](a, "gain", initial=1.25)
        rv = dart.RemoteVariable[dart.f64](b, "gain")
        check("handles created", all(h is not None for h in
                                    (pub_flag, sub_flag, pub_note, sub_note, vd, rv)))
        deadline = time.time() + 8.0
        while time.time() < deadline and (pub_flag.match_count() == 0
                                          or pub_note.match_count() == 0
                                          or rv.get() is None):
            a.poll(1)
            b.poll(1)
        check("bare topics matched",
              pub_flag.match_count() == 1 and pub_note.match_count() == 1)
        check("bare variable replicated the initial", rv.get() == 1.25)
        pub_flag.send(True)
        pub_note.send("a bare unbounded string")
        deadline = time.time() + 5.0
        while time.time() < deadline and len(got) < 2:
            a.poll(1)
            b.poll(1)
        check("bool delivered as a plain value", got.get("flag") is True)
        check("string delivered as a plain value", got.get("note") == "a bare unbounded string")
        rv.set(2.5)
        deadline = time.time() + 5.0
        while time.time() < deadline and vd.get() != 2.5:
            a.poll(1)
            b.poll(1)
        check("bare variable set converged", vd.get() == 2.5)
    finally:
        a.close()
        b.close()
    print("bare-root live leg: " + ("PASS\n" if ok else "FAIL\n"))
    return ok


# patterns leg types
@dataclass
class AddReq:
    a: dart.i32 = 0
    b: dart.i32 = 0


@dataclass
class AddRsp:
    sum: dart.i32 = 0


@dataclass
class Level:
    value: dart.i32 = 0


def patterns():
    """Functions and variables between two nodes on an isolated domain."""
    print("patterns leg: two nodes, domain 43, loopback")
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    srv = dart.Node("srv", None, on_event("srv"),
                    domain=43, multicast_interface=IFACE, max_topics=32)
    cli = dart.Node("cli", None, on_event("cli"),
                    domain=43, multicast_interface=IFACE, max_topics=32)

    # definitions on srv: the simple form, a raiser giving APP_ERROR, and a full form that
    # defers and completes off thread
    add = dart.FunctionDefinition[AddReq, AddRsp](srv, "add",
                                                  lambda q: AddRsp(sum=q.a + q.b))

    def _boom(q):
        raise RuntimeError("kaboom")   # noqa: the traceback print is expected
    dart.FunctionDefinition[AddReq, AddRsp](srv, "boom", _boom)

    def _late(q, request):
        d = request.defer()
        threading.Timer(0.05, lambda: d.complete(AddRsp(sum=q.a + q.b))).start()
    dart.FunctionDefinition[AddReq, AddRsp](srv, "late", _late)

    lvl_def = dart.VariableDefinition[Level](srv, "level", initial=Level(value=5),
                                             allow_force=True)

    srv.start()   # the service thread owns srv's loop, handlers fire on it

    # remotes on cli (manual poll: blocking calls drive cli's loop themselves)
    add_r = dart.RemoteFunction[AddReq, AddRsp](cli, "add")
    boom_r = dart.RemoteFunction[AddReq, AddRsp](cli, "boom")
    late_r = dart.RemoteFunction[AddReq, AddRsp](cli, "late")
    lvl = dart.RemoteVariable[Level](cli, "level")

    deadline = time.time() + 8.0
    while time.time() < deadline and not (add_r.has_definition()
                                          and boom_r.has_definition()
                                          and late_r.has_definition()):
        cli.poll(5)
    check("definitions discovered", add_r.has_definition() and boom_r.has_definition()
          and late_r.has_definition())

    # blocking calls
    r = add_r.call(AddReq(a=2, b=3), 3000)
    check("blocking call ok", r.ok and r.status == dart.CallStatus.OK)
    check("blocking call value", r.ok and r.value.sum == 5)
    check("provider set", r.ok and r.provider != 0)

    rb = boom_r.call(AddReq(a=1, b=1), 3000)
    check("raising handler -> APP_ERROR", rb.status == dart.CallStatus.APP_ERROR)
    threw = False
    try:
        _ = rb.value
    except dart.CallError:
        threw = True
    check(".value on not-ok raises CallError", threw)

    rl = late_r.call(AddReq(a=20, b=22), 3000)
    check("deferred completion", rl.ok and rl.value.sum == 42)
    check("caller count seen by definition", add.caller_count() == 1)

    # variable: catch_up hands the remote the initial value
    check("variable wait", lvl.wait(3000))
    v = lvl.get()
    check("initial value", v is not None and v.value == 5)
    check("has_definition", lvl.has_definition())
    check("remote set accepted", lvl.set(Level(value=9)) == dart.SendStatus.OK)
    deadline = time.time() + 5.0
    while time.time() < deadline and not ((v := lvl.get()) and v.value == 9):
        cli.poll(5)
    check("set round-trips to the remote", lvl.get().value == 9)
    check("definition applied it", lvl_def.get().value == 9)

    # force overrides with a shadow source, unforce restores the latest set
    check("force", lvl_def.force(Level(value=99)) == dart.SendStatus.OK)
    deadline = time.time() + 5.0
    while time.time() < deadline and not ((v := lvl.get()) and v.value == 99):
        cli.poll(5)
    check("forced value visible remotely", lvl.get().value == 99)
    check("remote sees forced()", lvl.forced())
    check("unforce", lvl_def.unforce() == dart.SendStatus.OK)
    deadline = time.time() + 5.0
    while time.time() < deadline and not ((v := lvl.get()) and v.value == 9):
        cli.poll(5)
    check("unforce restores the latest set", lvl.get().value == 9 and not lvl.forced())

    # variable events: on_change dedups + replays at registration, on_write counts
    # every applied write
    chg, wr = [], []
    lvl_def.on_change(lambda v, u: chg.append((v.value, u.forced, u.source)))
    check("on_change replays current at registration", len(chg) == 1 and chg[0][0] == 9)
    lvl_def.on_write(lambda v: wr.append(v.value))
    check("on_write does not replay", len(wr) == 0)
    check("identical re-set accepted", lvl_def.set(Level(value=9)) == dart.SendStatus.OK)
    check("identical re-set is a write, not a change", len(chg) == 1 and len(wr) == 1)
    check("new set accepted", lvl_def.set(Level(value=12)) == dart.SendStatus.OK)
    check("change fires inline with the new value",
          len(chg) == 2 and chg[1][0] == 12 and chg[1][2] == 0 and len(wr) == 2)
    rchg = []
    lvl.on_change(lambda v: rchg.append(v.value))
    check("remote on_change replays the cache", bool(rchg) and rchg[0] == 9)
    deadline = time.time() + 5.0
    while time.time() < deadline and (not rchg or rchg[-1] != 12):
        cli.poll(5)
    check("remote change arrives", bool(rchg) and rchg[-1] == 12)
    lvl_def.on_change(None)
    lvl_def.on_write(None)
    lvl.on_change(None)

    # async form: start cli's service thread, wait for the callback
    cli.start()
    done = threading.Event()
    async_rsp = []

    def on_rsp(resp):
        async_rsp.append(resp)
        done.set()
    add_r.call_async(AddReq(a=10, b=5), on_rsp)
    check("call_async answered", done.wait(5.0) and async_rsp[0].ok
          and async_rsp[0].value.sum == 15)

    # a blocking call is refused while the service thread owns the loop, loudly
    rr = add_r.call(AddReq(a=1, b=2), 100)
    check("blocking call refused under service thread",
          rr.status == dart.CallStatus.TIMEOUT
          and rr.send_status == dart.SendStatus.STATE)
    cli.stop()

    # a call still pending at close gets exactly one CANCELLED outcome, never hangs
    never = dart.RemoteFunction(cli, "never-served", timeout_us=60_000_000)
    cancelled = []
    cdone = threading.Event()

    def on_cancel(resp):
        cancelled.append(resp)
        cdone.set()
    never.call_async(None, on_cancel)
    srv.close()
    cli.close()
    check("pending call_async settles CANCELLED at close",
          cdone.wait(2.0) and cancelled[0].status == dart.CallStatus.CANCELLED)
    print("patterns: " + ("PASS\n" if ok else "FAIL\n"))
    return ok


# tasks leg types
@dataclass
class JobReq:
    count: dart.i32 = 0


@dataclass
class JobPrg:
    done: dart.i32 = 0


@dataclass
class JobRsp:
    total: dart.i32 = 0


def tasks():
    """Tasks between two nodes: progress ordering, cancel, no_cancel, blocking."""
    print("tasks leg: two nodes, domain 46, loopback")
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok  " if cond else " FAIL ") + name)
        ok = ok and cond

    srv = dart.Node("tsrv", None, on_event("tsrv"),
                    domain=46, multicast_interface=IFACE, max_topics=32)
    cli = dart.Node("tcli", None, on_event("tcli"),
                    domain=46, multicast_interface=IFACE, max_topics=32)
    try:
        # work: streams progress then returns a result
        def _work(task):
            total = 0
            for i in range(task.value.count):
                total += i + 1
                task.progress(JobPrg(done=i + 1))
            return JobRsp(total=total)
        work = dart.TaskDefinition[JobReq, JobPrg, JobRsp](srv, "work", _work)

        # grind: runs until cancelled, honors the cancel
        def _grind(task):
            task.progress(JobPrg(done=0))
            if not task.cancel_event.wait(8.0) or not task.cancelled:
                return JobRsp(total=-1)   # cancel never arrived: a visible failure
            raise dart.CancelledError("stopped")
        dart.TaskDefinition[JobReq, JobPrg, JobRsp](srv, "grind", _grind)

        # rigid: declares no_cancel, completes regardless
        def _rigid(task):
            task.progress(JobPrg(done=1))
            time.sleep(0.3)
            return JobRsp(total=7)
        dart.TaskDefinition[JobReq, JobPrg, JobRsp](srv, "rigid", _rigid,
                                                    no_cancel=True)

        srv.start()   # the service thread owns srv's loop, workers spawn off it

        work_r = dart.RemoteTask[JobReq, JobPrg, JobRsp](cli, "work")
        grind_r = dart.RemoteTask[JobReq, JobPrg, JobRsp](cli, "grind")
        rigid_r = dart.RemoteTask[JobReq, JobPrg, JobRsp](cli, "rigid")

        deadline = time.time() + 8.0
        while time.time() < deadline and not (work_r.has_definition()
                                              and grind_r.has_definition()
                                              and rigid_r.has_definition()):
            cli.poll(5)
        check("definitions discovered", work_r.has_definition()
              and grind_r.has_definition() and rigid_r.has_definition())

        cli.start()   # async legs: progress + responses fire on cli's service thread

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
        check("cancel accepted", grind_r.cancel(gid) == dart.SendStatus.OK)
        check("terminal CANCELLED with the handler's message",
              gdone.wait(5.0) and grsps[0].status == dart.CallStatus.CANCELLED
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
              rigid_r.cancel(nid) == dart.SendStatus.BAD_ROLE)
        check("rigid completes anyway",
              ndone.wait(5.0) and nrsps[0].ok and nrsps[0].value.total == 7)

        cli.stop()   # the blocking form drives the loop itself

        # blocking call with on_progress firing on the calling thread
        bprog, bthread = [], []

        def bprg(v):
            bprog.append(None if v is None else v.done)
            bthread.append(threading.current_thread() is threading.main_thread())
        br = work_r.call(JobReq(count=2), on_progress=bprg, timeout_ms=3000)
        check("blocking task ok", br.ok and br.value.total == 3)
        check("blocking progress on the calling thread, in order",
              bprog == [None, 1, 2] and all(bthread))

        check("caller count seen by the definition", work.caller_count() == 1)
    finally:
        srv.close()
        cli.close()
    print("tasks: " + ("PASS\n" if ok else "FAIL\n"))
    return ok


got = threading.Event()
received = []


def on_message(m):
    received.append(m)
    print("recv: [%s] from %s -> %r" % (m.topic_name, m.publisher_name, m.value))
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
    sub = dart.Node("sub", on_message, on_event("sub"),
                    domain=DOMAIN, multicast_interface=IFACE)
    pub = dart.Node("pub", None, on_event("pub"),
                    domain=DOMAIN, multicast_interface=IFACE)

    qos = dart.Qos(reliability=dart.Reliability.RELIABLE, keep_last=8)
    dart.Topic[Pose](sub, "pose", dart.Role.SUB_ONLY, qos)
    pubch = dart.Topic[Pose](pub, "pose", dart.Role.PUB_ONLY, qos)

    sent = Pose(stamp=7, x=1.5, y=-2.5, uuid=b"\x01\x02\x03\x04",
                frame="map", tags=["fast", "ok"], vel=Twist(dx=0.5, dy=0.25))

    # Single-threaded: drive both nodes by polling them in the loop (no start()).
    deadline = time.time() + 8.0
    while time.time() < deadline and not got.is_set():
        pubch.send(sent)
        pub.poll(1)
        sub.poll(1)

    ok = got.is_set()
    if ok:
        r = received[0].value
        ok = (isinstance(r, Pose) and r.stamp == 7 and abs(r.x - 1.5) < 1e-9
              and abs(r.y + 2.5) < 1e-9 and bytes(r.uuid) == b"\x01\x02\x03\x04"
              and r.frame == "map" and list(r.tags) == ["fast", "ok"]
              and abs(r.vel.dx - 0.5) < 1e-6 and abs(r.vel.dy - 0.25) < 1e-6)
        print("PASS" if ok else "FAIL: decoded value mismatch: %r" % (r,))
        print("Pose DSL (for C interop):\n" + dart.dsl(Pose))
    else:
        print("FAIL: no message delivered within timeout")

    # An over-cap string must raise, never silently truncate.
    if ok:
        try:
            pubch.send(Pose(frame="way-too-long-for-sixteen-bytes"))
            print("FAIL: over-cap string did not raise")
            ok = False
        except dart.SchemaError:
            print("PASS: over-cap string refused")

    # threaded: both nodes on their service threads, sent from this thread, delivered
    # with no poll() anywhere
    if ok:
        if not pub.start() or not sub.start():
            print("FAIL: start")
            ok = False
        elif pub.poll(0) != dart.SendStatus.STATE:
            print("FAIL: poll not refused while started")
            ok = False
        else:
            got.clear()
            sent.stamp = 8
            pubch.send(sent)
            ok = got.wait(3.0) and received[-1].value.stamp == 8
            print("PASS: threaded delivery via start() (evicted_unsent=%d)" % pub.evicted_unsent()
                  if ok else "FAIL: threaded delivery")
            pub.stop()
            sub.stop()

    pub.close()
    sub.close()

    # A handle outliving its node answers NO_TOPIC instead of touching freed memory.
    if ok:
        ok = (pubch.send(sent) == dart.SendStatus.NO_TOPIC and pubch.match_count() == 0
              and pubch.take() is None and pubch.name == "pose" and pub.name == "pub")
        print("PASS: handle after close" if ok else "FAIL: handle after close")

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
