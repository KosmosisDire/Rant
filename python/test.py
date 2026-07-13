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


got = threading.Event()
received = []


def on_message(m):
    received.append(m)
    print("recv: [%s] from %s -> %r" % (m.channel_name, m.sender_name, m.value))
    got.set()


def on_event(tag):
    return lambda e: print("event(%s):" % tag, e)


def main():
    print("opening nodes (first run compiles the embedded C, please wait)...")
    sub = dart.Node("sub", on_message, on_event("sub"),
                    domain=DOMAIN, multicast_interface=IFACE)
    pub = dart.Node("pub", None, on_event("pub"),
                    domain=DOMAIN, multicast_interface=IFACE)

    qos = dart.Qos(reliability=dart.Reliability.RELIABLE, keep_last=8)
    dart.Channel[Pose](sub, "pose", dart.Role.SUB_ONLY, qos)
    pubch = dart.Channel[Pose](pub, "pose", dart.Role.PUB_ONLY, qos)

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
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
