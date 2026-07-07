"""DART Python test: two nodes, reliable typed pub/sub, on one host.

Exercises the compile-on-import path, discovery/match, and schema reflection
(encode a Python object on one node, decode it back on the other). Exit 0 = the
message crossed and decoded to the expected values.

    python python/test.py
"""
import os
import sys
import threading
import time

# Import the shipped single-file wrapper from dist/ (dev layout).
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "..", "dist"))
import dart  # noqa: E402

DOMAIN = 42
IFACE = "127.0.0.1"   # pin discovery to loopback for a single-host run


@dart.schema
class Twist:
    dx: dart.f32
    dy: dart.f32


@dart.schema
class Pose:
    stamp: dart.u64
    x:     dart.f64
    y:     dart.f64
    uuid:  dart.u8[4]
    vel:   Twist


got = threading.Event()
received = []


def on_message(m):
    received.append(m)
    print("recv: [%s] from %s -> %r" % (m.channel_name, m.sender_name, m.value))
    got.set()


def on_event(e):
    print("event(sub):", e)


def main():
    print("opening nodes (first run compiles the embedded C, please wait)...")
    sub = dart.Node.open("sub", domain=DOMAIN, multicast_interface=IFACE,
                         on_message=on_message, on_event=on_event)
    pub = dart.Node.open("pub", domain=DOMAIN, multicast_interface=IFACE)

    qos = dart.Qos(reliability=dart.Reliability.RELIABLE, keep_last=8)
    sub.create_channel("pose", dart.Role.SUB_ONLY, Pose, qos=qos)
    pubch = pub.create_channel("pose", dart.Role.PUB_ONLY, Pose, qos=qos)

    sent = Pose(stamp=7, x=1.5, y=-2.5, uuid=b"\x01\x02\x03\x04", vel=Twist(dx=0.5, dy=0.25))

    # Single-threaded: drive both nodes by polling them in the loop (no start()).
    deadline = time.time() + 8.0
    while time.time() < deadline and not got.is_set():
        pubch.send(sent)
        pub.poll(1)
        sub.poll(1)

    ok = got.is_set()
    if ok:
        r = received[0].value
        match = (isinstance(r, Pose) and r.stamp == 7 and abs(r.x - 1.5) < 1e-9
                 and abs(r.y + 2.5) < 1e-9 and bytes(r.uuid) == b"\x01\x02\x03\x04"
                 and abs(r.vel.dx - 0.5) < 1e-6 and abs(r.vel.dy - 0.25) < 1e-6)
        print("PASS" if match else "FAIL: decoded value mismatch: %r" % (r,))
        ok = match
    else:
        print("FAIL: no message delivered within timeout")

    pub.close()
    sub.close()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
