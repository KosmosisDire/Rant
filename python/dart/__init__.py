"""The DART Python wrapper: ctypes over the shared library the CMake target dart_shared
builds. docs/python.md explains how to use it."""

import dataclasses as _dataclasses
import enum as _pyenum
import inspect as _inspect
import struct as _struct
import sys as _sys
import threading as _threading
import traceback as _traceback
from . import _native as _c

__all__ = [
    "Node", "NodeOptions", "Topic", "Publisher", "Subscriber", "Qos", "Message", "Event",
    "Schema", "Field", "SchemaError", "dsl",
    "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "f32", "f64", "bool_", "string",
    "enum",
    "Reliability", "Role", "SendStatus", "CallStatus", "LogLevel", "MetaSection", "EventKind",
    "ErrorKind", "FieldType",
    "FunctionDefinition", "RemoteFunction", "Request", "Response", "Deferred", "CallError",
    "TaskDefinition", "RemoteTask", "TaskRequest", "Progress", "CancelledError",
    "VariableDefinition", "RemoteVariable", "VariableUpdate",
    "LogLine", "MetaSnapshot",
    "Float2", "Float3", "Float4", "Double2", "Double3", "Double4", "Int2", "Int3", "Int4",
    "Quaternion", "Color", "Rect", "RectI", "Transform", "Twist", "GeoPoint",
    "Image", "VideoFrame", "ExternalVideoStream", "CameraIntrinsics", "ImageFormat",
    "VideoCodec", "VideoStreamKind", "DistortionModel", "JointState", "JointNames",
    "Timestamp", "Duration", "Uri", "Uuid", "Matrix3x3", "Matrix4x4", "timestamp_now",
]


# The enums. Values match the C wire, names mirror the C# wrapper.

class Reliability(_pyenum.IntEnum):
    BEST_EFFORT = 0
    RELIABLE = 1


class Role(_pyenum.IntEnum):
    PUBSUB = 0
    PUB_ONLY = 1
    SUB_ONLY = 2
    INACTIVE = 3


class SendStatus(_pyenum.IntEnum):
    OK = 0
    NO_TOPIC = -1
    TOO_BIG = -2
    BAD_ROLE = -3
    OUT_OF_MEMORY = -4
    STATE = -5       # wrong state: poll while started, or a call a callback may not make
    NOSYS = -6       # not compiled in (start() under DART_NO_THREADS)


class CallStatus(_pyenum.IntEnum):
    """A call's outcome, mirrors DartCallStatus. TIMEOUT and PEER_LOST are synthesized on the
        caller, RUNNING is task only and the one non terminal status (docs/tasks.md)."""
    OK = 0
    APP_ERROR = 1
    NO_HANDLER = 2
    TIMEOUT = 3
    PEER_LOST = 4
    CANCELLED = 5
    RUNNING = 6


class LogLevel(_pyenum.IntEnum):
    """Severity of a built-in @dart/log line. Mirrors DartLogLevel."""
    ERROR = 0
    WARN = 1
    INFO = 2


class MetaSection(_pyenum.IntFlag):
    """A @dart/meta request's section mask, OR the bits. ALL or 0 = every section."""
    NODE = 0x1
    PROC = 0x2
    TOPICS = 0x4
    PEERS = 0x8
    ALL = 0


class EventKind(_pyenum.IntEnum):
    PEER_UP = 0
    PEER_DOWN = 1
    PEER_INTEREST = 2
    MSG_LOST = 3
    ERROR = 4       # something went wrong, read Event.error


class ErrorKind(_pyenum.IntEnum):
    """The specific error carried by an EventKind.ERROR event (mirrors DartErrorKind)."""
    NONE = 0
    NAME_COLLISION = 1
    QOS_INCOMPATIBLE = 2
    KIND_MISMATCH = 3
    SCHEMA_MISMATCH = 4
    INTEREST_OVERFLOW = 5
    META_TRUNCATED_INTEREST = 6
    META_TRUNCATED_SCHEMA = 7
    PEER_META_TOO_BIG = 8
    MSG_TOO_BIG = 9
    PEER_REFUSED = 10
    EVICTED_UNSENT = 11
    UNMATCHED_SEND = 12
    DUPLICATE_AUTHORITY = 13
    OOM = 14
    PLATFORM = 15
    SOCKET = 16
    BIND = 17
    MCAST_JOIN = 18
    SEND = 19
    RECV = 20
    POLL = 21
    WAKER = 22
    BAD_ADDRESS = 23


class FieldType(_pyenum.IntEnum):
    U8 = 0
    U16 = 1
    U32 = 2
    U64 = 3
    I8 = 4
    I16 = 5
    I32 = 6
    I64 = 7
    F32 = 8
    F64 = 9
    BOOL = 10
    ARRAY = 11
    STRUCT = 12
    STRING = 13
    VSTRING = 14   # variable string (`string`): rides the message tail
    VARRAY = 15    # variable array (`elem[]` / `string<C>[]`): live element count
    MAP = 16       # self-describing tagged map (`map`)
    ENUM = 17      # named integer (`enum<uN>{...}`): wire is just the backing scalar
    NAMED = 18     # a nominal tag on another type, reported as Field.type_name


# raw kind bytes (== FieldType, kept short for the codec below)
_U8, _U16, _U32, _U64 = 0, 1, 2, 3
_I8, _I16, _I32, _I64 = 4, 5, 6, 7
_F32, _F64, _BOOL, _ARR, _STRUCT, _STR = 8, 9, 10, 11, 12, 13
_VSTR, _VARR, _MAP = 14, 15, 16    # the variable kinds (ride the message tail)
_ENUM = 17                          # named integer (wire = its backing scalar)
_NAMED = 18                         # a name on another type (unwrapped by the reflection API)
_SIGNED = frozenset((_I8, _I16, _I32, _I64))
_FLOATK = frozenset((_F32, _F64))
_FMT = {_U8: "B", _U16: "H", _U32: "I", _U64: "Q", _I8: "b", _I16: "h",
        _I32: "i", _I64: "q", _F32: "f", _F64: "d", _BOOL: "B"}
_SCALAR_SIZE = {_U8: 1, _U16: 2, _U32: 4, _U64: 8, _I8: 1, _I16: 2, _I32: 4,
                _I64: 8, _F32: 4, _F64: 8, _BOOL: 1}
_TOKEN = {_U8: "u8", _U16: "u16", _U32: "u32", _U64: "u64", _I8: "i8", _I16: "i16",
          _I32: "i32", _I64: "i64", _F32: "f32", _F64: "f64", _BOOL: "bool"}


class SchemaError(Exception):
    pass


class CallError(Exception):
    """Raised by Response.value when the call did not complete CallStatus.OK.
    Carries .status (the CallStatus) and .send_status (a synchronous refusal)."""
    def __init__(self, status, send_status, msg):
        super().__init__(msg)
        self.status = status
        self.send_status = send_status


class CancelledError(Exception):
    """Raise from a task handler to complete the call CallStatus.CANCELLED (the
    cooperative honor of a cancel). The exception text becomes Response.message."""


# The config and value dataclasses, mirroring the C++ structs. Zero = default.

@_dataclasses.dataclass
class Qos:
    reliability: Reliability = Reliability.BEST_EFFORT
    keep_last: int = 0
    catch_up: int = 0
    max_message_bytes: int = 0
    heartbeat_us: int = 0
    repair_delay_us: int = 0
    backpressure_wait_us: int = 0
    shm_max_bytes: int = 0
    queue_bytes: int = 0   # take and dispatch queue cap, 0 = lazy with a 1 MB cap
    max_rate_hz: int = 0   # best effort sub: kept samples per second, 0 = all
    no_timestamp: bool = False  # PUBLISHER: send without the per-message source timestamp, so
                                # receivers read msg.written_us == 0. Default stamps every message


@_dataclasses.dataclass
class NodeOptions:
    domain: int = 0
    max_topics: int = 0
    disable_shm: bool = False
    fetch_details: bool = False   # greedily fetch every peer topic's name + schema (observer UIs)
    match_wait_ms: int = 0        # send path match wait, 0 = 1 s, negative = disabled
    disable_logs: bool = False    # strip the built-in @dart/log/{error,warn,info} topics
    disable_meta: bool = False    # do not host the built-in @dart/meta introspection endpoint
    disable_error_logs: bool = False # suppress default error mirroring onto @dart/log/error
    data_port: int = 0
    discovery_group: str = ""
    discovery_port: int = 0
    multicast_interface: str = ""
    multicast_ttl: int = 0
    seed_peers: list = _dataclasses.field(default_factory=list)
                                  # "ip" or "ip:port" strings to also unicast announces to, so
                                  # discovery works where multicast is filtered
    unicast_only: bool = False    # join no group, announce to seed peers and ask them to relay us
                                  # (docs/discovery.md)
    self_ip: str = ""             # advertise this locator to every peer instead of letting each
                                  # learn it from the datagram source, for a static 1:1 mapping
    advertise_port: int = 0       # advertise THIS data port instead of the one we bound (0 = bound)
    fragment_size: int = 0
    recv_buffer_bytes: int = 0    # data socket OS buffers. 0 = the OS default, which is too
    send_buffer_bytes: int = 0    # small to hold a multi megabyte message whole
    announce_interval_us: int = 0
    peer_timeout_us: int = 0
    max_peers: int = 0


@_dataclasses.dataclass
class LogLine:
    """One decoded @dart/log line for a Node.on_log handler. wall_us is epoch us, mono_us the
        publisher's monotonic clock and recv_us this node's clock at receipt."""
    level: LogLevel
    node: str           # the publishing node's name
    node_id: int        # the publishing peer id
    wall_us: int
    mono_us: int
    recv_us: int
    written_us: int    # the carrying message's source stamp (see Message.written_us)
    text: str


@_dataclasses.dataclass
class MetaSnapshot:
    """A decoded @dart/meta reply. The node and proc scalars are fields, the full body stays
        in info. Absent sections leave zeros and have_proc False."""
    valid: bool = False
    status: CallStatus = CallStatus.TIMEOUT
    provider: int = 0           # the peer that answered
    info: dict = None           # the whole decoded body
    # node section
    name: str = ""
    uptime_us: int = 0
    wall_us: int = 0
    mem_in_use: int = 0
    mem_peak: int = 0
    alloc_calls: int = 0
    evicted_unsent: int = 0
    bp_waited_us: int = 0
    bp_waits: int = 0
    peers: int = 0
    max_peers: int = 0
    topics: int = 0
    max_topics: int = 0
    shm_tx: int = 0
    shm_rx: int = 0
    last_error: int = 0
    last_error_text: str = ""
    # the proc section, have_proc False where unmeasured
    have_proc: bool = False
    have_cpu: bool = False
    pid: int = 0
    cpu_us: int = 0
    rss: int = 0
    peak_rss: int = 0
    heap_total: int = 0
    heap_free: int = 0
    heap_min_free: int = 0
    heap_largest_free_block: int = 0

    @classmethod
    def _from_response(cls, r):
        s = cls(status=r.status, provider=r.provider)
        if not r.ok:
            return s
        body = r.value if isinstance(r.value, dict) else None
        info = body.get("info") if body else None
        if not isinstance(info, dict):
            return s
        s.info = info
        s.valid = True
        node = info.get("node")
        if isinstance(node, dict):
            g = node.get
            s.name = g("name", "") or ""
            s.uptime_us = int(g("uptime_us", 0)); s.wall_us = int(g("wall_us", 0))
            s.mem_in_use = int(g("mem_in_use", 0)); s.mem_peak = int(g("mem_peak", 0))
            s.alloc_calls = int(g("alloc_calls", 0)); s.evicted_unsent = int(g("evicted_unsent", 0))
            s.bp_waited_us = int(g("bp_waited_us", 0)); s.bp_waits = int(g("bp_waits", 0))
            s.peers = int(g("peers", 0)); s.max_peers = int(g("max_peers", 0))
            s.topics = int(g("topics", 0)); s.max_topics = int(g("max_topics", 0))
            s.shm_tx = int(g("shm_tx", 0)); s.shm_rx = int(g("shm_rx", 0))
            s.last_error = int(g("last_error", 0)); s.last_error_text = g("last_error_text", "") or ""
        proc = info.get("proc")
        if isinstance(proc, dict):
            g = proc.get
            s.have_proc = True
            s.have_cpu = "cpu_us" in proc
            s.pid = int(g("pid", 0)); s.cpu_us = int(g("cpu_us", 0))
            s.rss = int(g("rss", 0)); s.peak_rss = int(g("peak_rss", 0))
            s.heap_total = int(g("heap_total", 0)); s.heap_free = int(g("heap_free", 0))
            s.heap_min_free = int(g("heap_min_free", 0))
            s.heap_largest_free_block = int(g("heap_largest_free_block", 0))
        return s


@_dataclasses.dataclass
class Field:
    name: str
    kind: FieldType
    elem: FieldType
    count: int
    depth: int
    offset: int
    size: int
    str_cap: int = 0     # string capacity (STRING fields and STRING-element arrays)
    type_name: str = ""  # the field type's NAME ("Transform"), "" when anonymous
    elem_name: str = ""  # an array ELEMENT type's name, "" when anonymous
    elem_size: int = 0   # bytes of one array element, else 0
    arr_parent: int = 0xFFFF   # flat index of the enclosing struct ARRAY, 0xFFFF for none


# Reflection: the field type markers and the lazy per class schema specs.

class _Type:
    """A DART scalar field-type marker (dart.u8 ... dart.f64, dart.bool_)."""
    def __init__(self, kind, token):
        self.kind = kind
        self.token = token

    def __getitem__(self, count):
        if not isinstance(count, int) or count <= 0:
            raise TypeError("array size must be a positive int, e.g. dart.f64[9]")
        return _Array(self, count)

    def __repr__(self):
        return "dart." + self.token


class _String:
    """A capped string field marker: dart.string(16) is string<16>. Subscript it for a
        fixed array: dart.string(8)[4]."""
    def __init__(self, cap):
        if not isinstance(cap, int) or cap <= 0:
            raise TypeError("string cap must be a positive int, e.g. dart.string(16)")
        self.cap = cap

    def __getitem__(self, count):
        if not isinstance(count, int) or count <= 0:
            raise TypeError("array size must be a positive int, e.g. dart.string(8)[4]")
        return _Array(self, count)

    def __repr__(self):
        return "dart.string(%d)" % self.cap


class _Array:
    def __init__(self, elem, count):
        self.elem = elem     # a _Type or a _String
        self.count = count

    def __repr__(self):
        return "%r[%d]" % (self.elem, self.count)


u8 = _Type(_U8, "u8")
u16 = _Type(_U16, "u16")
u32 = _Type(_U32, "u32")
u64 = _Type(_U64, "u64")
i8 = _Type(_I8, "i8")
i16 = _Type(_I16, "i16")
i32 = _Type(_I32, "i32")
i64 = _Type(_I64, "i64")
f32 = _Type(_F32, "f32")
f64 = _Type(_F64, "f64")
bool_ = _Type(_BOOL, "bool")


def string(cap):
    """A capped UTF-8 string field: dart.string(16) is string<16> in the DSL. Every string
        field needs a cap, and dart.string(8)[4] is a fixed array of them."""
    return _String(cap)


# Enum fields come before the standard types below, which declare their own (the
# video family's format / codec / kind).

def _infer_enum_backing(cls):
    """Smallest scalar kind that fits every member value: unsigned when all >= 0, else
    signed. Cross-language matching wants an explicit backing (dart.enum(cls, dart.u8))."""
    vals = [int(m.value) for m in cls] or [0]
    lo, hi = min(vals), max(vals)
    if lo >= 0:
        return (_U8 if hi <= 0xFF else _U16 if hi <= 0xFFFF
                else _U32 if hi <= 0xFFFFFFFF else _U64)
    fits = lambda bits: lo >= -(1 << (bits - 1)) and hi <= (1 << (bits - 1)) - 1
    return _I8 if fits(8) else _I16 if fits(16) else _I32 if fits(32) else _I64


class _Enum:
    """An enum field marker (a named integer). Wraps an enum.Enum subclass whose member
    values ride the wire, plus the backing scalar kind."""
    def __init__(self, cls, backing=None):
        if not (isinstance(cls, type) and issubclass(cls, _pyenum.Enum)):
            raise TypeError("dart.enum expects an enum.Enum subclass")
        self.cls = cls
        if isinstance(backing, _Type):
            self.kind = backing.kind
        elif backing is None:
            self.kind = _infer_enum_backing(cls)
        else:
            raise TypeError("enum backing must be a dart scalar type, e.g. dart.u8")

    def __repr__(self):
        return "dart.enum(%s)" % self.cls.__name__


def enum(cls, backing=None):
    """A named integer field from an enum.Enum. Each member's value rides the wire. backing
        pins the wire width, else it is inferred. A bare enum annotation is the same."""
    return _Enum(cls, backing)


# The standard types (docs/stdtypes.md): pre named so two programs that both mean a 3D
# point say so with the same name and the same bytes. The names narrow matching.

class _StdAlias:
    """A standard type that is an alias of one wire type: it encodes like under but spells
        as its name, so its values are ordinary Python ints, bytes or str."""
    def __init__(self, name, under):
        self.name = name
        self.under = under      # a _Type, _Array or _String

    def __repr__(self):
        return "dart." + self.name


Timestamp = _StdAlias("Timestamp", i64)     # microseconds since the Unix epoch, UTC
Duration  = _StdAlias("Duration", i64)      # microseconds
Uuid      = _StdAlias("Uuid", u8[16])       # RFC 4122 byte order
Matrix3x3 = _StdAlias("Matrix3x3", f32[9])  # row-major
Matrix4x4 = _StdAlias("Matrix4x4", f32[16])
Uri       = _StdAlias("Uri", _String(256))


def _std(name):
    """Class decorator: this class IS the standard type `name`, so a field of it spells
    as the name (and must have the canonical shape, else compiling the schema fails)."""
    def wrap(cls):
        cls.__dart_std__ = name
        return _dataclasses.dataclass(cls)
    return wrap


@_std("Float2")
class Float2:
    x: f32 = 0.0
    y: f32 = 0.0


@_std("Float3")
class Float3:
    x: f32 = 0.0
    y: f32 = 0.0
    z: f32 = 0.0


@_std("Float4")
class Float4:
    x: f32 = 0.0
    y: f32 = 0.0
    z: f32 = 0.0
    w: f32 = 0.0


@_std("Double2")
class Double2:
    x: f64 = 0.0
    y: f64 = 0.0


@_std("Double3")
class Double3:
    x: f64 = 0.0
    y: f64 = 0.0
    z: f64 = 0.0


@_std("Double4")
class Double4:
    x: f64 = 0.0
    y: f64 = 0.0
    z: f64 = 0.0
    w: f64 = 0.0


@_std("Int2")
class Int2:
    x: i32 = 0
    y: i32 = 0


@_std("Int3")
class Int3:
    x: i32 = 0
    y: i32 = 0
    z: i32 = 0


@_std("Int4")
class Int4:
    x: i32 = 0
    y: i32 = 0
    z: i32 = 0
    w: i32 = 0


@_std("Quaternion")
class Quaternion:
    x: f64 = 0.0
    y: f64 = 0.0
    z: f64 = 0.0
    w: f64 = 1.0     # identity rotation


@_std("Color")
class Color:
    r: u8 = 0
    g: u8 = 0
    b: u8 = 0
    a: u8 = 255


@_std("Rect")
class Rect:
    x: f32 = 0.0
    y: f32 = 0.0
    w: f32 = 0.0
    h: f32 = 0.0


@_std("RectI")
class RectI:
    x: i32 = 0
    y: i32 = 0
    w: i32 = 0
    h: i32 = 0


# Meters and radians. parent "" = unstated, the cap keeps the packed 88 bytes 8 aligned.
@_std("Transform")
class Transform:
    translation: Double3 = _dataclasses.field(default_factory=Double3)
    rotation: Quaternion = _dataclasses.field(default_factory=Quaternion)
    parent: string(30) = ""


@_std("Twist")
class Twist:
    linear: Double3 = _dataclasses.field(default_factory=Double3)    # m/s
    angular: Double3 = _dataclasses.field(default_factory=Double3)   # rad/s


@_std("GeoPoint")
class GeoPoint:
    lat: f64 = 0.0      # degrees
    lon: f64 = 0.0      # degrees
    alt: f64 = 0.0      # meters


# The video family. Each member value IS the wire value, so the names and numbers
# below are the same in every binding.

class ImageFormat(_pyenum.IntEnum):
    """How an Image's data is laid out. A value >= 16 is a compressed container, so
    `data` holds the file bytes rather than pixels."""
    Mono8 = 0
    Mono16 = 1
    Rgb8 = 2
    Rgba8 = 3
    Bgr8 = 4
    Yuyv = 5
    Nv12 = 6
    Monof32 = 7
    Jpeg = 16
    Png = 17


class VideoCodec(_pyenum.IntEnum):
    """The codec a VideoFrame's data is encoded with. Unknown is the unstated
    codec hint (an ExternalVideoStream that does not state one)."""
    Unknown = 0
    Mjpeg = 1
    H264 = 2
    H265 = 3
    Av1 = 4


class VideoStreamKind(_pyenum.IntEnum):
    """The protocol an ExternalVideoStream's url speaks."""
    Rtsp = 0
    WebrtcWhep = 1
    Hls = 2
    Srt = 3
    Rtp = 4
    HttpMjpeg = 5
    Other = 15


@_std("Image")
class Image:
    width: u32 = 0
    height: u32 = 0
    stride: u32 = 0                             # bytes per row, 0 = tightly packed
    format: enum(ImageFormat, u8) = ImageFormat.Mono8
    data: list[u8] = b""                        # pixels, or the file bytes if compressed


@_std("VideoFrame")
class VideoFrame:
    codec: enum(VideoCodec, u8) = VideoCodec.Unknown
    width: u32 = 0                              # the coded size, 0 = unstated
    height: u32 = 0
    keyframe: bool_ = False
    pts: Timestamp = 0                          # presentation time, the Timestamp clock
    data: list[u8] = b""


# Fully fixed, so it works as a latched variable: hand a viewer a URL, not pixels. codec,
# width and height are hints for pickers, the stream stays authoritative once connected.
@_std("ExternalVideoStream")
class ExternalVideoStream:
    kind: enum(VideoStreamKind, u8) = VideoStreamKind.Rtsp
    codec: enum(VideoCodec, u8) = VideoCodec.Unknown
    width: u32 = 0
    height: u32 = 0
    url: Uri = ""
    name: string(32) = ""


class DistortionModel(_pyenum.IntEnum):
    """A lens distortion model. NoDistortion is an ideal pinhole."""
    NoDistortion = 0
    BrownConrady = 1
    Fisheye = 2
    Rational = 3


# The pinhole model and its lens distortion. coeffs is zero filled past the model's count.
@_std("CameraIntrinsics")
class CameraIntrinsics:
    width: u32 = 0                              # the resolution these numbers are valid for
    height: u32 = 0
    fx: f64 = 0.0
    fy: f64 = 0.0
    cx: f64 = 0.0
    cy: f64 = 0.0
    model: enum(DistortionModel, u8) = DistortionModel.NoDistortion
    coeffs: f64[8] = _dataclasses.field(default_factory=lambda: [0.0] * 8)


# SI: radians or meters, per second, and newtons or newton meters. velocity and effort
# may be empty. The names ride a JointNames variable, not every sample.
@_std("JointState")
class JointState:
    position: list[f64] = _dataclasses.field(default_factory=list)
    velocity: list[f64] = _dataclasses.field(default_factory=list)
    effort: list[f64] = _dataclasses.field(default_factory=list)


# Published once as a variable. The order every JointState array follows.
@_std("JointNames")
class JointNames:
    name: list[string(32)] = _dataclasses.field(default_factory=list)


def timestamp_now():
    """The wall clock in Timestamp units, microseconds since the Unix epoch UTC, the clock a
        message's written_us uses."""
    return int(_c.load().dart_timestamp_now())


class _FieldSpec:
    __slots__ = ("name", "kind", "elem", "count", "str_cap", "token", "nested", "enum_cls",
                 "type_name")

    def __init__(self, name, kind, elem=0, count=0, str_cap=0, token=None, nested=None,
                 enum_cls=None, type_name=None):
        self.type_name = type_name   # a standard type's NAME: the whole spelling
        self.name = name
        self.kind = kind
        self.elem = elem
        self.count = count
        self.str_cap = str_cap
        self.token = token
        self.nested = nested       # a _Spec for STRUCT fields
        self.enum_cls = enum_cls   # the enum.Enum class for ENUM fields (decode target)


class _Spec:
    """Reflected schema description of an annotated class (built lazily, cached)."""
    __slots__ = ("name", "fields", "cls")

    def __init__(self, name, fields, cls):
        self.name = name
        self.fields = fields
        self.cls = cls


_SPECS = {}
_SPECS_LOCK = _threading.RLock()   # reentrant: nested-struct reflection recurses under it


def _spec_of(cls):
    """The reflected schema spec of a class, built once and cached. Any annotated class works."""
    with _SPECS_LOCK:
        spec = _SPECS.get(cls)
        if spec is None:
            spec = _build_spec(cls)
            _SPECS[cls] = spec
        return spec


def _resolve(ann, g):
    return eval(ann, g, {}) if isinstance(ann, str) else ann   # future-annotation strings


def _var_array_spec(name, elem):
    """A VARR field spec from a list[...] element annotation (dart.<t>, dart.string(N),
    or int/float/bool). elem is the message tail's live element count, so no count here."""
    if isinstance(elem, _String):
        return _FieldSpec(name, _VARR, elem=_STR, str_cap=elem.cap)
    if isinstance(elem, _Type):
        return _FieldSpec(name, _VARR, elem=elem.kind, token=elem.token)
    if elem is int:
        return _FieldSpec(name, _VARR, elem=_I64, token="i64")
    if elem is float:
        return _FieldSpec(name, _VARR, elem=_F64, token="f64")
    if elem is bool:
        return _FieldSpec(name, _VARR, elem=_BOOL, token="bool")
    raise SchemaError("dart schema: variable array %r element must be dart.<t>, "
                      "dart.string(N), or int/float/bool (got %r)" % (name, elem))


def _field_spec(name, ann, g):
    ann = _resolve(ann, g)
    if isinstance(ann, _StdAlias):              # Timestamp / Uuid / Matrix4x4 / ...
        f = _field_spec(name, ann.under, g)     # encodes as what it aliases...
        f.type_name = ann.name                  # ...but spells as its name
        return f
    if isinstance(ann, _Type):
        return _FieldSpec(name, ann.kind, token=ann.token)
    if isinstance(ann, _String):
        return _FieldSpec(name, _STR, str_cap=ann.cap)
    if isinstance(ann, _Array):
        if isinstance(ann.elem, _String):
            return _FieldSpec(name, _ARR, elem=_STR, count=ann.count, str_cap=ann.elem.cap)
        return _FieldSpec(name, _ARR, elem=ann.elem.kind, count=ann.count, token=ann.elem.token)
    origin = getattr(ann, "__origin__", None)   # list[...] / dict[...] generic aliases
    if origin is list or origin is tuple:
        args = getattr(ann, "__args__", ())
        return _var_array_spec(name, args[0] if args else None)
    if ann is dict or origin is dict:
        return _FieldSpec(name, _MAP)           # self-describing tagged map
    if ann is str:
        return _FieldSpec(name, _VSTR)          # variable (unbounded) string
    if ann is int:
        return _FieldSpec(name, _I64, token="i64")
    if ann is float:
        return _FieldSpec(name, _F64, token="f64")
    if ann is bool:
        return _FieldSpec(name, _BOOL, token="bool")
    if isinstance(ann, _Enum):                                  # dart.enum(cls[, backing])
        return _FieldSpec(name, _ENUM, elem=ann.kind, enum_cls=ann.cls)
    if isinstance(ann, type) and issubclass(ann, _pyenum.Enum):   # a bare enum
        return _FieldSpec(name, _ENUM, elem=_infer_enum_backing(ann), enum_cls=ann)
    if isinstance(ann, type):                                    # a nested schema class,
        return _FieldSpec(name, _STRUCT, nested=_spec_of(ann),    # or a standard composite
                          type_name=getattr(ann, "__dart_std__", None))
    raise SchemaError("dart schema: field %r has unsupported type %r (use dart.u8..f64, "
                      "dart.bool_, dart.string(N), dart.<t>[N], str, list[dart.<t>], dict, "
                      "a nested schema class, or int/float/bool)" % (name, ann))


def _build_spec(cls):
    mod = _sys.modules.get(cls.__module__)
    g = getattr(mod, "__dict__", {})
    anns = getattr(cls, "__annotations__", {})
    if not anns:
        raise SchemaError("dart schema: %s has no annotated fields to map" % cls.__name__)
    name = getattr(cls, "__dart_name__", None) or cls.__name__
    return _Spec(name, [_field_spec(n, a, g) for n, a in anns.items()], cls)


def _value_spec(ann):
    """A spec for a bare type used as the whole schema: the root is anonymous, one field
        named "". None if ann is not a bare type."""
    if isinstance(ann, (_Type, _String, _Array, _Enum)):
        return _Spec(None, [_field_spec("", ann, {})], None)
    if getattr(ann, "__origin__", None) in (list, tuple, dict):   # list[...] IS a type: check first
        return _Spec(None, [_field_spec("", ann, {})], None)
    if isinstance(ann, type):
        if ann in (str, int, float, bool, dict) or issubclass(ann, _pyenum.Enum):
            return _Spec(None, [_field_spec("", ann, {})], None)
        return None                              # an annotated schema class: a struct root
    return None


def _elem_token(elem, str_cap):
    return "string<%d>" % str_cap if elem == _STR else _TOKEN[elem]


def _spec_text(spec):
    """The schema DSL text a spec compiles to. Pure reflection, no native lib."""
    def type_text(f):
        if f.type_name:               # a standard type: the name IS the spelling
            return f.type_name
        if f.kind == _STRUCT:
            return "{ %s }" % ", ".join(line(g) for g in f.nested.fields)
        if f.kind == _ARR:
            return "%s[%d]" % (_elem_token(f.elem, f.str_cap), f.count)
        if f.kind == _VARR:
            return "%s[]" % _elem_token(f.elem, f.str_cap)
        if f.kind == _STR:
            return "string<%d>" % f.str_cap
        if f.kind == _VSTR:
            return "string"
        if f.kind == _MAP:
            return "map"
        if f.kind == _ENUM:
            opts = ", ".join("%s=%d" % (m.name, int(m.value)) for m in f.enum_cls)
            return "enum<%s> { %s }" % (_TOKEN[f.elem], opts)
        return f.token
    def line(f):
        return "%s: %s" % (f.name, type_text(f))
    if spec.name is None:                     # a bare type: the whole schema is its spelling
        return type_text(spec.fields[0]) + "\n"
    return spec.name + "\n{\n" + ",\n".join("    " + line(f) for f in spec.fields) + "\n}\n"


def _enum_body_from_schema(lib, s, field):
    """`{ Name=value, ... }` read from a compiled schema's enum option table."""
    parts = []
    for k in range(lib.dart_schema_enum_count(s, field)):
        val = _c.c_int64()
        nm = _c.DartStringView()
        if lib.dart_schema_enum_variant(s, field, k, _c.byref(val), _c.byref(nm)):
            parts.append("%s=%d" % (_dstr(nm), val.value))
    return "{ %s }" % ", ".join(parts)


def _is_value_root(lib, s):
    """True when the schema is a BARE TYPE: an unnamed root of one anonymous field, so its
    message is a single value (encode/decode take and give that value, not a dict)."""
    if lib.dart_schema_field_count(s) != 1 or lib.dart_schema_name(s).len != 0:
        return False
    info = _c.DartSchemaFieldInfo()
    return (bool(lib.dart_schema_field_at(s, 0, _c.byref(info)))
            and info.depth == 0 and info.name.len == 0 and info.kind != _STRUCT)


def _schema_dsl(lib, s):
    """The DSL text of any compiled schema, straight from the C printer, so there is exactly
        one implementation of the spelling."""
    need = lib.dart_schema_print(s, None, 0)
    if not need:
        return ""
    buf = _c.create_string_buffer(need + 1)
    lib.dart_schema_print(s, buf, need + 1)
    return buf.value.decode("utf-8", "replace")


# Schema: a compiled message schema plus encode and decode. Build with Schema(text) or
# Schema(cls), and Topic[T] builds one for you.

def _dstr(s):
    return _c.string_at(s.data, s.len).decode("utf-8", "replace") if s.data and s.len else ""


def _compile_dsl(text):
    lib = _c.load()
    err = _c.c_char_p()
    s = lib.dart_schema_compile(_c.schema_alloc(), None, text.encode("utf-8"), _c.byref(err))
    if not s:
        near = err.value.decode("utf-8", "replace") if err.value else "?"
        raise SchemaError("schema compile failed near: " + near)
    return s


class Schema:
    """A compiled schema from DSL text, a schema class whose annotated fields are the wire
        fields, or a bare type whose message is that one value (docs/python.md)."""

    __slots__ = ("_s", "_spec", "_vroot")

    def __init__(self, source):
        self._spec = None
        self._vroot = None
        if isinstance(source, str):
            self._s = _compile_dsl(source)
            return
        self._spec = _value_spec(source)
        if self._spec is None:
            if not isinstance(source, type):
                raise TypeError("Schema(...) expects DSL text, a schema class, or a bare "
                                "type (dart.u8, dart.string(16), list[dart.f32], dict, ...)")
            self._spec = _spec_of(source)
        self._s = _compile_dsl(_spec_text(self._spec))

    @property
    def name(self):
        return _dstr(_c.load().dart_schema_name(self._s))

    @property
    def size(self):
        return _c.load().dart_schema_size(self._s)

    @property
    def hash(self):
        return _c.load().dart_schema_hash(self._s)

    @property
    def wire(self):
        b = _c.load().dart_schema_wire(self._s)
        return _c.string_at(b.data, b.len) if b.data and b.len else b""

    @property
    def dsl(self):
        """The schema's DSL text, reconstructed from the compiled form. Paste this
        into a C/C++ node's dart_schema_compile for interop, or just print it."""
        return _schema_dsl(_c.load(), self._s)

    @property
    def field_count(self):
        return _c.load().dart_schema_field_count(self._s)

    @property
    def is_value_root(self):
        """True when this schema is a BARE TYPE: its message is one value, so encode takes
        and decode returns that value instead of a field dict."""
        if self._vroot is None:
            self._vroot = _is_value_root(_c.load(), self._s)   # probed once
        return self._vroot

    def can_read(self, pub):
        """Can a reader declaring this schema read messages written with pub? Type names
                narrow: an anonymous type reads a named one of the same shape, never the reverse."""
        return bool(_c.load().dart_schema_subset(self._s, pub._s))

    def fields(self):
        lib = _c.load()
        out = []
        info = _c.DartSchemaFieldInfo()
        for i in range(lib.dart_schema_field_count(self._s)):
            if lib.dart_schema_field_at(self._s, i, _c.byref(info)):
                out.append(Field(_dstr(info.name), FieldType(info.kind),
                                 FieldType(info.elem), info.count, info.depth,
                                 info.offset, info.size, info.str_cap,
                                 _dstr(info.type_name), _dstr(info.elem_name),
                                 info.elem_size, info.arr_parent))
        return out

    def encode(self, value):
        """Encode a mapping or an object (attributes by field name) to message bytes."""
        return _encode(_c.load(), self._s, value)

    def decode(self, data):
        """Decode message bytes to a nested dict (any schema), the bare value (a bare-type
        schema), or a typed instance if this schema was built from a class."""
        return _from_decoded(self._spec, _decode(_c.load(), self._s, bytes(data)))

    def __del__(self):
        try:
            if self._s:
                _c.load().dart_schema_free(self._s, _c.schema_alloc(), None)
                self._s = None
        except Exception:
            pass


def _as_schema(schema_arg):
    if schema_arg is None:
        return None
    if isinstance(schema_arg, Schema):
        return schema_arg
    return Schema(schema_arg)      # DSL text, a schema class, or a bare type


def dsl(x):
    """The DSL text for a schema source: a class or bare type with no lib load, a compiled
        Schema, or DSL text returned as is. For display or for a C node."""
    if isinstance(x, str):
        return x
    if isinstance(x, Schema):
        return x.dsl
    spec = _value_spec(x)
    if spec is not None:
        return _spec_text(spec)
    if isinstance(x, type):
        return _spec_text(_spec_of(x))
    raise TypeError("dsl() expects a schema class, a bare type, a Schema, or DSL text")


# Encode and decode walk the compiled schema's flat depth first field table, so both
# work for any schema, reflected, text compiled or a peer's.

def _get(container, name):
    if isinstance(container, dict):
        return container.get(name)          # missing is None, which keeps the zeroed default
    return getattr(container, name, None)


def _pack_array(elem_kind, val):
    if isinstance(val, (bytes, bytearray, memoryview)):
        return bytes(val)
    fmt = _FMT[elem_kind]
    if elem_kind in _FLOATK:
        return _struct.pack("<%d%s" % (len(val), fmt), *(float(x) for x in val))
    return _struct.pack("<%d%s" % (len(val), fmt), *(int(x) for x in val))


def _str_view(val, keep):
    sb = val.encode("utf-8") if isinstance(val, str) else bytes(val)
    keep.append(sb)   # keep the buffer alive across the setter call
    return _c.DartStringView(_c.cast(_c.c_char_p(sb), _c.c_void_p) if sb else None, len(sb))


def _bytes_view(b, keep):
    keep.append(b)    # keep the buffer alive across the setter call
    return _c.DartBytes(_c.cast(_c.c_char_p(b), _c.c_void_p) if b else None, len(b))


def _pack_string_slots(strings, cap):
    """A variable string array's frame: whole [u16 len][cap bytes] slots (the same
    layout _unpack_string_array reads back), one per element, live count = element count."""
    out = bytearray()
    for item in strings:
        sb = item.encode("utf-8") if isinstance(item, str) else bytes(item)
        if len(sb) > cap:
            raise SchemaError("string too long (cap %d): %r" % (cap, item))
        out += _struct.pack("<H", len(sb)) + sb + b"\x00" * (cap - len(sb))
    return bytes(out)


# The map body, the self describing tagged value tree of a map field, built and parsed
# here rather than mirroring the C writer. spec/schema.md has the wire, the C validates.

def _map_int_bytes(v):
    if v < 0:
        if -0x80 <= v <= 0x7f:               k, fmt = _I8, "<b"
        elif -0x8000 <= v <= 0x7fff:         k, fmt = _I16, "<h"
        elif -0x80000000 <= v <= 0x7fffffff: k, fmt = _I32, "<i"
        else:                                k, fmt = _I64, "<q"
    else:
        if v <= 0xff:                        k, fmt = _U8, "<B"
        elif v <= 0xffff:                    k, fmt = _U16, "<H"
        elif v <= 0xffffffff:                k, fmt = _U32, "<I"
        else:                                k, fmt = _U64, "<Q"
    return bytes((k,)) + _struct.pack(fmt, v)


def _encode_map_value(v):
    if isinstance(v, bool):
        return bytes((_BOOL, 1 if v else 0))
    if isinstance(v, int):
        return _map_int_bytes(v)
    if isinstance(v, float):
        return bytes((_F64,)) + _struct.pack("<d", v)
    if isinstance(v, str):
        sb = v.encode("utf-8")
        return bytes((_VSTR,)) + _struct.pack("<H", len(sb)) + sb
    if isinstance(v, dict):
        return bytes((_MAP,)) + _encode_map_body(v)
    if isinstance(v, (list, tuple)):
        return bytes((_VARR,)) + _struct.pack("<H", len(v)) + b"".join(_encode_map_value(x) for x in v)
    raise SchemaError("dart map: unsupported value type %r (use int/float/bool/str/dict/list)"
                      % type(v).__name__)


def _encode_map_body(d):
    if not isinstance(d, dict):
        raise SchemaError("dart map field expects a dict, got %r" % type(d).__name__)
    parts = []
    for key, val in d.items():
        if val is None:
            continue                    # a map has no null kind, omit the key
        kb = str(key).encode("utf-8")
        if len(kb) > 255:
            raise SchemaError("dart map: key too long (max 255 bytes): %r" % key)
        parts.append(bytes((len(kb),)) + kb + _encode_map_value(val))
    return _struct.pack("<H", len(parts)) + b"".join(parts)


def _encode(lib, s, src):
    # One walk of the flattened field table: resolve each value, and pre-serialize the
    # variable-field payloads so the tail can be sized before the buffer is allocated.
    if _is_value_root(lib, s):
        src = {"": src}              # a bare type: the value IS the one anonymous field
    names, srcs, ops = [], [src], []
    var_bytes = 0
    for i in range(lib.dart_schema_field_count(s)):
        info = _c.DartSchemaFieldInfo()
        lib.dart_schema_field_at(s, i, _c.byref(info))
        name = _dstr(info.name)
        d = info.depth
        while len(names) <= d:
            names.append(None)
        names[d] = name
        parent = srcs[d]
        val = None if parent is None else _get(parent, name)
        if info.kind == _STRUCT:
            while len(srcs) <= d + 1:
                srcs.append(None)
            srcs[d + 1] = val            # None keeps the whole subtree at its zeroed default
            continue
        if val is None:
            continue                     # null field: keep the zeroed default
        path = ".".join(names[:d + 1]).encode("utf-8")
        kind, elem, count, cap = info.kind, info.elem, info.count, info.str_cap
        if kind == _VSTR:
            payload = val.encode("utf-8") if isinstance(val, str) else bytes(val)
            var_bytes += len(payload)
            ops.append((kind, elem, count, cap, path, payload))
        elif kind == _VARR:
            payload = _pack_string_slots(val, cap) if elem == _STR else _pack_array(elem, val)
            var_bytes += len(payload)
            ops.append((kind, elem, count, cap, path, payload))
        elif kind == _MAP:
            payload = _encode_map_body(val)
            var_bytes += len(payload)
            ops.append((kind, elem, count, cap, path, payload))
        else:
            ops.append((kind, elem, count, cap, path, val))

    # msg_min is the fixed section plus one empty frame per variable field, and each
    # variable frame then grows by exactly its payload length
    size = lib.dart_schema_msg_min(s) + var_bytes
    buf = (_c.c_ubyte * (size or 1))()
    lib.dart_schema_message_default(s, buf, size)
    keep = []
    for kind, elem, count, cap, path, val in ops:
        if kind == _STR:
            if not lib.dart_set_string(buf, size, s, path, _str_view(val, keep)):
                raise SchemaError("string too long for %s (cap %d)" % (path.decode("utf-8"), cap))
        elif kind == _VSTR:
            lib.dart_set_string(buf, size, s, path, _str_view(val, keep))
        elif kind == _ARR and elem == _STR:
            for j, item in enumerate(val):
                if j >= count:
                    break
                if not lib.dart_set_string_at(buf, size, s, path, j, _str_view(item, keep)):
                    raise SchemaError("string too long for %s[%d] (cap %d)"
                                      % (path.decode("utf-8"), j, cap))
        elif kind == _ARR:
            lib.dart_set_array(buf, size, s, path, _bytes_view(_pack_array(elem, val), keep))
        elif kind == _VARR:
            lib.dart_set_array(buf, size, s, path, _bytes_view(val, keep))
        elif kind == _MAP:
            if not lib.dart_set_map(buf, size, s, path, _bytes_view(val, keep)):
                raise SchemaError("invalid map for %s" % path.decode("utf-8"))
        elif kind == _F32:
            lib.dart_set_f32(buf, size, s, path, float(val))
        elif kind == _F64:
            lib.dart_set_f64(buf, size, s, path, float(val))
        elif kind == _ENUM:
            if isinstance(val, str):                       # by option name
                if not lib.dart_set_enum(buf, size, s, path, val.encode("utf-8")):
                    raise SchemaError("unknown enum option %r for %s" % (val, path.decode("utf-8")))
            else:                                          # by number (an int or an enum member)
                n = int(val.value if isinstance(val, _pyenum.Enum) else val)
                if elem in _SIGNED:
                    lib.dart_set_int(buf, size, s, path, n)
                else:
                    lib.dart_set_uint(buf, size, s, path, n)
        elif kind in _SIGNED:
            lib.dart_set_int(buf, size, s, path, int(val))
        elif kind == _BOOL:
            lib.dart_set_uint(buf, size, s, path, 1 if val else 0)
        else:
            lib.dart_set_uint(buf, size, s, path, int(val))
    return bytes(buf)[:lib.dart_schema_msg_len(s, buf, size)]


# string array slots are [u16 len][cap bytes] each. Clamp len like the C reader so a
# hostile message can never over read
def _unpack_string_array(raw, count, cap):
    out = []
    slot = 2 + cap
    for i in range(count):
        off = i * slot
        if off + 2 > len(raw):
            out.append("")
            continue
        ln = raw[off] | (raw[off + 1] << 8)
        if ln > cap:
            ln = cap
        if off + 2 + ln > len(raw):
            ln = len(raw) - off - 2
        out.append(raw[off + 2:off + 2 + ln].decode("utf-8", "replace"))
    return out


# Parse a map body (see _encode_map_body) back to a dict. Fully bounds-checked and
# tolerant: a short/hostile body never over-reads, it just stops early.
def _decode_map_value(buf, off, end, depth):
    if off >= end:
        return None, end
    kind = buf[off]
    off += 1
    if kind == _BOOL:
        return (bool(buf[off]), off + 1) if off < end else (False, end)
    if kind in _SCALAR_SIZE:            # U8..F64 (BOOL handled above)
        sz = _SCALAR_SIZE[kind]
        if off + sz > end:
            return 0, end
        return _struct.unpack_from("<" + _FMT[kind], buf, off)[0], off + sz
    if kind == _VSTR:
        if off + 2 > end:
            return "", end
        ln = buf[off] | (buf[off + 1] << 8)
        off += 2
        if off + ln > end:
            ln = end - off
        return buf[off:off + ln].decode("utf-8", "replace"), off + ln
    if kind == _VARR:
        if off + 2 > end:
            return [], end
        n = buf[off] | (buf[off + 1] << 8)
        off += 2
        out = []
        for _ in range(n):
            if off >= end:
                break
            v, off = _decode_map_value(buf, off, end, depth)
            out.append(v)
        return out, off
    if kind == _MAP:
        return _decode_map_body(buf, off, end, depth + 1)
    return None, end                    # unknown kind: stop cleanly


def _decode_map_body(buf, off, end, depth=1):
    if depth > 8 or off + 2 > end:      # DART_SCHEMA_MAX_DEPTH
        return {}, end
    n = buf[off] | (buf[off + 1] << 8)
    off += 2
    out = {}
    for _ in range(n):
        if off >= end:
            break
        klen = buf[off]
        off += 1
        if off + klen > end:
            break
        key = buf[off:off + klen].decode("utf-8", "replace")
        off += klen
        out[key], off = _decode_map_value(buf, off, end, depth)
    return out, off


def _value_to_py(val):
    k = val.kind
    if k == _STR or k == _VSTR:
        return (_c.string_at(val.bytes.data, val.bytes.len).decode("utf-8", "replace")
                if val.bytes.data and val.bytes.len else "")
    if k == _ARR or k == _VARR:
        raw = _c.string_at(val.bytes.data, val.bytes.len) if val.bytes.data and val.bytes.len else b""
        if val.elem == _STR:
            return _unpack_string_array(raw, val.count, val.str_cap)
        if val.elem in (_U8, _I8):
            return raw
        esz = _SCALAR_SIZE[val.elem]
        return list(_struct.unpack("<%d%s" % (len(raw) // esz, _FMT[val.elem]), raw)) if raw else []
    if k == _MAP:
        raw = _c.string_at(val.bytes.data, val.bytes.len) if val.bytes.data and val.bytes.len else b""
        return _decode_map_body(raw, 0, len(raw))[0]
    if k in _FLOATK:
        return val.v.f
    if k == _ENUM:
        return val.v.i          # the number. _to_obj maps it to the enum member for a typed decode
    if k in _SIGNED:
        return val.v.i
    if k == _BOOL:
        return bool(val.v.u)
    return val.v.u


def _decode(lib, s, msg):
    mb = _c.DartBytes(_c.cast(_c.c_char_p(msg), _c.c_void_p) if msg else None, len(msg))
    if _is_value_root(lib, s):       # a bare type: hand back the value, not a one-key dict
        val = _c.DartValue()
        lib.dart_get_value(mb, s, 0, _c.byref(val))
        return _value_to_py(val)
    root = {}
    dests = [root]
    for i in range(lib.dart_schema_field_count(s)):
        info = _c.DartSchemaFieldInfo()
        lib.dart_schema_field_at(s, i, _c.byref(info))
        name = _dstr(info.name)
        d = info.depth
        parent = dests[d]
        if info.kind == _STRUCT:
            child = {}
            parent[name] = child
            while len(dests) <= d + 1:
                dests.append(None)
            dests[d + 1] = child
        else:
            val = _c.DartValue()
            lib.dart_get_value(mb, s, i, _c.byref(val))
            parent[name] = _value_to_py(val)
    return root


def _instantiate(cls, kwargs):
    try:
        return cls(**kwargs)               # dataclass / matching keyword __init__
    except TypeError:
        obj = cls.__new__(cls)             # plain annotated class: set fields directly
        for k, v in kwargs.items():
            setattr(obj, k, v)
        return obj


def _to_obj(spec, d):
    kwargs = {}
    for f in spec.fields:
        v = d.get(f.name)
        if f.kind == _STRUCT and v is not None:
            kwargs[f.name] = _to_obj(f.nested, v)
        elif f.kind == _ENUM and v is not None and f.enum_cls is not None:
            try:
                kwargs[f.name] = f.enum_cls(v)      # map the number to the enum member
            except ValueError:
                kwargs[f.name] = v                  # unknown/newer value: keep the raw int
        else:
            kwargs[f.name] = v
    return _instantiate(spec.cls, kwargs)


def _from_decoded(spec, d):
    """Apply a spec's typing to what _decode produced: a class instance, the bare value (an
        enum member where the spec names one), or the plain form with no spec."""
    if spec is None:
        return d
    if spec.name is None:                       # a bare type: d IS the value
        f = spec.fields[0]
        if f.kind == _ENUM and f.enum_cls is not None and d is not None:
            try:
                return f.enum_cls(d)            # the number as its enum member
            except ValueError:
                return d                        # an unknown/newer number stays raw
        return d
    return _to_obj(spec, d)


# The public runtime API.

def _send_status(r):
    return SendStatus(r) if r in SendStatus._value2member_map_ else r


def _c_view(payload):
    """(DartBytes, keepalive buffer) over a bytes payload. Hold the buffer across the call."""
    b = _c.DartBytes()
    buf = None
    if payload:
        buf = (_c.c_ubyte * len(payload)).from_buffer_copy(payload)
        b.data = _c.cast(buf, _c.c_void_p)
        b.len = len(payload)
    return b, buf


def _payload_bytes(sch, value):
    """Encode a payload: bytes and str pass through, None = empty, anything else goes through
        the schema. A bare string schema encodes a str as a framed value, not loose text."""
    if value is None:
        return b""
    if isinstance(value, (bytes, bytearray, memoryview)):
        return bytes(value)
    if isinstance(value, str) and (sch is None or not sch.is_value_root):
        return value.encode("utf-8")
    if sch is None:
        raise TypeError("no schema on this handle; pass bytes/str, or construct "
                        "the typed [T] form / pass schema= to send objects")
    return sch.encode(value)


def _decode_payload(wire_schema, local, data):
    """Decode payload bytes: the wire schema wins, else the local Schema, else the raw bytes
        pass through. Returns (value, ok). A typed local schema yields its class instance."""
    sp = wire_schema or (local._s if local is not None else None)
    if not sp:
        return data, True
    try:
        d = _decode(_c.load(), sp, data)
        return _from_decoded(local._spec if local is not None else None, d), True
    except Exception:
        return None, False


def _arity(fn):
    """1 or 2: how many positional args a handler accepts. Bound methods drop self and
        *args counts as the 2 arg form."""
    try:
        params = _inspect.signature(fn).parameters.values()
    except (TypeError, ValueError):
        return 1
    n = 0
    for p in params:
        if p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD):
            n += 1
        elif p.kind == p.VAR_POSITIONAL:
            return 2
    return 2 if n >= 2 else 1


# role to pub and sub bit pair, bit 0 = pub, bit 1 = sub, for same name role widening
def _role_bits(role):
    role = int(role)
    return (3 if role == Role.PUBSUB else 1 if role == Role.PUB_ONLY
            else 2 if role == Role.SUB_ONLY else 0)


def _role_from_bits(bits):
    return (Role.PUBSUB if bits == 3 else Role.PUB_ONLY if bits == 1
            else Role.SUB_ONLY if bits == 2 else Role.INACTIVE)


class Message:
    """A delivered message, copied out so it outlives the callback. value is the decoded
        object and fields the dict. recv_us and written_us are explained in docs/node.md."""
    __slots__ = ("topic_index", "publisher_id", "publisher_name", "topic_name",
                 "data", "recv_us", "written_us", "capture_us", "fields", "value")

    @classmethod
    def _from_c(cls, m, spec):
        self = cls.__new__(cls)
        self.topic_index = m.topic_index
        self.publisher_id = m.publisher_id
        self.publisher_name = _dstr(m.publisher_name)
        self.topic_name = _dstr(m.topic_name)
        self.recv_us = m.recv_us
        self.written_us = m.written_us
        self.capture_us = m.capture_us
        self.data = _c.string_at(m.data.data, m.data.len) if m.data.data and m.data.len else b""
        self.fields = None
        self.value = None
        if m.schema:
            try:
                self.fields = _decode(_c.load(), m.schema, self.data)
                self.value = _from_decoded(spec, self.fields)
            except Exception:
                _traceback.print_exc()
        return self

    @property
    def text(self):
        return self.data.decode("utf-8", "replace")

    def __repr__(self):
        return "Message(topic=%r, from=%r, %d bytes)" % (
            self.topic_name, self.publisher_name, len(self.data))


class Event:
    """A peer, message loss or error notification. Everything that went wrong arrives as
        EventKind.ERROR with error set, and str(event) is the text either way."""
    __slots__ = ("kind", "error", "topic_name", "peer", "topic", "os_error",
                 "lost_first", "lost_count", "too_big_bytes", "identity",
                 "publish_topics", "receive_topics", "schema_detail", "peer_name", "_line")

    @classmethod
    def _from_c(cls, e):
        self = cls.__new__(cls)
        self.kind = EventKind(e.kind) if e.kind in EventKind._value2member_map_ else e.kind
        self.error = ErrorKind(e.error) if e.error in ErrorKind._value2member_map_ else e.error
        self.topic_name = e.topic_name.decode("utf-8", "replace") if e.topic_name else None
        self.peer = e.peer
        self.topic = e.topic
        self.os_error = e.os_error
        self.lost_first = e.lost_first
        self.lost_count = e.lost_count
        self.too_big_bytes = e.too_big_bytes
        self.identity = e.identity
        self.publish_topics = e.publish_topics
        self.receive_topics = e.receive_topics
        self.schema_detail = (e.schema_detail.decode("utf-8", "replace")
                              if e.schema_detail else None)
        self.peer_name = e.peer_name.decode("utf-8", "replace") if e.peer_name else None
        buf = _c.create_string_buffer(192)
        _c.load().dart_event_str(_c.byref(e), buf, len(buf))
        self._line = buf.value.decode("utf-8", "replace")
        return self

    @property
    def is_error(self):
        """True if this event reports a failure (kind == EventKind.ERROR)."""
        return self.kind == EventKind.ERROR

    def __str__(self):
        return self._line

    def __repr__(self):
        return "Event(%s)" % self._line


class _TypedTopic:
    """The result of Topic[T]: a typed-topic factory bound to schema class (or bare
    type) T, callable exactly like the Topic constructor minus the schema argument."""
    __slots__ = ("_type",)

    def __init__(self, cls_type):
        self._type = cls_type

    def __call__(self, node, name, role=Role.PUBSUB, qos=None, **qos_kwargs):
        return Topic(node, name, self._type, role, qos, **qos_kwargs)

    def __repr__(self):
        return "dart.Topic[%s]" % getattr(self._type, "__name__", self._type)


class Topic:
    """A topic handle owned by the Node. Topic(node, name) is raw, a schema argument makes
        it typed, and Topic[T](node, name) is the typed shorthand."""
    __slots__ = ("_node", "_h", "_schema", "_name")

    def __init__(self, node, name, schema=None, role=Role.PUBSUB, qos=None, **qos_kwargs):
        sch = _as_schema(schema)
        self._node = node
        self._name = name
        self._schema = sch
        self._h = node._create_or_share(name, role, sch, qos, qos_kwargs)

    @classmethod
    def _from_handle(cls, node, handle):
        # wrap an already-existing native handle (e.g. a @dart/log topic): no name
        # registry entry, no schema. send/set_role/counts/query like any topic.
        t = cls.__new__(cls)
        t._node = node
        t._name = None
        t._schema = None
        t._h = handle
        return t

    def __class_getitem__(cls, item):
        return _TypedTopic(item)

    @property
    def name(self):
        return self._name

    def _ptr(self):
        # NULL once the node is closed, so the C answers NO_TOPIC instead of touching freed memory
        return self._h if self._node._h else None

    def send(self, data, capture_us=0):
        """Publish. data is bytes or str for raw, else a mapping or object encoded by the
                schema. A bare string schema takes a str. capture_us is when the data was
                true rather than when it was sent, 0 = unstated. Returns a SendStatus."""
        if isinstance(data, (bytes, bytearray, memoryview)):
            payload = bytes(data)
        elif isinstance(data, str) and (self._schema is None
                                        or not self._schema.is_value_root):
            payload = data.encode("utf-8")
        else:
            if self._schema is None:
                raise TypeError("topic has no schema; send bytes/str, or create the "
                                "topic with a schema to send objects/dicts")
            payload = self._schema.encode(data)
        b, buf = _c_view(payload)
        o = _c.DartSendOpts(capture_us=int(capture_us or 0))
        r = self._node._lib.dart_topic_send(self._ptr(), b, _c.byref(o))
        del buf
        return _send_status(r)

    def set_role(self, role):
        return _send_status(self._node._lib.dart_topic_set_role(self._ptr(), int(role)))

    def retire(self):
        """Retire the topic so the name can be re created with another schema (docs/topics.md).
                On OK this handle is invalid. Refused with STATE from a callback."""
        node = self._node
        with node._create_lock:
            if not self._ptr():
                return _send_status(-1)
            idx = node._lib.dart_topic_index(self._ptr())
            r = node._lib.dart_topic_retire(self._h)
            if r == 0:
                for nm, rec in list(node._topics_by_name.items()):
                    if rec[0] == self._h:
                        del node._topics_by_name[nm]
                        break
                # the slot may be reused by a different topic: its old decode spec and
                # message handlers must never apply to the successor
                node._topic_specs.pop(idx, None)
                node._sub_handlers.pop(idx, None)
                self._h = None
            return _send_status(r)

    @property
    def index(self):
        return self._node._lib.dart_topic_index(self._ptr())

    def match_count(self):
        return self._node._lib.dart_topic_match_count(self._ptr())

    def ready(self):
        """True when a send would not wait on the match wait: a subscriber is matched, or
                matching has converged. For a GUI: disable the wait, park payloads while False."""
        return self._node._lib.dart_topic_ready(self._ptr()) == 1

    def pending_count(self):
        """Unresolved candidate matches right now. 0 = matching has converged for every known
                peer."""
        return self._node._lib.dart_topic_pending_count(self._ptr())

    def drain(self, timeout_ms):
        """Pump until every reader has acked, or timeout. Call before close."""
        return self._node._lib.dart_topic_drain(self._ptr(), timeout_ms) == 1

    def take(self, timeout_ms=0):
        """Pop the next queued message, copied out, or None if nothing arrived in timeout_ms
                (0 = check, negative = forever). The first take or dispatch queues the topic."""
        if self._ptr() is None:
            return None
        m = _c.DartMsg()
        r = self._node._lib.dart_topic_take(self._ptr(), _c.byref(m), timeout_ms)
        if r < 0:
            raise RuntimeError("take failed (%s)" % (SendStatus(r)
                               if r in SendStatus._value2member_map_ else r))
        if r != 1:
            return None
        return Message._from_c(m, self._node._topic_specs.get(m.topic_index))

    def dispatch(self, max_msgs=0, timeout_ms=0):
        """Drain the queue by running on_message on the calling thread, oldest first, up to
                max_msgs (0 = all), waiting like take. These run without the node lock."""
        return self._node._lib.dart_topic_dispatch(self._ptr(), max_msgs, timeout_ms)

    def queue_stats(self):
        """(messages, bytes, capacity, dropped) of the consumer queue, zeros when not queued."""
        m, b, c, d = _c.c_uint32(), _c.c_uint32(), _c.c_uint32(), _c.c_uint32()
        self._node._lib.dart_topic_queue_stats(self._ptr(), _c.byref(m), _c.byref(b),
                                                 _c.byref(c), _c.byref(d))
        return m.value, b.value, c.value, d.value

    def counts(self):
        """(tx_msgs, tx_bytes, rx_msgs, rx_bytes), the cumulative traffic this node committed to
                the topic and delivered from it. Always on."""
        tm, tb, rm, rb = _c.c_uint64(), _c.c_uint64(), _c.c_uint64(), _c.c_uint64()
        self._node._lib.dart_topic_counts(self._ptr(), _c.byref(tm), _c.byref(tb),
                                           _c.byref(rm), _c.byref(rb))
        return tm.value, tb.value, rm.value, rb.value

    @property
    def schema(self):
        return self._schema


class Node:
    """A DART node: owns sockets, discovery, and topics. Construct it directly:
    Node(name, on_message, on_event, **opts)."""

    __slots__ = ("_lib", "_h", "_id", "_alloc", "_on_msg", "_on_evt",
                 "_topic_specs", "_schemas", "_topics_by_name", "_create_lock",
                 "_sub_handlers", "_pattern_boxes", "_async_live", "_pat_lock", "_name")

    def __init__(self, name, on_message, on_event, options=None, **opts):
        """Open a node. An empty name is auto generated. on_message may be None, on_event is
                required. Other config is keyword args or options=NodeOptions(...)."""
        self._lib = None
        self._name = name or None
        self._h = None
        self._id = None
        self._alloc = None
        self._on_msg = on_message
        self._on_evt = on_event
        self._topic_specs = {}
        self._schemas = []       # compiled schemas kept alive for the node's life
        self._topics_by_name = {}   # name to [handle, role bits, schema hash]
        self._create_lock = _threading.Lock()
        self._sub_handlers = {}     # topic index to [Message handlers], for Subscriber
        self._pattern_boxes = []    # pattern handler box ids (reaped at close)
        self._async_live = set()    # in-flight async-call box ids
        self._pat_lock = _threading.Lock()
        if on_event is None:
            raise ValueError("on_event is required: it carries diagnostics that "
                             "must never be missed (pass e.g. print)")
        if options is None:
            options = NodeOptions(**opts)
        elif opts:
            raise TypeError("pass either options= or keyword options, not both")
        self._lib = lib = _c.load()

        with _REG_LOCK:
            global _NEXT_ID
            self._id = _NEXT_ID
            _NEXT_ID += 1
            _NODES[self._id] = self

        co = _c.DartNodeOpts()
        _c.memset(_c.byref(co), 0, _c.sizeof(co))
        co.domain = options.domain
        co.max_topics = options.max_topics
        co.disable_shm = 1 if options.disable_shm else 0
        co.fetch_details = 1 if options.fetch_details else 0
        co.match_wait_ms = options.match_wait_ms
        co.disable_logs = 1 if options.disable_logs else 0
        co.disable_meta = 1 if options.disable_meta else 0
        co.disable_error_logs = 1 if options.disable_error_logs else 0
        co.user_data = _c.c_void_p(self._id)
        co.net.data_port = options.data_port
        co.net.discovery_group = options.discovery_group.encode() if options.discovery_group else None
        co.net.discovery_port = options.discovery_port
        co.net.multicast_interface = (options.multicast_interface.encode()
                                      if options.multicast_interface else None)
        co.net.multicast_ttl = options.multicast_ttl
        seeds, n_seeds = _c.seed_addrs(options.seed_peers)   # copied by open, alive for the call
        if n_seeds:
            co.net.seed_peers = _c.cast(seeds, _c.c_void_p)
            co.net.n_seed_peers = n_seeds
        co.net.unicast_only = 1 if options.unicast_only else 0
        co.net.self_ip = options.self_ip.encode() if options.self_ip else None
        co.net.advertise_port = options.advertise_port
        co.net.fragment_size = options.fragment_size
        co.net.recv_buffer_bytes = options.recv_buffer_bytes
        co.net.send_buffer_bytes = options.send_buffer_bytes
        co.discovery.announce_interval_us = options.announce_interval_us
        co.discovery.peer_timeout_us = options.peer_timeout_us
        co.discovery.max_peers = options.max_peers

        alloc = lib.dart_allocator_heap(_c.DART_ALLOCATOR_PAGE)
        self._alloc = alloc

        cname = name.encode("utf-8") if name else None
        h = lib.dart_node_open(_c.byref(alloc), cname, _on_message, _on_event, _c.byref(co))
        if not h:
            with _REG_LOCK:
                _NODES.pop(self._id, None)
            # no handle on failure: read the reason from the process-global slot
            err = Event._from_c(lib.dart_last_error(None))
            raise RuntimeError("Node(...) failed: %s" % err)
        self._h = h

    @property
    def name(self):
        """The name given at open, None when the node generated one."""
        return self._name

    def on_message(self, fn):
        """Rebind the message handler set at construction. Returns fn, so it works as a
        decorator."""
        self._on_msg = fn
        return fn

    def on_event(self, fn):
        """Rebind the event handler set at construction. Returns fn, so it works as a
        decorator."""
        self._on_evt = fn
        return fn

    # The create behind every Topic carrying constructor. Same name creates on this node
    # share the native slot with a widened role, and a different schema is refused.
    def _create_or_share(self, name, role, sch, qos, qos_kwargs):
        if not name:
            raise ValueError("topic name required")
        with self._create_lock:
            rec = self._topics_by_name.get(name)
            sh = sch.hash if sch else 0
            if rec is not None:
                if sh and rec[2] and sh != rec[2]:
                    raise RuntimeError("topic %r already exists on this node with a "
                                       "different schema" % name)
                bits = rec[1] | _role_bits(role)
                if bits != rec[1]:
                    self._lib.dart_topic_set_role(rec[0], int(_role_from_bits(bits)))
                    rec[1] = bits
                if sch is not None:
                    idx = self._lib.dart_topic_index(rec[0])
                    if self._topic_specs.get(idx) is None:
                        self._topic_specs[idx] = sch._spec
                    self._schemas.append(sch)
                    if not rec[2]:
                        rec[2] = sh
                return rec[0]
            h = self._create_native(name, role, sch, qos, qos_kwargs)
            self._topics_by_name[name] = [h, _role_bits(role), sh]
            return h

    # the native create: makes the handle and registers the schema (kept alive)
    # + decode spec against the topic index.
    def _create_native(self, name, role, sch, qos, qos_kwargs):
        if qos is None:
            qos = Qos(**qos_kwargs)
        elif qos_kwargs:
            raise TypeError("pass either qos= or keyword qos options, not both")
        co = _c.DartTopicOpts()
        _c.memset(_c.byref(co), 0, _c.sizeof(co))
        co.qos.reliability = int(qos.reliability)
        co.qos.keep_last = qos.keep_last
        co.qos.catch_up = qos.catch_up
        co.qos.max_message_bytes = qos.max_message_bytes
        co.qos.heartbeat_us = qos.heartbeat_us
        co.qos.repair_delay_us = qos.repair_delay_us
        co.qos.backpressure_wait_us = qos.backpressure_wait_us
        co.qos.shm_max_bytes = qos.shm_max_bytes
        co.qos.queue_bytes = qos.queue_bytes
        co.qos.max_rate_hz = qos.max_rate_hz
        co.qos.no_timestamp = 1 if qos.no_timestamp else 0
        h = self._lib.dart_node_create_topic(
            self._h, name.encode("utf-8"), int(role),
            sch._s if sch else None, _c.byref(co))
        if not h:
            raise RuntimeError("Topic(%r) create failed: %s" % (name, self.last_error()))
        idx = self._lib.dart_topic_index(h)
        self._topic_specs[idx] = sch._spec if sch else None
        if sch is not None:
            self._schemas.append(sch)
        return h

    def poll(self, timeout_ms=0):
        """One loop tick: discovery, receive, timers and queued sends. Blocks up to timeout_ms
                in the socket wait, 0 = non blocking. Returns STATE while start() runs."""
        return self._lib.dart_node_poll(self._h, timeout_ms)

    def start(self):
        """Run the C service thread. Handlers fire on it under the GIL, never two at once, and
                every call stays safe from any thread. Returns True on success."""
        return self._lib.dart_node_start(self._h) == 0

    def stop(self):
        """Stop and join the service thread. Idempotent, implied by close."""
        self._lib.dart_node_stop(self._h)

    def is_started(self):
        return self._lib.dart_node_is_started(self._h) == 1

    def dispatch(self, max_msgs=0, timeout_ms=0):
        """Dispatch every queued topic on the calling thread, waiting up to timeout_ms for any
                to hold data. The one liner for a frame paced consumer."""
        return self._lib.dart_node_dispatch(self._h, max_msgs, timeout_ms)

    def settle(self, timeout_ms=-1):
        """Block until discovery and matching settle, so everything sent now reaches everyone.
                Call after creating the topics. timeout_ms < 0 = 3 announce intervals."""
        return self._lib.dart_node_settle(self._h, timeout_ms) == 1

    # ---- built-in logs (the @dart/log/{error,warn,info} topics) ----

    def log(self, level, text):
        """Publish a formatted line on a level's log topic, truncated at DART_LOG_MAX. Returns
                a SendStatus, NOSYS when logs are disabled."""
        b = text.encode("utf-8") if isinstance(text, str) else bytes(text)
        return _send_status(self._lib.dart_node_log_text(self._h, int(level), b, len(b)))

    def log_error(self, text):
        return self.log(LogLevel.ERROR, text)

    def log_warn(self, text):
        return self.log(LogLevel.WARN, text)

    def log_info(self, text):
        return self.log(LogLevel.INFO, text)

    def log_topic(self, level):
        """This node's own handle for a level's log topic (None when disabled): widen
        its role and read it like any topic, or use on_log()."""
        h = self._lib.dart_node_log_topic(self._h, int(level))
        return Topic._from_handle(self, h) if h else None

    def on_log(self, level, handler):
        """Subscribe to a level's mesh wide log stream: every other node's lines at that level
                as a LogLine, on the polling thread. Returns False when logs are disabled."""
        h = self._lib.dart_node_log_topic(self._h, int(level))
        if not h:
            return False
        if self._lib.dart_topic_set_role(h, int(Role.PUBSUB)) != 0:
            return False
        idx = self._lib.dart_topic_index(h)
        lvl = LogLevel(int(level))

        def _wrap(m):
            f = m.fields or {}
            handler(LogLine(level=lvl, node=m.publisher_name, node_id=m.publisher_id,
                            wall_us=int(f.get("wall_us", 0)), mono_us=int(f.get("mono_us", 0)),
                            recv_us=m.recv_us, written_us=m.written_us,
                            text=f.get("text", "") or ""))
        self._add_sub_handler(idx, _wrap)
        return True

    # ---- @dart/meta introspection ----

    def meta_function(self):
        """The local @dart/meta caller handle (None when meta is disabled). Call it
        directed at a peer id. Most callers want meta()/meta_async()."""
        h = self._lib.dart_node_meta_function(self._h)
        return RemoteFunction._from_handle(self, h) if h else None

    def meta(self, peer, sections=MetaSection.ALL, timeout_ms=1000):
        """Blocking: a @dart/meta call directed at peer, decoded into a MetaSnapshot. Drives the
                loop, so never under start() or from a callback. sections is a MetaSection mask."""
        fn = self.meta_function()
        if fn is None:
            return MetaSnapshot(status=CallStatus.NO_HANDLER)
        req = b"" if int(sections) == 0 else _struct.pack("<I", int(sections))
        return MetaSnapshot._from_response(fn.call(req, timeout_ms, provider=peer))

    def meta_async(self, peer, on_snapshot, sections=MetaSection.ALL):
        """Async: a @dart/meta call directed at peer. on_snapshot fires once from the polling
                thread. Works under start(). Returns the launch SendStatus."""
        fn = self.meta_function()
        if fn is None:
            on_snapshot(MetaSnapshot(status=CallStatus.NO_HANDLER))
            return SendStatus.NO_TOPIC
        req = b"" if int(sections) == 0 else _struct.pack("<I", int(sections))
        return fn.call_async(req, lambda r: on_snapshot(MetaSnapshot._from_response(r)),
                             provider=peer)

    # ---- pattern-layer bookkeeping (reaped at close) ----
    def _retain(self, *schemas):
        for s in schemas:
            if s is not None:
                self._schemas.append(s)

    def _register_box(self, box_id):
        with self._pat_lock:
            self._pattern_boxes.append(box_id)

    def _register_async(self, box_id):
        with self._pat_lock:
            self._async_live.add(box_id)

    def _drop_async(self, box_id):
        with self._pat_lock:
            self._async_live.discard(box_id)

    def _add_sub_handler(self, index, fn):
        with self._pat_lock:
            self._sub_handlers.setdefault(index, []).append(fn)

    def evicted_unsent(self):
        """Sends that evicted never-sent history after the bounded wait (the
        EVICTED_UNSENT error count): the send-burst/overload indicator."""
        return self._lib.dart_node_evicted_unsent(self._h)

    def last_error(self):
        """The most recent error this node reported, as an Event (also delivered via
        on_event). kind is PEER_UP with error == ErrorKind.NONE if none has occurred."""
        return Event._from_c(self._lib.dart_last_error(self._h))

    def memory_stats(self):
        """(in_use, peak, alloc_calls). A flat alloc_calls over a window proves the
        hot path is allocation-free."""
        in_use, peak, calls = _c.c_size_t(), _c.c_size_t(), _c.c_uint64()
        self._lib.dart_node_mem_stats(self._h, _c.byref(in_use), _c.byref(peak), _c.byref(calls))
        return in_use.value, peak.value, calls.value

    def backpressure_stats(self):
        """(waited_us, waited_sends) since open."""
        us, n = _c.c_uint64(), _c.c_uint32()
        self._lib.dart_node_backpressure_stats(self._h, _c.byref(us), _c.byref(n))
        return us.value, n.value

    def close(self, send_bye=True):
        """Stop the service thread and tear the node down. Returns True, or False when refused
                from inside a handler, where the node stays live."""
        if self._h:
            if self._lib.dart_node_close(self._h, 1 if send_bye else 0) != 0:
                return False
            self._h = None
        with _REG_LOCK:
            _NODES.pop(self._id, None)
        # Reap this node's handler boxes and backstop any async call the C did not cancel. The C
        # fires every pending on_response with CANCELLED at close, so the loop normally finds none.
        with self._pat_lock:
            boxes, self._pattern_boxes = self._pattern_boxes, []
            stragglers, self._async_live = list(self._async_live), set()
        for box_id in boxes:
            _pbox_pop(box_id)
        for box_id in stragglers:
            box = _pbox_pop(box_id)
            if box is not None:
                r = Response()
                r.status = CallStatus.CANCELLED
                try:
                    box.on_response(r)
                except Exception:
                    _traceback.print_exc()
        return True

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass


# The patterns over topic kinds plus the side named Publisher and Subscriber handles.
# Each subscripts like Topic[T] for the typed form, untyped forms take schema args.

class _TypedPattern:
    """The result of Handle[T]: a factory bound to schema class(es), callable like
    the untyped constructor minus its schema argument(s)."""
    __slots__ = ("_cls", "_schemas")

    def __init__(self, cls, schemas):
        self._cls = cls
        self._schemas = schemas   # schema kwarg name to schema class

    def __call__(self, *args, **kwargs):
        for k in self._schemas:
            if k in kwargs:
                raise TypeError("%s is fixed by the [T] subscript" % k)
        kwargs.update(self._schemas)
        return self._cls(*args, **kwargs)

    def __repr__(self):
        return "dart.%s[%s]" % (self._cls.__name__, ", ".join(
            getattr(t, "__name__", str(t)) for t in self._schemas.values()))


def _typed_pattern(cls, item, kw_names):
    items = item if isinstance(item, tuple) else (item,)
    if len(items) != len(kw_names):
        raise TypeError("%s[...] takes %d schema class(es)" % (cls.__name__, len(kw_names)))
    return _TypedPattern(cls, dict(zip(kw_names, items)))


class Request:
    """A function call as seen by the definition's full form handler: the request metadata
        plus the reply surface, valid only while the handler runs (docs/python.md)."""
    __slots__ = ("_ptr", "_fn", "_rsp_schema", "_done",
                 "data", "caller", "caller_name", "function_name", "recv_us", "written_us")

    def __init__(self, ptr, fn, rsp_schema, r, data):
        self._ptr = ptr           # the exact native pointer, dropped at expiry
        self._fn = fn
        self._rsp_schema = rsp_schema
        self._done = False
        self.data = data
        self.caller = r.caller
        self.caller_name = _dstr(r.caller_name)
        self.function_name = _dstr(r.function_name)
        self.recv_us = r.recv_us
        self.written_us = r.written_us   # the caller's wall clock when it wrote the request

    @property
    def answered(self):
        """True once reply/fail/defer has been called."""
        return self._done

    def reply(self, rsp=None):
        """Answer CallStatus.OK with rsp (bytes/str, or a typed value)."""
        self._guard()
        b, buf = _c_view(_payload_bytes(self._rsp_schema, rsp))
        _c.load().dart_request_reply(self._ptr, b)
        del buf
        self._done = True

    def fail(self, message=None, rsp=None):
        """Answer APP_ERROR. message is the text shown on the caller, truncated at 255 bytes,
                empty = the default. rsp may still carry structured failure data."""
        self._guard()
        b, buf = _c_view(_payload_bytes(self._rsp_schema, rsp))
        _c.load().dart_request_fail(self._ptr, message.encode("utf-8") if message else None, b)
        del buf
        self._done = True

    def defer(self):
        """Park the reply: suppresses the auto-ack and lets the handler return
        now. The returned Deferred completes the call later, from any thread."""
        self._guard()
        token = _c.load().dart_request_defer(self._ptr)
        if not token:
            raise RuntimeError("defer failed")
        self._done = True
        return Deferred(self._fn, self._rsp_schema, token)

    def _expire(self):
        self._ptr = None

    def _guard(self):
        if self._ptr is None:
            raise RuntimeError("request expired: answer inside the handler "
                               "callback, or defer() first")
        if self._done:
            raise RuntimeError("request already answered")


class Deferred:
    """A parked function reply from Request.defer: complete or fail exactly once, from any
        thread. Dropping it leaves the caller to its timeout."""
    __slots__ = ("_fn", "_rsp_schema", "_token", "_lock")

    def __init__(self, fn, rsp_schema, token):
        self._fn = fn
        self._rsp_schema = rsp_schema
        self._token = token
        self._lock = _threading.Lock()

    @property
    def valid(self):
        return self._token != 0

    def complete(self, rsp=None, message=None):
        """Answer CallStatus.OK with rsp. True if the completion was accepted.
        message is optional debug/warning text (Response.message on the caller)."""
        return self._finish(CallStatus.OK, message, rsp)

    def fail(self, message=None, rsp=None):
        """Answer CallStatus.APP_ERROR (message as in Request.fail)."""
        return self._finish(CallStatus.APP_ERROR, message, rsp)

    def _finish(self, status, message, rsp):
        with self._lock:
            token, self._token = self._token, 0
        if not token:
            return False
        b, buf = _c_view(_payload_bytes(self._rsp_schema, rsp))
        r = _c.load().dart_function_complete(self._fn, token, int(status),
                                           message.encode("utf-8") if message else None, b)
        del buf
        return r == 0


class Response:
    """An owning call outcome, the payload copied out. status and data never raise, reading
        value when not ok raises CallError. send_status carries a synchronous refusal."""
    __slots__ = ("status", "send_status", "provider", "written_us", "data", "message",
                 "_value", "_decoded")

    def __init__(self):
        self.status = CallStatus.TIMEOUT
        self.send_status = SendStatus.OK
        self.provider = 0
        self.written_us = 0   # the provider's wall clock when it wrote the reply (0 = synthesized)
        self.data = b""
        self.message = ""     # the outcome text to display on a failure: the definition's message,
                              # else the default status text. "" only on OK with no message
        self._value = None
        self._decoded = False

    @classmethod
    def _from_c(cls, o, rsp_schema):
        self = cls()
        self.status = (CallStatus(o.status)
                       if o.status in CallStatus._value2member_map_ else o.status)
        self.provider = o.provider
        self.written_us = o.written_us
        self.data = _c.string_at(o.data.data, o.data.len) if o.data.data and o.data.len else b""
        self.message = _dstr(o.message)
        if self.status == CallStatus.OK:
            # decode NOW: the wire-schema view dies with the C callback/call
            self._value, self._decoded = _decode_payload(o.schema, rsp_schema, self.data)
        return self

    @property
    def ok(self):
        return self.status == CallStatus.OK

    @property
    def value(self):
        """The decoded reply, a typed instance or dict, raw bytes for an untyped pair. Raises
                CallError when the call did not complete OK."""
        if not self.ok:
            name = self.status.name if isinstance(self.status, CallStatus) else self.status
            tail = "" if not self.message else " (%s)" % self.message
            tail += "" if self.send_status == SendStatus.OK else " (send %s)" % self.send_status
            raise CallError(self.status, self.send_status,
                            "call did not complete OK: %s%s" % (name, tail))
        if not self._decoded:
            raise CallError(self.status, self.send_status, "response payload failed to decode")
        return self._value

    def __repr__(self):
        return "Response(%s, %d bytes)" % (
            self.status.name if isinstance(self.status, CallStatus) else self.status,
            len(self.data))


class Progress:
    """One task progress update for an on_progress handler's 2 arg form: value (None for the
        RUNNING ack), data, call_id, provider, written_us and recv_us."""
    __slots__ = ("value", "data", "call_id", "provider", "written_us", "recv_us")

    def __repr__(self):
        return "Progress(call=%d, from=%d, value=%r)" % (
            self.call_id, self.provider, self.value)


def _progress_cb(on_progress, prg_schema):
    """A per call ctypes progress trampoline that decodes and arity dispatches. The object
        must stay alive until the call's terminal outcome."""
    arity = _arity(on_progress)

    @_c.PrgFn
    def cb(prg_ptr):
        try:
            p = prg_ptr.contents
            data = _c.string_at(p.data.data, p.data.len) if p.data.data and p.data.len else b""
            if not data:
                val = None       # the RUNNING ack
            else:
                val, ok = _decode_payload(p.schema, prg_schema, data)
                if not ok:
                    val = data
            if arity == 1:
                on_progress(val)
            else:
                info = Progress()
                info.value = val
                info.data = data
                info.call_id = p.call_id
                info.provider = p.provider
                info.written_us = p.written_us
                info.recv_us = p.recv_us
                on_progress(val, info)
        except Exception:
            _traceback.print_exc()
    return cb


class _FnBox:
    """Handler-side state for one function definition (alive until node close)."""
    __slots__ = ("fn", "handler", "arity", "req_schema", "rsp_schema")

    def __init__(self, handler, req_schema, rsp_schema):
        self.fn = None            # set right after create (no callback before poll)
        self.handler = handler
        self.arity = _arity(handler)
        self.req_schema = req_schema
        self.rsp_schema = rsp_schema

    def dispatch(self, req_ptr):
        r = req_ptr.contents
        data = _c.string_at(r.data.data, r.data.len) if r.data.data and r.data.len else b""
        request = Request(req_ptr, self.fn, self.rsp_schema, r, data)
        try:
            val, ok = _decode_payload(r.schema, self.req_schema, data)
            if not ok:
                request.fail("request decode failed")
                return
            if self.arity == 1:
                out = self.handler(val)
                if out is not None and not request.answered:
                    request.reply(out)
            else:
                self.handler(val, request)
        except Exception as e:
            # a raising handler answers APP_ERROR with str(e) as the message. The exception never
            # crosses into C
            _traceback.print_exc()
            if not request.answered:
                try:
                    request.fail(str(e) or "handler threw")
                except Exception:
                    pass
        finally:
            request._expire()


class TaskRequest:
    """A task call as seen by the definition's handler on its own worker thread. Returning
        completes OK, CancelledError completes CANCELLED, any other exception APP_ERROR."""
    __slots__ = ("_fn", "_prg_schema", "_token", "cancel_event",
                 "value", "data", "caller", "caller_name", "function_name",
                 "recv_us", "written_us")

    def __init__(self, fn, prg_schema, token, r, value, data):
        self._fn = fn
        self._prg_schema = prg_schema
        self._token = token
        self.cancel_event = _threading.Event()   # set the moment a cancel arrives
        self.value = value
        self.data = data
        self.caller = r.caller
        self.caller_name = _dstr(r.caller_name)
        self.function_name = _dstr(r.function_name)
        self.recv_us = r.recv_us
        self.written_us = r.written_us

    def progress(self, value=None):
        """Broadcast a progress update, encoded via the prg schema. Returns a SendStatus, STATE
                once the call completed."""
        b, buf = _c_view(_payload_bytes(self._prg_schema, value))
        r = _c.load().dart_function_progress(self._fn, self._token, b)
        del buf
        return _send_status(r)

    @property
    def cancelled(self):
        """True once the caller or a third party requested cancellation. Honor it by raising
                CancelledError, or run to completion. cancel_event is the same signal."""
        if self.cancel_event.is_set():
            return True
        if _c.load().dart_function_cancelled(self._fn, self._token) == 1:
            self.cancel_event.set()   # backstop: a cancel that beat the C slot
            return True
        return False


class _TaskBox:
    """Definition side state for one task, alive until node close. The C callback on the poll
        thread defers and spawns a daemon worker per call, and on_cancel fans out to Events."""
    __slots__ = ("fn", "handler", "req_schema", "prg_schema", "rsp_schema",
                 "cancel_events", "lock")

    def __init__(self, handler, req_schema, prg_schema, rsp_schema):
        self.fn = None            # set right after create (no callback before poll)
        self.handler = handler
        self.req_schema = req_schema
        self.prg_schema = prg_schema
        self.rsp_schema = rsp_schema
        self.cancel_events = {}   # defer token to threading.Event
        self.lock = _threading.Lock()

    def dispatch(self, req_ptr):   # poll thread: copy what the views hold, return fast
        lib = _c.load()
        r = req_ptr.contents
        data = _c.string_at(r.data.data, r.data.len) if r.data.data and r.data.len else b""
        val, ok = _decode_payload(r.schema, self.req_schema, data)
        if not ok:
            lib.dart_request_fail(req_ptr, b"request decode failed", _c.DartBytes())
            return
        token = lib.dart_request_defer(req_ptr)   # implies RUNNING to the caller
        if not token:
            lib.dart_request_fail(req_ptr, b"defer failed", _c.DartBytes())
            return
        task = TaskRequest(self.fn, self.prg_schema, token, r, val, data)
        with self.lock:
            self.cancel_events[token] = task.cancel_event
        _threading.Thread(target=self._run, args=(task,), daemon=True).start()

    def cancel(self, token):       # poll thread, from the C on_cancel slot
        with self.lock:
            ev = self.cancel_events.get(token)
        if ev is not None:
            ev.set()

    def _run(self, task):          # the dedicated worker thread for one call
        status, message, rsp = CallStatus.OK, None, None
        try:
            rsp = self.handler(task)
        except CancelledError as e:
            status, message, rsp = CallStatus.CANCELLED, str(e) or None, None
        except Exception as e:
            _traceback.print_exc()
            status, message, rsp = CallStatus.APP_ERROR, str(e) or "handler threw", None
        try:
            payload = _payload_bytes(self.rsp_schema, rsp)
        except Exception as e:
            _traceback.print_exc()
            status, message, payload = (CallStatus.APP_ERROR,
                                        str(e) or "response encode failed", b"")
        with self.lock:
            self.cancel_events.pop(task._token, None)
        b, buf = _c_view(payload)
        # a stale token (the definition retired / node closed mid-run) returns
        # STATE: swallowed, the caller already got its one CANCELLED outcome
        _c.load().dart_function_complete(self.fn, task._token, int(status),
                                       message.encode("utf-8") if message else None, b)
        del buf


class _AsyncBox:
    """One in flight async call, released when its response fires, including the CANCELLED
        one synthesized at node close. progress_cb keeps the trampoline alive until then."""
    __slots__ = ("node", "on_response", "rsp_schema", "id", "progress_cb")

    def __init__(self, node, on_response, rsp_schema):
        self.node = node
        self.on_response = on_response
        self.rsp_schema = rsp_schema
        self.id = 0
        self.progress_cb = None


class VariableUpdate:
    """The state just applied to a variable, for on_change and on_write handlers: value,
        data, name, forced, write_seq, source (0 = local), recv_us and written_us."""
    __slots__ = ("name", "data", "value", "forced", "write_seq", "source", "recv_us",
                 "written_us")

    def __repr__(self):
        return "VariableUpdate(%r, value=%r, forced=%r, seq=%d, source=%d)" % (
            self.name, self.value, self.forced, self.write_seq, self.source)


class _VarBox:
    __slots__ = ("handler", "arity", "schema")

    def __init__(self, handler, schema):
        self.handler = handler
        self.arity = _arity(handler)
        self.schema = schema


class FunctionDefinition:
    """The implementation side of a function: one reply per call, one definition per name.
        Handler forms and the typed shorthand are in docs/python.md. None answers NO_HANDLER."""
    __slots__ = ("_node", "_fn", "_req_schema", "_rsp_schema", "_name")

    def __init__(self, node, name, handler, req_schema=None, rsp_schema=None,
                 backpressure_wait_us=0, timeout_us=0, keep_last=0):
        self._node = node
        self._name = name
        self._req_schema = _as_schema(req_schema)
        self._rsp_schema = _as_schema(rsp_schema)
        co = _c.DartFunctionOpts(backpressure_wait_us, timeout_us, keep_last)
        box_id = 0
        box = None
        if handler is not None:
            box = _FnBox(handler, self._req_schema, self._rsp_schema)
            box_id = _pbox_add(box)
        h = node._lib.dart_node_create_function_definition(
            node._h, name.encode("utf-8"),
            self._req_schema._s if self._req_schema else None,
            self._rsp_schema._s if self._rsp_schema else None,
            _on_request if box else _c.NULL_REQ_FN, _c.c_void_p(box_id), _c.byref(co))
        if not h:
            if box:
                _pbox_pop(box_id)
            raise RuntimeError("FunctionDefinition(%r) create failed: %s"
                               % (name, node.last_error()))
        self._fn = h
        if box:
            box.fn = h
            node._register_box(box_id)
        node._retain(self._req_schema, self._rsp_schema)

    @property
    def name(self):
        return self._name

    def _ptr(self):
        # NULL once the node is closed, so the C answers NO_TOPIC instead of touching freed memory
        return self._fn if self._node._h else None

    def __class_getitem__(cls, item):
        return _typed_pattern(cls, item, ("req_schema", "rsp_schema"))

    def caller_count(self):
        """Callers currently matched to this definition."""
        return self._node._lib.dart_function_match_count(self._ptr())

    def retire(self):
        """Retire the definition: park its channels and release the name for a successor. The
                handle is unusable after. Refused with STATE from a callback."""
        rc = self._node._lib.dart_function_retire(self._ptr())
        if rc == 0:
            self._fn = None
        return _send_status(rc)


class RemoteFunction:
    """A reference to a function defined on another node.
    RemoteFunction[Req, Rsp](node, name) is the typed shorthand."""
    __slots__ = ("_node", "_fn", "_req_schema", "_rsp_schema", "_name")

    def __init__(self, node, name, req_schema=None, rsp_schema=None,
                 backpressure_wait_us=0, timeout_us=0, keep_last=0):
        self._node = node
        self._name = name
        self._req_schema = _as_schema(req_schema)
        self._rsp_schema = _as_schema(rsp_schema)
        co = _c.DartFunctionOpts(backpressure_wait_us, timeout_us, keep_last)
        h = node._lib.dart_node_create_remote_function(
            node._h, name.encode("utf-8"),
            self._req_schema._s if self._req_schema else None,
            self._rsp_schema._s if self._rsp_schema else None, _c.byref(co))
        if not h:
            raise RuntimeError("RemoteFunction(%r) create failed: %s"
                               % (name, node.last_error()))
        self._fn = h
        node._retain(self._req_schema, self._rsp_schema)

    @property
    def name(self):
        return self._name

    def _ptr(self):
        # NULL once the node is closed, so the C answers NO_TOPIC instead of touching freed memory
        return self._fn if self._node._h else None

    def __class_getitem__(cls, item):
        return _typed_pattern(cls, item, ("req_schema", "rsp_schema"))

    @classmethod
    def _from_handle(cls, node, handle, req_schema=None, rsp_schema=None):
        # wrap a node-owned function handle (the @dart/meta endpoint): callable,
        # never created or destroyed here.
        self = cls.__new__(cls)
        self._node = node
        self._name = None
        self._fn = handle
        self._req_schema = req_schema
        self._rsp_schema = rsp_schema
        return self

    def call(self, req=None, timeout_ms=-1, provider=0):
        """Blocking call: drives the loop until the response or timeout_ms, negative = the
                default. Refused from a callback or under a service thread. Never raises."""
        b, buf = _c_view(_payload_bytes(self._req_schema, req))
        out = _c.DartResponse()
        co = _c.DartCallOpts(int(provider)) if provider else None
        rc = self._node._lib.dart_function_call(self._ptr(), b, _c.byref(out), timeout_ms,
                                                _c.cast(_c.byref(co), _c.c_void_p) if co else None)
        del buf
        if rc == 1:
            return Response._from_c(out, self._rsp_schema)
        r = Response()
        if rc < 0:
            r.send_status = _send_status(rc)   # never launched: message stays ""
        else:
            r.message = _dstr(out.message)     # timed out: the C fills "timeout"
        return r

    def call_async(self, req, on_response, provider=0):
        """Async call: returns the launch SendStatus once the request is committed, and
                on_response fires exactly once with an owning Response from the polling thread."""
        if not callable(on_response):
            raise TypeError("on_response must be callable")
        box = _AsyncBox(self._node, on_response, self._rsp_schema)
        box_id = _pbox_add(box)
        box.id = box_id
        self._node._register_async(box_id)
        b, buf = _c_view(_payload_bytes(self._req_schema, req))
        co = _c.DartCallOpts(int(provider)) if provider else None
        rc = self._node._lib.dart_function_call_async(self._ptr(), b, _on_response,
                                                      _c.c_void_p(box_id),
                                                      _c.cast(_c.byref(co), _c.c_void_p) if co else None)
        del buf
        st = _send_status(rc)
        if rc != 0:
            _pbox_pop(box_id)
            self._node._drop_async(box_id)
            r = Response()
            r.send_status = st
            try:
                on_response(r)
            except Exception:
                _traceback.print_exc()
        return st

    def match_count(self):
        """Providers currently matched (the definition side present)."""
        return self._node._lib.dart_function_match_count(self._ptr())

    def has_definition(self):
        return self.match_count() > 0

    def retire(self):
        """Retire the remote: park its channels and release the name. Every outstanding call
                completes CANCELLED. Unusable after, refused with STATE from a callback."""
        rc = self._node._lib.dart_function_retire(self._ptr())
        if rc == 0:
            self._fn = None
        return _send_status(rc)


class TaskDefinition:
    """The implementation side of a task. The handler runs on a worker thread per call with a
        TaskRequest (docs/python.md). None answers NO_HANDLER, the options mirror DartTaskOpts."""
    __slots__ = ("_node", "_fn", "_req_schema", "_prg_schema", "_rsp_schema", "_name")

    def __init__(self, node, name, handler, req_schema=None, prg_schema=None,
                 rsp_schema=None, progress_best_effort=False, progress_keep_last=0,
                 no_cancel=False, exclusive=False, multi=False, timeout_us=0,
                 backpressure_wait_us=0, keep_last=0):
        self._node = node
        self._name = name
        self._req_schema = _as_schema(req_schema)
        self._prg_schema = _as_schema(prg_schema)
        self._rsp_schema = _as_schema(rsp_schema)
        co = _c.DartTaskOpts()
        _c.memset(_c.byref(co), 0, _c.sizeof(co))
        co.progress_best_effort = 1 if progress_best_effort else 0
        co.progress_keep_last = progress_keep_last
        co.no_cancel = 1 if no_cancel else 0
        co.exclusive = 1 if exclusive else 0
        co.multi = 1 if multi else 0
        co.timeout_us = timeout_us
        co.backpressure_wait_us = backpressure_wait_us
        co.keep_last = keep_last
        box_id = 0
        box = None
        if handler is not None:
            box = _TaskBox(handler, self._req_schema, self._prg_schema,
                           self._rsp_schema)
            box_id = _pbox_add(box)
        h = node._lib.dart_node_create_task_definition(
            node._h, name.encode("utf-8"),
            self._req_schema._s if self._req_schema else None,
            self._prg_schema._s if self._prg_schema else None,
            self._rsp_schema._s if self._rsp_schema else None,
            _on_request if box else _c.NULL_REQ_FN, _c.c_void_p(box_id), _c.byref(co))
        if not h:
            if box:
                _pbox_pop(box_id)
            raise RuntimeError("TaskDefinition(%r) create failed: %s"
                               % (name, node.last_error()))
        self._fn = h
        if box:
            box.fn = h
            node._register_box(box_id)
            # the one C cancel slot: fans out to the per-call cancel Events
            node._lib.dart_function_on_cancel(h, _on_task_cancel, _c.c_void_p(box_id))
        node._retain(self._req_schema, self._prg_schema, self._rsp_schema)

    @property
    def name(self):
        return self._name

    def _ptr(self):
        # NULL once the node is closed, so the C answers NO_TOPIC instead of touching freed memory
        return self._fn if self._node._h else None

    def __class_getitem__(cls, item):
        return _typed_pattern(cls, item, ("req_schema", "prg_schema", "rsp_schema"))

    def caller_count(self):
        """Callers currently matched to this definition."""
        return self._node._lib.dart_function_match_count(self._ptr())

    def retire(self):
        """Retire the definition: every live deferred call answers CANCELLED while the channels
                are up, a later worker completion is refused silently. Refused from a callback."""
        rc = self._node._lib.dart_function_retire(self._ptr())
        if rc == 0:
            self._fn = None
        return _send_status(rc)


class RemoteTask:
    """A reference to a task defined elsewhere. A request is always directed at one
        provider, and the timeout bounds only the first response (docs/tasks.md)."""
    __slots__ = ("_node", "_fn", "_req_schema", "_prg_schema", "_rsp_schema", "_name")

    def __init__(self, node, name, req_schema=None, prg_schema=None,
                 rsp_schema=None, progress_best_effort=False,
                 progress_keep_last=0, timeout_us=0, backpressure_wait_us=0,
                 keep_last=0):
        self._node = node
        self._name = name
        self._req_schema = _as_schema(req_schema)
        self._prg_schema = _as_schema(prg_schema)
        self._rsp_schema = _as_schema(rsp_schema)
        co = _c.DartTaskOpts()
        _c.memset(_c.byref(co), 0, _c.sizeof(co))
        co.progress_best_effort = 1 if progress_best_effort else 0
        co.progress_keep_last = progress_keep_last
        co.timeout_us = timeout_us
        co.backpressure_wait_us = backpressure_wait_us
        co.keep_last = keep_last
        h = node._lib.dart_node_create_remote_task(
            node._h, name.encode("utf-8"),
            self._req_schema._s if self._req_schema else None,
            self._prg_schema._s if self._prg_schema else None,
            self._rsp_schema._s if self._rsp_schema else None, _c.byref(co))
        if not h:
            raise RuntimeError("RemoteTask(%r) create failed: %s"
                               % (name, node.last_error()))
        self._fn = h
        node._retain(self._req_schema, self._prg_schema, self._rsp_schema)

    @property
    def name(self):
        return self._name

    def _ptr(self):
        # NULL once the node is closed, so the C answers NO_TOPIC instead of touching freed memory
        return self._fn if self._node._h else None

    def __class_getitem__(cls, item):
        return _typed_pattern(cls, item, ("req_schema", "prg_schema", "rsp_schema"))

    def call(self, req=None, on_progress=None, timeout_ms=-1, provider=0):
        """Blocking call: drives the loop until the terminal outcome, with on_progress on this
                thread. Refused from a callback or under a service thread. Never raises."""
        b, buf = _c_view(_payload_bytes(self._req_schema, req))
        out = _c.DartResponse()
        co = _c.DartCallOpts()
        co.provider = int(provider)
        keep_cb = None
        if on_progress is not None:
            # fires only inside dart_function_call, so the local ref holds it
            keep_cb = _progress_cb(on_progress, self._prg_schema)
            co.on_progress = keep_cb
        rc = self._node._lib.dart_function_call(self._ptr(), b, _c.byref(out), timeout_ms,
                                                _c.cast(_c.byref(co), _c.c_void_p))
        del buf, keep_cb
        if rc == 1:
            return Response._from_c(out, self._rsp_schema)
        r = Response()
        if rc < 0:
            r.send_status = _send_status(rc)   # never launched: message stays ""
        else:
            r.message = _dstr(out.message)     # timed out: the C fills "timeout"
        return r

    def call_async(self, req, on_response, on_progress=None, provider=0):
        """Async call: returns the call id for cancel(), 0 when never committed. on_progress
                fires per update and on_response exactly once, from the polling thread."""
        if not callable(on_response):
            raise TypeError("on_response must be callable")
        box = _AsyncBox(self._node, on_response, self._rsp_schema)
        box_id = _pbox_add(box)
        box.id = box_id
        self._node._register_async(box_id)
        b, buf = _c_view(_payload_bytes(self._req_schema, req))
        call_id = _c.c_uint32(0)
        co = _c.DartCallOpts()
        co.provider = int(provider)
        co.id_out = _c.pointer(call_id)
        if on_progress is not None:
            box.progress_cb = _progress_cb(on_progress, self._prg_schema)
            co.on_progress = box.progress_cb
        rc = self._node._lib.dart_function_call_async(self._ptr(), b, _on_response,
                                                      _c.c_void_p(box_id),
                                                      _c.cast(_c.byref(co), _c.c_void_p))
        del buf
        if rc != 0:
            _pbox_pop(box_id)
            self._node._drop_async(box_id)
            r = Response()
            r.send_status = _send_status(rc)
            try:
                on_response(r)
            except Exception:
                _traceback.print_exc()
            return 0
        return call_id.value

    def cancel(self, call_id):
        """Request cancellation of the call. Cooperative and never acked, the terminal status
                answers. BAD_ROLE when the provider declared no_cancel, STATE when not pending."""
        return _send_status(self._node._lib.dart_function_cancel(self._ptr(),
                                                                 int(call_id)))

    def match_count(self):
        """Providers currently matched (the definition side present)."""
        return self._node._lib.dart_function_match_count(self._ptr())

    def has_definition(self):
        return self.match_count() > 0

    def retire(self):
        """Retire the remote: every outstanding call completes CANCELLED. Unusable after,
                refused with STATE from a callback."""
        rc = self._node._lib.dart_function_retire(self._ptr())
        if rc == 0:
            self._fn = None
        return _send_status(rc)


class VariableDefinition:
    """Replicated state with one owner: this node holds the authoritative value. Methods
        only, so every write returns its SendStatus. VariableDefinition[T] is the typed form."""
    __slots__ = ("_node", "_var", "_schema", "_name")
    _remote = False

    @property
    def name(self):
        return self._name

    def _ptr(self):
        # NULL once the node is closed, so the C answers NO_TOPIC instead of touching freed memory
        return self._var if self._node._h else None

    def __init__(self, node, name, schema=None, initial=None, read_only=False,
                 allow_force=False, catch_up=0, keep_last=0, backpressure_wait_us=0):
        self._node = node
        self._name = name
        sch = self._schema = _as_schema(schema)
        co = _c.DartVariableOpts()
        _c.memset(_c.byref(co), 0, _c.sizeof(co))
        co.access = 1 if read_only else 0
        co.allow_force = 1 if allow_force else 0
        co.catch_up = catch_up
        co.keep_last = keep_last
        co.backpressure_wait_us = backpressure_wait_us
        b, buf = _c_view(_payload_bytes(sch, initial) if initial is not None else b"")
        co.initial = b
        create = (node._lib.dart_node_create_remote_variable if self._remote
                  else node._lib.dart_node_create_variable_definition)
        h = create(node._h, name.encode("utf-8"), sch._s if sch else None, _c.byref(co))
        del buf
        if not h:
            raise RuntimeError("%s(%r) create failed: %s"
                               % (type(self).__name__, name, node.last_error()))
        self._var = h
        node._retain(sch)

    def __class_getitem__(cls, item):
        return _typed_pattern(cls, item, ("schema",))

    def get(self):
        """The current value, the store or the cached latest, copied out under the node lock.
                None when no value exists yet."""
        lib = self._node._lib
        lib.dart_node_lock(self._node._h)
        try:
            b = _c.DartBytes()
            if lib.dart_variable_get(self._ptr(), _c.byref(b)) != 1:
                return None
            data = _c.string_at(b.data, b.len) if b.data and b.len else b""
        finally:
            lib.dart_node_unlock(self._node._h)
        if self._schema is None:
            return data
        val, ok = _decode_payload(None, self._schema, data)
        return val if ok else data

    def set(self, value):
        """Set the value: apply and publish, or send over the set channel. NO_TOPIC = no owner
                matched, BAD_ROLE = the owner advertises no set channel."""
        b, buf = _c_view(_payload_bytes(self._schema, value))
        r = self._node._lib.dart_variable_set(self._ptr(), b)
        del buf
        return _send_status(r)

    def force(self, value):
        """Force the value: sets are absorbed into the shadow source until unforce restores the
                latest absorbed set. Needs allow_force on the definition, else STATE."""
        b, buf = _c_view(_payload_bytes(self._schema, value))
        r = self._node._lib.dart_variable_force(self._ptr(), b)
        del buf
        return _send_status(r)

    def unforce(self):
        return _send_status(self._node._lib.dart_variable_unforce(self._ptr()))

    def forced(self):
        """True while forced: authoritative on the definition, the last received flag on a
                remote."""
        return self._node._lib.dart_variable_forced(self._ptr()) == 1

    def wait(self, timeout_ms):
        """Block driving the loop until a value exists or timeout_ms elapses. Refused from a
                callback or under a service thread."""
        return self._node._lib.dart_variable_wait(self._ptr(), timeout_ms) == 1

    def remote_count(self):
        """Remotes matched to this definition."""
        return self._node._lib.dart_variable_match_count(self._ptr())

    def on_change(self, handler):
        """Observe changes: fires on every state change and replays the current value at
                registration, inline on the thread that applied the write. None clears."""
        return self._observe(handler, True)

    def on_write(self, handler):
        """Observe every applied write, identical bytes or not, with no replay at registration.
                The same forms and threading as on_change. None clears."""
        return self._observe(handler, False)

    def _observe(self, handler, change):
        lib = self._node._lib
        reg = lib.dart_variable_on_change if change else lib.dart_variable_on_write
        if handler is None:
            reg(self._ptr(), _c.NULL_VAR_FN, None)
            return None
        box_id = _pbox_add(_VarBox(handler, self._schema))
        self._node._register_box(box_id)
        reg(self._ptr(), _on_var_update, _c.c_void_p(box_id))
        return handler

    def retire(self):
        """Retire the handle: park its channels and release the name, else a re created same
                name handle is shadowed by the live twin. Refused with STATE from a callback."""
        rc = self._node._lib.dart_variable_retire(self._ptr())
        if rc == 0:
            self._var = None
        return _send_status(rc)


class RemoteVariable(VariableDefinition):
    """A reference to a variable owned elsewhere: reads see the cached latest, writes go
        over the set channel with no response. RemoteVariable[T] is the typed form."""
    _remote = True

    def __init__(self, node, name, schema=None, catch_up=0, keep_last=0,
                 backpressure_wait_us=0):
        super().__init__(node, name, schema, None, False, False, catch_up,
                         keep_last, backpressure_wait_us)

    def match_count(self):
        """Owners currently matched (0 = no owner present)."""
        return self.remote_count()

    def has_definition(self):
        return self.remote_count() > 0


class Publisher:
    """The publish side of a topic. Same name constructions on one node share the slot with
        a widened role. Publisher[T] is the typed form, qos as keyword args or qos=Qos()."""
    __slots__ = ("topic",)

    def __init__(self, node, name, schema=None, qos=None, **qos_kwargs):
        self.topic = Topic(node, name, schema, Role.PUB_ONLY, qos, **qos_kwargs)

    def __class_getitem__(cls, item):
        return _typed_pattern(cls, item, ("schema",))

    @property
    def name(self):
        return self.topic.name

    def send(self, data, capture_us=0):
        """Publish (typed value, mapping, or bytes/str). SendStatus."""
        return self.topic.send(data, capture_us)

    def match_count(self):
        return self.topic.match_count()

    def ready(self):
        return self.topic.ready()

    def pending_count(self):
        return self.topic.pending_count()


class Subscriber:
    """The subscribe side. The arity dispatched handler fires per message on the polling
        thread instead of on_message, or consume with take and dispatch. Subscriber[T] is typed."""
    __slots__ = ("topic", "_schema")

    def __init__(self, node, name, handler=None, schema=None, qos=None, **qos_kwargs):
        self.topic = Topic(node, name, schema, Role.SUB_ONLY, qos, **qos_kwargs)
        self._schema = self.topic.schema
        if handler is not None:
            if _arity(handler) == 1:
                fn = lambda m: handler(m.value if m.value is not None else m.data)
            else:
                fn = lambda m: handler(m.value if m.value is not None else m.data, m)
            node._add_sub_handler(self.topic.index, fn)

    def __class_getitem__(cls, item):
        return _typed_pattern(cls, item, ("schema",))

    @property
    def name(self):
        return self.topic.name

    def match_count(self):
        """Publishers currently matched."""
        return self.topic.match_count()

    def ready(self):
        """True once matching has converged, so a publisher's first send reaches this side."""
        return self.topic.ready()

    def take(self, timeout_ms=0):
        """Typed take: the next queued message's decoded value, the whole Message without a
                schema, None when nothing arrived in time. The first use queues the topic."""
        m = self.topic.take(timeout_ms)
        if m is None or self._schema is None:
            return m
        return m.value if m.value is not None else m.data

    def dispatch(self, max_msgs=0, timeout_ms=0):
        """Drain the queue on the calling thread through this subscriber's
        handler (see Topic.dispatch). Returns the number dispatched."""
        return self.topic.dispatch(max_msgs, timeout_ms)


# Callback dispatch. The C callbacks carry no user pointer, but every DartMsg and
# DartEvent carries the node id stashed in user. One trampoline per module serves all.
_NODES = {}
_REG_LOCK = _threading.Lock()
_NEXT_ID = 1


@_c.MsgFn
def _on_message(msg_ptr):
    try:
        m = msg_ptr.contents
        node = _NODES.get(m.user)
        if node is None:
            return
        # Subscriber handlers on this topic index receive the message INSTEAD of
        # the node-wide on_message.
        handlers = node._sub_handlers.get(m.topic_index)
        if handlers:
            msg = Message._from_c(m, node._topic_specs.get(m.topic_index))
            for fn in list(handlers):
                fn(msg)
        elif node._on_msg is not None:
            node._on_msg(Message._from_c(m, node._topic_specs.get(m.topic_index)))
    except Exception:
        _traceback.print_exc()


@_c.EvtFn
def _on_event(ev_ptr):
    try:
        e = ev_ptr.contents
        node = _NODES.get(e.user)
        if node is not None and node._on_evt is not None:
            node._on_evt(Event._from_c(e))
    except Exception:
        _traceback.print_exc()


# Pattern handler boxes: the C side user pointer carries an id into this registry, never
# a Python reference, so native code never holds a collectable callback.
_PBOXES = {}
_PBOX_LOCK = _threading.Lock()
_PBOX_NEXT = 1


def _pbox_add(box):
    global _PBOX_NEXT
    with _PBOX_LOCK:
        box_id = _PBOX_NEXT
        _PBOX_NEXT += 1
        _PBOXES[box_id] = box
        return box_id


def _pbox_get(box_id):
    with _PBOX_LOCK:
        return _PBOXES.get(box_id)


def _pbox_pop(box_id):
    with _PBOX_LOCK:
        return _PBOXES.pop(box_id, None)


@_c.ReqFn
def _on_request(req_ptr, user):
    try:
        box = _pbox_get(user)
        if box is not None:
            box.dispatch(req_ptr)
    except Exception:
        _traceback.print_exc()


@_c.CancelFn
def _on_task_cancel(token, user):
    try:
        box = _pbox_get(user)
        if box is not None:
            box.cancel(token)
    except Exception:
        _traceback.print_exc()


@_c.RspFn
def _on_response(rsp_ptr):
    try:
        o = rsp_ptr.contents
        box = _pbox_pop(o.user)
        if box is None:
            return
        box.node._drop_async(box.id)
        resp = Response._from_c(o, box.rsp_schema)
        try:
            box.on_response(resp)
        except Exception:
            _traceback.print_exc()
    except Exception:
        _traceback.print_exc()


@_c.VarFn
def _on_var_update(upd_ptr, user):
    try:
        box = _pbox_get(user)
        if box is None:
            return
        u = upd_ptr.contents
        data = _c.string_at(u.value.data, u.value.len) if u.value.data and u.value.len else b""
        val, ok = _decode_payload(u.schema, box.schema, data)
        if not ok:
            val = data
        if box.arity == 1:
            box.handler(val)
        else:
            upd = VariableUpdate()
            upd.name = _c.string_at(u.name.data, u.name.len).decode("utf-8", "replace") \
                       if u.name.data and u.name.len else ""
            upd.data = data
            upd.value = val
            upd.forced = u.forced != 0
            upd.write_seq = u.write_seq
            upd.source = u.source
            upd.recv_us = u.recv_us
            upd.written_us = u.written_us
            box.handler(val, upd)
    except Exception:
        _traceback.print_exc()
