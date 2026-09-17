"""The public surface of the rant package as the type checker sees it. The runtime is
__init__.py, and docs/python.md explains the API. Every handle comes from a node method and
is generic in its schema class, so node.publisher("pose", Pose) sends Pose and
node.subscriber("pose", Pose).take() returns Pose or None."""

from enum import Enum, IntEnum, IntFlag
from threading import Event as _ThreadEvent
from typing import Any, Callable, Generic, Iterable, Mapping, NamedTuple, TypeAlias, TypeVar, overload

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

_Payload: TypeAlias = bytes | bytearray | memoryview | str | Mapping[str, Any]

class Threading(IntEnum):
    SERVICE_THREAD = 0
    MANUAL = 1

class SendStatus(IntEnum):
    OK = 0
    NO_TOPIC = -1
    TOO_BIG = -2
    BAD_ROLE = -3
    OUT_OF_MEMORY = -4
    STATE = -5
    NOSYS = -6
    SCHEMA = -7

class CallStatus(IntEnum):
    OK = 0
    APP_ERROR = 1
    NO_HANDLER = 2
    TIMEOUT = 3
    PEER_LOST = 4
    CANCELLED = 5
    RUNNING = 6
    NO_PROVIDER = 7

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
    BAD_NAME = 24
    STATE = 25
    BAD_SCHEMA = 26

class PeerLiveness(IntEnum):
    ACTIVE = 0
    DROPPED = 1

class EntityKind(IntEnum):
    TOPIC = 0
    FUNCTION = 1
    VARIABLE = 2
    TASK = 3

class Error(Exception):
    """A refused construction. event is the C diagnostic behind it, else None."""
    event: Event | None
    def __init__(self, msg: str, event: Event | None = None) -> None: ...

class SchemaError(Error): ...

class CallError(Error):
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
    def dsl(self) -> str: ...
    @property
    def fields(self) -> list[Schema.Field]: ...
    def enum_variants(self, field: int) -> list[tuple[str, int]]: ...
    def can_read(self, pub: Schema) -> bool: ...
    def encode(self, value: Any) -> bytes: ...
    def decode(self, data: bytes) -> Any: ...

_SchemaArg: TypeAlias = type[Any] | Schema | str | None

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
    """A peer, message loss or error notification. The fields past topic_name are the C
    event's, zero or None where the kind does not set them (docs/node.md). str(event) is
    the one line text."""
    kind: EventKind
    error: ErrorKind
    peer: int
    peer_name: str | None
    topic: int
    topic_name: str | None
    os_error: int
    lost_first: int
    lost_count: int
    too_big_bytes: int
    identity: int
    publish_topics: int
    receive_topics: int
    schema_detail: str | None
    @property
    def is_error(self) -> bool: ...

class NodeStats:
    """A node's counters as one snapshot, from node.stats."""
    evicted_unsent: int
    mem_in_use: int
    mem_peak: int
    alloc_calls: int
    backpressure_waited_us: int
    backpressure_waits: int

class TopicCounts(NamedTuple):
    tx_msgs: int
    tx_bytes: int
    rx_msgs: int
    rx_bytes: int

class QueueStats(NamedTuple):
    messages: int
    bytes: int
    capacity: int
    dropped: int

class LogLine:
    level: LogLevel
    node: str
    node_id: int
    wall_us: int
    mono_us: int
    recv_us: int
    written_us: int
    text: str

class Peer:
    """One discovered peer. A dropped peer stays listed, so gate on active."""
    id: int
    uuid: bytes
    name: str
    address: str
    liveness: PeerLiveness
    last_heard_us: int
    epoch: int
    catching_up: bool
    fragment_size: int
    rtt_us: int
    rtt_jitter_us: int
    rtt_min_us: int
    rtt_samples: int
    @property
    def active(self) -> bool: ...

class Entity:
    """One entity of the mesh: a topic, a function, a variable or a task. The schemas are
    owned copies, None when untyped or not fetched."""
    kind: EntityKind
    name: str
    hash: int
    provides: bool
    consumes: bool
    reliable: bool
    writable: bool
    forceable: bool
    cancellable: bool
    exclusive: bool
    multi: bool
    incomplete: bool
    conflict: bool
    providers: int
    consumers: int
    provider: int
    from_: str
    schema: Schema | None
    schema_hash: int
    rsp_schema: Schema | None
    rsp_schema_hash: int
    progress_schema: Schema | None
    progress_schema_hash: int
    generation: int

class MetaSnapshot:
    """A decoded @rant/meta reply: the node and proc scalars, and the whole body in info."""
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

class Reflection:
    """The walks of docs/reflection.md, from node.reflection. Every result is a copied
    snapshot, so it outlives the poll and needs no lock."""
    def peers(self) -> list[Peer]: ...
    def entities(self, peer: int = 0) -> list[Entity]: ...
    def mesh(self) -> list[Entity]: ...
    def find(self, kind: EntityKind, name: str) -> Entity | None: ...
    @property
    def epoch(self) -> int: ...
    def meta(self, peer: int, sections: MetaSection = ..., timeout: float | None = 1.0) -> MetaSnapshot: ...
    def meta_async(self, peer: int, on_snapshot: Callable[[MetaSnapshot], object],
                   sections: MetaSection = ...) -> SendStatus: ...

_MsgHandler: TypeAlias = Callable[[T], object] | Callable[[T, Message[T]], object] | None
_FnHandler: TypeAlias = (Callable[[Req], Rsp | _Payload | None]
                         | Callable[[Req, Request[Rsp]], object] | None)
_TaskHandler: TypeAlias = (Callable[[Req], Rsp | _Payload | None]
                           | Callable[[Req, TaskContext[Prg]], Rsp | _Payload | None] | None)
_RspHandler: TypeAlias = Callable[[Response[Rsp]], object]
_PrgHandler: TypeAlias = (Callable[[Prg | None], object]
                          | Callable[[Prg | None, Progress[Prg]], object] | None)
_VarHandler: TypeAlias = (Callable[[T], object]
                          | Callable[[T, VariableUpdate[T]], object] | None)

class Node:
    """One participant on the mesh: Node(name, **options). Its publisher, subscriber,
    function, task and variable methods create the handles. Options are
    docs/getting-started.md's, durations in seconds with 0 = the default. on_event prints
    to stderr when None. The service thread runs from construction unless threading is
    MANUAL."""
    def __init__(self, name: str | None = None, *,
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
                 max_peers: int = 0,
                 threading: Threading = Threading.SERVICE_THREAD) -> None: ...
    @property
    def name(self) -> str | None: ...
    @property
    def threading(self) -> Threading: ...
    @property
    def stats(self) -> NodeStats: ...
    @property
    def last_error(self) -> Event: ...
    @property
    def reflection(self) -> Reflection: ...

    # The topic keywords are docs/topics.md's, durations in seconds, 0 = the default.
    @overload
    def publisher(self, name: str, schema: type[T], *, reliable: bool = False,
                  keep_last: int = 0, catch_up: int = 0, max_message_bytes: int = 0,
                  heartbeat: float = 0.0, repair_delay: float = 0.0,
                  backpressure_wait: float = 0.0, shm_max_bytes: int = 0, queue_bytes: int = 0,
                  max_rate_hz: int = 0, no_timestamp: bool = False,
                  reflect_from_mesh: bool = False) -> Publisher[T]: ...
    @overload
    def publisher(self, name: str, schema: Schema | str, *, reliable: bool = False,
                  keep_last: int = 0, catch_up: int = 0, max_message_bytes: int = 0,
                  heartbeat: float = 0.0, repair_delay: float = 0.0,
                  backpressure_wait: float = 0.0, shm_max_bytes: int = 0, queue_bytes: int = 0,
                  max_rate_hz: int = 0, no_timestamp: bool = False,
                  reflect_from_mesh: bool = False) -> Publisher[Any]: ...
    @overload
    def publisher(self, name: str, schema: None = None, *, reliable: bool = False,
                  keep_last: int = 0, catch_up: int = 0, max_message_bytes: int = 0,
                  heartbeat: float = 0.0, repair_delay: float = 0.0,
                  backpressure_wait: float = 0.0, shm_max_bytes: int = 0, queue_bytes: int = 0,
                  max_rate_hz: int = 0, no_timestamp: bool = False,
                  reflect_from_mesh: bool = False) -> Publisher[bytes]: ...
    @overload
    def subscriber(self, name: str, schema: type[T], handler: _MsgHandler[T] = None, *,
                   reliable: bool = False, keep_last: int = 0, catch_up: int = 0,
                   max_message_bytes: int = 0, heartbeat: float = 0.0, repair_delay: float = 0.0,
                   backpressure_wait: float = 0.0, shm_max_bytes: int = 0, queue_bytes: int = 0,
                   max_rate_hz: int = 0, no_timestamp: bool = False,
                   reflect_from_mesh: bool = False) -> Subscriber[T]: ...
    @overload
    def subscriber(self, name: str, schema: Schema | str, handler: _MsgHandler[Any] = None, *,
                   reliable: bool = False, keep_last: int = 0, catch_up: int = 0,
                   max_message_bytes: int = 0, heartbeat: float = 0.0, repair_delay: float = 0.0,
                   backpressure_wait: float = 0.0, shm_max_bytes: int = 0, queue_bytes: int = 0,
                   max_rate_hz: int = 0, no_timestamp: bool = False,
                   reflect_from_mesh: bool = False) -> Subscriber[Any]: ...
    @overload
    def subscriber(self, name: str, schema: None = None,
                   handler: Callable[[bytes], object] | Callable[[bytes, Message[bytes]], object] | None = None, *,
                   reliable: bool = False, keep_last: int = 0, catch_up: int = 0,
                   max_message_bytes: int = 0, heartbeat: float = 0.0, repair_delay: float = 0.0,
                   backpressure_wait: float = 0.0, shm_max_bytes: int = 0, queue_bytes: int = 0,
                   max_rate_hz: int = 0, no_timestamp: bool = False,
                   reflect_from_mesh: bool = False) -> Subscriber[bytes]: ...

    def function_definition(self, name: str, handler: _FnHandler[Req, Rsp],
                            req_schema: type[Req] | Schema | str | None = None,
                            rsp_schema: type[Rsp] | Schema | str | None = None, *,
                            backpressure_wait: float = 0.0, timeout: float = 0.0,
                            keep_last: int = 0, multi: bool = False,
                            reflect_from_mesh: bool = False) -> FunctionDefinition[Req, Rsp]: ...
    def remote_function(self, name: str,
                        req_schema: type[Req] | Schema | str | None = None,
                        rsp_schema: type[Rsp] | Schema | str | None = None, *,
                        backpressure_wait: float = 0.0, timeout: float = 0.0,
                        keep_last: int = 0, multi: bool = False,
                        reflect_from_mesh: bool = False) -> RemoteFunction[Req, Rsp]: ...
    def task_definition(self, name: str, handler: _TaskHandler[Req, Prg, Rsp],
                        req_schema: type[Req] | Schema | str | None = None,
                        prg_schema: type[Prg] | Schema | str | None = None,
                        rsp_schema: type[Rsp] | Schema | str | None = None, *,
                        progress_best_effort: bool = False, progress_keep_last: int = 0,
                        no_cancel: bool = False, exclusive: bool = False, multi: bool = False,
                        timeout: float = 0.0, backpressure_wait: float = 0.0,
                        keep_last: int = 0,
                        reflect_from_mesh: bool = False) -> TaskDefinition[Req, Prg, Rsp]: ...
    def remote_task(self, name: str,
                    req_schema: type[Req] | Schema | str | None = None,
                    prg_schema: type[Prg] | Schema | str | None = None,
                    rsp_schema: type[Rsp] | Schema | str | None = None, *,
                    progress_best_effort: bool = False, progress_keep_last: int = 0,
                    timeout: float = 0.0, backpressure_wait: float = 0.0, keep_last: int = 0,
                    reflect_from_mesh: bool = False) -> RemoteTask[Req, Prg, Rsp]: ...
    def variable_definition(self, name: str, schema: type[T] | Schema | str | None = None, *,
                            initial: T | _Payload | None = None, read_only: bool = False,
                            allow_force: bool = False, catch_up: int = 0, keep_last: int = 0,
                            backpressure_wait: float = 0.0,
                            reflect_from_mesh: bool = False) -> VariableDefinition[T]: ...
    def remote_variable(self, name: str, schema: type[T] | Schema | str | None = None, *,
                        catch_up: int = 0, keep_last: int = 0, backpressure_wait: float = 0.0,
                        reflect_from_mesh: bool = False) -> RemoteVariable[T]: ...

    def poll(self, timeout: float | None = 0.0) -> int: ...
    def dispatch(self, max_msgs: int = 0, timeout: float | None = 0.0) -> int: ...
    def settle(self, timeout: float | None = None) -> bool: ...
    def log(self, level: LogLevel, text: str | bytes) -> SendStatus: ...
    def on_log(self, handler: Callable[[LogLine], object] | None) -> Callable[[LogLine], object] | None: ...
    def close(self, send_bye: bool = True) -> bool: ...
    def __enter__(self) -> Node: ...
    def __exit__(self, *exc: object) -> None: ...

class Publisher(Generic[T]):
    """The publishing side of a topic, from node.publisher."""
    @property
    def name(self) -> str: ...
    @property
    def schema(self) -> Schema | None: ...
    def send(self, value: T | _Payload, capture_us: int = 0) -> SendStatus: ...
    @property
    def match_count(self) -> int: ...
    @property
    def ready(self) -> bool: ...
    @property
    def counts(self) -> TopicCounts: ...
    def drain(self, timeout: float | None) -> bool: ...
    def refresh(self) -> bool: ...
    def close(self) -> bool: ...
    def __enter__(self) -> Publisher[T]: ...
    def __exit__(self, *exc: object) -> None: ...

class Subscriber(Generic[T]):
    """The subscribing side of a topic, from node.subscriber. The handler takes the decoded T,
    or T and the Message, and runs on the polling thread. Without one, take() and dispatch()
    consume the queue."""
    @property
    def name(self) -> str: ...
    @property
    def schema(self) -> Schema | None: ...
    def take(self, timeout: float | None = 0.0) -> T | None: ...
    def dispatch(self, max_msgs: int = 0, timeout: float | None = 0.0) -> int: ...
    @property
    def counts(self) -> TopicCounts: ...
    @property
    def queue_stats(self) -> QueueStats: ...
    def refresh(self) -> bool: ...
    def close(self) -> bool: ...
    def __enter__(self) -> Subscriber[T]: ...
    def __exit__(self, *exc: object) -> None: ...

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
    def reply(self, rsp: Rsp | _Payload | None = None) -> None: ...
    def fail(self, message: str | None = None, rsp: Rsp | _Payload | None = None) -> None: ...
    def defer(self) -> Deferred[Rsp]: ...

class Deferred(Generic[Rsp]):
    """A parked reply from Request.defer: complete or fail once, from any thread."""
    @property
    def valid(self) -> bool: ...
    def complete(self, rsp: Rsp | _Payload | None = None, message: str | None = None) -> bool: ...
    def fail(self, message: str | None = None, rsp: Rsp | _Payload | None = None) -> bool: ...

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

class TaskContext(Generic[Prg]):
    """What a task handler works through, on a worker thread per call: progress,
    cancellation and who called."""
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

class FunctionDefinition(Generic[Req, Rsp]):
    """The implementation side of a function, from node.function_definition."""
    @property
    def name(self) -> str: ...
    @property
    def match_count(self) -> int: ...
    def refresh(self) -> bool: ...
    def close(self) -> bool: ...
    def __enter__(self) -> FunctionDefinition[Req, Rsp]: ...
    def __exit__(self, *exc: object) -> None: ...

class RemoteFunction(Generic[Req, Rsp]):
    """The caller side of a function defined on another node, from node.remote_function."""
    @property
    def name(self) -> str: ...
    def call(self, req: Req | _Payload | None = None, timeout: float | None = None,
             provider: int = 0) -> Response[Rsp]: ...
    def call_async(self, req: Req | _Payload | None, on_response: _RspHandler[Rsp],
                   provider: int = 0) -> SendStatus: ...
    @property
    def match_count(self) -> int: ...
    def refresh(self) -> bool: ...
    def close(self) -> bool: ...
    def __enter__(self) -> RemoteFunction[Req, Rsp]: ...
    def __exit__(self, *exc: object) -> None: ...

class TaskDefinition(Generic[Req, Prg, Rsp]):
    """The implementation side of a task, from node.task_definition: a handler per call on
    its own thread."""
    @property
    def name(self) -> str: ...
    @property
    def match_count(self) -> int: ...
    def refresh(self) -> bool: ...
    def close(self) -> bool: ...
    def __enter__(self) -> TaskDefinition[Req, Prg, Rsp]: ...
    def __exit__(self, *exc: object) -> None: ...

class RemoteTask(Generic[Req, Prg, Rsp]):
    """The caller side of a task defined elsewhere, from node.remote_task. Every request is
    directed at one provider."""
    @property
    def name(self) -> str: ...
    def call(self, req: Req | _Payload | None = None, on_progress: _PrgHandler[Prg] = None,
             timeout: float | None = None, provider: int = 0) -> Response[Rsp]: ...
    def call_async(self, req: Req | _Payload | None, on_response: _RspHandler[Rsp],
                   on_progress: _PrgHandler[Prg] = None, provider: int = 0) -> int: ...
    def cancel(self, call_id: int) -> SendStatus: ...
    @property
    def match_count(self) -> int: ...
    def refresh(self) -> bool: ...
    def close(self) -> bool: ...
    def __enter__(self) -> RemoteTask[Req, Prg, Rsp]: ...
    def __exit__(self, *exc: object) -> None: ...

class Variable(Generic[T]):
    """The surface shared by VariableDefinition and RemoteVariable: read the latest value,
    write, force and observe. Reads are status free, every write returns its SendStatus."""
    @property
    def name(self) -> str: ...
    @property
    def schema(self) -> Schema | None: ...
    def get(self) -> T | None: ...
    def set(self, value: T | _Payload) -> SendStatus: ...
    def force(self, value: T | _Payload) -> SendStatus: ...
    def unforce(self) -> SendStatus: ...
    @property
    def forced(self) -> bool: ...
    def wait(self, timeout: float | None) -> bool: ...
    @property
    def match_count(self) -> int: ...
    def on_change(self, handler: _VarHandler[T]) -> _VarHandler[T]: ...
    def on_write(self, handler: _VarHandler[T]) -> _VarHandler[T]: ...
    def refresh(self) -> bool: ...
    def close(self) -> bool: ...
    def __enter__(self) -> Variable[T]: ...
    def __exit__(self, *exc: object) -> None: ...

class VariableDefinition(Variable[T]):
    """The authoritative variable, from node.variable_definition: this node owns the value."""

class RemoteVariable(Variable[T]):
    """A variable owned elsewhere, from node.remote_variable: reads see the cached latest,
    writes go to the owner."""
