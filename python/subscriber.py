"""A 'tick' subscriber reporting the received rate, the pair of python/publisher.py or
csharp/publisher. Args: [seconds] [interface]."""
import os
import sys
import time
from dataclasses import dataclass, field

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dist"))
import dart  # noqa: E402


@dataclass
class Tick:
    seq: dart.u64 = 0
    when: dart.Timestamp = 0                                  # Unix-epoch microseconds, UTC
    value: dart.f64 = 0.0
    at: dart.Pose = field(default_factory=dart.Pose)          # meters + a quaternion


count = 0
last = Tick()


def on_message(msg):
    global count, last
    count += 1
    last = msg.value                     # the decoded Tick: standard types and all


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 0.0
    iface = sys.argv[2] if len(sys.argv) > 2 else None
    node = dart.Node("py-subscriber", on_message,
                     lambda e: print("event:", e, file=sys.stderr),
                     multicast_interface=iface)
    dart.Topic[Tick](node, "tick", dart.Role.SUB_ONLY)
    print("subscribing to 'tick' (manual poll), reporting received Hz (Ctrl+C to stop)")

    start = time.perf_counter()
    last_report, last_count = start, 0
    try:
        while True:
            node.poll(1)                          # block up to 1 ms, wakes on receive and delivers
            now = time.perf_counter()
            if now - last_report >= 1.0:
                rate = (count - last_count) / (now - last_report)
                # `when` is the publisher's wall clock in the same units everywhere, so the
                # difference against ours is one-way latency plus clock skew.
                age_ms = (dart.timestamp_now() - last.when) / 1000.0 if last.when else 0.0
                p = last.at.position
                print("received=%d  rate=%.0f Hz  age=%.1f ms  at=(%.2f, %.2f, %.2f)"
                      % (count, rate, age_ms, p.x, p.y, p.z))
                last_report, last_count = now, count
            if seconds and now - start >= seconds:
                break
    except KeyboardInterrupt:
        pass
    finally:
        node.close()


if __name__ == "__main__":
    sys.exit(main())
