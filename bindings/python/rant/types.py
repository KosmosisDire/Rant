"""The standard types of docs/stdtypes.md, which are the rant_types package, plus the clock that
needs the native library."""

from rant_types import *  # noqa: F401,F403
from . import _native as _c


def now():
    """The wall clock in Timestamp units, microseconds since the Unix epoch UTC, the clock a
        message's written_us uses."""
    return int(_c.load().rant_timestamp_now())
