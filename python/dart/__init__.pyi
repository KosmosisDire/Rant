"""The public surface of the dart package as the type checker sees it. The runtime is
__init__.py, and docs/python.md explains the API. Handles are generic in their schema class,
so Publisher[Pose] sends Pose and Subscriber[Pose].take() returns Pose or None."""

from enum import Enum, IntEnum, IntFlag
from threading import Event as _ThreadEvent
from typing import Any, Callable, Generic, Iterable, Mapping, TypeAlias, TypeVar

from . import types as types

__all__: list[str]

T = TypeVar("T")
Req = TypeVar("Req")
Prg = TypeVar("Prg")
Rsp = TypeVar("Rsp")

# The field vocabulary. A scalar annotates a field directly. A capped string, a fixed or
# variable array and a pinned enum width are spelled Annotated[T, "<dsl field type>"], so
# the checker sees T and the runtime reads the DSL: Annotated[str, "string<16>"],
# Annotated[bytes, "u8[4]"], Annotated[list[float], "f32[]"], Annotated[Mode, "u8"].
u8: TypeAlias = int
u16: TypeAlias = int
u32: TypeAlias = int
u64: TypeAlias = int
i8: TypeAlias = int
i16: TypeAlias = int
i32: TypeAlias = int
i64: TypeAlias = int
f32: TypeAlias = float
f64: TypeAlias = float
def string(cap: int) -> Any: ...
def enum(cls: type[Enum], backing: Any = None) -> Any: ...
def dsl(x: Any) -> str: ...

_SchemaArg: TypeAlias = type[Any] | Schema | str | None
_Payload: TypeAlias = bytes | bytearray | memoryview | str | Mapping[str, Any]

class Role(IntEnum):
    PUBSUB = 0
    PUB_ONLY = 1
    SUB_ONLY = 2
    INACTIVE = 3

class SendStatus(IntEnum):
    OK = 0
    NO_TOPIC = -1
    TOO_BIG = -2
    BAD_ROLE = -3
    OUT_OF_MEMORY = -4
    STATE = -5
    NOSYS = -6

class CallStatus(IntEnum):
    OK = 0
    APP_ERROR = 1
    NO_HANDLER = 2
    TIMEOUT = 3
    PEER_LOST = 4
    CANCELLED = 5
    RUNNING = 6

class LogLevel(IntEnum):
    ERROR = 0
    WARN = 1
    INFO = 2

class MetaSection(IntFlag):
    NODE = 1
    PROC = 2
    TOPICS = 4
    PEERS = 8
    ALL = 0

class EventKind(IntEnum):
    PEER_UP = 0
    PEER_DOWN = 1
    PEER_INTEREST = 2
    MSG_LOST = 3
    ERROR = 4

class ErrorKind(IntEnum):
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

class SchemaError(Exception): ...

class CallError(Exception):
    status: CallStatus
    send_status: SendStatus
    def __init__(self, status: CallStatus, send_status: SendStatus, msg: str) -> None: ...

class CancelledError(Exception): ...

class Schema:
    """A compiled schema: DSL text, an annotated class or a bare type."""
    class FieldType(IntEnum):
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
        VSTRING = 14
        VARRAY = 15
        MAP = 16
        ENUM = 17
        NAMED = 18

    class Field:
        name: str
        kind: Schema.FieldType
        elem: Schema.FieldType
        count: int
        depth: int
        offset: int
        size: int
        str_cap: int
        type_name: str
        elem_name: str
        elem_size: int
        arr_parent: int

    def __init__(self, source: type[Any] | str | Any) -> None: ...
    @property
    def name(self) -> str: ...
    @property
    def hash(self) -> int: ...
    @property
    def size(self) -> int: ...
    @property
    def wire(self) -> bytes: ...
    @property
    def dsl(self) -> str: ...
    @property
    def field_count(self) -> int: ...
    @property
    def is_value_root(self) -> bool: ...
    def fields(self) -> list[Schema.Field]: ...
    def can_read(self, pub: Schema) -> bool: ...
    def encode(self, value: Any) -> bytes: ...
    def decode(self, data: bytes) -> Any: ...

class Message(Generic[T]):
    """A delivered message, copied out. value is the decoded T, None on a raw topic."""
    value: T | None
    data: bytes
    topic_name: str
    publisher_id: int
    publisher_name: str
    recv_us: int
    written_us: int
    capture_us: int

class Event:
    """A peer, message loss or error notification. details holds only the fields the kind
    names (docs/node.md), and str(event) is the one line text."""
    kind: EventKind
    error: ErrorKind
    peer: int
    peer_name: str | None
    topic: int
    topic_name: str | None
    details: dict[str, Any]
    @property
    def is_error(self) -> bool: ...

class LogLine:
    level: LogLevel
    node: str
    node_id: int
    wall_us: int
    mono_us: int
    recv_us: int
    written_us: int
    text: str

class MetaSnapshot:
    """A decoded @dart/meta reply: the node and proc scalars, and the whole body in info."""
    valid: bool
    status: CallStatus
    provider: int
    info: dict[str, Any] | None
    name: str
    uptime_us: int
    wall_us: int
    mem_in_use: int
    mem_peak: int
    alloc_calls: int
    evicted_unsent: int
    bp_waited_us: int
    bp_waits: int
    peers: int
    max_peers: int
    topics: int
    max_topics: int
    shm_tx: int
    shm_rx: int
    last_error: int
    last_error_text: str
    have_proc: bool
    have_cpu: bool
    pid: int
    cpu_us: int
    rss: int
    peak_rss: int
    heap_total: int
    heap_free: int
    heap_min_free: int
    heap_largest_free_block: int

class _NodeLog:
    """node.log: the built in log topics. Callable with a level and a line."""
    def __call__(self, level: LogLevel, text: str | bytes) -> SendStatus: ...
    def error(self, text: str | bytes) -> SendStatus: ...
    def warn(self, text: str | bytes) -> SendStatus: ...
    def info(self, text: str | bytes) -> SendStatus: ...
    def on(self, level: LogLevel, handler: Callable[[LogLine], object]) -> bool: ...
    def topic(self, level: LogLevel) -> Topic[Any] | None: ...

class _NodeStats:
    """node.stats: the node's counters."""
    def evicted_unsent(self) -> int: ...
    def memory(self) -> tuple[int, int, int]: ...
    def backpressure(self) -> tuple[int, int]: ...

class _TopicStats:
    """topic.stats: the counters behind a topic."""
    def traffic(self) -> tuple[int, int, int, int]: ...
    def queue(self) -> tuple[int, int, int, int]: ...

class Node:
    """A DART node: sockets, discovery and topics. Options are docs/getting-started.md's,
    durations in seconds with 0 = the default. on_event prints to stderr when None."""
    def __init__(self, name: str | None = None, *,
                 on_message: Callable[[Message[Any]], object] | None = None,
                 on_event: Callable[[Event], object] | None = None,
                 domain: int = 0, multicast_interface: str | None = None, max_topics: int = 0,
                 match_wait: float | None = 0.0, disable_shm: bool = False,
                 fetch_details: bool = False, disable_logs: bool = False,
                 disable_meta: bool = False, disable_error_logs: bool = False,
                 data_port: int = 0, discovery_group: str | None = None,
                 discovery_port: int = 0, multicast_ttl: int = 0,
                 seed_peers: Iterable[str] = (), unicast_only: bool = False,
                 self_ip: str | None = None, advertise_port: int = 0, fragment_size: int = 0,
                 recv_buffer_bytes: int = 0, send_buffer_bytes: int = 0,
                 announce_interval: float = 0.0, peer_timeout: float = 0.0,
                 max_peers: int = 0) -> None: ...
    @property
    def name(self) -> str | None: ...
    @property
    def log(self) -> _NodeLog: ...
    @property
    def stats(self) -> _NodeStats: ...
    def start(self) -> bool: ...
    def stop(self) -> bool: ...
    def is_started(self) -> bool: ...
    def poll(self, timeout: float | None = 0.0) -> int: ...
    def dispatch(self, max_msgs: int = 0, timeout: float | None = 0.0) -> int: ...
    def settle(self, timeout: float | None = None) -> bool: ...
    def close(self, send_bye: bool = True) -> bool: ...
    def on_message(self, fn: Callable[[Message[Any]], object] | None) -> Callable[[Message[Any]], object] | None: ...
    def on_event(self, fn: Callable[[Event], object]) -> Callable[[Event], object]: ...
    def last_error(self) -> Event: ...
    def meta(self, peer: int, sections: MetaSection = ..., timeout: float | None = 1.0) -> MetaSnapshot: ...
    def meta_async(self, peer: int, on_snapshot: Callable[[MetaSnapshot], object],
                   sections: MetaSection = ...) -> SendStatus: ...

class Topic(Generic[T]):
    """A topic handle. Topic[T](node, name) is typed, Topic(node, name) raw. The QoS
    keywords are docs/topics.md's, durations in seconds, 0 = the default."""
    def __init__(self, node: Node, name: str, schema: type[T] | Schema | str | None = None, *,
                 role: Role = ..., reliable: bool = False, keep_last: int = 0,
                 catch_up: int = 0, max_message_bytes: int = 0, heartbeat: float = 0.0,
                 repair_delay: float = 0.0, backpressure_wait: float = 0.0,
                 shm_max_bytes: int = 0, queue_bytes: int = 0, max_rate_hz: int = 0,
                 no_timestamp: bool = False) -> None: ...
    @property
    def name(self) -> str | None: ...
    @property
    def schema(self) -> Schema | None: ...
    @property
    def stats(self) -> _TopicStats: ...
    def send(self, data: T | _Payload, capture_us: int = 0) -> SendStatus: ...
    def take(self, timeout: float | None = 0.0) -> Message[T] | None: ...
    def dispatch(self, max_msgs: int = 0, timeout: float | None = 0.0) -> int: ...
    def match_count(self) -> int: ...
    def ready(self) -> bool: ...
    def pending_count(self) -> int: ...
    def drain(self, timeout: float | None) -> bool: ...
    def set_role(self, role: Role) -> SendStatus: ...
    def retire(self) -> SendStatus: ...

class Publisher(Generic[T]):
    """The publish side of a topic. Publisher[T](node, name, **qos)."""
    topic: Topic[T]
    def __init__(self, node: Node, name: str, schema: type[T] | Schema | str | None = None, *,
                 reliable: bool = False, keep_last: int = 0, catch_up: int = 0,
                 max_message_bytes: int = 0, heartbeat: float = 0.0, repair_delay: float = 0.0,
                 backpressure_wait: float = 0.0, shm_max_bytes: int = 0, queue_bytes: int = 0,
                 max_rate_hz: int = 0, no_timestamp: bool = False) -> None: ...
    @property
    def name(self) -> str | None: ...
    def send(self, data: T | _Payload, capture_us: int = 0) -> SendStatus: ...
    def match_count(self) -> int: ...
    def ready(self) -> bool: ...
    def pending_count(self) -> int: ...

class Subscriber(Generic[T]):
    """The subscribe side. The handler takes the decoded T, or T and the Message, and runs
    on the polling thread. Without one, take() and dispatch() consume the queue."""
    topic: Topic[T]
    def __init__(self, node: Node, name: str,
                 handler: Callable[[T], object] | Callable[[T, Message[T]], object] | None = None,
                 schema: type[T] | Schema | str | None = None, *,
                 reliable: bool = False, keep_last: int = 0, catch_up: int = 0,
                 max_message_bytes: int = 0, heartbeat: float = 0.0, repair_delay: float = 0.0,
                 backpressure_wait: float = 0.0, shm_max_bytes: int = 0, queue_bytes: int = 0,
                 max_rate_hz: int = 0, no_timestamp: bool = False) -> None: ...
    @property
    def name(self) -> str | None: ...
    def take(self, timeout: float | None = 0.0) -> T | None: ...
    def dispatch(self, max_msgs: int = 0, timeout: float | None = 0.0) -> int: ...
    def match_count(self) -> int: ...
    def ready(self) -> bool: ...

class Request(Generic[Rsp]):
    """A function call as the two argument handler sees it, valid while the handler runs."""
    data: bytes
    caller: int
    caller_name: str
    function_name: str
    recv_us: int
    written_us: int
    @property
    def answered(self) -> bool: ...
    def reply(self, rsp: Rsp | _Payload | None = None) -> SendStatus: ...
    def fail(self, message: str | None = None, rsp: Rsp | _Payload | None = None) -> SendStatus: ...
    def defer(self) -> Deferred[Rsp]: ...

class Deferred(Generic[Rsp]):
    """A parked reply from Request.defer: complete or fail once, from any thread."""
    @property
    def valid(self) -> bool: ...
    def complete(self, rsp: Rsp | _Payload | None = None, message: str | None = None) -> SendStatus: ...
    def fail(self, message: str | None = None, rsp: Rsp | _Payload | None = None) -> SendStatus: ...

class Response(Generic[Rsp]):
    """A call's outcome. status and data never raise, value raises CallError unless ok."""
    status: CallStatus
    send_status: SendStatus
    provider: int
    written_us: int
    data: bytes
    message: str
    @property
    def ok(self) -> bool: ...
    @property
    def value(self) -> Rsp: ...

class Progress(Generic[Prg]):
    """One task progress update. value is None for the RUNNING ack."""
    value: Prg | None
    data: bytes
    call_id: int
    provider: int
    written_us: int
    recv_us: int

class TaskRequest(Generic[Req, Prg]):
    """A task call as its handler sees it, on a worker thread per call."""
    value: Req
    data: bytes
    caller: int
    caller_name: str
    function_name: str
    recv_us: int
    written_us: int
    cancel_event: _ThreadEvent
    @property
    def cancelled(self) -> bool: ...
    def progress(self, value: Prg | _Payload | None = None) -> SendStatus: ...

class VariableUpdate(Generic[T]):
    """The state just applied to a variable, for on_change and on_write handlers."""
    name: str
    value: T | None
    data: bytes
    forced: bool
    write_seq: int
    source: int
    recv_us: int
    written_us: int

_FnHandler: TypeAlias = (Callable[[Req], Rsp | _Payload | None]
                         | Callable[[Req, Request[Rsp]], object] | None)
_RspHandler: TypeAlias = Callable[[Response[Rsp]], object]
_PrgHandler: TypeAlias = (Callable[[Prg | None], object]
                          | Callable[[Prg | None, Progress[Prg]], object] | None)
_VarHandler: TypeAlias = (Callable[[T], object]
                          | Callable[[T, VariableUpdate[T]], object] | None)

class FunctionDefinition(Generic[Req, Rsp]):
    """The implementation side of a function: one reply per call, one definition per name."""
    def __init__(self, node: Node, name: str, handler: _FnHandler[Req, Rsp],
                 req_schema: type[Req] | Schema | str | None = None,
                 rsp_schema: type[Rsp] | Schema | str | None = None, *,
                 backpressure_wait: float = 0.0, timeout: float = 0.0, keep_last: int = 0) -> None: ...
    @property
    def name(self) -> str | None: ...
    def match_count(self) -> int: ...
    def retire(self) -> SendStatus: ...

class RemoteFunction(Generic[Req, Rsp]):
    """A function defined on another node."""
    def __init__(self, node: Node, name: str,
                 req_schema: type[Req] | Schema | str | None = None,
                 rsp_schema: type[Rsp] | Schema | str | None = None, *,
                 backpressure_wait: float = 0.0, timeout: float = 0.0, keep_last: int = 0) -> None: ...
    @property
    def name(self) -> str | None: ...
    def call(self, req: Req | _Payload | None = None, timeout: float | None = None,
             provider: int = 0) -> Response[Rsp]: ...
    def call_async(self, req: Req | _Payload | None, on_response: _RspHandler[Rsp],
                   provider: int = 0) -> SendStatus: ...
    def match_count(self) -> int: ...
    def retire(self) -> SendStatus: ...

class TaskDefinition(Generic[Req, Prg, Rsp]):
    """The implementation side of a task: a handler per call on its own thread."""
    def __init__(self, node: Node, name: str,
                 handler: Callable[[TaskRequest[Req, Prg]], Rsp | _Payload | None] | None,
                 req_schema: type[Req] | Schema | str | None = None,
                 prg_schema: type[Prg] | Schema | str | None = None,
                 rsp_schema: type[Rsp] | Schema | str | None = None, *,
                 progress_best_effort: bool = False, progress_keep_last: int = 0,
                 no_cancel: bool = False, exclusive: bool = False, multi: bool = False,
                 timeout: float = 0.0, backpressure_wait: float = 0.0, keep_last: int = 0) -> None: ...
    @property
    def name(self) -> str | None: ...
    def match_count(self) -> int: ...
    def retire(self) -> SendStatus: ...

class RemoteTask(Generic[Req, Prg, Rsp]):
    """A task defined elsewhere. Every request is directed at one provider."""
    def __init__(self, node: Node, name: str,
                 req_schema: type[Req] | Schema | str | None = None,
                 prg_schema: type[Prg] | Schema | str | None = None,
                 rsp_schema: type[Rsp] | Schema | str | None = None, *,
                 progress_best_effort: bool = False, progress_keep_last: int = 0,
                 timeout: float = 0.0, backpressure_wait: float = 0.0, keep_last: int = 0) -> None: ...
    @property
    def name(self) -> str | None: ...
    def call(self, req: Req | _Payload | None = None, on_progress: _PrgHandler[Prg] = None,
             timeout: float | None = None, provider: int = 0) -> Response[Rsp]: ...
    def call_async(self, req: Req | _Payload | None, on_response: _RspHandler[Rsp],
                   on_progress: _PrgHandler[Prg] = None, provider: int = 0) -> int: ...
    def cancel(self, call_id: int) -> SendStatus: ...
    def match_count(self) -> int: ...
    def retire(self) -> SendStatus: ...

class VariableDefinition(Generic[T]):
    """Replicated state with one owner: this node holds the authoritative value."""
    def __init__(self, node: Node, name: str, schema: type[T] | Schema | str | None = None, *,
                 initial: T | _Payload | None = None, read_only: bool = False,
                 allow_force: bool = False, catch_up: int = 0, keep_last: int = 0,
                 backpressure_wait: float = 0.0) -> None: ...
    @property
    def name(self) -> str | None: ...
    def get(self) -> T | None: ...
    def set(self, value: T | _Payload) -> SendStatus: ...
    def force(self, value: T | _Payload) -> SendStatus: ...
    def unforce(self) -> SendStatus: ...
    def forced(self) -> bool: ...
    def wait(self, timeout: float | None) -> bool: ...
    def on_change(self, handler: _VarHandler[T]) -> _VarHandler[T]: ...
    def on_write(self, handler: _VarHandler[T]) -> _VarHandler[T]: ...
    def match_count(self) -> int: ...
    def retire(self) -> SendStatus: ...

class RemoteVariable(VariableDefinition[T]):
    """A variable owned elsewhere: reads see the cached latest, writes go to the owner."""
    def __init__(self, node: Node, name: str, schema: type[T] | Schema | str | None = None, *,
                 catch_up: int = 0, keep_last: int = 0, backpressure_wait: float = 0.0) -> None: ...
