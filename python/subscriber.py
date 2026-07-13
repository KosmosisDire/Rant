"""DART Python subscriber: subscribes to 'tick' and reports the received message rate.
Pair with python/publisher.py or csharp/publisher. Single-threaded manual poll.

    python python/subscriber.py [seconds] [interface]
"""
import os
import sys
import time
from dataclasses import dataclass

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dist"))
import dart  # noqa: E402


@dataclass
class Tick:
    seq: dart.u64 = 0
    t_us: dart.u64 = 0
    value: dart.f64 = 0.0


count = 0


def on_message(_):
    global count
    count += 1


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 0.0
    iface = sys.argv[2] if len(sys.argv) > 2 else None
    node = dart.Node("py-subscriber", on_message,
                     lambda e: print("event:", e, file=sys.stderr),
                     multicast_interface=iface)
    dart.Channel[Tick](node, "tick", dart.Role.SUB_ONLY)
    print("subscribing to 'tick' (manual poll), reporting received Hz (Ctrl+C to stop)")

    start = time.perf_counter()
    last_report, last_count = start, 0
    try:
        while True:
            node.poll(1)                          # block up to 1ms; wakes on RX and delivers
            now = time.perf_counter()
            if now - last_report >= 1.0:
                rate = (count - last_count) / (now - last_report)
                print("received=%d  rate=%.0f Hz" % (count, rate))
                last_report, last_count = now, count
            if seconds and now - start >= seconds:
                break
    except KeyboardInterrupt:
        pass
    finally:
        node.close()


if __name__ == "__main__":
    sys.exit(main())
