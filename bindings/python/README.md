# rant-middleware

The Python wrapper for Rant: peer discovery over UDP
multicast plus reliable realtime UDP pub/sub, with typed messages. The wheel carries the
native library, so nothing compiles on your machine.

```sh
pip install rant-middleware
```

```python
from dataclasses import dataclass
import rant

@dataclass
class Pose:
    stamp: rant.u64 = 0
    x:     rant.f64 = 0.0
    frame: rant.string(16) = ""

node = rant.Node("robot1", on_message=lambda m: print(m.value), domain=7)
pose = rant.Topic[Pose](node, "pose", reliable=True)
node.start()
pose.send(Pose(stamp=1, x=1.0, frame="map"))
```

The guide is docs/python.md in the repository: https://github.com/KosmosisDire/Rant
