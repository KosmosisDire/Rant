"""DART Python test: two nodes, reliable typed pub/sub, on one host.

Exercises the compile-on-import path, discovery/match, and schema reflection
(encode a Python object on one node, decode it back on the other), including
capped strings and string arrays. Exit 0 = the message crossed and decoded to
the expected values.

    python python/test.py
"""
import os
import sys
import threading
import time
from dataclasses import dataclass, field

# Import the shipped single-file wrapper from dist/ (dev layout).
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "..", "dist"))
import dart  # noqa: E402

DOMAIN = 42
IFACE = "127.0.0.1"   # pin discovery to loopback for a single-host run


# Any annotated class is a schema (no decorator); @dataclass just gives a handy ctor.
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
    """Encode -> decode round-trip of the variable kinds (no networking)."""
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
    """Functions / variables / signals between two nodes on an isolated domain."""
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

    # definitions on srv: simple form (return = reply), a raiser (-> APP_ERROR),
    # and a full form that defers and completes off-thread
    add = dart.FunctionDefinition[AddReq, AddRsp](srv, "add",
                                                  lambda q: AddRsp(sum=q.a + q.b))

    def _boom(q):
        raise RuntimeError("kaboom")   # noqa: the traceback print is expected
    dart.FunctionDefinition[AddReq, AddRsp](srv, "boom", _boom)

    def _late(q, request):
        d = request.defer()
        threading.Timer(0.05, lambda: d.complete(AddRsp(sum=q.a + q.b))).start()
    dart.FunctionDefinition[AddReq, AddRsp](srv, "late", _late)

    sig_count = []
    dart.Signal(srv, "estop", lambda v: sig_count.append(v))
    lvl_def = dart.VariableDefinition[Level](srv, "level", initial=Level(value=5),
                                             allow_force=True)

    srv.start()   # service thread owns srv's loop; handlers fire on it

    # remotes on cli (manual poll: blocking calls drive cli's loop themselves)
    add_r = dart.RemoteFunction[AddReq, AddRsp](cli, "add")
    boom_r = dart.RemoteFunction[AddReq, AddRsp](cli, "boom")
    late_r = dart.RemoteFunction[AddReq, AddRsp](cli, "late")
    sig_out = dart.Signal(cli, "estop")
    lvl = dart.RemoteVariable[Level](cli, "level")

    deadline = time.time() + 8.0
    while time.time() < deadline and not (add_r.has_definition()
                                          and boom_r.has_definition()
                                          and late_r.has_definition()
                                          and sig_out.listener_count() > 0):
        cli.poll(5)
    check("definitions discovered", add_r.has_definition() and boom_r.has_definition()
          and late_r.has_definition())
    check("signal listener matched", sig_out.listener_count() == 1)

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

    # force overrides with a shadow source; unforce restores the latest set
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

    # signal: three payload-less emits, delivered on srv's service thread
    check("emit accepted", sig_out.emit() == dart.SendStatus.OK)
    sig_out.emit()
    sig_out.emit()
    deadline = time.time() + 5.0
    while time.time() < deadline and len(sig_count) < 3:
        cli.poll(5)
    check("three signals delivered", len(sig_count) == 3)

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
    print("opening nodes (first run compiles the embedded C, please wait)...")
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

    # Threaded: both nodes on their C-level service threads; send from this thread,
    # delivery arrives with no poll() anywhere.
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

    if ok:
        ok = patterns()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
