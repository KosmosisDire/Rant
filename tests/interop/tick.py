"""The Python side of an interop pair: `tick.py pub <seconds>` publishes the Tick schema every
language's tick program shares, `tick.py sub <seconds> <publisher>` exits 0 once twenty ticks
from that node decoded right and 1 on a wrong one or when the seconds run out. Loopback, on
RANT_DOMAIN."""
import math
import os
import sys
import time
from dataclasses import dataclass, field

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "bindings", "python"))
import rant    # noqa: E402

TICK_WANT = 20


@dataclass
class Hop:
    name: str = ""
    pts: list[rant.types.Float2] = field(default_factory=list)


@dataclass
class Tick:
    seq: rant.u64 = 0
    when: rant.types.Timestamp = 0
    value: rant.f64 = 0.0
    at: rant.types.Transform = field(default_factory=rant.types.Transform)
    hops: list[Hop] = field(default_factory=list)


def near(a, b):
    return abs(a - b) < 1e-9


def main(argv):
    sub = len(argv) > 0 and argv[0] == "sub"
    seconds = float(argv[1]) if len(argv) > 1 else 10.0
    sender = argv[2] if len(argv) > 2 else ""
    good, bad = [0], [0]
    node = rant.Node("py-tick-sub" if sub else "py-tick-pub", multicast_interface="127.0.0.1",
                     threading=rant.Threading.MANUAL)

    def on_tick(t, m):
        angle = t.seq * 0.01
        p = t.at.pose.position
        hops_ok = (len(t.hops) == 1 and t.hops[0].name == "h%d" % t.seq and len(t.hops[0].pts) == 1
                   and t.hops[0].pts[0].x == t.seq % 1000 and t.hops[0].pts[0].y == 0.5)
        if m.publisher_name == sender and t.when > 0 and near(t.value, math.sin(angle)) \
                and near(p.x, math.cos(angle)) and near(p.y, math.sin(angle)) and hops_ok:
            good[0] += 1
        else:
            if bad[0] == 0:
                print("FAIL: tick %d decoded wrong" % t.seq)
            bad[0] += 1

    deadline = time.monotonic() + seconds
    try:
        if sub:
            node.subscriber("tick", Tick, on_tick, reliable=True, keep_last=64)
            while time.monotonic() < deadline and not bad[0] and good[0] < TICK_WANT:
                node.poll(0.002)
            ok = good[0] >= TICK_WANT and not bad[0]
            print("%s: %d ticks decoded right, %d wrong" % ("PASS" if ok else "FAIL", good[0], bad[0]))
            return 0 if ok else 1
        topic = node.publisher("tick", Tick, reliable=True, keep_last=64)
        seq = 0
        while time.monotonic() < deadline:
            if topic.match_count > 0:
                angle = seq * 0.01
                topic.send(Tick(seq=seq, when=rant.types.now(), value=math.sin(angle),
                                at=rant.types.Transform(rant.types.Pose(rant.types.Double3(
                                    math.cos(angle), math.sin(angle), 0.0), rant.types.identity_rotation())),
                                hops=[Hop("h%d" % seq, [rant.types.Float2(seq % 1000, 0.5)])]))
                seq += 1
            node.poll(0.002)
        return 0
    finally:
        node.close()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
