# ramble-middleware

The Python wrapper for Ramble: peer discovery over UDP
multicast plus reliable realtime UDP pub/sub, with typed messages. The wheel carries the
native library, so nothing compiles on your machine.

```sh
pip install ramble-middleware
```

```python
from dataclasses import dataclass
import ramble

@dataclass
class Pose:
    stamp: ramble.u64 = 0
    x:     ramble.f64 = 0.0
    frame: ramble.string(16) = ""

node = ramble.Node("robot1", on_message=lambda m: print(m.value), domain=7)
pose = ramble.Topic[Pose](node, "pose", reliable=True)
node.start()
pose.send(Pose(stamp=1, x=1.0, frame="map"))
```

The guide is docs/python.md in the repository: https://github.com/KosmosisDire/Ramble
