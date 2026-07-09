"""DART Python subscriber: subscribes to 'tick' and reports the received message rate.
Pair with python/publisher.py or csharp/publisher. Single-threaded manual poll.

    python python/subscriber.py [seconds] [interface]
"""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dist"))
import dart  # noqa: E402


@dart.schema
class Tick:
    seq: dart.u64
    t_us: dart.u64
    value: dart.f64


count = 0


def on_message(_):
    global count
    count += 1


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 0.0
    iface = sys.argv[2] if len(sys.argv) > 2 else None
    node = dart.Node.open("py-subscriber", multicast_interface=iface, on_message=on_message,
                          on_event=lambda e: print("event:", e, file=sys.stderr))
    node.create_channel("tick", dart.Role.SUB_ONLY, Tick)
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
