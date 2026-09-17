"""A typed 'tick' publisher at 1000 Hz on the default interface and domain, driven by a
manual poll. It shares its schema with bindings/csharp/publisher. Args: [seconds] [interface]."""
import math
import os
import sys
import time
from dataclasses import dataclass, field

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rant    # noqa: E402

HZ = 1000


@dataclass
class Tick:
    seq: rant.u64 = 0
    when: rant.types.Timestamp = 0                                    # Unix-epoch microseconds, UTC
    value: rant.f64 = 0.0
    at: rant.types.Transform = field(default_factory=rant.types.Transform)       # meters and a quaternion


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 0.0   # 0 = run forever
    iface = sys.argv[2] if len(sys.argv) > 2 else None
    node = rant.Node("py-publisher", multicast_interface=iface, threading=rant.Threading.MANUAL)
    # keep_last deep enough that a small per-loop burst is not evicted before it flushes.
    ch = node.publisher("tick", Tick, keep_last=64)
    print("publishing 'tick' at %d Hz on the default interface, domain 0 (Ctrl+C to stop)" % HZ)
    print("schema: " + " ".join(rant.dsl(Tick).split()))

    period = 1.0 / HZ
    start = time.perf_counter()
    seq = 0
    last_report, last_seq = start, 0
    try:
        while True:
            now = time.perf_counter()
            due = int((now - start) / period)             # how many ticks should exist by now
            while seq < due:
                angle = seq * 0.01
                ch.send(Tick(seq=seq, when=rant.types.now(), value=math.sin(angle),
                             at=rant.types.Transform(translation=rant.types.Double3(math.cos(angle),
                                                                       math.sin(angle), 0.0))))
                seq += 1
            node.poll(0)                                   # non-blocking: flush the burst + service RX
            if now - last_report >= 1.0:
                rate = (seq - last_seq) / (now - last_report)
                print("seq=%d  rate=%.0f Hz  subscribers=%d" % (seq, rate, ch.match_count))
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
