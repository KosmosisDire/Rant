"""The native side of the wrapper: the ctypes layouts mirroring the C structs
(spec/bindings.md), the callback types and the library loader. Private to the package."""

import ctypes
import os
import platform
import sys
import threading
from ctypes import (POINTER, CFUNCTYPE, Structure, Union, byref, cast, memset,
                    pointer, sizeof, string_at, create_string_buffer, c_char_p, c_void_p,
                    c_int, c_int32, c_int64, c_uint8, c_uint16, c_uint32, c_uint64,
                    c_size_t, c_double, c_float, c_ubyte)


def library_path():
    """DART_LIBRARY, else the copy the wheel carries next to this file, else the
    dist/native/<rid>/ copy of a source checkout."""
    name = {"win32": "dart.dll", "darwin": "libdart.dylib"}.get(sys.platform, "libdart.so")
    override = os.environ.get("DART_LIBRARY")
    if override:
        return override
    here = os.path.dirname(os.path.abspath(__file__))
    bundled = os.path.join(here, name)
    if os.path.exists(bundled):
        return bundled
    arch = "arm64" if platform.machine().lower() in ("arm64", "aarch64") else "x64"
    if sys.platform == "darwin":
        rid = "osx"
    else:
        rid = ("win-" if sys.platform == "win32" else "linux-") + arch
    in_tree = os.path.join(here, "..", "..", "dist", "native", rid, name)
    if os.path.exists(in_tree):
        return in_tree
    raise RuntimeError("dart: no native library found. Install the wheel (pip install "
                       "dart-middleware), build the CMake target dart_shared in a source "
                       "checkout, or point DART_LIBRARY at the library.")


# The ctypes layouts, which must mirror the C structs exactly (spec/bindings.md).

class DartBytes(Structure):
    _fields_ = [("data", c_void_p), ("len", c_size_t)]


# the C DartString, a length carrying view. Named View so the public string field
# marker owns the plain dart.string name
class DartStringView(Structure):
    _fields_ = [("data", c_void_p), ("len", c_size_t)]


class DartQos(Structure):
    _fields_ = [
        ("reliability", c_int),
        ("keep_last", c_uint16),
        ("catch_up", c_uint16),
        ("max_message_bytes", c_uint32),
        ("heartbeat_us", c_uint32),
        ("repair_delay_us", c_uint32),
        ("backpressure_wait_us", c_uint32),
        ("shm_max_bytes", c_uint32),
        ("queue_bytes", c_uint32),
        ("max_rate_hz", c_uint16),
        ("no_timestamp", c_uint8),
    ]


class DartTopicOpts(Structure):
    _fields_ = [("qos", DartQos)]


class DartDiscoveryAddr(Structure):
    _fields_ = [
        ("ip", c_uint8 * 16),   # network-order bytes
        ("ip_len", c_uint8),    # 4 = IPv4, 16 = IPv6
        ("port", c_uint16),     # host order, 0 = the discovery port
    ]


def seed_addrs(seeds):
    """Marshal "ip" / "ip:port" strings into a C array of DartDiscoveryAddr (IPv4).
    The node copies the array at open, so it need not outlive the call."""
    if not seeds:
        return None, 0
    arr = (DartDiscoveryAddr * len(seeds))()
    for i, s in enumerate(seeds):
        host, _, port = s.partition(":")
        octets = host.split(".")
        if len(octets) != 4:
            raise ValueError("seed_peers entry %r is not an IPv4 address" % s)
        for k, o in enumerate(octets):
            v = int(o)
            if not 0 <= v <= 255:
                raise ValueError("seed_peers entry %r is not an IPv4 address" % s)
            arr[i].ip[k] = v
        arr[i].ip_len = 4
        arr[i].port = int(port) if port else 0    # 0 = the discovery port
    return arr, len(seeds)


class DartNodeNet(Structure):
    _fields_ = [
        ("data_port", c_uint16),
        ("discovery_group", c_char_p),
        ("discovery_port", c_uint16),
        ("multicast_interface", c_char_p),
        ("multicast_ttl", c_uint8),
        ("seed_peers", c_void_p),
        ("n_seed_peers", c_uint16),
        ("unicast_only", c_uint8),
        ("recv_buffer_bytes", c_uint32),
        ("send_buffer_bytes", c_uint32),
        ("fragment_size", c_uint16),
        ("self_ip", c_char_p),
        ("advertise_port", c_uint16),
    ]


class DartNodeDiscovery(Structure):
    _fields_ = [
        ("announce_interval_us", c_uint32),
        ("peer_timeout_us", c_uint32),
        ("max_peers", c_uint16),
    ]


class DartNodeOpts(Structure):
    _fields_ = [
        ("domain", c_uint16),
        ("max_topics", c_uint16),
        ("user_data", c_void_p),
        ("disable_shm", c_uint8),
        ("fetch_details", c_uint8),
        ("match_wait_ms", c_int32),
        ("disable_logs", c_uint8),
        ("disable_meta", c_uint8),
        ("disable_error_logs", c_uint8),
        ("net", DartNodeNet),
        ("discovery", DartNodeDiscovery),
    ]


class DartMsg(Structure):
    _fields_ = [
        ("node", c_void_p),
        ("user", c_void_p),
        ("topic_index", c_uint16),
        ("publisher_id", c_uint32),
        ("publisher_name", DartStringView),
        ("topic_name", DartStringView),
        ("header", DartBytes),   # the pattern header view, None on a plain topic
        ("data", DartBytes),
        ("schema", c_void_p),
        ("recv_us", c_uint64),
        ("written_us", c_uint64),
        ("capture_us", c_uint64),
    ]


# Optional per send config. capture_us 0 = unstated, and costs no wire bytes.
class DartSendOpts(Structure):
    _fields_ = [("capture_us", c_uint64)]


class DartEvent(Structure):
    _fields_ = [
        ("kind", c_int),
        ("error", c_int),
        ("topic_name", c_char_p),
        ("user", c_void_p),
        ("peer", c_uint32),
        ("topic", c_uint16),
        ("os_error", c_int),
        ("ip", c_uint8 * 16),
        ("ip_len", c_uint8),
        ("port", c_uint16),
        ("lost_first", c_uint64),
        ("lost_count", c_uint64),
        ("too_big_bytes", c_uint64),
        ("identity", c_uint64),
        ("publish_topics", c_uint16),
        ("receive_topics", c_uint16),
        ("schema_detail", c_char_p),
        ("peer_name", c_char_p),
    ]


class DartAllocator(Structure):
    _fields_ = [
        ("page_realloc", c_void_p),
        ("shared", c_void_p),
        ("owned", c_void_p),
        ("free_pool", c_void_p),
        ("page_size", c_uint32),
        ("max_bytes", c_size_t),
        ("in_use", c_size_t),
        ("pooled", c_size_t),
        ("peak", c_size_t),
        ("alloc_calls", c_uint64),
        ("pages_live", c_uint64),
    ]


class DartSchemaFieldInfo(Structure):
    _fields_ = [
        ("name", DartStringView),
        ("type_name", DartStringView),   # the field type's NAME, empty when anonymous
        ("elem_name", DartStringView),   # an array ELEMENT type's name, empty when anonymous
        ("kind", c_uint8),
        ("elem", c_uint8),
        ("count", c_uint16),
        ("depth", c_uint16),
        ("str_cap", c_uint16),
        ("arr_parent", c_uint16),        # flat index of the enclosing struct ARRAY, 0xFFFF none
        ("offset", c_uint32),
        ("size", c_uint32),
        ("elem_size", c_uint32),         # bytes of one array element, else 0
    ]


class _DartValueV(Union):
    _fields_ = [("u", c_uint64), ("i", c_int64), ("f", c_double)]


class DartValue(Structure):
    _fields_ = [
        ("kind", c_uint8),
        ("elem", c_uint8),
        ("count", c_uint16),
        ("str_cap", c_uint16),
        ("v", _DartValueV),
        ("bytes", DartBytes),
    ]


# the pattern struct mirrors of src/patterns/core.h, field order and types exact

# The public head of the C DartRequest, only ever touched through the callback's pointer:
# the reply machinery lives behind the struct, so the exact pointer is what reply takes.
class DartRequest(Structure):
    _fields_ = [
        ("node", c_void_p),
        ("function_name", DartStringView),
        ("data", DartBytes),
        ("schema", c_void_p),
        ("caller", c_uint32),
        ("caller_name", DartStringView),
        ("recv_us", c_uint64),
        ("written_us", c_uint64),
    ]


class DartResponse(Structure):
    _fields_ = [
        ("status", c_int),
        ("data", DartBytes),
        ("schema", c_void_p),
        ("provider", c_uint32),
        ("user", c_void_p),
        ("written_us", c_uint64),
        ("message", DartStringView),   # outcome text (default status text if none sent)
    ]


class DartFunctionOpts(Structure):
    _fields_ = [
        ("backpressure_wait_us", c_uint32),
        ("timeout_us", c_uint32),
        ("keep_last", c_uint16),
        ("multi", c_uint8),
    ]


class DartVariableOpts(Structure):
    _fields_ = [
        ("initial", DartBytes),
        ("access", c_uint8),
        ("allow_force", c_uint8),
        ("catch_up", c_uint16),
        ("keep_last", c_uint16),
        ("backpressure_wait_us", c_uint32),
    ]


class DartVariableUpdate(Structure):
    _fields_ = [
        ("variable", c_void_p),
        ("name", DartStringView),
        ("value", DartBytes),
        ("schema", c_void_p),
        ("forced", c_uint8),
        ("write_seq", c_uint32),
        ("source", c_uint32),
        ("recv_us", c_uint64),
        ("written_us", c_uint64),
    ]


class DartTaskOpts(Structure):
    _fields_ = [
        ("progress_best_effort", c_uint8),
        ("progress_keep_last", c_uint16),
        ("no_cancel", c_uint8),
        ("exclusive", c_uint8),
        ("multi", c_uint8),
        ("timeout_us", c_uint32),
        ("backpressure_wait_us", c_uint32),
        ("keep_last", c_uint16),
    ]


class DartProgress(Structure):
    _fields_ = [
        ("call_id", c_uint32),
        ("provider", c_uint32),
        ("data", DartBytes),      # len 0 = the RUNNING ack
        ("schema", c_void_p),
        ("written_us", c_uint64),
        ("recv_us", c_uint64),
        ("user", c_void_p),
    ]


PrgFn = CFUNCTYPE(None, POINTER(DartProgress))


class DartCallOpts(Structure):
    # direct a call at one definition by peer id, 0 = first answer wins. The task fields
    # stay NULL on a plain call
    _fields_ = [
        ("provider", c_uint32),
        ("on_progress", PrgFn),
        ("progress_user", c_void_p),
        ("id_out", POINTER(c_uint32)),
    ]


DART_ALLOCATOR_PAGE = 64 * 1024   # DART_ALLOCATOR_PAGE default

MsgFn = CFUNCTYPE(None, POINTER(DartMsg))
EvtFn = CFUNCTYPE(None, POINTER(DartEvent))
AllocFn = CFUNCTYPE(c_void_p, c_void_p, c_void_p, c_size_t)
ReqFn = CFUNCTYPE(None, POINTER(DartRequest), c_void_p)
RspFn = CFUNCTYPE(None, POINTER(DartResponse))
VarFn = CFUNCTYPE(None, POINTER(DartVariableUpdate), c_void_p)
CancelFn = CFUNCTYPE(None, c_uint64, c_void_p)
NULL_REQ_FN = cast(None, ReqFn)   # a CFUNCTYPE argtype refuses a bare None
NULL_VAR_FN = cast(None, VarFn)


def bind(lib):
    F = lambda fn, args, ret: (setattr(getattr(lib, fn), "argtypes", args),
                               setattr(getattr(lib, fn), "restype", ret))
    F("dart_allocator_heap", [c_uint32], DartAllocator)
    F("dart_heap_realloc", [c_void_p, c_void_p, c_size_t], c_void_p)
    F("dart_node_open", [POINTER(DartAllocator), c_char_p, MsgFn, EvtFn,
                         POINTER(DartNodeOpts)], c_void_p)
    F("dart_last_error", [c_void_p], DartEvent)
    F("dart_node_poll", [c_void_p, c_int], c_int)
    F("dart_node_close", [c_void_p, c_int], c_int)
    F("dart_node_start", [c_void_p], c_int)
    F("dart_node_stop", [c_void_p], c_int)
    F("dart_node_is_started", [c_void_p], c_int)
    F("dart_node_evicted_unsent", [c_void_p], c_uint32)
    F("dart_node_create_topic", [c_void_p, c_char_p, c_int, c_void_p,
                                   POINTER(DartTopicOpts)], c_void_p)
    F("dart_node_topic", [c_void_p, c_uint16], c_void_p)
    F("dart_topic_send", [c_void_p, DartBytes, POINTER(DartSendOpts)], c_int)
    F("dart_topic_set_role", [c_void_p, c_int], c_int)
    F("dart_topic_retire", [c_void_p], c_int)
    F("dart_topic_index", [c_void_p], c_uint16)
    F("dart_topic_match_count", [c_void_p], c_int)
    F("dart_topic_drain", [c_void_p, c_int], c_int)
    F("dart_topic_take", [c_void_p, POINTER(DartMsg), c_int], c_int)
    F("dart_topic_dispatch", [c_void_p, c_int, c_int], c_int)
    F("dart_node_dispatch", [c_void_p, c_int, c_int], c_int)
    F("dart_topic_queue_stats", [c_void_p, POINTER(c_uint32), POINTER(c_uint32),
                                   POINTER(c_uint32), POINTER(c_uint32)], None)
    F("dart_node_mem_stats", [c_void_p, POINTER(c_size_t), POINTER(c_size_t),
                              POINTER(c_uint64)], None)
    F("dart_node_backpressure_stats", [c_void_p, POINTER(c_uint64),
                                       POINTER(c_uint32)], None)
    F("dart_topic_counts", [c_void_p, POINTER(c_uint64), POINTER(c_uint64),
                            POINTER(c_uint64), POINTER(c_uint64)], None)
    F("dart_node_log_text", [c_void_p, c_int, c_char_p, c_int], c_int)
    F("dart_node_log_topic", [c_void_p, c_int], c_void_p)
    F("dart_node_meta_function", [c_void_p], c_void_p)
    F("dart_event_str", [POINTER(DartEvent), c_char_p, c_size_t], c_char_p)
    F("dart_node_settle", [c_void_p, c_int], c_int)
    F("dart_topic_ready", [c_void_p], c_int)
    F("dart_topic_pending_count", [c_void_p], c_int)
    F("dart_node_lock", [c_void_p], None)
    F("dart_node_unlock", [c_void_p], None)
    # patterns: functions / variables
    F("dart_node_create_function_definition", [c_void_p, c_char_p, c_void_p,
                                               c_void_p, ReqFn, c_void_p,
                                               POINTER(DartFunctionOpts)], c_void_p)
    F("dart_node_create_remote_function", [c_void_p, c_char_p, c_void_p, c_void_p,
                                           POINTER(DartFunctionOpts)], c_void_p)
    F("dart_function_call", [c_void_p, DartBytes, POINTER(DartResponse), c_int, c_void_p], c_int)
    F("dart_function_call_async", [c_void_p, DartBytes, RspFn, c_void_p, c_void_p], c_int)
    F("dart_function_match_count", [c_void_p], c_int)
    F("dart_function_retire", [c_void_p], c_int)
    F("dart_request_reply", [POINTER(DartRequest), DartBytes], None)
    F("dart_request_fail", [POINTER(DartRequest), c_char_p, DartBytes], None)
    F("dart_request_defer", [POINTER(DartRequest)], c_uint64)
    F("dart_function_complete", [c_void_p, c_uint64, c_int, c_char_p, DartBytes], c_int)
    # patterns: tasks
    F("dart_node_create_task_definition", [c_void_p, c_char_p, c_void_p, c_void_p,
                                           c_void_p, ReqFn, c_void_p,
                                           POINTER(DartTaskOpts)], c_void_p)
    F("dart_node_create_remote_task", [c_void_p, c_char_p, c_void_p, c_void_p,
                                       c_void_p, POINTER(DartTaskOpts)], c_void_p)
    F("dart_request_start", [POINTER(DartRequest)], c_int)
    F("dart_function_progress", [c_void_p, c_uint64, DartBytes], c_int)
    F("dart_function_cancelled", [c_void_p, c_uint64], c_int)
    F("dart_function_on_cancel", [c_void_p, CancelFn, c_void_p], c_int)
    F("dart_function_cancel", [c_void_p, c_uint32], c_int)
    F("dart_node_create_variable_definition", [c_void_p, c_char_p, c_void_p,
                                               POINTER(DartVariableOpts)], c_void_p)
    F("dart_node_create_remote_variable", [c_void_p, c_char_p, c_void_p,
                                           POINTER(DartVariableOpts)], c_void_p)
    F("dart_variable_get", [c_void_p, POINTER(DartBytes)], c_int)
    F("dart_variable_set", [c_void_p, DartBytes], c_int)
    F("dart_variable_force", [c_void_p, DartBytes], c_int)
    F("dart_variable_unforce", [c_void_p], c_int)
    F("dart_variable_forced", [c_void_p], c_int)
    F("dart_variable_wait", [c_void_p, c_int], c_int)
    F("dart_variable_match_count", [c_void_p], c_int)
    F("dart_variable_retire", [c_void_p], c_int)
    F("dart_variable_on_change", [c_void_p, VarFn, c_void_p], c_int)
    F("dart_variable_on_write", [c_void_p, VarFn, c_void_p], c_int)
    # serialize / schema
    F("dart_schema_compile", [AllocFn, c_void_p, c_char_p, POINTER(c_char_p)], c_void_p)
    F("dart_schema_free", [c_void_p, AllocFn, c_void_p], None)
    F("dart_schema_wire", [c_void_p], DartBytes)
    F("dart_schema_hash", [c_void_p], c_uint64)
    F("dart_schema_name", [c_void_p], DartStringView)
    F("dart_schema_print", [c_void_p, c_char_p, c_size_t], c_uint32)
    F("dart_schema_subset", [c_void_p, c_void_p], c_int)
    F("dart_std_name", [c_int], c_char_p)
    F("dart_std_by_name", [DartStringView], c_int)
    F("dart_std_recognize", [c_void_p, AllocFn, c_void_p], c_int)
    F("dart_std_recognize_field", [c_void_p, c_uint16, AllocFn, c_void_p], c_int)
    F("dart_std_recognize_elem", [c_void_p, c_uint16, AllocFn, c_void_p], c_int)
    F("dart_timestamp_now", [], c_int64)
    F("dart_schema_size", [c_void_p], c_uint32)
    F("dart_schema_field_count", [c_void_p], c_uint16)
    F("dart_schema_field_at", [c_void_p, c_uint16, POINTER(DartSchemaFieldInfo)], c_int)
    F("dart_schema_field_index", [c_void_p, c_char_p], c_int)
    F("dart_schema_enum_count", [c_void_p, c_uint16], c_uint16)
    F("dart_schema_enum_variant", [c_void_p, c_uint16, c_uint16, POINTER(c_int64),
                                   POINTER(DartStringView)], c_int)
    F("dart_get_enum", [DartBytes, c_void_p, c_char_p], DartStringView)
    F("dart_set_enum", [c_void_p, c_size_t, c_void_p, c_char_p, c_char_p], c_int)
    F("dart_schema_message_default", [c_void_p, c_void_p, c_size_t], c_int)
    F("dart_schema_scalar_size", [c_int], c_uint32)
    F("dart_set_uint", [c_void_p, c_size_t, c_void_p, c_char_p, c_uint64], c_int)
    F("dart_set_int", [c_void_p, c_size_t, c_void_p, c_char_p, c_int64], c_int)
    F("dart_set_f64", [c_void_p, c_size_t, c_void_p, c_char_p, c_double], c_int)
    F("dart_set_f32", [c_void_p, c_size_t, c_void_p, c_char_p, c_float], c_int)
    F("dart_set_array", [c_void_p, c_size_t, c_void_p, c_char_p, DartBytes], c_int)
    F("dart_set_string", [c_void_p, c_size_t, c_void_p, c_char_p, DartStringView], c_int)
    F("dart_set_string_at", [c_void_p, c_size_t, c_void_p, c_char_p, c_uint16,
                             DartStringView], c_int)
    F("dart_get_uint", [DartBytes, c_void_p, c_char_p], c_uint64)
    F("dart_get_int", [DartBytes, c_void_p, c_char_p], c_int64)
    F("dart_get_f64", [DartBytes, c_void_p, c_char_p], c_double)
    F("dart_get_f32", [DartBytes, c_void_p, c_char_p], c_float)
    F("dart_get_array", [DartBytes, c_void_p, c_char_p], DartBytes)
    F("dart_get_value", [DartBytes, c_void_p, c_uint16, POINTER(DartValue)], c_int)
    F("dart_set_value", [c_void_p, c_size_t, c_void_p, c_uint16, POINTER(DartValue)], c_int)
    F("dart_schema_msg_min", [c_void_p], c_uint32)
    F("dart_schema_msg_len", [c_void_p, c_void_p, c_size_t], c_uint32)
    F("dart_set_map", [c_void_p, c_size_t, c_void_p, c_char_p, DartBytes], c_int)


LIB = None
LIB_LOCK = threading.Lock()


def load():
    global LIB
    if LIB is not None:
        return LIB
    with LIB_LOCK:
        if LIB is None:
            lib = ctypes.CDLL(library_path())
            bind(lib)
            LIB = lib
    return LIB


# The schema layer needs a realloc style hook. dart_heap_realloc is the library's own,
# so a schema allocation stays in native code with no callback into Python.
SCHEMA_ALLOC = None


def schema_alloc():
    global SCHEMA_ALLOC
    if SCHEMA_ALLOC is None:
        SCHEMA_ALLOC = cast(load().dart_heap_realloc, AllocFn)
    return SCHEMA_ALLOC
