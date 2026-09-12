# dart-middleware

The Python wrapper for DART (Discovery And Realtime Transport): peer discovery over UDP
multicast plus reliable realtime UDP pub/sub, with typed messages. The wheel carries the
native library, so nothing compiles on your machine.

```sh
pip install dart-middleware
```

```python
from dataclasses import dataclass
import dart

@dataclass
class Pose:
    stamp: dart.u64 = 0
    x:     dart.f64 = 0.0
    frame: dart.string(16) = ""

node = dart.Node("robot1",
                 on_message=lambda m: print(m.value),
                 on_event=lambda e: print("event:", e),
                 domain=7)
pose = dart.Topic[Pose](node, "pose", qos=dart.Qos(reliability=dart.Reliability.RELIABLE))
node.start()
pose.send(Pose(stamp=1, x=1.0, frame="map"))
```

The guide is docs/python.md in the repository: https://github.com/KosmosisDire/DART
