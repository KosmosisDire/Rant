"""DART Python publisher: streams a typed 'tick' message at 1000 Hz on the DEFAULT
interface and domain (0), so any subscriber on the LAN (any language) can receive it.
Single-threaded: it drives the node with a manual poll in the send loop (the wrapper
has no background thread). Uses the same schema/topic as csharp/publisher, so the two
interoperate.

    python python/publisher.py [seconds] [interface]   # seconds/interface optional
"""
import math
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dist"))
import dart  # noqa: E402

HZ = 1000


@dart.schema
class Tick:
    seq: dart.u64
    t_us: dart.u64
    value: dart.f64


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 0.0   # 0 => run forever
    iface = sys.argv[2] if len(sys.argv) > 2 else None
    node = dart.Node.open("py-publisher", multicast_interface=iface)   # default iface, domain 0
    # keep_last deep enough that a small per-loop burst is not evicted before it flushes.
    ch = node.create_channel("tick", dart.Role.PUB_ONLY, Tick, qos=dart.Qos(keep_last=64))
    print("publishing 'tick' at %d Hz on the default interface, domain 0 (Ctrl+C to stop)" % HZ)
    print("schema: " + " ".join(Tick.__dart_dsl__.split()))

    period = 1.0 / HZ
    start = time.perf_counter()
    seq = 0
    last_report, last_seq = start, 0
    try:
        while True:
            now = time.perf_counter()
            due = int((now - start) / period)             # how many ticks should exist by now
            while seq < due:
                ch.send(Tick(seq=seq, t_us=int(now * 1e6), value=math.sin(seq * 0.01)))
                seq += 1
            node.poll(0)                                   # non-blocking: flush the burst + service RX
            if now - last_report >= 1.0:
                rate = (seq - last_seq) / (now - last_report)
                print("seq=%d  rate=%.0f Hz  subscribers=%d" % (seq, rate, ch.match_count()))
                last_report, last_seq = now, seq
            if seconds and now - start >= seconds:
                break
            time.sleep(0.0005)
    except KeyboardInterrupt:
        pass
    finally:
        node.close()


if __name__ == "__main__":
    sys.exit(main())
