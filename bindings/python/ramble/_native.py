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
    """RAMBLE_LIBRARY, else the copy the wheel carries next to this file, else the
    dist/native/<rid>/ copy of a source checkout."""
    name = {"win32": "ramble.dll", "darwin": "libramble.dylib"}.get(sys.platform, "libramble.so")
    override = os.environ.get("RAMBLE_LIBRARY")
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
    raise RuntimeError("ramble: no native library found. Install the wheel (pip install "
                       "ramble-middleware), build the CMake target ramble_shared in a source "
                       "checkout, or point RAMBLE_LIBRARY at the library.")


# The ctypes layouts, which must mirror the C structs exactly (spec/bindings.md).

class RambleBytes(Structure):
    _fields_ = [("data", c_void_p), ("len", c_size_t)]


# the C RambleString, a length carrying view. Named View so the public string field
# marker owns the plain ramble.string name
class RambleStringView(Structure):
    _fields_ = [("data", c_void_p), ("len", c_size_t)]


class RambleQos(Structure):
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


class RambleTopicOpts(Structure):
    _fields_ = [("qos", RambleQos)]


class RambleDiscoveryAddr(Structure):
    _fields_ = [
        ("ip", c_uint8 * 16),   # network-order bytes
        ("ip_len", c_uint8),    # 4 = IPv4, 16 = IPv6
        ("port", c_uint16),     # host order, 0 = the discovery port
    ]


def seed_addrs(seeds):
    """Marshal "ip" / "ip:port" strings into a C array of RambleDiscoveryAddr (IPv4).
    The node copies the array at open, so it need not outlive the call."""
    if not seeds:
        return None, 0
    arr = (RambleDiscoveryAddr * len(seeds))()
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


class RambleNodeNet(Structure):
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


class RambleNodeDiscovery(Structure):
    _fields_ = [
        ("announce_interval_us", c_uint32),
        ("peer_timeout_us", c_uint32),
        ("max_peers", c_uint16),
    ]


class RambleNodeOpts(Structure):
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
        ("net", RambleNodeNet),
        ("discovery", RambleNodeDiscovery),
    ]


class RambleMsg(Structure):
    _fields_ = [
        ("node", c_void_p),
        ("user", c_void_p),
        ("topic_index", c_uint16),
        ("publisher_id", c_uint32),
        ("publisher_name", RambleStringView),
        ("topic_name", RambleStringView),
        ("header", RambleBytes),   # the pattern header view, None on a plain topic
        ("data", RambleBytes),
        ("schema", c_void_p),
        ("recv_us", c_uint64),
        ("written_us", c_uint64),
        ("capture_us", c_uint64),
    ]


# Optional per send config. capture_us 0 = unstated, and costs no wire bytes.
class RambleSendOpts(Structure):
    _fields_ = [("capture_us", c_uint64)]


class RambleEvent(Structure):
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


class RambleAllocator(Structure):
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


class RambleSchemaFieldInfo(Structure):
    _fields_ = [
        ("name", RambleStringView),
        ("type_name", RambleStringView),   # the field type's NAME, empty when anonymous
        ("elem_name", RambleStringView),   # an array ELEMENT type's name, empty when anonymous
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


class _RambleValueV(Union):
    _fields_ = [("u", c_uint64), ("i", c_int64), ("f", c_double)]


class RambleValue(Structure):
    _fields_ = [
        ("kind", c_uint8),
        ("elem", c_uint8),
        ("count", c_uint16),
        ("str_cap", c_uint16),
        ("v", _RambleValueV),
        ("bytes", RambleBytes),
    ]


# the pattern struct mirrors of src/patterns/core.h, field order and types exact

# The public head of the C RambleRequest, only ever touched through the callback's pointer:
# the reply machinery lives behind the struct, so the exact pointer is what reply takes.
class RambleRequest(Structure):
    _fields_ = [
        ("node", c_void_p),
        ("function_name", RambleStringView),
        ("data", RambleBytes),
        ("schema", c_void_p),
        ("caller", c_uint32),
        ("caller_name", RambleStringView),
        ("recv_us", c_uint64),
        ("written_us", c_uint64),
    ]


class RambleResponse(Structure):
    _fields_ = [
        ("status", c_int),
        ("data", RambleBytes),
        ("schema", c_void_p),
        ("provider", c_uint32),
        ("user", c_void_p),
        ("written_us", c_uint64),
        ("message", RambleStringView),   # outcome text (default status text if none sent)
    ]


class RambleFunctionOpts(Structure):
    _fields_ = [
        ("backpressure_wait_us", c_uint32),
        ("timeout_us", c_uint32),
        ("keep_last", c_uint16),
        ("multi", c_uint8),
    ]


class RambleVariableOpts(Structure):
    _fields_ = [
        ("initial", RambleBytes),
        ("access", c_uint8),
        ("allow_force", c_uint8),
        ("catch_up", c_uint16),
        ("keep_last", c_uint16),
        ("backpressure_wait_us", c_uint32),
    ]


class RambleVariableUpdate(Structure):
    _fields_ = [
        ("variable", c_void_p),
        ("name", RambleStringView),
        ("value", RambleBytes),
        ("schema", c_void_p),
        ("forced", c_uint8),
        ("write_seq", c_uint32),
        ("source", c_uint32),
        ("recv_us", c_uint64),
        ("written_us", c_uint64),
    ]


class RambleTaskOpts(Structure):
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


class RambleProgress(Structure):
    _fields_ = [
        ("call_id", c_uint32),
        ("provider", c_uint32),
        ("data", RambleBytes),      # len 0 = the RUNNING ack
        ("schema", c_void_p),
        ("written_us", c_uint64),
        ("recv_us", c_uint64),
        ("user", c_void_p),
    ]


PrgFn = CFUNCTYPE(None, POINTER(RambleProgress))


class RambleCallOpts(Structure):
    # direct a call at one definition by peer id, 0 = first answer wins. The task fields
    # stay NULL on a plain call
    _fields_ = [
        ("provider", c_uint32),
        ("on_progress", PrgFn),
        ("progress_user", c_void_p),
        ("id_out", POINTER(c_uint32)),
    ]


RAMBLE_ALLOCATOR_PAGE = 64 * 1024   # RAMBLE_ALLOCATOR_PAGE default

MsgFn = CFUNCTYPE(None, POINTER(RambleMsg))
EvtFn = CFUNCTYPE(None, POINTER(RambleEvent))
AllocFn = CFUNCTYPE(c_void_p, c_void_p, c_void_p, c_size_t)
ReqFn = CFUNCTYPE(None, POINTER(RambleRequest), c_void_p)
RspFn = CFUNCTYPE(None, POINTER(RambleResponse))
VarFn = CFUNCTYPE(None, POINTER(RambleVariableUpdate), c_void_p)
CancelFn = CFUNCTYPE(None, c_uint64, c_void_p)
NULL_REQ_FN = cast(None, ReqFn)   # a CFUNCTYPE argtype refuses a bare None
NULL_VAR_FN = cast(None, VarFn)


def bind(lib):
    F = lambda fn, args, ret: (setattr(getattr(lib, fn), "argtypes", args),
                               setattr(getattr(lib, fn), "restype", ret))
    F("ramble_allocator_heap", [c_uint32], RambleAllocator)
    F("ramble_heap_realloc", [c_void_p, c_void_p, c_size_t], c_void_p)
    F("ramble_node_open", [POINTER(RambleAllocator), c_char_p, MsgFn, EvtFn,
                         POINTER(RambleNodeOpts)], c_void_p)
    F("ramble_last_error", [c_void_p], RambleEvent)
    F("ramble_node_poll", [c_void_p, c_int], c_int)
    F("ramble_node_close", [c_void_p, c_int], c_int)
    F("ramble_node_start", [c_void_p], c_int)
    F("ramble_node_stop", [c_void_p], c_int)
    F("ramble_node_is_started", [c_void_p], c_int)
    F("ramble_node_evicted_unsent", [c_void_p], c_uint32)
    F("ramble_node_create_topic", [c_void_p, c_char_p, c_int, c_void_p,
                                   POINTER(RambleTopicOpts)], c_void_p)
    F("ramble_node_topic", [c_void_p, c_uint16], c_void_p)
    F("ramble_topic_send", [c_void_p, RambleBytes, POINTER(RambleSendOpts)], c_int)
    F("ramble_topic_set_role", [c_void_p, c_int], c_int)
    F("ramble_topic_retire", [c_void_p], c_int)
    F("ramble_topic_index", [c_void_p], c_uint16)
    F("ramble_topic_match_count", [c_void_p], c_int)
    F("ramble_topic_drain", [c_void_p, c_int], c_int)
    F("ramble_topic_take", [c_void_p, POINTER(RambleMsg), c_int], c_int)
    F("ramble_topic_dispatch", [c_void_p, c_int, c_int], c_int)
    F("ramble_node_dispatch", [c_void_p, c_int, c_int], c_int)
    F("ramble_topic_queue_stats", [c_void_p, POINTER(c_uint32), POINTER(c_uint32),
                                   POINTER(c_uint32), POINTER(c_uint32)], None)
    F("ramble_node_mem_stats", [c_void_p, POINTER(c_size_t), POINTER(c_size_t),
                              POINTER(c_uint64)], None)
    F("ramble_node_backpressure_stats", [c_void_p, POINTER(c_uint64),
                                       POINTER(c_uint32)], None)
    F("ramble_topic_counts", [c_void_p, POINTER(c_uint64), POINTER(c_uint64),
                            POINTER(c_uint64), POINTER(c_uint64)], None)
    F("ramble_node_log_text", [c_void_p, c_int, c_char_p, c_int], c_int)
    F("ramble_node_log_topic", [c_void_p, c_int], c_void_p)
    F("ramble_node_meta_function", [c_void_p], c_void_p)
    F("ramble_event_str", [POINTER(RambleEvent), c_char_p, c_size_t], c_char_p)
    F("ramble_node_settle", [c_void_p, c_int], c_int)
    F("ramble_topic_ready", [c_void_p], c_int)
    F("ramble_topic_pending_count", [c_void_p], c_int)
    F("ramble_node_lock", [c_void_p], None)
    F("ramble_node_unlock", [c_void_p], None)
    # patterns: functions / variables
    F("ramble_node_create_function_definition", [c_void_p, c_char_p, c_void_p,
                                               c_void_p, ReqFn, c_void_p,
                                               POINTER(RambleFunctionOpts)], c_void_p)
    F("ramble_node_create_remote_function", [c_void_p, c_char_p, c_void_p, c_void_p,
                                           POINTER(RambleFunctionOpts)], c_void_p)
    F("ramble_function_call", [c_void_p, RambleBytes, POINTER(RambleResponse), c_int, c_void_p], c_int)
    F("ramble_function_call_async", [c_void_p, RambleBytes, RspFn, c_void_p, c_void_p], c_int)
    F("ramble_function_match_count", [c_void_p], c_int)
    F("ramble_function_retire", [c_void_p], c_int)
    F("ramble_request_reply", [POINTER(RambleRequest), RambleBytes], None)
    F("ramble_request_fail", [POINTER(RambleRequest), c_char_p, RambleBytes], None)
    F("ramble_request_defer", [POINTER(RambleRequest)], c_uint64)
    F("ramble_function_complete", [c_void_p, c_uint64, c_int, c_char_p, RambleBytes], c_int)
    # patterns: tasks
    F("ramble_node_create_task_definition", [c_void_p, c_char_p, c_void_p, c_void_p,
                                           c_void_p, ReqFn, c_void_p,
                                           POINTER(RambleTaskOpts)], c_void_p)
    F("ramble_node_create_remote_task", [c_void_p, c_char_p, c_void_p, c_void_p,
                                       c_void_p, POINTER(RambleTaskOpts)], c_void_p)
    F("ramble_request_start", [POINTER(RambleRequest)], c_int)
    F("ramble_function_progress", [c_void_p, c_uint64, RambleBytes], c_int)
    F("ramble_function_cancelled", [c_void_p, c_uint64], c_int)
    F("ramble_function_on_cancel", [c_void_p, CancelFn, c_void_p], c_int)
    F("ramble_function_cancel", [c_void_p, c_uint32], c_int)
    F("ramble_node_create_variable_definition", [c_void_p, c_char_p, c_void_p,
                                               POINTER(RambleVariableOpts)], c_void_p)
    F("ramble_node_create_remote_variable", [c_void_p, c_char_p, c_void_p,
                                           POINTER(RambleVariableOpts)], c_void_p)
    F("ramble_variable_get", [c_void_p, POINTER(RambleBytes)], c_int)
    F("ramble_variable_set", [c_void_p, RambleBytes], c_int)
    F("ramble_variable_force", [c_void_p, RambleBytes], c_int)
    F("ramble_variable_unforce", [c_void_p], c_int)
    F("ramble_variable_forced", [c_void_p], c_int)
    F("ramble_variable_wait", [c_void_p, c_int], c_int)
    F("ramble_variable_match_count", [c_void_p], c_int)
    F("ramble_variable_retire", [c_void_p], c_int)
    F("ramble_variable_on_change", [c_void_p, VarFn, c_void_p], c_int)
    F("ramble_variable_on_write", [c_void_p, VarFn, c_void_p], c_int)
    # serialize / schema
    F("ramble_schema_compile", [AllocFn, c_void_p, c_char_p, POINTER(c_char_p)], c_void_p)
    F("ramble_schema_free", [c_void_p, AllocFn, c_void_p], None)
    F("ramble_schema_wire", [c_void_p], RambleBytes)
    F("ramble_schema_hash", [c_void_p], c_uint64)
    F("ramble_schema_name", [c_void_p], RambleStringView)
    F("ramble_schema_print", [c_void_p, c_char_p, c_size_t], c_uint32)
    F("ramble_schema_subset", [c_void_p, c_void_p], c_int)
    F("ramble_std_name", [c_int], c_char_p)
    F("ramble_std_by_name", [RambleStringView], c_int)
    F("ramble_std_recognize", [c_void_p, AllocFn, c_void_p], c_int)
    F("ramble_std_recognize_field", [c_void_p, c_uint16, AllocFn, c_void_p], c_int)
    F("ramble_std_recognize_elem", [c_void_p, c_uint16, AllocFn, c_void_p], c_int)
    F("ramble_timestamp_now", [], c_int64)
    F("ramble_schema_size", [c_void_p], c_uint32)
    F("ramble_schema_field_count", [c_void_p], c_uint16)
    F("ramble_schema_field_at", [c_void_p, c_uint16, POINTER(RambleSchemaFieldInfo)], c_int)
    F("ramble_schema_field_index", [c_void_p, c_char_p], c_int)
    F("ramble_schema_enum_count", [c_void_p, c_uint16], c_uint16)
    F("ramble_schema_enum_variant", [c_void_p, c_uint16, c_uint16, POINTER(c_int64),
                                   POINTER(RambleStringView)], c_int)
    F("ramble_get_enum", [RambleBytes, c_void_p, c_char_p], RambleStringView)
    F("ramble_set_enum", [c_void_p, c_size_t, c_void_p, c_char_p, c_char_p], c_int)
    F("ramble_schema_message_default", [c_void_p, c_void_p, c_size_t], c_int)
    F("ramble_schema_scalar_size", [c_int], c_uint32)
    F("ramble_set_uint", [c_void_p, c_size_t, c_void_p, c_char_p, c_uint64], c_int)
    F("ramble_set_int", [c_void_p, c_size_t, c_void_p, c_char_p, c_int64], c_int)
    F("ramble_set_f64", [c_void_p, c_size_t, c_void_p, c_char_p, c_double], c_int)
    F("ramble_set_f32", [c_void_p, c_size_t, c_void_p, c_char_p, c_float], c_int)
    F("ramble_set_array", [c_void_p, c_size_t, c_void_p, c_char_p, RambleBytes], c_int)
    F("ramble_set_string", [c_void_p, c_size_t, c_void_p, c_char_p, RambleStringView], c_int)
    F("ramble_set_string_at", [c_void_p, c_size_t, c_void_p, c_char_p, c_uint16,
                             RambleStringView], c_int)
    F("ramble_get_uint", [RambleBytes, c_void_p, c_char_p], c_uint64)
    F("ramble_get_int", [RambleBytes, c_void_p, c_char_p], c_int64)
    F("ramble_get_f64", [RambleBytes, c_void_p, c_char_p], c_double)
    F("ramble_get_f32", [RambleBytes, c_void_p, c_char_p], c_float)
    F("ramble_get_array", [RambleBytes, c_void_p, c_char_p], RambleBytes)
    F("ramble_get_value", [RambleBytes, c_void_p, c_uint16, POINTER(RambleValue)], c_int)
    F("ramble_set_value", [c_void_p, c_size_t, c_void_p, c_uint16, POINTER(RambleValue)], c_int)
    F("ramble_schema_msg_min", [c_void_p], c_uint32)
    F("ramble_schema_msg_len", [c_void_p, c_void_p, c_size_t], c_uint32)
    F("ramble_set_map", [c_void_p, c_size_t, c_void_p, c_char_p, RambleBytes], c_int)


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


# The schema layer needs a realloc style hook. ramble_heap_realloc is the library's own,
# so a schema allocation stays in native code with no callback into Python.
SCHEMA_ALLOC = None


def schema_alloc():
    global SCHEMA_ALLOC
    if SCHEMA_ALLOC is None:
        SCHEMA_ALLOC = cast(load().ramble_heap_realloc, AllocFn)
    return SCHEMA_ALLOC
