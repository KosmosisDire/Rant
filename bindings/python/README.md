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
class Conveyor:
    speed:   rant.f64 = 0.0
    running: bool = False
    part:    rant.string(16) = ""

node = rant.Node("robot1", domain=7)
belt = node.publisher("line1/conveyor", Conveyor, reliable=True)
belt.send(Conveyor(speed=0.5, running=True, part="bracket"))
node.subscriber("line1/conveyor", Conveyor, lambda c: print(c))
```

The guide is docs/python.md in the repository: https://github.com/KosmosisDire/Rant
