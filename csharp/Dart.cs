// The C# wrapper: a P/Invoke layer over the prebuilt native library, IL2CPP safe with
// static callbacks dispatched by id. docs/csharp.md explains how to use it.

using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

#if UNITY_5_3_OR_NEWER
using MonoPInvokeCallbackAttribute = AOT.MonoPInvokeCallbackAttribute;
#endif

namespace Dart
{
#if !UNITY_5_3_OR_NEWER
    // Off Unity this attribute is synthesized as a no op, so the callback methods stay
    // annotated identically to the Unity IL2CPP build.
    [AttributeUsage(AttributeTargets.Method)]
    internal sealed class MonoPInvokeCallbackAttribute : Attribute
    {
        public MonoPInvokeCallbackAttribute(Type t) { }
    }
#endif

    public enum Reliability { BestEffort = 0, Reliable = 1 }
    public enum Role { PubSub = 0, PubOnly = 1, SubOnly = 2, Inactive = 3 }
    public enum SendStatus
    {
        Ok = 0, NoTopic = -1, TooBig = -2, BadRole = -3, OutOfMemory = -4,
        State = -5,   // wrong state: Poll while started, or a call a handler may not make
        NoSys = -6    // not compiled in (Start under DART_NO_THREADS)
    }

    public enum EventKind
    {
        PeerUp = 0, PeerDown, PeerInterest, MessageLost, Error
    }

    // The error carried by an EventKind.Error event, DartEvent.Error and DartNode.LastError.
    public enum ErrorKind
    {
        None = 0,
        NameCollision, QosIncompatible, KindMismatch, SchemaMismatch, InterestOverflow,
        MetaTruncatedInterest, MetaTruncatedSchema, PeerMetaTooBig, MessageTooBig,
        PeerRefused, EvictedUnsent, UnmatchedSend, DuplicateAuthority,
        Oom, Platform, Socket, Bind, McastJoin, Send, Recv, Poll, Waker, BadAddress
    }

    // Schema field kinds for reflection, the value is the wire kind byte. Named is a nominal
    // tag reflection unwraps into Field.TypeName, so a field never reports it as its Kind.
    public enum FieldType : byte
    {
        U8 = 0, U16, U32, U64, I8, I16, I32, I64, F32, F64, Bool, Array, Struct, String,
        VString, VArray, Map, Enum, Named
    }

    // A call's outcome, mirrors DartCallStatus. Timeout and PeerLost are synthesized on the
    // caller, and Cancelled also for calls still pending when the node closes.
    public enum CallStatus
    {
        Ok = 0, AppError = 1, NoHandler = 2, Timeout = 3, PeerLost = 4, Cancelled = 5,
        Running = 6   // task, the one NON-terminal status: accepted and running
    }

    // Severity of a built-in @dart/log line. Mirrors DartLogLevel.
    public enum LogLevel { Error = 0, Warn = 1, Info = 2 }

    // A @dart/meta request's section mask, OR the bits. 0 = every section. Mirrors DART_META_*.
    [Flags]
    public enum MetaSection : uint { Node = 0x1, Proc = 0x2, Topics = 0x4, Peers = 0x8, All = 0 }

    // ---- native struct layouts (mirror the C exactly) ---------------------------

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartBytes { public IntPtr data; public UIntPtr len; }

    // the C DartString, a length carrying view. Named View so the [DartString] attribute
    // owns the public name
    [StructLayout(LayoutKind.Sequential)]
    internal struct DartStringView { public IntPtr data; public UIntPtr len; }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartQos
    {
        public int reliability;
        public ushort keep_last;
        public ushort catch_up;
        public uint max_message_bytes;
        public uint heartbeat_us;
        public uint repair_delay_us;
        public uint backpressure_wait_us;
        public uint shm_max_bytes;
        public uint queue_bytes;
        public ushort max_rate_hz;
        public byte no_timestamp;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartTopicOpts { public DartQos qos; public byte reflect_from_mesh; }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartNodeNet
    {
        public ushort data_port;
        public IntPtr discovery_group;         // const char*
        public ushort discovery_port;
        public IntPtr multicast_interface;     // const char*
        public byte multicast_ttl;
        public IntPtr seed_peers;              // const DartDiscoveryAddr*
        public ushort n_seed_peers;
        public byte unicast_only;
        public uint recv_buffer_bytes;
        public uint send_buffer_bytes;
        public ushort fragment_size;
        public IntPtr self_ip;                 // const char*
        public ushort advertise_port;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartDiscoveryAddr
    {
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)] public byte[] ip;
        public byte ip_len;                    // 4 = IPv4, 16 = IPv6
        public ushort port;                    // host order, 0 = the discovery port
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartNodeDiscovery
    {
        public uint announce_interval_us;
        public uint peer_timeout_us;
        public ushort max_peers;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartNodeOpts
    {
        public ushort domain;
        public ushort max_topics;
        public IntPtr user_data;
        public byte disable_shm;
        public byte fetch_details;
        public int match_wait_ms;              // send path match wait, 0 = 1 s, negative = off
        public byte disable_logs;              // strip the built-in @dart/log topics
        public byte disable_meta;              // do not host the @dart/meta endpoint
        public byte disable_error_logs;   // no error mirroring onto @dart/log/error
        public DartNodeNet net;
        public DartNodeDiscovery discovery;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartMsg
    {
        public IntPtr node;
        public IntPtr user;
        public ushort topic_index;
        public uint publisher_id;
        public DartStringView publisher_name;
        public DartStringView topic_name;
        public DartBytes header;   // the pattern header view, null on a plain topic
        public DartBytes data;
        public IntPtr schema;
        public ulong recv_us;
        public ulong written_us;
        public ulong capture_us;
    }

    // Optional per send config. capture_us 0 = unstated, and costs no wire bytes.
    [StructLayout(LayoutKind.Sequential)]
    internal struct DartSendOpts
    {
        public ulong capture_us;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartEventNative
    {
        public int kind;
        public int error;                      // DartErrorKind (Error events)
        public IntPtr topic_name;            // const char*, topic scoped events only
        public IntPtr user;
        public uint peer;
        public ushort topic;
        public int os_error;                   // errno / WSAGetLastError (socket failures)
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)] public byte[] ip;
        public byte ip_len;
        public ushort port;
        public ulong lost_first;
        public ulong lost_count;
        public ulong too_big_bytes;
        public ulong identity;
        public ushort publish_topics;
        public ushort receive_topics;
        public IntPtr schema_detail;           // const char*, SchemaMismatch: what was incompatible
        public IntPtr peer_name;   // const char*, the peer's node name
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartAllocator
    {
        public IntPtr page_realloc;
        public IntPtr shared;
        public IntPtr owned;
        public IntPtr free_pool;
        public uint page_size;
        public UIntPtr max_bytes;
        public UIntPtr in_use;
        public UIntPtr pooled;
        public UIntPtr peak;
        public ulong alloc_calls;
        public ulong pages_live;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartSchemaFieldInfo
    {
        public DartStringView name;
        public DartStringView type_name;   // the field type's NAME, empty when anonymous
        public DartStringView elem_name;   // an array ELEMENT type's name, empty when anonymous
        public byte kind;
        public byte elem;
        public ushort count;
        public ushort depth;
        public ushort str_cap;
        public ushort arr_parent;          // flat index of the enclosing struct ARRAY, 0xFFFF none
        public uint offset;
        public uint size;
        public uint elem_size;             // bytes of one array element, else 0
    }

    [StructLayout(LayoutKind.Explicit)]
    internal struct DartValueUnion
    {
        [FieldOffset(0)] public ulong u;
        [FieldOffset(0)] public long i;
        [FieldOffset(0)] public double f;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartValue
    {
        public byte kind;
        public byte elem;
        public ushort count;
        public ushort str_cap;
        public DartValueUnion v;
        public DartBytes bytes;
    }

    // the pattern struct mirrors of src/patterns/core.h, field order and types exact

    // The public head of the C DartRequest, only ever read through the callback's pointer:
    // the reply machinery lives behind the struct, so the exact pointer is what reply takes.
    [StructLayout(LayoutKind.Sequential)]
    internal struct DartRequestNative
    {
        public IntPtr node;
        public DartStringView function_name;
        public DartBytes data;
        public IntPtr schema;                  // const DartSchema*
        public uint caller;
        public DartStringView caller_name;
        public ulong recv_us;
        public ulong written_us;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartResponseNative
    {
        public int status;                     // DartCallStatus
        public DartBytes data;
        public IntPtr schema;                  // const DartSchema*
        public uint provider;
        public IntPtr user;
        public ulong written_us;
        public DartStringView message;         // outcome text (default status text if none sent)
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartFunctionOpts
    {
        public uint backpressure_wait_us;
        public uint timeout_us;
        public ushort keep_last;        // req and rsp ring depth, 0 = 10
        public byte reflect_from_mesh;
        public byte multi;              // duplicate-authority diagnostic suppressed (@dart/meta)
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartVariableOpts
    {
        public DartBytes initial;
        public byte access;                    // DartVarAccess
        public byte allow_force;
        public ushort catch_up;
        public ushort keep_last;
        public uint backpressure_wait_us;
        public byte reflect_from_mesh;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartVariableUpdateNative
    {
        public IntPtr variable;
        public DartStringView name;
        public DartBytes value;
        public IntPtr schema;
        public byte forced;
        public uint write_seq;
        public uint source;
        public ulong recv_us;
        public ulong written_us;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartCallOpts
    {
        public uint provider;        // direct a call at one definition by peer id (0 = undirected)
        public IntPtr on_progress;   // DartProgressFn for task calls, null = updates discarded
        public IntPtr progress_user; // handed back as DartProgress.user
        public IntPtr id_out;        // uint32_t*: filled with the call id at commit
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartTaskOpts
    {
        public byte progress_best_effort;
        public ushort progress_keep_last;
        public byte no_cancel;
        public byte exclusive;
        public byte multi;
        public uint timeout_us;
        public uint backpressure_wait_us;
        public ushort keep_last;        // req and rsp ring depth, 0 = 10
        public byte reflect_from_mesh;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartProgressNative
    {
        public uint call_id;
        public uint provider;
        public DartBytes data;
        public IntPtr schema;                  // const DartSchema*
        public ulong written_us;
        public ulong recv_us;
        public IntPtr user;                    // DartCallOpts.progress_user
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void DartMsgFn(IntPtr msg);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void DartEventFn(IntPtr ev);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate IntPtr DartAllocFn(IntPtr user, IntPtr ptr, UIntPtr size);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void DartRequestFn(IntPtr request, IntPtr user);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void DartResponseFn(IntPtr response);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void DartVariableUpdateFn(IntPtr update, IntPtr user);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void DartProgressFn(IntPtr progress);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void DartCancelFn(ulong token, IntPtr user);

    // ---- native entry points ----------------------------------------------------

    internal static class Native
    {
        internal const string LIB = "dart";
        private const CallingConvention CC = CallingConvention.Cdecl;

        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr dart_node_open(ref DartAllocator alloc, byte[] name,
            DartMsgFn on_message, DartEventFn on_event, ref DartNodeOpts opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern DartEventNative dart_last_error(IntPtr node);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_node_poll(IntPtr node, int timeout_ms);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_node_close(IntPtr node, int send_bye);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_node_start(IntPtr node);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_node_stop(IntPtr node);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_node_is_started(IntPtr node);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern uint dart_node_evicted_unsent(IntPtr node);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr dart_node_create_topic(IntPtr node, byte[] name, int role,
            IntPtr schema, ref DartTopicOpts opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr dart_node_topic(IntPtr node, ushort index);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_topic_send(IntPtr ch, DartBytes data,
                                                   ref DartSendOpts opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_topic_set_role(IntPtr ch, int role);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_topic_retire(IntPtr ch);

        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_topic_refresh(IntPtr ch);

        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr dart_topic_schema(IntPtr ch);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern ushort dart_topic_index(IntPtr ch);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_topic_match_count(IntPtr ch);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_topic_drain(IntPtr ch, int timeout_ms);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_topic_take(IntPtr ch, ref DartMsg msg, int timeout_ms);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_topic_dispatch(IntPtr ch, int max_msgs, int timeout_ms);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_node_dispatch(IntPtr node, int max_msgs, int timeout_ms);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern void dart_topic_queue_stats(IntPtr ch, out uint msgs,
            out uint bytes, out uint capacity, out uint dropped);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern void dart_node_mem_stats(IntPtr node, out UIntPtr in_use,
            out UIntPtr peak, out ulong alloc_calls);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern void dart_node_backpressure_stats(IntPtr node, out ulong waited_us,
            out uint waited_sends);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern void dart_topic_counts(IntPtr ch, out ulong tx_msgs, out ulong tx_bytes,
            out ulong rx_msgs, out ulong rx_bytes);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_node_log_text(IntPtr node, int level, byte[] text, int len);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr dart_node_log_topic(IntPtr node, int level);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr dart_node_meta_function(IntPtr node);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr dart_event_str(IntPtr ev, byte[] buf, UIntPtr cap);

        // serialize / schema
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr dart_schema_compile(DartAllocFn alloc, IntPtr user,
            byte[] text, out IntPtr err);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern void dart_schema_free(IntPtr s, DartAllocFn alloc, IntPtr user);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern DartBytes dart_schema_wire(IntPtr s);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern ulong dart_schema_hash(IntPtr s);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr dart_schema_copy(IntPtr s, DartAllocFn alloc, IntPtr user);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern DartStringView dart_schema_name(IntPtr s);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern uint dart_schema_print(IntPtr s, IntPtr buf, UIntPtr cap);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_schema_subset(IntPtr sub, IntPtr pub);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_std_recognize(IntPtr s, DartAllocFn alloc, IntPtr user);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_std_recognize_field(IntPtr s, ushort field,
                                                            DartAllocFn alloc, IntPtr user);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern long dart_timestamp_now();
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern uint dart_schema_size(IntPtr s);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern ushort dart_schema_field_count(IntPtr s);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_schema_field_at(IntPtr s, ushort i, out DartSchemaFieldInfo info);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern ushort dart_schema_enum_count(IntPtr s, ushort field);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_schema_enum_variant(IntPtr s, ushort field, ushort i,
            out long value, out DartStringView name);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_schema_message_default(IntPtr s, IntPtr buf, UIntPtr cap);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_set_uint(IntPtr buf, UIntPtr cap, IntPtr s, byte[] field, ulong v);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_set_int(IntPtr buf, UIntPtr cap, IntPtr s, byte[] field, long v);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_set_f64(IntPtr buf, UIntPtr cap, IntPtr s, byte[] field, double v);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_set_f32(IntPtr buf, UIntPtr cap, IntPtr s, byte[] field, float v);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_set_array(IntPtr buf, UIntPtr cap, IntPtr s, byte[] field, DartBytes elems);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_set_string(IntPtr buf, UIntPtr cap, IntPtr s, byte[] field, DartStringView v);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_set_string_at(IntPtr buf, UIntPtr cap, IntPtr s, byte[] field,
            ushort index, DartStringView v);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_get_value(DartBytes msg, IntPtr s, ushort field, out DartValue outv);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern uint dart_schema_msg_min(IntPtr s);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern uint dart_schema_msg_len(IntPtr s, IntPtr buf, UIntPtr cap);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_set_map(IntPtr buf, UIntPtr cap, IntPtr s, byte[] field, DartBytes map);

        // node lock (bracket zero-copy views) + match-wait companions
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern void dart_node_lock(IntPtr node);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern void dart_node_unlock(IntPtr node);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_node_settle(IntPtr node, int timeout_ms);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_topic_ready(IntPtr ch);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_topic_pending_count(IntPtr ch);

        // patterns: functions
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr dart_node_create_function_definition(IntPtr node, byte[] name,
            IntPtr req_schema, IntPtr rsp_schema, DartRequestFn on_request, IntPtr user,
            ref DartFunctionOpts opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr dart_node_create_remote_function(IntPtr node, byte[] name,
            IntPtr req_schema, IntPtr rsp_schema, ref DartFunctionOpts opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_function_call(IntPtr fn, DartBytes req,
            out DartResponseNative response, int timeout_ms, IntPtr opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_function_call_async(IntPtr fn, DartBytes req,
            DartResponseFn on_response, IntPtr user, IntPtr opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_function_match_count(IntPtr fn);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_function_retire(IntPtr fn);

        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_function_refresh(IntPtr fn);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern void dart_request_reply(IntPtr request, DartBytes rsp);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern void dart_request_fail(IntPtr request, byte[] message, DartBytes rsp);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern ulong dart_request_defer(IntPtr request);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_function_complete(IntPtr fn, ulong token, int status, byte[] message, DartBytes rsp);

        // patterns: tasks
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr dart_node_create_task_definition(IntPtr node, byte[] name,
            IntPtr req_schema, IntPtr prg_schema, IntPtr rsp_schema, DartRequestFn on_request,
            IntPtr user, ref DartTaskOpts opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr dart_node_create_remote_task(IntPtr node, byte[] name,
            IntPtr req_schema, IntPtr prg_schema, IntPtr rsp_schema, ref DartTaskOpts opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_request_start(IntPtr request);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_function_progress(IntPtr fn, ulong token, DartBytes progress);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_function_cancelled(IntPtr fn, ulong token);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_function_on_cancel(IntPtr fn, DartCancelFn on_cancel, IntPtr user);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_function_cancel(IntPtr fn, uint call_id);

        // patterns: variables
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr dart_node_create_variable_definition(IntPtr node, byte[] name,
            IntPtr schema, ref DartVariableOpts opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr dart_node_create_remote_variable(IntPtr node, byte[] name,
            IntPtr schema, ref DartVariableOpts opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_variable_get(IntPtr var, out DartBytes value);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_variable_set(IntPtr var, DartBytes value);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_variable_force(IntPtr var, DartBytes value);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_variable_unforce(IntPtr var);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_variable_forced(IntPtr var);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_variable_wait(IntPtr var, int timeout_ms);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_variable_match_count(IntPtr var);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_variable_retire(IntPtr var);

        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_variable_refresh(IntPtr var);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_variable_on_change(IntPtr var, DartVariableUpdateFn on_change, IntPtr user);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_variable_on_write(IntPtr var, DartVariableUpdateFn on_write, IntPtr user);
    }

    // ---- config + reflection attributes -----------------------------------------

    /// <summary>Per topic QoS, the C DartQos as a class. Every field zero means the default,
    /// so a null Qos is every default. docs/topics.md explains them.</summary>
    public sealed class Qos
    {
        public Reliability Reliability = Reliability.BestEffort;
        public ushort KeepLast = 0;           // retained for late join and repair. 0 = 1, or 10 reliable
        public ushort CatchUp = 0;            // messages a new subscriber gets at once. 0 = future only
        public uint MaxMessageBytes = 0;      // size hint that pins the SHM class, never a cap
        public uint HeartbeatUs = 0;          // reliable idle publisher ping. 0 = 250 ms
        public uint RepairDelayUs = 0;        // the reliable re ask bound. 0 = adaptive from the round trip
        public uint BackpressureWaitUs = 0;   // reliable send pause for a slow subscriber. 0 = none
        public uint ShmMaxBytes = 0;          // pin the topic to one SHM class. 0 = per message
        public uint QueueBytes = 0;           // consumer queue cap. Setting it queues from creation
        public ushort MaxRateHz = 0;          // subscriber side, best effort: a delivery cap per publisher
        public bool NoTimestamp = false;      // publisher side: no source stamp, receivers see WrittenUs 0
        /// <summary>Not a QoS field: it rides beside them in the C topic opts. A null schema and
        /// a BestEffort reliability then follow the mesh. See docs/reflection.md.</summary>
        public bool ReflectFromMesh = false;

        public Qos() { }

        /// <summary>Copy, so a layer that fills in a default never changes its caller's object.</summary>
        public Qos(Qos other)
        {
            if (other == null) return;
            Reliability = other.Reliability;
            KeepLast = other.KeepLast;
            CatchUp = other.CatchUp;
            MaxMessageBytes = other.MaxMessageBytes;
            HeartbeatUs = other.HeartbeatUs;
            RepairDelayUs = other.RepairDelayUs;
            BackpressureWaitUs = other.BackpressureWaitUs;
            ShmMaxBytes = other.ShmMaxBytes;
            QueueBytes = other.QueueBytes;
            MaxRateHz = other.MaxRateHz;
            NoTimestamp = other.NoTimestamp;
            ReflectFromMesh = other.ReflectFromMesh;
        }

        internal DartQos ToNative()
        {
            return new DartQos
            {
                reliability = (int)Reliability,
                keep_last = KeepLast,
                catch_up = CatchUp,
                max_message_bytes = MaxMessageBytes,
                heartbeat_us = HeartbeatUs,
                repair_delay_us = RepairDelayUs,
                backpressure_wait_us = BackpressureWaitUs,
                shm_max_bytes = ShmMaxBytes,
                queue_bytes = QueueBytes,
                max_rate_hz = MaxRateHz,
                no_timestamp = (byte)(NoTimestamp ? 1 : 0),
            };
        }
    }

    /// <summary>Override the wire type name of a message struct or class, the class name by
    /// default. Never required.</summary>
    [AttributeUsage(AttributeTargets.Struct | AttributeTargets.Class)]
    public sealed class DartSchemaAttribute : Attribute
    {
        public string Name;
        public DartSchemaAttribute(string name = null) { Name = name; }
    }

    /// <summary>A fixed length array field with this element count. Without it an array
    /// field is a variable array whose length rides the message tail.</summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class DartArrayAttribute : Attribute
    {
        public int Count;
        public DartArrayAttribute(int count) { Count = count; }
    }

    /// <summary>A capped string field, the max UTF-8 byte length. Without it a string is
    /// unbounded. On a string[] it makes a variable array, with [DartArray] a fixed one.</summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class DartStringAttribute : Attribute
    {
        public int Cap;
        public DartStringAttribute(int cap) { Cap = cap; }
    }

    /// <summary>Name a field's type with a standard type (docs/stdtypes.md), so the name
    /// narrows matching. The shape must be the canonical one or compiling fails.</summary>
    [AttributeUsage(AttributeTargets.Field | AttributeTargets.Struct | AttributeTargets.Class)]
    public sealed class DartTypeNameAttribute : Attribute
    {
        public string Name;
        public DartTypeNameAttribute(string name) { Name = name; }
    }

    // The standard composites as plain mirrors of their wire shape (docs/stdtypes.md). The
    // [DartField] overrides give the canonical lowercase wire names every language agrees on.
    [DartTypeName("Float2")] public struct Float2
    { [DartField("x")] public float X; [DartField("y")] public float Y; }
    [DartTypeName("Float3")] public struct Float3
    { [DartField("x")] public float X; [DartField("y")] public float Y;
      [DartField("z")] public float Z; }
    [DartTypeName("Float4")] public struct Float4
    { [DartField("x")] public float X; [DartField("y")] public float Y;
      [DartField("z")] public float Z; [DartField("w")] public float W; }
    [DartTypeName("Double2")] public struct Double2
    { [DartField("x")] public double X; [DartField("y")] public double Y; }
    [DartTypeName("Double3")] public struct Double3
    { [DartField("x")] public double X; [DartField("y")] public double Y;
      [DartField("z")] public double Z; }
    [DartTypeName("Double4")] public struct Double4
    { [DartField("x")] public double X; [DartField("y")] public double Y;
      [DartField("z")] public double Z; [DartField("w")] public double W; }
    [DartTypeName("Int2")] public struct Int2
    { [DartField("x")] public int X; [DartField("y")] public int Y; }
    [DartTypeName("Int3")] public struct Int3
    { [DartField("x")] public int X; [DartField("y")] public int Y; [DartField("z")] public int Z; }
    [DartTypeName("Int4")] public struct Int4
    { [DartField("x")] public int X; [DartField("y")] public int Y;
      [DartField("z")] public int Z; [DartField("w")] public int W; }
    [DartTypeName("Quaternion")] public struct Quaternion    // stored x, y, z, w
    { [DartField("x")] public double X; [DartField("y")] public double Y;
      [DartField("z")] public double Z; [DartField("w")] public double W; }
    [DartTypeName("Color")] public struct Color              // sRGB, straight alpha
    { [DartField("r")] public byte R; [DartField("g")] public byte G;
      [DartField("b")] public byte B; [DartField("a")] public byte A; }
    [DartTypeName("Rect")] public struct Rect
    { [DartField("x")] public float X; [DartField("y")] public float Y;
      [DartField("w")] public float W; [DartField("h")] public float H; }
    [DartTypeName("RectI")] public struct RectI
    { [DartField("x")] public int X; [DartField("y")] public int Y;
      [DartField("w")] public int W; [DartField("h")] public int H; }
    // Meters and radians. Parent "" = unstated, the cap keeps the packed 88 bytes 8 aligned.
    [DartTypeName("Transform")] public struct Transform
    { [DartField("translation")] public Double3 Translation;
      [DartField("rotation")] public Quaternion Rotation;
      [DartField("parent")] [DartString(30)] public string Parent; }
    [DartTypeName("Twist")] public struct Twist               // m/s and rad/s
    { [DartField("linear")] public Double3 Linear; [DartField("angular")] public Double3 Angular; }
    [DartTypeName("GeoPoint")] public struct GeoPoint         // degrees, degrees, meters
    { [DartField("lat")] public double Lat; [DartField("lon")] public double Lon;
      [DartField("alt")] public double Alt; }

    /// <summary>How an Image's data is laid out. A value of 16 or more is a compressed
    /// container, so data holds the file bytes rather than pixels.</summary>
    public enum ImageFormat : byte
    { Mono8 = 0, Mono16 = 1, Rgb8 = 2, Rgba8 = 3, Bgr8 = 4, Yuyv = 5, Nv12 = 6, Monof32 = 7,
      Jpeg = 16, Png = 17 }
    /// <summary>The codec a VideoFrame's data is encoded with. Unknown is the unstated
    /// codec hint (an ExternalVideoStream that does not state one).</summary>
    public enum VideoCodec : byte { Unknown = 0, Mjpeg = 1, H264 = 2, H265 = 3, Av1 = 4 }
    /// <summary>The protocol an ExternalVideoStream's url speaks.</summary>
    public enum VideoStreamKind : byte
    { Rtsp = 0, WebrtcWhep = 1, Hls = 2, Srt = 3, Rtp = 4, HttpMjpeg = 5, Other = 15 }
    [DartTypeName("Image")] public struct Image               // stride 0 = packed rows
    { [DartField("width")] public uint Width; [DartField("height")] public uint Height;
      [DartField("stride")] public uint Stride;
      [DartField("format")] public ImageFormat Format;
      [DartField("data")] public byte[] Data; }               // pixels, or the file bytes
    [DartTypeName("VideoFrame")] public struct VideoFrame     // width/height 0 = unstated
    { [DartField("codec")] public VideoCodec Codec;
      [DartField("width")] public uint Width; [DartField("height")] public uint Height;
      [DartField("keyframe")] public bool Keyframe;
      [DartField("pts")] [DartTypeName("Timestamp")] public long Pts;   // the Timestamp clock
      [DartField("data")] public byte[] Data; }
    // Fully fixed, so it works as a latched variable: hand a viewer a URL, not pixels. Codec,
    // Width and Height are hints for pickers, the stream stays authoritative once connected.
    [DartTypeName("ExternalVideoStream")] public struct ExternalVideoStream
    { [DartField("kind")] public VideoStreamKind Kind;
      [DartField("codec")] public VideoCodec Codec;
      [DartField("width")] public uint Width; [DartField("height")] public uint Height;
      [DartField("url")] [DartTypeName("Uri")] [DartString(256)] public string Url;
      [DartField("name")] [DartString(32)] public string Name; }

    /// <summary>A lens distortion model. NoDistortion is an ideal pinhole.</summary>
    public enum DistortionModel : byte
    { NoDistortion = 0, BrownConrady = 1, Fisheye = 2, Rational = 3 }
    // The pinhole model and its lens distortion. Coeffs is zero filled past the model's count.
    [DartTypeName("CameraIntrinsics")] public struct CameraIntrinsics
    { [DartField("width")] public uint Width; [DartField("height")] public uint Height;
      [DartField("fx")] public double Fx; [DartField("fy")] public double Fy;
      [DartField("cx")] public double Cx; [DartField("cy")] public double Cy;
      [DartField("model")] public DistortionModel Model;
      [DartField("coeffs")] [DartArray(8)] public double[] Coeffs; }
    // SI: radians or meters, per second, and newtons or newton meters. Velocity and Effort
    // may be empty. The names ride a JointNames variable, not every sample.
    [DartTypeName("JointState")] public struct JointState
    { [DartField("position")] public double[] Position;
      [DartField("velocity")] public double[] Velocity;
      [DartField("effort")] public double[] Effort; }
    // Published once as a variable. The order every JointState array follows.
    [DartTypeName("JointNames")] public struct JointNames
    { [DartField("name")] [DartString(32)] public string[] Name; }

    /// <summary>The standard-type values that need a platform.</summary>
    public static class Std
    {
        /// <summary>Now in Timestamp units, microseconds since the Unix epoch UTC, the clock a
        /// message's WrittenUs uses.</summary>
        public static long Now() => Native.dart_timestamp_now();
        /// <summary>An identity Quaternion (w = 1).</summary>
        public static Quaternion IdentityRotation() => new Quaternion { W = 1.0 };
        /// <summary>A Color from 0xRRGGBBAA.</summary>
        public static Color ColorFromHex(uint rgba)
            => new Color { R = (byte)(rgba >> 24), G = (byte)(rgba >> 16),
                           B = (byte)(rgba >> 8),  A = (byte)rgba };
    }

    /// <summary>Override a field's wire name (must match peers, like a topic name).</summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class DartFieldAttribute : Attribute
    {
        public string Name;
        public DartFieldAttribute(string name) { Name = name; }
    }

    public class SchemaException : Exception
    {
        public SchemaException(string m) : base(m) { }
    }

    /// <summary>A call was refused: a non Ok SendStatus surfaced through a throwing surface
    /// such as the VariableDefinition&lt;T&gt;.Value setter.</summary>
    public class DartException : Exception
    {
        public SendStatus Status;
        public DartException(SendStatus status, string m) : base(m) { Status = status; }
    }

    /// <summary>Reading DartResponse&lt;TRsp&gt;.Value when the call did not complete Ok.</summary>
    public class CallException : Exception
    {
        public CallStatus Status;
        public CallException(CallStatus status, string m) : base(m) { Status = status; }
    }

    // ---- schema: DSL compile, reflection, encode/decode -------------------------

    public sealed class Schema : IDisposable
    {
        internal IntPtr Handle;
        internal Type ClrType;   // set for reflection-built schemas (decode target)

        /// <summary>Compile schema DSL text such as "Pose { x: f32 }".</summary>
        public Schema(string text)
        {
            IntPtr err;
            IntPtr h = Native.dart_schema_compile(Codec.SchemaAlloc, IntPtr.Zero, Codec.CStr(text), out err);
            if (h == IntPtr.Zero)
                throw new SchemaException("schema compile failed near: " + Codec.PtrToStr(err));
            Handle = h;
        }

        /// <summary>Reflect a type into a compiled schema: public fields become the wire fields.
        /// A bare type is the whole schema, an anonymous root whose message is one value.</summary>
        public Schema(Type t) : this(Codec.TypeDsl(t)) { ClrType = t; }

        /// <summary>Adopt a compiled schema this wrapper already owns, such as a copy of a
        /// publisher's. Freeing it is this object's job from here on.</summary>
        internal Schema(IntPtr owned) { Handle = owned; }

        public string Name => Codec.Str(Native.dart_schema_name(Handle));
        public uint Size => Native.dart_schema_size(Handle);
        public ulong Hash => Native.dart_schema_hash(Handle);
        public ushort FieldCount => Native.dart_schema_field_count(Handle);
        public byte[] Wire => Codec.Bytes(Native.dart_schema_wire(Handle));

        /// <summary>The DSL text reconstructed from the compiled schema (works for any
        /// schema, including one parsed from a peer). Paste into a C node for interop.</summary>
        public string Dsl => Codec.SchemaDsl(Handle);

        /// <summary>True when this schema is a BARE TYPE: its message is one value, so Encode
        /// takes that value and Decode returns it instead of a field dictionary.</summary>
        public bool IsValueRoot
        {
            get
            {
                if (_valueRoot < 0) _valueRoot = Codec.IsValueRoot(Handle) ? 1 : 0;   // probed once
                return _valueRoot != 0;
            }
        }
        private int _valueRoot = -1;

        /// <summary>Can a reader declaring this schema read messages written with pub? Type
        /// names narrow: an anonymous type reads a named one, never the reverse.</summary>
        public bool CanRead(Schema pub) => Native.dart_schema_subset(Handle, pub.Handle) != 0;

        public byte[] Encode(object value) => Codec.Encode(Handle, value);
        /// <summary>The decoded fields by name. A bare type schema yields its one value under
        /// the empty name.</summary>
        public Dictionary<string, object> DecodeFields(byte[] data) => Codec.DecodeDict(Handle, data);
        public object Decode(byte[] data)
        {
            var d = Codec.DecodeDict(Handle, data);
            return ClrType != null ? Codec.ToObject(ClrType, d) : Codec.RootOrFields(Handle, d);
        }

        public void Dispose()
        {
            if (Handle != IntPtr.Zero)
            {
                Native.dart_schema_free(Handle, Codec.SchemaAlloc, IntPtr.Zero);
                Handle = IntPtr.Zero;
            }
            GC.SuppressFinalize(this);
        }
        ~Schema() { Dispose(); }
    }

    // ---- delivered message / event ----------------------------------------------

    /// <summary>A delivered message. The payload is copied out so it outlives the callback,
    /// but the decode happens on the first read of Fields or Value and never if neither is
    /// read. Read it from one thread, as handlers do.</summary>
    public sealed class DartMessage
    {
        public ushort TopicIndex;
        public uint PublisherId;
        public string PublisherName;
        public string TopicName;
        /// <summary>The payload bytes, copied out of the transport buffer.</summary>
        public byte[] Data { get; private set; }
        /// <summary>The node's monotonic clock in microseconds when the poll received the
        /// message, at enqueue for a queued topic (docs/node.md).</summary>
        public ulong RecvUs;
        /// <summary>The sender's wall clock in UTC microseconds when its send committed, kept
        /// across repair and replay. 0 = opted out. Never mix it with RecvUs.</summary>
        public ulong WrittenUs;
        /// <summary>When the publisher says the data was true, UTC microseconds, which is not
        /// when it was sent. 0 = none given and WrittenUs is all there is (docs/node.md).</summary>
        public ulong CaptureUs;

        // The node's own copy of the publisher's schema, so a decode after the callback is
        // safe. Held by reference, so it outlives the node when this message does. Null on
        // a raw topic or an untyped publisher.
        private Schema _schema;
        private Type _clrType;
        private Dictionary<string, object> _fields;
        private object _value;
        private bool _decoded;

        /// <summary>The decoded fields, null on a raw topic or when the decode failed.
        /// Decodes on the first read.</summary>
        public Dictionary<string, object> Fields { get { Decode(); return _fields; } }
        /// <summary>The typed instance for a typed topic, the bare value for a bare-type
        /// schema, else Fields. Decodes on the first read.</summary>
        public object Value { get { Decode(); return _value; } }

        // Decode failures stay where they were: reported once, leaving Fields and Value null,
        // so one bad publisher never throws out of a handler that only wanted the bytes.
        private void Decode()
        {
            if (_decoded) return;
            _decoded = true;
            if (_schema == null || _schema.Handle == IntPtr.Zero) return;
            try
            {
                _fields = Codec.DecodeDict(_schema.Handle, Data);
                _value = _clrType != null ? Codec.ToObject(_clrType, _fields)
                                          : Codec.RootOrFields(_schema.Handle, _fields);
            }
            catch (Exception e) { Console.Error.WriteLine("dart decode: " + e); }
        }

        public string Text => Encoding.UTF8.GetString(Data);
        public T As<T>() => (T)Value;

        internal static DartMessage FromNative(ref DartMsg m, Type clrType, Schema ownedSchema)
        {
            return new DartMessage
            {
                TopicIndex = m.topic_index,
                PublisherId = m.publisher_id,
                PublisherName = Codec.Str(m.publisher_name),
                TopicName = Codec.Str(m.topic_name),
                Data = Codec.Bytes(m.data),
                RecvUs = m.recv_us,
                WrittenUs = m.written_us,
                CaptureUs = m.capture_us,
                _schema = ownedSchema,
                _clrType = clrType,
            };
        }

        public override string ToString()
            => $"DartMessage(topic={TopicName}, from={PublisherName}, {Data.Length} bytes)";
    }

    public sealed class DartEvent
    {
        public EventKind Kind;
        public ErrorKind Error;   // the error when Kind == EventKind.Error, else None
        public string TopicName;      // our topic's name for topic-scoped events, else null
        public uint Peer;
        public ushort Topic;
        public int OsError;             // errno / WSAGetLastError for socket failures, else 0
        public ulong LostFirst;
        public ulong LostCount;
        public ulong TooBigBytes;
        public string SchemaDetail;     // SchemaMismatch: what exactly was incompatible, else null
        public string PeerName;   // peer scoped events: the peer's node name, else null
        private string _line;

        /// <summary>True if this event reports something going wrong.</summary>
        public bool IsError => Kind == EventKind.Error;

        // format an event returned BY VALUE (dart_last_error): dart_event_str wants a
        // pointer, so briefly marshal the struct to unmanaged memory.
        internal static DartEvent FromValue(DartEventNative e)
        {
            IntPtr p = Marshal.AllocHGlobal(Marshal.SizeOf<DartEventNative>());
            try { Marshal.StructureToPtr(e, p, false); return FromNative(p, ref e); }
            finally { Marshal.FreeHGlobal(p); }
        }

        internal static DartEvent FromNative(IntPtr evPtr, ref DartEventNative e)
        {
            var buf = new byte[192];
            Native.dart_event_str(evPtr, buf, (UIntPtr)buf.Length);
            return new DartEvent
            {
                Kind = (EventKind)e.kind,
                Error = (ErrorKind)e.error,
                TopicName = e.topic_name != IntPtr.Zero ? Codec.PtrToStr(e.topic_name) : null,
                Peer = e.peer,
                Topic = e.topic,
                OsError = e.os_error,
                LostFirst = e.lost_first,
                LostCount = e.lost_count,
                TooBigBytes = e.too_big_bytes,
                SchemaDetail = e.schema_detail != IntPtr.Zero ? Codec.PtrToStr(e.schema_detail) : null,
                PeerName = e.peer_name != IntPtr.Zero ? Codec.PtrToStr(e.peer_name) : null,
                _line = Codec.CBufStr(buf),
            };
        }

        public override string ToString() => _line;
    }

    /// <summary>One decoded @dart/log line for a DartNode.OnLog handler. WallUs is epoch us,
    /// MonoUs the publisher's monotonic clock, RecvUs this node's clock at receipt.</summary>
    public sealed class DartLogLine
    {
        public LogLevel Level;
        public string Node;      // the publishing node's name
        public uint NodeId;      // the publishing peer id
        public ulong WallUs;
        public ulong MonoUs;
        public ulong RecvUs;
        /// <summary>The carrying message's source stamp (see DartMessage.WrittenUs).</summary>
        public ulong WrittenUs;
        public string Text;

        public override string ToString() => $"[{Level}] {Node}: {Text}";
    }

    /// <summary>A decoded @dart/meta reply. The node and proc scalars are fields, the full
    /// body stays in Info. Absent sections leave zeros and HaveProc false.</summary>
    public sealed class DartMetaSnapshot
    {
        public bool Valid;
        public CallStatus Status = CallStatus.Timeout;
        public uint Provider;                                   // the peer that answered
        public IReadOnlyDictionary<string, object> Info;        // the whole decoded body

        // node section
        public string Name;
        public ulong UptimeUs, WallUs, MemInUse, MemPeak, AllocCalls;
        public ulong EvictedUnsent, BpWaitedUs, BpWaits;
        public ulong Peers, MaxPeers, Topics, MaxTopics, ShmTx, ShmRx, LastError;
        public string LastErrorText;

        // the proc section, HaveProc false where unmeasured
        public bool HaveProc, HaveCpu;
        public ulong Pid, CpuUs, Rss, PeakRss, HeapTotal, HeapFree, HeapMinFree, HeapLargestFreeBlock;

        private static ulong U(Dictionary<string, object> d, string k)
            => d.TryGetValue(k, out var o) ? (o is ulong u ? u : o is long l ? (ulong)l : 0UL) : 0UL;
        private static string S(Dictionary<string, object> d, string k)
            => d.TryGetValue(k, out var o) ? o as string ?? "" : "";

        internal static DartMetaSnapshot FromResponse(DartResponse r)
        {
            var s = new DartMetaSnapshot { Status = r.Status, Provider = r.Provider };
            if (r.Status != CallStatus.Ok || r.SchemaPtr == IntPtr.Zero) return s;
            var top = Codec.DecodeDict(r.SchemaPtr, r.Data);
            if (!(top.TryGetValue("info", out var io) && io is Dictionary<string, object> info)) return s;
            s.Info = info; s.Valid = true;
            if (info.TryGetValue("node", out var no) && no is Dictionary<string, object> node)
            {
                s.Name = S(node, "name");
                s.UptimeUs = U(node, "uptime_us"); s.WallUs = U(node, "wall_us");
                s.MemInUse = U(node, "mem_in_use"); s.MemPeak = U(node, "mem_peak");
                s.AllocCalls = U(node, "alloc_calls"); s.EvictedUnsent = U(node, "evicted_unsent");
                s.BpWaitedUs = U(node, "bp_waited_us"); s.BpWaits = U(node, "bp_waits");
                s.Peers = U(node, "peers"); s.MaxPeers = U(node, "max_peers");
                s.Topics = U(node, "topics"); s.MaxTopics = U(node, "max_topics");
                s.ShmTx = U(node, "shm_tx"); s.ShmRx = U(node, "shm_rx");
                s.LastError = U(node, "last_error"); s.LastErrorText = S(node, "last_error_text");
            }
            if (info.TryGetValue("proc", out var po) && po is Dictionary<string, object> proc)
            {
                s.HaveProc = true;
                s.HaveCpu = proc.ContainsKey("cpu_us");
                s.Pid = U(proc, "pid"); s.CpuUs = U(proc, "cpu_us");
                s.Rss = U(proc, "rss"); s.PeakRss = U(proc, "peak_rss");
                s.HeapTotal = U(proc, "heap_total"); s.HeapFree = U(proc, "heap_free");
                s.HeapMinFree = U(proc, "heap_min_free");
                s.HeapLargestFreeBlock = U(proc, "heap_largest_free_block");
            }
            return s;
        }
    }

    // A wrapper handle pointing into the node's arena. Close frees that arena, so the node
    // zeroes every handle there: the C refuses a NULL one (DART_ERR_NO_TOPIC) instead of
    // reading freed memory.
    internal interface INodeHandle { void Invalidate(); }

    // ---- topic ----------------------------------------------------------------

    public class Topic : INodeHandle
    {
        internal readonly DartNode _node;
        internal IntPtr _handle;   // zeroed by Retire, and by the node at Close

        void INodeHandle.Invalidate() { _handle = IntPtr.Zero; }
        internal readonly Schema Schema;

        /// <summary>Create a raw (schemaless) topic on the node: send/receive bytes or
        /// UTF-8 strings. A null qos is every default.</summary>
        public Topic(DartNode node, string name, Role role = Role.PubSub, Qos qos = null)
            : this(node, name, (Schema)null, role, qos) { }

        /// <summary>Create a typed topic with an explicit Schema. Topic&lt;T&gt; is the shorthand
        /// for the reflected case. Same-name topics on one node share the native slot with a
        /// widened role (registry in DartNode).</summary>
        public Topic(DartNode node, string name, Schema schema, Role role = Role.PubSub, Qos qos = null)
        {
            _node = node;
            Schema = schema;
            _handle = node.CreateOrShareTopic(name, role, schema, qos);
            node.RegisterHandle(this);
        }

        // Wrap an already-existing native handle (e.g. a @dart/log topic from the node):
        // no name registry entry, no schema. Query/send/set-role like any topic.
        internal Topic(DartNode node, IntPtr handle)
        {
            _node = node;
            Schema = null;
            _handle = handle;
            node.RegisterHandle(this);
        }

        /// <summary>Publish bytes/string (raw) or a message object (encoded via the
        /// topic schema). captureUs is when the data was true rather than when it was
        /// sent, in Std.Now() units; 0 leaves it unstated and costs no wire bytes.
        /// Returns a SendStatus.</summary>
        public SendStatus Send(byte[] data, long captureUs = 0)
        {
            int r;
            var h = GCHandle.Alloc(data, GCHandleType.Pinned);
            try
            {
                var b = new DartBytes
                {
                    data = data != null && data.Length > 0 ? h.AddrOfPinnedObject() : IntPtr.Zero,
                    len = (UIntPtr)(data?.Length ?? 0)
                };
                var o = new DartSendOpts { capture_us = (ulong)captureUs };
                r = Native.dart_topic_send(_handle, b, ref o);
            }
            finally { h.Free(); }
            return (SendStatus)r;
        }

        public SendStatus Send(string text, long captureUs = 0)
            => Send(Encoding.UTF8.GetBytes(text), captureUs);

        public SendStatus Send(object value, long captureUs = 0)
        {
            // A bare-type schema frames its own value, so bytes/string go through it too:
            // a `string` root is a framed value, not loose text.
            bool bare = Schema != null && Schema.IsValueRoot;
            if (!bare && value is byte[] b) return Send(b, captureUs);
            if (!bare && value is string s) return Send(s, captureUs);
            if (Schema == null)
                throw new InvalidOperationException(
                    "topic has no schema; send bytes/string, or create the topic with a schema");
            return Send(Schema.Encode(value), captureUs);
        }

        public SendStatus SetRole(Role role)
        {
            return (SendStatus)Native.dart_topic_set_role(_handle, (int)role);
        }

        /// <summary>Retire the topic so the name can be re created with another schema
        /// (docs/topics.md). On Ok this handle is invalid. Refused from a callback.</summary>
        public SendStatus Retire() => _node.RetireTopic(this);

        /// <summary>A ReflectFromMesh topic: re read the mesh and re type in place when the
        /// provider moved. True when it was re typed. See docs/reflection.md.</summary>
        public bool Refresh() => _handle != IntPtr.Zero && Native.dart_topic_refresh(_handle) == 1;

        public ushort Index => Native.dart_topic_index(_handle);

        public int MatchCount()
        {
            return Native.dart_topic_match_count(_handle);
        }

        /// <summary>True when a send would not wait on the match wait: a subscriber is matched
        /// or matching has converged. For a GUI: park payloads while false.</summary>
        public bool Ready => Native.dart_topic_ready(_handle) == 1;

        /// <summary>Unresolved candidate matches right now. 0 = matching has converged for
        /// every known peer.</summary>
        public int PendingCount => Native.dart_topic_pending_count(_handle);

        public bool Drain(int timeoutMs)
        {
            return Native.dart_topic_drain(_handle, timeoutMs) == 1;
        }

        /// <summary>Pop the next queued message, fully copied out. The first TryTake or Dispatch
        /// queues the topic (docs/node.md). timeoutMs 0 = check, negative = forever.</summary>
        public bool TryTake(out DartMessage message, int timeoutMs = 0)
        {
            message = null;
            var m = new DartMsg();
            if (Native.dart_topic_take(_handle, ref m, timeoutMs) != 1) return false;
            message = DartMessage.FromNative(ref m, _node.ClrTypeOf(m.topic_index),
                                             _node.OwnedSchema(m.schema));
            return true;
        }

        /// <summary>Drain the queue by running OnMessage on the calling thread, oldest first, up
        /// to maxMsgs (0 = all), waiting like TryTake. These run without the node lock.</summary>
        public int Dispatch(int maxMsgs = 0, int timeoutMs = 0)
            => Native.dart_topic_dispatch(_handle, maxMsgs, timeoutMs);

        /// <summary>Consumer queue observability, all zeros when not queued.</summary>
        public (uint Messages, uint Bytes, uint Capacity, uint Dropped) QueueStats()
        {
            Native.dart_topic_queue_stats(_handle, out uint m, out uint b, out uint c, out uint d);
            return (m, b, c, d);
        }

        /// <summary>The cumulative traffic this node committed to the topic and delivered from
        /// it. Always on, and in the @dart/meta snapshot.</summary>
        public (ulong TxMsgs, ulong TxBytes, ulong RxMsgs, ulong RxBytes) Counts()
        {
            Native.dart_topic_counts(_handle, out ulong tm, out ulong tb, out ulong rm, out ulong rb);
            return (tm, tb, rm, rb);
        }
    }

    /// <summary>A typed topic: T's public fields are the schema, or a bare T is the schema
    /// itself and Send and TryTake carry the plain value (docs/csharp.md).</summary>
    public sealed class Topic<T> : Topic
    {
        public Topic(DartNode node, string name, Role role = Role.PubSub, Qos qos = null)
            : base(node, name, new Schema(typeof(T)), role, qos) { }

        public SendStatus Send(T value, long captureUs = 0) => Send((object)value, captureUs);

        /// <summary>Typed take: decodes straight from the queue.</summary>
        public bool TryTake(out T value, int timeoutMs = 0)
        {
            value = default(T);
            DartMessage m;
            if (!TryTake(out m, timeoutMs) || !(m.Value is T)) return false;
            value = (T)m.Value;
            return true;
        }
    }

    // ---- node -------------------------------------------------------------------

    public sealed class DartNode : IDisposable
    {
        private IntPtr _handle;
        private long _id;
        private DartAllocator _alloc;
        private IntPtr _discGroup;   // native strings the node retains for its lifetime
        private IntPtr _mcastIf;
        private Action<DartMessage> _onMsg;
        private Action<DartEvent> _onEvt;
        private readonly Dictionary<ushort, Type> _topicTypes = new Dictionary<ushort, Type>();
        private readonly List<Schema> _schemas = new List<Schema>();
        // A publisher's schema is a node owned view good only until the next poll, so a
        // message that decodes later needs our own copy. Keyed by the schema hash, so one
        // copy serves every message on every topic that carries it.
        private readonly Dictionary<ulong, Schema> _msgSchemas = new Dictionary<ulong, Schema>();
        // its own lock: the delivery path reaches it while the node lock is held, and
        // _createLock is held across a create, which takes the node lock the other way round
        private readonly object _msgSchemaLock = new object();
        // same name topic sharing with role widening, serialized by the ctor path's lock
        private sealed class TopicRec { public IntPtr Handle; public byte Bits; public ulong SchemaHash; }
        private readonly Dictionary<string, TopicRec> _topicsByName = new Dictionary<string, TopicRec>();
        private readonly object _createLock = new object();
        // per topic subscriber handlers, copy on write arrays so the poll thread read never
        // takes more than a volatile fetch
        private volatile Dictionary<ushort, Action<DartMessage>[]> _subHandlers = new Dictionary<ushort, Action<DartMessage>[]>();
        private readonly object _subLock = new object();
        // pattern handler boxes + in-flight async calls this node owns (reaped at Close)
        private readonly List<long> _patternBoxes = new List<long>();
        private readonly List<WeakReference<INodeHandle>> _handles = new List<WeakReference<INodeHandle>>();
        private readonly HashSet<long> _asyncLive = new HashSet<long>();
        internal readonly object PatternLock = new object();

        // rooted so the GC never collects the trampolines handed to native code.
        private static readonly DartMsgFn s_onMsg = OnMessageTramp;
        private static readonly DartEventFn s_onEvt = OnEventTramp;
        // Read once per delivered message and per event, written only at open and close, so
        // it is replaced whole under s_reg and read without a lock.
        private static volatile Dictionary<long, DartNode> s_nodes = new Dictionary<long, DartNode>();
        private static readonly object s_reg = new object();
        private static long s_nextId = 1;

        /// <summary>Open a node. onMessage may be null, onEvent is required and both are wired
        /// before the constructor returns. Options are named parameters (docs/csharp.md).</summary>
        public DartNode(string name, Action<DartMessage> onMessage, Action<DartEvent> onEvent,
                    int domain = 0, int maxTopics = 0, bool disableShm = false,
                    bool fetchDetails = false, int matchWaitMs = 0,
                    bool disableLogs = false, bool disableMeta = false, bool disableErrorLogs = false,
                    int dataPort = 0, string discoveryGroup = null, int discoveryPort = 0,
                    string multicastInterface = null, int multicastTtl = 0,
                    string[] seedPeers = null, bool unicastOnly = false,
                    string selfIp = null, int advertisePort = 0,
                    int fragmentSize = 0, int recvBufferBytes = 0, int sendBufferBytes = 0,
                    int announceIntervalUs = 0, int peerTimeoutUs = 0,
                    int maxPeers = 0)
        {
            if (onEvent == null)
                throw new ArgumentNullException(nameof(onEvent),
                    "onEvent carries the node's diagnostics (errors, peer lifecycle) and must not be null");
            _onMsg = onMessage;
            _onEvt = onEvent;
            lock (s_reg)
            {
                _id = s_nextId++;
                var next = new Dictionary<long, DartNode>(s_nodes) { [_id] = this };
                s_nodes = next;
            }

            var co = new DartNodeOpts
            {
                domain = (ushort)domain,
                max_topics = (ushort)maxTopics,
                disable_shm = (byte)(disableShm ? 1 : 0),
                fetch_details = (byte)(fetchDetails ? 1 : 0),
                match_wait_ms = matchWaitMs,
                disable_logs = (byte)(disableLogs ? 1 : 0),
                disable_meta = (byte)(disableMeta ? 1 : 0),
                disable_error_logs = (byte)(disableErrorLogs ? 1 : 0),
                user_data = (IntPtr)_id,
            };
            // The node retains these pointers for its lifetime, so keep them alive
            // (freed in Close), matching the C++ wrapper.
            _discGroup = Codec.CStrPtr(discoveryGroup);
            _mcastIf = Codec.CStrPtr(multicastInterface);
            co.net.data_port = (ushort)dataPort;
            co.net.discovery_group = _discGroup;
            co.net.discovery_port = (ushort)discoveryPort;
            co.net.multicast_interface = _mcastIf;
            co.net.multicast_ttl = (byte)multicastTtl;
            ushort nSeeds;
            IntPtr seedBlock = SeedArray(seedPeers, out nSeeds);   // copied by open, freed below
            co.net.seed_peers = seedBlock;
            co.net.n_seed_peers = nSeeds;
            co.net.unicast_only = (byte)(unicastOnly ? 1 : 0);
            // parsed into 4 bytes during open, never retained: free it right after (unlike
            // the group/interface strings, which the wrapper keeps for the node's life)
            IntPtr selfIpPtr = Codec.CStrPtr(selfIp);
            co.net.self_ip = selfIpPtr;
            co.net.advertise_port = (ushort)advertisePort;
            co.net.fragment_size = (ushort)fragmentSize;
            co.net.recv_buffer_bytes = (uint)recvBufferBytes;
            co.net.send_buffer_bytes = (uint)sendBufferBytes;
            co.discovery.announce_interval_us = (uint)announceIntervalUs;
            co.discovery.peer_timeout_us = (uint)peerTimeoutUs;
            co.discovery.max_peers = (ushort)maxPeers;

            _alloc = Codec.DefaultAllocator();
            byte[] cname = string.IsNullOrEmpty(name) ? null : Codec.CStr(name);
            IntPtr h = Native.dart_node_open(ref _alloc, cname, s_onMsg, s_onEvt, ref co);
            if (seedBlock != IntPtr.Zero) Marshal.FreeHGlobal(seedBlock);
            Codec.FreeCStr(selfIpPtr);

            if (h == IntPtr.Zero)
            {
                lock (s_reg) { var next = new Dictionary<long, DartNode>(s_nodes); next.Remove(_id); s_nodes = next; }
                Codec.FreeCStr(_discGroup); Codec.FreeCStr(_mcastIf);
                // the node does not exist, so read the reason from the process-global slot
                DartEvent err = LastOpenError();
                throw new InvalidOperationException("dart_node_open failed: " + err);
            }
            _handle = h;
        }

        // Marshal "ip" / "ip:port" seeds into one unmanaged array. The node COPIES it at
        // open, so the caller frees the block as soon as open returns.
        static IntPtr SeedArray(string[] seeds, out ushort count)
        {
            count = 0;
            if (seeds == null || seeds.Length == 0) return IntPtr.Zero;
            int stride = Marshal.SizeOf<DartDiscoveryAddr>();
            IntPtr block = Marshal.AllocHGlobal(stride * seeds.Length);
            try
            {
                for (int i = 0; i < seeds.Length; i++)
                {
                    string s = seeds[i];
                    int colon = s.IndexOf(':');
                    string[] oct = (colon < 0 ? s : s.Substring(0, colon)).Split('.');
                    if (oct.Length != 4)
                        throw new ArgumentException("seedPeers entry '" + s + "' is not an IPv4 address");
                    var a = new DartDiscoveryAddr { ip = new byte[16], ip_len = 4 };
                    for (int k = 0; k < 4; k++) a.ip[k] = byte.Parse(oct[k]);
                    if (colon >= 0) a.port = ushort.Parse(s.Substring(colon + 1));
                    Marshal.StructureToPtr(a, IntPtr.Add(block, i * stride), false);
                }
            }
            catch { Marshal.FreeHGlobal(block); throw; }
            count = (ushort)seeds.Length;
            return block;
        }

        /// <summary>Rebind the message handler set at construction. Rarely needed: the
        /// constructor already requires an initial one.</summary>
        public DartNode OnMessage(Action<DartMessage> fn) { _onMsg = fn; return this; }

        /// <summary>Where this node's callbacks run. Null runs them inline on the polling or
        /// service thread. Set it and every event, pattern handler, variable observer, progress
        /// report and awaited call result is handed to it instead: a UI toolkit posts to its
        /// frame. Messages are unaffected, they have the consumer queue (docs/node.md).</summary>
        public Action<Action> CallbackDispatcher;

        // Every callback funnels through here so a dispatcher is honored in one place.
        internal void RunCallback(Action a)
        {
            Action<Action> d = CallbackDispatcher;
            if (d == null) { a(); return; }
            try { d(a); }
            catch (Exception e) { Console.Error.WriteLine("dart callback dispatcher: " + e); }
        }
        /// <summary>Rebind the event handler set at construction. Rarely needed: the
        /// constructor already requires an initial one.</summary>
        public DartNode OnEvent(Action<DartEvent> fn) { _onEvt = fn; return this; }

        // The native create behind the Topic constructors. Same name creates on this node share
        // the native slot with a widened role, and a different schema is refused.
        internal IntPtr CreateOrShareTopic(string name, Role role, Schema schema, Qos qos)
        {
            if (string.IsNullOrEmpty(name)) throw new ArgumentException("topic name required", nameof(name));
            lock (_createLock)
            {
                TopicRec rec;
                ulong sh = schema != null ? schema.Hash : 0;
                if (_topicsByName.TryGetValue(name, out rec))
                {
                    if (sh != 0 && rec.SchemaHash != 0 && sh != rec.SchemaHash)
                        throw new InvalidOperationException(
                            "topic '" + name + "' already exists on this node with a different schema"
                            + " (Retire() it to retype the name)");
                    byte bits = (byte)(rec.Bits | RoleBits(role));
                    if (bits != rec.Bits)
                    {
                        Native.dart_topic_set_role(rec.Handle, (int)RoleFromBits(bits));
                        rec.Bits = bits;
                    }
                    if (schema != null) _schemas.Add(schema);
                    return rec.Handle;
                }

                qos = qos ?? new Qos();
                var co = new DartTopicOpts
                {
                    qos = qos.ToNative(),
                    reflect_from_mesh = (byte)(qos.ReflectFromMesh ? 1 : 0),
                };
                IntPtr h = Native.dart_node_create_topic(_handle, Codec.CStr(name), (int)role,
                    schema != null ? schema.Handle : IntPtr.Zero, ref co);
                if (h == IntPtr.Zero)
                    throw new InvalidOperationException("topic create failed: " + LastError);
                ushort idx = Native.dart_topic_index(h);
                if (schema != null) { _schemas.Add(schema); _topicTypes[idx] = schema.ClrType; }
                _topicsByName[name] = new TopicRec { Handle = h, Bits = RoleBits(role), SchemaHash = sh };
                return h;
            }
        }

        // Topic.Retire: release the native slot for reuse and forget every wrapper registration
        // for the index, since a reused slot may carry a different topic.
        internal SendStatus RetireTopic(Topic t)
        {
            lock (_createLock)
            {
                if (t._handle == IntPtr.Zero) return SendStatus.NoTopic;
                ushort idx = Native.dart_topic_index(t._handle);
                int r = Native.dart_topic_retire(t._handle);
                if (r != 0) return (SendStatus)r;
                string dead = null;
                foreach (var kv in _topicsByName)
                    if (kv.Value.Handle == t._handle) { dead = kv.Key; break; }
                if (dead != null) _topicsByName.Remove(dead);
                _topicTypes.Remove(idx);
                lock (_subLock)
            {
                var next = new Dictionary<ushort, Action<DartMessage>[]>(_subHandlers);
                next.Remove(idx);
                _subHandlers = next;
            }
                t._handle = IntPtr.Zero;
                return SendStatus.Ok;
            }
        }

        // role to pub and sub bit pair, bit 0 = pub, bit 1 = sub, for role widening
        private static byte RoleBits(Role r)
            => r == Role.PubSub ? (byte)3 : r == Role.PubOnly ? (byte)1
             : r == Role.SubOnly ? (byte)2 : (byte)0;
        private static Role RoleFromBits(byte b)
            => b == 3 ? Role.PubSub : b == 1 ? Role.PubOnly : b == 2 ? Role.SubOnly : Role.Inactive;

        // Subscriber handlers per topic index, copy on write. When any exist for an index they
        // receive the message instead of the node wide onMessage.
        internal void AddSubHandler(ushort index, Action<DartMessage> fn)
        {
            lock (_subLock)
            {
                var next = new Dictionary<ushort, Action<DartMessage>[]>(_subHandlers);
                Action<DartMessage>[] cur;
                if (!next.TryGetValue(index, out cur)) cur = Array.Empty<Action<DartMessage>>();
                var nv = new Action<DartMessage>[cur.Length + 1];
                Array.Copy(cur, nv, cur.Length);
                nv[cur.Length] = fn;
                next[index] = nv;
                _subHandlers = next;   // published whole, so a reader never sees a torn map
            }
        }

        // No lock: the map is replaced whole on every write, so this volatile read gets one
        // consistent version. Runs once per delivered message.
        private Action<DartMessage>[] SubHandlersOf(ushort index)
        {
            Action<DartMessage>[] hs;
            _subHandlers.TryGetValue(index, out hs);
            return hs;
        }

        // Our copy of a publisher's schema, made once per distinct schema. Called on the
        // delivering thread, where the view passed in is still valid.
        internal Schema OwnedSchema(IntPtr view)
        {
            if (view == IntPtr.Zero) return null;
            ulong hash = Native.dart_schema_hash(view);
            lock (_msgSchemaLock)
            {
                Schema owned;
                if (_msgSchemas.TryGetValue(hash, out owned)) return owned;
                IntPtr h = Native.dart_schema_copy(view, Codec.SchemaAlloc, IntPtr.Zero);
                if (h == IntPtr.Zero) return null;
                owned = new Schema(h);
                _msgSchemas[hash] = owned;
                return owned;
            }
        }

        // pattern-layer bookkeeping (reaped at Close)
        internal void RetainSchema(Schema s) { if (s != null) lock (_createLock) _schemas.Add(s); }
        internal void RegisterPatternBox(long id) { lock (PatternLock) _patternBoxes.Add(id); }
        internal void RegisterHandle(INodeHandle h)
        {
            lock (PatternLock) _handles.Add(new WeakReference<INodeHandle>(h));
        }
        internal void RegisterAsync(long id) { lock (PatternLock) _asyncLive.Add(id); }
        internal void UnregisterAsync(long id) { lock (PatternLock) _asyncLive.Remove(id); }

        /// <summary>One loop tick: discovery, receive, timers and queued sends. Blocks up to
        /// timeoutMs in the socket wait, 0 = non blocking. State while Start() runs.</summary>
        public int Poll(int timeoutMs = 0)
        {
            return Native.dart_node_poll(_handle, timeoutMs);
        }

        /// <summary>Run the C service thread. Handlers fire on it, never two at once, and every
        /// call stays safe from any thread. False if already started or threads are out.</summary>
        public bool Start()
        {
            return Native.dart_node_start(_handle) == 0;
        }

        /// <summary>Stop and join the service thread. Idempotent, implied by Close.</summary>
        public void Stop() => Native.dart_node_stop(_handle);

        public bool IsStarted => Native.dart_node_is_started(_handle) == 1;

        /// <summary>Block until discovery and matching settle, so everything sent now reaches
        /// everyone. Call after creating the topics. timeoutMs &lt; 0 = 3 intervals.</summary>
        public bool Settle(int timeoutMs = -1) => Native.dart_node_settle(_handle, timeoutMs) == 1;

        internal IntPtr Handle => _handle;

        /// <summary>Dispatch every queued topic on the calling thread, waiting up to timeoutMs
        /// for any to hold data. Per frame in Unity, so every queued handler runs there.</summary>
        public int Dispatch(int maxMsgs = 0, int timeoutMs = 0)
            => Native.dart_node_dispatch(_handle, maxMsgs, timeoutMs);

        // ---- built-in logs (the @dart/log/{error,warn,info} topics) --------------------

        /// <summary>Publish a line on a level's log topic (already-formatted text, truncated
        /// at DART_LOG_MAX). SendStatus.NoSys when logs are disabled. Thread-safe.</summary>
        public SendStatus Log(LogLevel level, string text)
        {
            byte[] b = Encoding.UTF8.GetBytes(text ?? "");
            return (SendStatus)Native.dart_node_log_text(_handle, (int)level, b, b.Length);
        }
        public SendStatus LogError(string text) => Log(LogLevel.Error, text);
        public SendStatus LogWarn(string text) => Log(LogLevel.Warn, text);
        public SendStatus LogInfo(string text) => Log(LogLevel.Info, text);

        /// <summary>This node's own handle for a level's log topic (null when disabled):
        /// widen its role and read it like any topic, or use OnLog.</summary>
        public Topic LogTopic(LogLevel level)
        {
            IntPtr ch = Native.dart_node_log_topic(_handle, (int)level);
            return ch == IntPtr.Zero ? null : new Topic(this, ch);
        }

        /// <summary>Subscribe to a level's mesh wide log stream: every other node's lines at that
        /// level as a DartLogLine, on the polling thread. False when logs are disabled.</summary>
        public bool OnLog(LogLevel level, Action<DartLogLine> handler)
        {
            if (handler == null) return false;
            IntPtr ch = Native.dart_node_log_topic(_handle, (int)level);
            if (ch == IntPtr.Zero) return false;
            if (Native.dart_topic_set_role(ch, (int)Role.PubSub) != 0) return false;
            ushort idx = Native.dart_topic_index(ch);
            AddSubHandler(idx, m => handler(new DartLogLine
            {
                Level = level, Node = m.PublisherName, NodeId = m.PublisherId, RecvUs = m.RecvUs,
                WrittenUs = m.WrittenUs,
                WallUs = LogFieldU(m, "wall_us"), MonoUs = LogFieldU(m, "mono_us"),
                Text = m.Fields != null && m.Fields.TryGetValue("text", out var t) ? t as string ?? "" : "",
            }));
            return true;
        }

        private static ulong LogFieldU(DartMessage m, string k)
            => m.Fields != null && m.Fields.TryGetValue(k, out var o)
               ? (o is ulong u ? u : o is long l ? (ulong)l : 0UL) : 0UL;

        // ---- @dart/meta introspection --------------------------------------------------

        /// <summary>The local @dart/meta caller handle, null when meta is disabled. Direct it at
        /// a peer id. Most callers want MetaAsync.</summary>
        public RemoteFunction MetaFunction()
        {
            IntPtr fn = Native.dart_node_meta_function(_handle);
            return fn == IntPtr.Zero ? null : new RemoteFunction(this, fn);
        }

        /// <summary>Fetch a peer's snapshot: a directed @dart/meta call decoded into a
        /// DartMetaSnapshot. The Task never faults. sections is a MetaSection mask.</summary>
        public async Task<DartMetaSnapshot> MetaAsync(uint peer, MetaSection sections = MetaSection.All)
        {
            RemoteFunction fn = MetaFunction();
            if (fn == null) return new DartMetaSnapshot { Status = CallStatus.NoHandler };
            byte[] req = sections == MetaSection.All
                ? Array.Empty<byte>() : BitConverter.GetBytes((uint)sections);
            DartResponse r = await fn.CallAsync(req, peer).ConfigureAwait(false);
            return DartMetaSnapshot.FromResponse(r);
        }

        internal Type ClrTypeOf(ushort index)
        {
            Type t;
            _topicTypes.TryGetValue(index, out t);
            return t;
        }

        /// <summary>The most recent error this node reported (also delivered via OnEvent).
        /// DartEvent.Kind is PeerUp with Error == None if none has occurred yet.</summary>
        public DartEvent LastError => DartEvent.FromValue(Native.dart_last_error(_handle));

        /// <summary>Why the most recent node open failed, from the process global slot. The
        /// constructor already throws with this message.</summary>
        public static DartEvent LastOpenError() => DartEvent.FromValue(Native.dart_last_error(IntPtr.Zero));

        /// <summary>Sends that evicted never-sent history after the bounded wait (the
        /// ErrorKind.EvictedUnsent count): the send-burst/overload indicator.</summary>
        public uint EvictedUnsent => Native.dart_node_evicted_unsent(_handle);

        public (ulong inUse, ulong peak, ulong allocCalls) MemoryStats()
        {
            UIntPtr u, p; ulong c;
            Native.dart_node_mem_stats(_handle, out u, out p, out c);
            return ((ulong)u, (ulong)p, c);
        }

        public (ulong waitedUs, uint waitedSends) BackpressureStats()
        {
            ulong us; uint n;
            Native.dart_node_backpressure_stats(_handle, out us, out n);
            return (us, n);
        }

        /// <summary>Stop the service thread and tear the node down. False when refused from a
        /// handler, where the node stays live: close from another thread.</summary>
        public bool Close(bool sendBye = true)
        {
            if (_handle != IntPtr.Zero)
            {
                if (Native.dart_node_close(_handle, sendBye ? 1 : 0) != 0) return false;
                _handle = IntPtr.Zero;
            }
            lock (s_reg) { var next = new Dictionary<long, DartNode>(s_nodes); next.Remove(_id); s_nodes = next; }
            // reap this node's pattern handler boxes and complete any still-pending
            // async calls (the C never fires their callbacks after close)
            lock (PatternLock)
            {
                // the arena these point into is gone: a later call must refuse, not read it
                foreach (WeakReference<INodeHandle> w in _handles)
                {
                    INodeHandle h;
                    if (w.TryGetTarget(out h)) h.Invalidate();
                }
                _handles.Clear();
                foreach (long id in _patternBoxes) Patterns.DropBox(id);
                _patternBoxes.Clear();
                foreach (long id in _asyncLive) Patterns.AbandonAsync(id);
                _asyncLive.Clear();
            }
            foreach (var s in _schemas) s.Dispose();
            _schemas.Clear();
            // Not disposed: a DartMessage taken before Close may still decode against one.
            // Dropping the node's reference leaves each to its finalizer.
            lock (_msgSchemaLock) _msgSchemas.Clear();
            Codec.FreeCStr(_discGroup); _discGroup = IntPtr.Zero;
            Codec.FreeCStr(_mcastIf); _mcastIf = IntPtr.Zero;
            return true;
        }

        public void Dispose() { Close(); GC.SuppressFinalize(this); }
        ~DartNode() { try { Close(); } catch { } }

        [MonoPInvokeCallback(typeof(DartMsgFn))]
        private static void OnMessageTramp(IntPtr msgPtr)
        {
            try
            {
                var m = Marshal.PtrToStructure<DartMsg>(msgPtr);
                DartNode node; Type clr = null;
                s_nodes.TryGetValue((long)m.user, out node);
                if (node == null) return;
                var hs = node.SubHandlersOf(m.topic_index);
                if (hs == null && node._onMsg == null) return;
                node._topicTypes.TryGetValue(m.topic_index, out clr);
                // the payload is copied and the schema is ours, so a later decode is safe
                var msg = DartMessage.FromNative(ref m, clr, node.OwnedSchema(m.schema));
                if (hs != null) { foreach (var h in hs) h(msg); }
                else node._onMsg(msg);
            }
            catch (Exception e) { Console.Error.WriteLine("dart on_message: " + e); }
        }

        [MonoPInvokeCallback(typeof(DartEventFn))]
        private static void OnEventTramp(IntPtr evPtr)
        {
            try
            {
                var e = Marshal.PtrToStructure<DartEventNative>(evPtr);
                DartNode node;
                s_nodes.TryGetValue((long)e.user, out node);
                if (node == null || node._onEvt == null) return;
                DartEvent ev = DartEvent.FromNative(evPtr, ref e);   // copied past the callback
                Action<DartEvent> fn = node._onEvt;
                node.RunCallback(() => fn(ev));
            }
            catch (Exception ex) { Console.Error.WriteLine("dart on_event: " + ex); }
        }
    }

    // ---- patterns: shared plumbing ----------------------------------------------

    // Pins a byte[] for the duration of one native call ({NULL,0} for null/empty).
    internal struct PinnedBytes : IDisposable
    {
        private GCHandle _h;
        internal DartBytes B;
        internal PinnedBytes(byte[] d)
        {
            if (d == null || d.Length == 0)
            {
                _h = default(GCHandle);
                B = new DartBytes { data = IntPtr.Zero, len = UIntPtr.Zero };
            }
            else
            {
                _h = GCHandle.Alloc(d, GCHandleType.Pinned);
                B = new DartBytes { data = _h.AddrOfPinnedObject(), len = (UIntPtr)d.Length };
            }
        }
        public void Dispose() { if (_h.IsAllocated) _h.Free(); }
    }

    // Static trampolines + id-keyed handler boxes (IL2CPP-safe: the native user
    // pointer carries an id, never a managed reference).
    internal static class Patterns
    {
        private static readonly object s_lock = new object();
        private static long s_next = 1;
        private static readonly Dictionary<long, object> s_boxes = new Dictionary<long, object>();
        private static readonly Dictionary<long, AsyncCall> s_async = new Dictionary<long, AsyncCall>();

        internal sealed class RequestBox
        {
            public Action<DartRequest> Handler;
            public IntPtr Fn;   // set right after create (the callback cannot fire before poll)
            public DartNode Node;
        }
        internal sealed class VarBox
        {
            public Action<VariableUpdate> Handler;
            public DartNode Node;
        }
        internal sealed class AsyncCall
        {
            public TaskCompletionSource<DartResponse> Tcs;
            public DartNode DartNode;
            public Action<TaskProgress> OnProgress;   // task calls only, else null
        }

        // The definition side registry of one task's live calls: defer token to the per call
        // CancellationTokenSource. Cancel runs the registrations inline, Monitor is reentrant.
        internal sealed class TaskCancelBox
        {
            private readonly Dictionary<ulong, CancellationTokenSource> _live =
                new Dictionary<ulong, CancellationTokenSource>();

            public void Add(ulong token, CancellationTokenSource cts)
            {
                lock (_live) _live[token] = cts;
            }
            public void Cancel(ulong token)
            {
                lock (_live)
                {
                    CancellationTokenSource cts;
                    if (_live.TryGetValue(token, out cts))
                        try { cts.Cancel(); }
                        catch (Exception e) { Console.Error.WriteLine("dart on_cancel: " + e); }
                }
            }
            public void Drop(ulong token)
            {
                CancellationTokenSource cts;
                lock (_live) { if (_live.TryGetValue(token, out cts)) _live.Remove(token); }
                if (cts != null) try { cts.Dispose(); } catch (Exception) { }
            }
        }

        internal static long AddBox(object box)
        {
            lock (s_lock) { long id = s_next++; s_boxes[id] = box; return id; }
        }
        internal static object GetBox(long id)
        {
            lock (s_lock) { object b; s_boxes.TryGetValue(id, out b); return b; }
        }
        internal static void DropBox(long id) { lock (s_lock) s_boxes.Remove(id); }

        internal static long AddAsync(AsyncCall c)
        {
            lock (s_lock) { long id = s_next++; s_async[id] = c; return id; }
        }
        internal static AsyncCall TakeAsync(long id)
        {
            lock (s_lock)
            {
                AsyncCall c;
                if (s_async.TryGetValue(id, out c)) s_async.Remove(id);
                return c;
            }
        }
        // Progress is non-terminal: look without removing.
        internal static AsyncCall PeekAsync(long id)
        {
            lock (s_lock) { AsyncCall c; s_async.TryGetValue(id, out c); return c; }
        }
        // A backstop only: the C fires every pending callback with CANCELLED at close, so this
        // normally finds nothing. A straggler completes the same way instead of hanging its Task.
        internal static void AbandonAsync(long id)
        {
            AsyncCall c = TakeAsync(id);
            if (c != null) c.Tcs.TrySetResult(new DartResponse { Status = CallStatus.Cancelled, Message = "cancelled" });
        }

        // rooted delegates handed to native code
        internal static readonly DartRequestFn OnRequest = OnRequestTramp;
        internal static readonly DartResponseFn OnResponse = OnResponseTramp;
        internal static readonly DartVariableUpdateFn OnVarUpdate = OnVarUpdateTramp;
        internal static readonly DartProgressFn OnProgress = OnProgressTramp;
        internal static readonly DartCancelFn OnCancel = OnCancelTramp;

        // A thrown handler's response message.
        internal static string FailText(Exception e)
            => string.IsNullOrEmpty(e.Message) ? "handler threw" : e.Message;

        [MonoPInvokeCallback(typeof(DartRequestFn))]
        private static void OnRequestTramp(IntPtr reqPtr, IntPtr user)
        {
            try
            {
                var box = GetBox((long)user) as RequestBox;
                if (box == null) return;
                var r = new DartRequest(reqPtr, box.Fn);
                // The handler runs after this callback returns, where the native request is
                // dead, so park the reply now and answer through the Deferred instead.
                if (box.Node != null && box.Node.CallbackDispatcher != null)
                {
                    r.Rebind(r.Defer());
                    RequestBox b = box;
                    box.Node.RunCallback(() =>
                    {
                        try { b.Handler(r); }
                        catch (Exception e)
                        {
                            r.FailQuiet(string.IsNullOrEmpty(e.Message) ? "handler threw" : e.Message);
                            Console.Error.WriteLine("dart on_request: " + e);
                        }
                    });
                    return;
                }
                try { box.Handler(r); }
                catch (Exception e)
                {
                    // a thrown handler answers AppError with the exception's text, which never
                    // crosses into C
                    r.FailQuiet(string.IsNullOrEmpty(e.Message) ? "handler threw" : e.Message);
                    Console.Error.WriteLine("dart on_request: " + e);
                }
                finally { r.Expire(); }
            }
            catch (Exception e) { Console.Error.WriteLine("dart on_request: " + e); }
        }

        [MonoPInvokeCallback(typeof(DartResponseFn))]
        private static void OnResponseTramp(IntPtr rspPtr)
        {
            try
            {
                var o = Marshal.PtrToStructure<DartResponseNative>(rspPtr);
                AsyncCall call = TakeAsync((long)o.user);
                if (call == null) return;
                call.DartNode.UnregisterAsync((long)o.user);
                var r = new DartResponse
                {
                    Status = (CallStatus)o.status,
                    Provider = o.provider,
                    WrittenUs = o.written_us,
                    SchemaPtr = o.schema,
                    Data = Codec.Bytes(o.data),   // copied out: the view dies with the callback
                    Message = Codec.Str(o.message),
                };
                if (call.DartNode != null) call.DartNode.RunCallback(() => call.Tcs.TrySetResult(r));
                else call.Tcs.TrySetResult(r);
            }
            catch (Exception e) { Console.Error.WriteLine("dart on_response: " + e); }
        }

        [MonoPInvokeCallback(typeof(DartProgressFn))]
        private static void OnProgressTramp(IntPtr prgPtr)
        {
            try
            {
                var p = Marshal.PtrToStructure<DartProgressNative>(prgPtr);
                AsyncCall call = PeekAsync((long)p.user);
                if (call == null || call.OnProgress == null) return;
                var prg = new TaskProgress
                {
                    CallId = p.call_id,
                    Provider = p.provider,
                    Value = (ulong)p.data.len != 0 ? Codec.Bytes(p.data) : null,
                    WrittenUs = p.written_us,
                    RecvUs = p.recv_us,
                    SchemaPtr = p.schema,
                };
                Action<TaskProgress> sink = call.OnProgress;
                if (call.DartNode != null) call.DartNode.RunCallback(() => sink(prg));
                else sink(prg);
            }
            catch (Exception e) { Console.Error.WriteLine("dart on_progress: " + e); }
        }

        [MonoPInvokeCallback(typeof(DartCancelFn))]
        private static void OnCancelTramp(ulong token, IntPtr user)
        {
            try
            {
                var box = GetBox((long)user) as TaskCancelBox;
                if (box != null) box.Cancel(token);
            }
            catch (Exception e) { Console.Error.WriteLine("dart on_cancel: " + e); }
        }

        [MonoPInvokeCallback(typeof(DartVariableUpdateFn))]
        private static void OnVarUpdateTramp(IntPtr updPtr, IntPtr user)
        {
            try
            {
                var box = GetBox((long)user) as VarBox;
                if (box == null) return;
                var u = Marshal.PtrToStructure<DartVariableUpdateNative>(updPtr);
                var vu = new VariableUpdate
                {
                    Name = Codec.Str(u.name),
                    Data = Codec.Bytes(u.value),   // copied out: the view dies with the callback
                    Forced = u.forced != 0,
                    WriteSeq = u.write_seq,
                    Source = u.source,
                    RecvUs = u.recv_us,
                    WrittenUs = u.written_us,
                    SchemaPtr = u.schema,
                };
                Action<VariableUpdate> fn = box.Handler;
                if (box.Node != null) box.Node.RunCallback(() => fn(vu));
                else fn(vu);
            }
            catch (Exception e) { Console.Error.WriteLine("dart on_variable_update: " + e); }
        }

        // Decode wire bytes to a typed value: prefer the wire schema (the publisher's
        // layout bound to ours), fall back to the local one.
        internal static bool TryDecode(Schema local, IntPtr wireSchema, byte[] data, Type t, out object v)
        {
            v = null;
            IntPtr sp = wireSchema != IntPtr.Zero ? wireSchema
                      : local != null ? local.Handle : IntPtr.Zero;
            if (sp == IntPtr.Zero) return false;
            try
            {
                v = Codec.ToObject(t, Codec.DecodeDict(sp, data));
                return true;
            }
            catch (Exception) { return false; }
        }
    }

    // ---- patterns: functions ----------------------------------------------------

    /// <summary>The request seen by a FunctionDefinition handler, valid only inside the
    /// callback. Reply there, or Defer() and complete later. No reply acknowledges Ok.</summary>
    public sealed class DartRequest
    {
        private IntPtr _ptr;          // the exact native pointer, zeroed when the callback returns
        private readonly IntPtr _fn;
        private Deferred _deferred;   // set when the reply was parked for another thread
        private bool _done;
        internal readonly IntPtr SchemaPtr;

        public byte[] Data { get; private set; }
        public uint Caller { get; private set; }
        public string CallerName { get; private set; }
        public string FunctionName { get; private set; }
        public ulong RecvUs { get; private set; }
        /// <summary>The caller's wall clock when it sent the request (0 = unstamped).</summary>
        public ulong WrittenUs { get; private set; }
        /// <summary>True once Reply/Fail/Defer has been called.</summary>
        public bool Answered => _done;

        internal DartRequest(IntPtr ptr, IntPtr fn)
        {
            _ptr = ptr;
            _fn = fn;
            var r = Marshal.PtrToStructure<DartRequestNative>(ptr);
            Data = Codec.Bytes(r.data);
            Caller = r.caller;
            CallerName = Codec.Str(r.caller_name);
            FunctionName = Codec.Str(r.function_name);
            RecvUs = r.recv_us;
            WrittenUs = r.written_us;
            SchemaPtr = r.schema;
        }

        /// <summary>Answer CallStatus.Ok with rsp.</summary>
        public void Reply(byte[] rsp)
        {
            Guard();
            _done = true;
            if (_deferred != null) { _deferred.Complete(rsp); return; }
            using (var p = new PinnedBytes(rsp)) Native.dart_request_reply(_ptr, p.B);
        }

        /// <summary>Answer AppError. message is the text shown on the caller, truncated at 255
        /// bytes, empty = the default. rsp may still carry structured failure data.</summary>
        public void Fail(string message = null, byte[] rsp = null)
        {
            Guard();
            _done = true;
            if (_deferred != null) { _deferred.Fail(message, rsp); return; }
            using (var p = new PinnedBytes(rsp))
                Native.dart_request_fail(_ptr, string.IsNullOrEmpty(message) ? null : Codec.CStr(message), p.B);
        }

        /// <summary>Park the reply and return now. The Deferred completes the call later from
        /// any thread.</summary>
        public Deferred Defer()
        {
            Guard();
            _done = true;
            if (_deferred != null) return _deferred;
            ulong token = Native.dart_request_defer(_ptr);
            return new Deferred(_fn, token);
        }

        // The reply is parked so the handler can run on another thread. Every field is already
        // copied out, so only the answer has to move off the dead native pointer.
        internal void Rebind(Deferred d) { _ptr = IntPtr.Zero; _deferred = d; _done = false; }

        internal void FailQuiet(string message = null)
        {
            if ((_ptr != IntPtr.Zero || _deferred != null) && !_done) Fail(message);
        }
        internal void Expire() { _ptr = IntPtr.Zero; }
        private void Guard()
        {
            if (_ptr == IntPtr.Zero && _deferred == null)
                throw new InvalidOperationException(
                    "request expired: answer inside the handler callback, or Defer() first");
            if (_done) throw new InvalidOperationException("request already answered");
        }
    }

    /// <summary>A parked function reply (from DartRequest.Defer): complete exactly once,
    /// from any thread. Dropping it leaves the caller to its timeout.</summary>
    public sealed class Deferred
    {
        private readonly IntPtr _fn;
        private long _token;

        internal Deferred(IntPtr fn, ulong token) { _fn = fn; _token = (long)token; }

        public bool Valid => _fn != IntPtr.Zero && Interlocked.Read(ref _token) != 0;

        /// <summary>message as in DartRequest.Fail, also carried on Ok as debug text.</summary>
        public bool Complete(byte[] rsp = null, string message = null) => Finish(CallStatus.Ok, message, rsp);
        public bool Fail(string message = null, byte[] rsp = null) => Finish(CallStatus.AppError, message, rsp);
        /// <summary>Complete Cancelled, the cooperative honor of a task cancel.</summary>
        public bool CompleteCancelled(string message = null) => Finish(CallStatus.Cancelled, message, null);

        internal IntPtr Fn => _fn;
        internal ulong Token => (ulong)Interlocked.Read(ref _token);

        private bool Finish(CallStatus status, string message, byte[] rsp)
        {
            long token = Interlocked.Exchange(ref _token, 0);   // single-shot
            if (_fn == IntPtr.Zero || token == 0) return false;
            using (var p = new PinnedBytes(rsp))
                return Native.dart_function_complete(_fn, (ulong)token, (int)status,
                    string.IsNullOrEmpty(message) ? null : Codec.CStr(message), p.B) == 0;
        }
    }

    /// <summary>The untyped implementation side of a function: one reply per call, one
    /// definition per name. A null handler answers NoHandler.</summary>
    public class FunctionDefinition : INodeHandle
    {
        internal IntPtr Fn;   // zeroed by Retire, and by the node at Close
        internal readonly DartNode DartNode;

        void INodeHandle.Invalidate() { Fn = IntPtr.Zero; }

        public FunctionDefinition(DartNode node, string name, Schema requestSchema, Schema responseSchema,
                                  Action<DartRequest> handler, int backpressureWaitMs = 0, int timeoutMs = 0,
                                  int keepLast = 0, bool reflectFromMesh = false)
        {
            DartNode = node;
            var co = new DartFunctionOpts
            {
                backpressure_wait_us = (uint)backpressureWaitMs * 1000u,
                timeout_us = (uint)timeoutMs * 1000u,
                keep_last = (ushort)keepLast,
                reflect_from_mesh = (byte)(reflectFromMesh ? 1 : 0),
            };
            long id = 0;
            Patterns.RequestBox box = null;
            if (handler != null)
            {
                box = new Patterns.RequestBox { Handler = handler, Node = node };
                id = Patterns.AddBox(box);
            }
            Fn = Native.dart_node_create_function_definition(node.Handle, Codec.CStr(name),
                requestSchema != null ? requestSchema.Handle : IntPtr.Zero,
                responseSchema != null ? responseSchema.Handle : IntPtr.Zero,
                box != null ? Patterns.OnRequest : null, (IntPtr)id, ref co);
            if (Fn == IntPtr.Zero)
            {
                if (box != null) Patterns.DropBox(id);
                throw new InvalidOperationException("function definition create failed: " + node.LastError);
            }
            if (box != null) { box.Fn = Fn; node.RegisterPatternBox(id); }
            node.RetainSchema(requestSchema);
            node.RetainSchema(responseSchema);
            node.RegisterHandle(this);
        }

        /// <summary>The async handler form: the Task's completion answers the call, its result
        /// Ok and an exception AppError. On the polling thread until the first await.</summary>
        public FunctionDefinition(DartNode node, string name, Schema requestSchema, Schema responseSchema,
                                  Func<DartRequest, Task<byte[]>> handler,
                                  int backpressureWaitMs = 0, int timeoutMs = 0, int keepLast = 0,
                                  bool reflectFromMesh = false)
            : this(node, name, requestSchema, responseSchema, AsyncAdapter(handler),
                   backpressureWaitMs, timeoutMs, keepLast) { }

        // Defer FIRST (a continuation may finish before the invocation returns), then the
        // Task's completion answers through the Deferred.
        internal static Action<DartRequest> AsyncAdapter(Func<DartRequest, Task<byte[]>> handler)
        {
            if (handler == null) return null;
            return r =>
            {
                Deferred d = r.Defer();
                Task<byte[]> t;
                try { t = handler(r); }
                catch (Exception e) { d.Fail(Patterns.FailText(e)); return; }
                _ = FinishAsync(t, d);
            };
        }

        private static async Task FinishAsync(Task<byte[]> t, Deferred d)
        {
            try { d.Complete(await t.ConfigureAwait(false)); }
            catch (Exception e) { d.Fail(Patterns.FailText(e)); }
        }

        /// <summary>Callers currently matched to this definition.</summary>
        public int CallerCount => Native.dart_function_match_count(Fn);

        /// <summary>Retire the definition: park its channels and release the name, else a re
        /// created same name handle is shadowed. Unusable after, refused from a callback.</summary>
        public SendStatus Retire()
        {
            var rc = (SendStatus)Native.dart_function_retire(Fn);
            if (rc == SendStatus.Ok) Fn = IntPtr.Zero;
            return rc;
        }

        /// <summary>A reflectFromMesh handle: re type every channel in place when the mesh
        /// moved. True when it was re typed. See docs/reflection.md.</summary>
        public bool Refresh() => Fn != IntPtr.Zero && Native.dart_function_refresh(Fn) == 1;
    }

    /// <summary>An owning call outcome, the payload copied out. SendStatus carries a
    /// synchronous refusal, and Status stays Timeout then.</summary>
    public sealed class DartResponse
    {
        public CallStatus Status { get; internal set; } = CallStatus.Timeout;
        public SendStatus SendStatus { get; internal set; } = SendStatus.Ok;
        public uint Provider { get; internal set; }
        /// <summary>The provider's wall clock when it sent the response, 0 = synthesized.</summary>
        public ulong WrittenUs { get; internal set; }
        public byte[] Data { get; internal set; } = Array.Empty<byte>();
        /// <summary>The outcome text to display on a failure: the definition's message, else
        /// the default status text. Empty only on Ok with no message and on a refusal.</summary>
        public string Message { get; internal set; } = "";
        internal IntPtr SchemaPtr;

        public bool Ok => Status == CallStatus.Ok;
    }

    /// <summary>A reference to a function definition on another node (untyped).</summary>
    public class RemoteFunction : INodeHandle
    {
        internal IntPtr Fn;   // zeroed by Retire, and by the node at Close
        internal readonly DartNode DartNode;

        void INodeHandle.Invalidate() { Fn = IntPtr.Zero; }

        public RemoteFunction(DartNode node, string name, Schema requestSchema = null,
                              Schema responseSchema = null, int backpressureWaitMs = 0, int timeoutMs = 0,
                              int keepLast = 0, bool reflectFromMesh = false)
        {
            DartNode = node;
            var co = new DartFunctionOpts
            {
                backpressure_wait_us = (uint)backpressureWaitMs * 1000u,
                timeout_us = (uint)timeoutMs * 1000u,
                keep_last = (ushort)keepLast,
                reflect_from_mesh = (byte)(reflectFromMesh ? 1 : 0),
            };
            Fn = Native.dart_node_create_remote_function(node.Handle, Codec.CStr(name),
                requestSchema != null ? requestSchema.Handle : IntPtr.Zero,
                responseSchema != null ? responseSchema.Handle : IntPtr.Zero, ref co);
            if (Fn == IntPtr.Zero)
                throw new InvalidOperationException("remote function create failed: " + node.LastError);
            node.RetainSchema(requestSchema);
            node.RetainSchema(responseSchema);
            node.RegisterHandle(this);
        }

        // Wrap an existing node-owned function handle (the @dart/meta endpoint): callable,
        // never created or destroyed here.
        internal RemoteFunction(DartNode node, IntPtr fn)
        {
            DartNode = node; Fn = fn;
            node.RegisterHandle(this);
        }

        // Pin a DartCallOpts for one native call (IntPtr.Zero when undirected).
        private static GCHandle OptsHandle(uint provider, out IntPtr ptr)
        {
            if (provider == 0) { ptr = IntPtr.Zero; return default(GCHandle); }
            var g = GCHandle.Alloc(new DartCallOpts[] { new DartCallOpts { provider = provider } },
                                   GCHandleType.Pinned);
            ptr = g.AddrOfPinnedObject();
            return g;
        }

        /// <summary>Blocking call: drives the loop until the response or timeoutMs, negative =
        /// the default. Refused from a callback or under a service thread. Never throws.</summary>
        public DartResponse Call(byte[] request, int timeoutMs = -1, uint provider = 0)
        {
            var r = new DartResponse();
            DartResponseNative o;
            int rc;
            GCHandle og = OptsHandle(provider, out IntPtr optp);
            try
            {
                using (var p = new PinnedBytes(request))
                    rc = Native.dart_function_call(Fn, p.B, out o, timeoutMs, optp);
            }
            finally { if (og.IsAllocated) og.Free(); }
            if (rc == 1)
            {
                r.Status = (CallStatus)o.status;
                r.Provider = o.provider;
                r.WrittenUs = o.written_us;
                r.SchemaPtr = o.schema;
                r.Data = Codec.Bytes(o.data);   // the view lasts until the next call, copy now
            }
            else if (rc < 0)
            {
                r.SendStatus = (SendStatus)rc;  // Status stays Timeout: never answered
            }
            if (rc >= 0)                        // answered or timed out: the outcome text is filled
                r.Message = Codec.Str(o.message);
            return r;
        }

        /// <summary>Async call: the Task completes with the outcome and never faults. The
        /// response fires from the polling thread and continuations run off it.</summary>
        public Task<DartResponse> CallAsync(byte[] request, uint provider = 0)
        {
            // A dispatcher already completes on the thread the caller chose, so continuations
            // belong there. With none, keep them off the polling thread.
            var tcs = new TaskCompletionSource<DartResponse>(
                DartNode.CallbackDispatcher != null ? TaskCreationOptions.None
                                                    : TaskCreationOptions.RunContinuationsAsynchronously);
            long id = Patterns.AddAsync(new Patterns.AsyncCall { Tcs = tcs, DartNode = DartNode });
            DartNode.RegisterAsync(id);
            int rc;
            GCHandle og = OptsHandle(provider, out IntPtr optp);
            try
            {
                using (var p = new PinnedBytes(request))
                    rc = Native.dart_function_call_async(Fn, p.B, Patterns.OnResponse, (IntPtr)id, optp);
            }
            finally { if (og.IsAllocated) og.Free(); }
            if (rc != 0)
            {
                Patterns.TakeAsync(id);
                DartNode.UnregisterAsync(id);
                tcs.TrySetResult(new DartResponse { SendStatus = (SendStatus)rc });
            }
            return tcs.Task;
        }

        /// <summary>Providers currently matched (the definition side present).</summary>
        public int MatchCount => Native.dart_function_match_count(Fn);
        public bool HasDefinition => MatchCount > 0;

        /// <summary>Retire the remote: park its channels and release the name. Every outstanding
        /// call completes Cancelled. Unusable after, refused from a callback.</summary>
        public SendStatus Retire()
        {
            var rc = (SendStatus)Native.dart_function_retire(Fn);
            if (rc == SendStatus.Ok) Fn = IntPtr.Zero;
            return rc;
        }

        /// <summary>A reflectFromMesh handle: re type every channel in place when the mesh
        /// moved. True when it was re typed. See docs/reflection.md.</summary>
        public bool Refresh() => Fn != IntPtr.Zero && Native.dart_function_refresh(Fn) == 1;
    }

    // ---- patterns: tasks --------------------------------------------------------

    /// <summary>What a task handler works through: stream progress, observe cancellation.
    /// Thread safe across awaits. Once the call completed, Progress returns State.</summary>
    public sealed class TaskContext
    {
        private readonly IntPtr _fn;
        private readonly ulong _token;

        /// <summary>Cancelled the moment a cancel arrives. Honor it by throwing
        /// OperationCanceledException, or run to completion anyway.</summary>
        public CancellationToken CancellationToken { get; }
        public uint Caller { get; }
        public string CallerName { get; }
        public ulong RecvUs { get; }
        public ulong WrittenUs { get; }

        internal TaskContext(IntPtr fn, ulong token, CancellationToken ct, DartRequest r)
        {
            _fn = fn; _token = token;
            CancellationToken = ct;
            Caller = r.Caller; CallerName = r.CallerName;
            RecvUs = r.RecvUs; WrittenUs = r.WrittenUs;
        }

        /// <summary>Broadcast one progress update on the task's progress channel. A reliable
        /// subscriber backpressures end to end.</summary>
        public SendStatus Progress(byte[] value)
        {
            using (var p = new PinnedBytes(value))
                return (SendStatus)Native.dart_function_progress(_fn, _token, p.B);
        }

        /// <summary>Convenience view of CancellationToken (with the native flag as a
        /// backstop).</summary>
        public bool Cancelled => CancellationToken.IsCancellationRequested
            || Native.dart_function_cancelled(_fn, _token) == 1;
    }

    /// <summary>The untyped implementation side of a task. The handler is an async delegate
    /// whose completion answers the call (docs/csharp.md). Null answers NoHandler.</summary>
    public class TaskDefinition : INodeHandle
    {
        internal IntPtr Fn;   // zeroed by Retire, and by the node at Close
        internal readonly DartNode DartNode;

        void INodeHandle.Invalidate() { Fn = IntPtr.Zero; }

        public TaskDefinition(DartNode node, string name, Schema requestSchema, Schema progressSchema,
                              Schema responseSchema, Func<DartRequest, TaskContext, Task<byte[]>> handler,
                              bool progressBestEffort = false, int progressKeepLast = 0,
                              bool noCancel = false, bool exclusive = false, bool multi = false,
                              int backpressureWaitMs = 0, int timeoutMs = 0, int keepLast = 0,
                              bool reflectFromMesh = false)
        {
            DartNode = node;
            var co = new DartTaskOpts
            {
                keep_last = (ushort)keepLast,
                reflect_from_mesh = (byte)(reflectFromMesh ? 1 : 0),
                progress_best_effort = (byte)(progressBestEffort ? 1 : 0),
                progress_keep_last = (ushort)progressKeepLast,
                no_cancel = (byte)(noCancel ? 1 : 0),
                exclusive = (byte)(exclusive ? 1 : 0),
                multi = (byte)(multi ? 1 : 0),
                backpressure_wait_us = (uint)backpressureWaitMs * 1000u,
                timeout_us = (uint)timeoutMs * 1000u,
            };
            long id = 0, cancelId = 0;
            Patterns.RequestBox box = null;
            Patterns.TaskCancelBox cancels = null;
            if (handler != null)
            {
                cancels = new Patterns.TaskCancelBox();
                cancelId = Patterns.AddBox(cancels);
                var b = box = new Patterns.RequestBox { Node = node };
                var c = cancels;
                box.Handler = r => RunCall(b, c, handler, r);
                id = Patterns.AddBox(box);
            }
            Fn = Native.dart_node_create_task_definition(node.Handle, Codec.CStr(name),
                requestSchema != null ? requestSchema.Handle : IntPtr.Zero,
                progressSchema != null ? progressSchema.Handle : IntPtr.Zero,
                responseSchema != null ? responseSchema.Handle : IntPtr.Zero,
                box != null ? Patterns.OnRequest : null, (IntPtr)id, ref co);
            if (Fn == IntPtr.Zero)
            {
                if (box != null) { Patterns.DropBox(id); Patterns.DropBox(cancelId); }
                throw new InvalidOperationException("task definition create failed: " + node.LastError);
            }
            if (box != null)
            {
                box.Fn = Fn;
                node.RegisterPatternBox(id);
                node.RegisterPatternBox(cancelId);
                // the definition's ONE native cancel slot fans out to the per-call CTSes
                Native.dart_function_on_cancel(Fn, Patterns.OnCancel, (IntPtr)cancelId);
            }
            node.RetainSchema(requestSchema);
            node.RetainSchema(progressSchema);
            node.RetainSchema(responseSchema);
            node.RegisterHandle(this);
        }

        // Poll thread: defer, which implies RUNNING, arm the per call CancellationTokenSource,
        // invoke the async delegate. Wherever its completion lands answers the call.
        private static void RunCall(Patterns.RequestBox box, Patterns.TaskCancelBox cancels,
                                    Func<DartRequest, TaskContext, Task<byte[]>> handler, DartRequest r)
        {
            Deferred d = r.Defer();
            ulong token = d.Token;
            var cts = new CancellationTokenSource();
            cancels.Add(token, cts);
            var ctx = new TaskContext(box.Fn, token, cts.Token, r);
            Task<byte[]> t;
            try { t = handler(r, ctx); }
            catch (OperationCanceledException) { cancels.Drop(token); d.CompleteCancelled(); return; }
            catch (Exception e) { cancels.Drop(token); d.Fail(Patterns.FailText(e)); return; }
            _ = FinishCall(t, d, cancels, token);
        }

        private static async Task FinishCall(Task<byte[]> t, Deferred d,
                                             Patterns.TaskCancelBox cancels, ulong token)
        {
            try { d.Complete(await t.ConfigureAwait(false)); }
            catch (OperationCanceledException) { d.CompleteCancelled(); }
            catch (Exception e) { d.Fail(Patterns.FailText(e)); }
            finally { cancels.Drop(token); }
        }

        /// <summary>Callers currently matched to this definition.</summary>
        public int CallerCount => Native.dart_function_match_count(Fn);

        /// <summary>Retire the definition: every live deferred call answers Cancelled while the
        /// channels are up, a later completion is refused. Refused from a callback.</summary>
        public SendStatus Retire()
        {
            var rc = (SendStatus)Native.dart_function_retire(Fn);
            if (rc == SendStatus.Ok) Fn = IntPtr.Zero;
            return rc;
        }

        /// <summary>A reflectFromMesh handle: re type every channel in place when the mesh
        /// moved. True when it was re typed. See docs/reflection.md.</summary>
        public bool Refresh() => Fn != IntPtr.Zero && Native.dart_function_refresh(Fn) == 1;
    }

    /// <summary>One task progress update, the untyped form. Value is the payload copied out,
    /// null = the RUNNING acknowledgment.</summary>
    public sealed class TaskProgress
    {
        public uint CallId { get; internal set; }
        /// <summary>The peer working the call.</summary>
        public uint Provider { get; internal set; }
        public byte[] Value { get; internal set; }
        /// <summary>The provider's wall clock when it sent the update (0 = unstamped).</summary>
        public ulong WrittenUs { get; internal set; }
        public ulong RecvUs { get; internal set; }
        internal IntPtr SchemaPtr;
    }

    /// <summary>The untyped reference to a task defined elsewhere. A request is always
    /// directed at one provider, and the timeout bounds only the first response.</summary>
    public class RemoteTask : INodeHandle
    {
        internal IntPtr Fn;   // zeroed by Retire, and by the node at Close
        internal readonly DartNode DartNode;

        void INodeHandle.Invalidate() { Fn = IntPtr.Zero; }

        public RemoteTask(DartNode node, string name, Schema requestSchema = null,
                          Schema progressSchema = null, Schema responseSchema = null,
                          bool progressBestEffort = false, int progressKeepLast = 0,
                          int backpressureWaitMs = 0, int timeoutMs = 0, int keepLast = 0,
                          bool reflectFromMesh = false)
        {
            DartNode = node;
            var co = new DartTaskOpts
            {
                keep_last = (ushort)keepLast,
                reflect_from_mesh = (byte)(reflectFromMesh ? 1 : 0),
                progress_best_effort = (byte)(progressBestEffort ? 1 : 0),
                progress_keep_last = (ushort)progressKeepLast,
                backpressure_wait_us = (uint)backpressureWaitMs * 1000u,
                timeout_us = (uint)timeoutMs * 1000u,
            };
            Fn = Native.dart_node_create_remote_task(node.Handle, Codec.CStr(name),
                requestSchema != null ? requestSchema.Handle : IntPtr.Zero,
                progressSchema != null ? progressSchema.Handle : IntPtr.Zero,
                responseSchema != null ? responseSchema.Handle : IntPtr.Zero, ref co);
            if (Fn == IntPtr.Zero)
                throw new InvalidOperationException("remote task create failed: " + node.LastError);
            node.RetainSchema(requestSchema);
            node.RetainSchema(progressSchema);
            node.RetainSchema(responseSchema);
            node.RegisterHandle(this);
        }

        /// <summary>Start the task: the Task completes with the terminal outcome and never
        /// faults. progress fires per update, null for RUNNING. The token cancels.</summary>
        public Task<DartResponse> CallAsync(byte[] request, IProgress<TaskProgress> progress = null,
                                            CancellationToken cancellationToken = default, uint provider = 0)
            => CallAsync(request, out _, progress, cancellationToken, provider);

        /// <summary>As above, and callId receives the call id at commit, the handle for Cancel
        /// from anywhere. 0 when the request never committed.</summary>
        public Task<DartResponse> CallAsync(byte[] request, out uint callId,
                                            IProgress<TaskProgress> progress = null,
                                            CancellationToken cancellationToken = default, uint provider = 0)
        {
            Action<TaskProgress> sink = null;
            if (progress != null) { var pr = progress; sink = v => pr.Report(v); }
            return CallCore(request, out callId, sink, cancellationToken, provider);
        }

        internal Task<DartResponse> CallCore(byte[] request, out uint callId, Action<TaskProgress> sink,
                                             CancellationToken cancellationToken, uint provider)
        {
            // A dispatcher already completes on the thread the caller chose, so continuations
            // belong there. With none, keep them off the polling thread.
            var tcs = new TaskCompletionSource<DartResponse>(
                DartNode.CallbackDispatcher != null ? TaskCreationOptions.None
                                                    : TaskCreationOptions.RunContinuationsAsynchronously);
            long id = Patterns.AddAsync(new Patterns.AsyncCall
            {
                Tcs = tcs, DartNode = DartNode, OnProgress = sink,
            });
            DartNode.RegisterAsync(id);
            var opts = new DartCallOpts[1];
            opts[0].provider = provider;
            if (sink != null)
            {
                opts[0].on_progress = Marshal.GetFunctionPointerForDelegate(Patterns.OnProgress);
                opts[0].progress_user = (IntPtr)id;
            }
            var idBox = new uint[1];
            GCHandle idHandle = GCHandle.Alloc(idBox, GCHandleType.Pinned);
            GCHandle optsHandle = GCHandle.Alloc(opts, GCHandleType.Pinned);
            int rc;
            try
            {
                opts[0].id_out = idHandle.AddrOfPinnedObject();   // filled at commit
                using (var p = new PinnedBytes(request))
                    rc = Native.dart_function_call_async(Fn, p.B, Patterns.OnResponse, (IntPtr)id,
                                                         optsHandle.AddrOfPinnedObject());
            }
            finally
            {
                optsHandle.Free();
                callId = idBox[0];
                idHandle.Free();
            }
            if (rc != 0)
            {
                Patterns.TakeAsync(id);
                DartNode.UnregisterAsync(id);
                tcs.TrySetResult(new DartResponse { SendStatus = (SendStatus)rc });
                return tcs.Task;
            }
            if (cancellationToken.CanBeCanceled)
            {
                uint cid = callId;
                CancellationTokenRegistration reg = cancellationToken.Register(() => Cancel(cid));
                // release the registration off the poll thread once the outcome lands, never from
                // the response trampoline, where Dispose can wait on a cancel callback
                tcs.Task.ContinueWith(_ => reg.Dispose(), CancellationToken.None,
                    TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
            }
            return tcs.Task;
        }

        /// <summary>Request cancellation of the call. Cooperative and never acked, the terminal
        /// status answers. BadRole when the provider declared noCancel, State if done.</summary>
        public SendStatus Cancel(uint callId) => (SendStatus)Native.dart_function_cancel(Fn, callId);

        /// <summary>Providers currently matched (the definition side present).</summary>
        public int MatchCount => Native.dart_function_match_count(Fn);
        public bool HasDefinition => MatchCount > 0;

        /// <summary>Retire the remote: every outstanding call completes Cancelled. Unusable
        /// after, refused from a callback.</summary>
        public SendStatus Retire()
        {
            var rc = (SendStatus)Native.dart_function_retire(Fn);
            if (rc == SendStatus.Ok) Fn = IntPtr.Zero;
            return rc;
        }

        /// <summary>A reflectFromMesh handle: re type every channel in place when the mesh
        /// moved. True when it was re typed. See docs/reflection.md.</summary>
        public bool Refresh() => Fn != IntPtr.Zero && Native.dart_function_refresh(Fn) == 1;
    }

    // ---- patterns: variables ----------------------------------------------------

    /// <summary>The state just applied to a variable, as handed to OnChange/OnWrite
    /// handlers. A private copy: safe to hold past the callback.</summary>
    public sealed class VariableUpdate
    {
        public string Name { get; internal set; }
        public byte[] Data { get; internal set; }
        public bool Forced { get; internal set; }
        public uint WriteSeq { get; internal set; }
        /// <summary>Peer id the write arrived from (0 = a local call on this node).</summary>
        public uint Source { get; internal set; }
        public ulong RecvUs { get; internal set; }
        /// <summary>The writer's wall clock for this write, our own for a local one.</summary>
        public ulong WrittenUs { get; internal set; }
        internal IntPtr SchemaPtr;
    }

    /// <summary>Replicated state, ONE owner: this node holds the authoritative value
    /// (untyped: Schema + byte[]). Remotes cache the latest published value.</summary>
    public class VariableDefinition : INodeHandle
    {
        internal IntPtr Var;   // zeroed by Retire, and by the node at Close
        internal readonly DartNode DartNode;

        void INodeHandle.Invalidate() { Var = IntPtr.Zero; }
        internal readonly string Name;

        public VariableDefinition(DartNode node, string name, Schema schema, byte[] initial = null,
                                  bool readOnly = false, bool allowForce = false,
                                  int catchUp = 0, int keepLast = 0, int backpressureWaitMs = 0,
                                  bool reflectFromMesh = false)
            : this(node, name, schema, initial, readOnly, allowForce, catchUp, keepLast,
                   backpressureWaitMs, reflectFromMesh, true) { }

        private protected VariableDefinition(DartNode node, string name, Schema schema, byte[] initial,
                                             bool readOnly, bool allowForce, int catchUp,
                                             int keepLast, int backpressureWaitMs,
                                             bool reflectFromMesh, bool definition)
        {
            DartNode = node;
            Name = name;
            var co = new DartVariableOpts
            {
                access = (byte)(readOnly ? 1 : 0),
                allow_force = (byte)(allowForce ? 1 : 0),
                catch_up = (ushort)catchUp,
                keep_last = (ushort)keepLast,
                backpressure_wait_us = (uint)backpressureWaitMs * 1000u,
                reflect_from_mesh = (byte)(reflectFromMesh ? 1 : 0),
            };
            using (var p = new PinnedBytes(initial))
            {
                co.initial = p.B;
                Var = definition
                    ? Native.dart_node_create_variable_definition(node.Handle, Codec.CStr(name),
                          schema != null ? schema.Handle : IntPtr.Zero, ref co)
                    : Native.dart_node_create_remote_variable(node.Handle, Codec.CStr(name),
                          schema != null ? schema.Handle : IntPtr.Zero, ref co);
            }
            if (Var == IntPtr.Zero)
                throw new InvalidOperationException((definition ? "variable definition" : "remote variable")
                    + " create failed: " + node.LastError);
            node.RetainSchema(schema);
            node.RegisterHandle(this);
        }

        /// <summary>Read the current value copied out, the store or the cached latest. False
        /// when no value exists yet.</summary>
        public bool TryGet(out byte[] value)
        {
            value = null;
            // the returned view is valid only until the next poll: copy under the node lock
            Native.dart_node_lock(DartNode.Handle);
            try
            {
                DartBytes b;
                if (Native.dart_variable_get(Var, out b) != 1) return false;
                value = Codec.Bytes(b);
                return true;
            }
            finally { Native.dart_node_unlock(DartNode.Handle); }
        }

        /// <summary>Set the value: apply and publish, or send over the set channel. BadRole =
        /// the owner advertises no set channel.</summary>
        public SendStatus Set(byte[] value)
        {
            using (var p = new PinnedBytes(value)) return (SendStatus)Native.dart_variable_set(Var, p.B);
        }

        /// <summary>Force the value: writes are absorbed into the shadow source until Unforce
        /// restores the latest absorbed set. Needs allowForce on the definition.</summary>
        public SendStatus Force(byte[] value)
        {
            using (var p = new PinnedBytes(value)) return (SendStatus)Native.dart_variable_force(Var, p.B);
        }
        public SendStatus Unforce() => (SendStatus)Native.dart_variable_unforce(Var);
        public bool Forced => Native.dart_variable_forced(Var) == 1;

        /// <summary>Remotes matched to this definition (on a RemoteVariable: owners
        /// matched, 0 = no owner present).</summary>
        public int RemoteCount => Native.dart_variable_match_count(Var);

        /// <summary>Block driving the loop until a value exists or timeoutMs elapses. Refused
        /// from a callback or under a service thread.</summary>
        public bool Wait(int timeoutMs) => Native.dart_variable_wait(Var, timeoutMs) == 1;

        /// <summary>Observe changes: fires on every state change and replays the current value
        /// at registration, inline on the thread that applied the write. Null clears.</summary>
        public IDisposable OnChange(Action<VariableUpdate> handler) => Observe(handler, true);

        /// <summary>Observe every applied write, identical bytes or not, with no replay at
        /// registration. The same threading as OnChange. Null clears every observer.</summary>
        public IDisposable OnWrite(Action<VariableUpdate> handler) => Observe(handler, false);

        // One native registration per kind fans out to every observer, as a topic index does.
        // The C replays the current value at registration, so a later observer is replayed the
        // last one seen instead, and both see the same first value.
        private readonly List<Action<VariableUpdate>> _change = new List<Action<VariableUpdate>>();
        private readonly List<Action<VariableUpdate>> _write = new List<Action<VariableUpdate>>();
        private bool _changeBound, _writeBound, _hasLast;
        private VariableUpdate _last;

        private IDisposable Observe(Action<VariableUpdate> handler, bool change)
        {
            List<Action<VariableUpdate>> list = change ? _change : _write;
            if (handler == null)
            {
                lock (list) list.Clear();
                if (change) { Native.dart_variable_on_change(Var, null, IntPtr.Zero); _changeBound = false; }
                else { Native.dart_variable_on_write(Var, null, IntPtr.Zero); _writeBound = false; }
                return null;
            }
            bool bound = change ? _changeBound : _writeBound;
            lock (list) list.Add(handler);
            if (!bound)
            {
                List<Action<VariableUpdate>> l = list;
                bool ch = change;
                long id = Patterns.AddBox(new Patterns.VarBox
                {
                    Handler = u => Fan(l, ch, u),
                    Node = DartNode,
                });
                DartNode.RegisterPatternBox(id);
                if (change) { Native.dart_variable_on_change(Var, Patterns.OnVarUpdate, (IntPtr)id); _changeBound = true; }
                else { Native.dart_variable_on_write(Var, Patterns.OnVarUpdate, (IntPtr)id); _writeBound = true; }
            }
            else if (change)
            {
                bool had; VariableUpdate last;
                lock (list) { had = _hasLast; last = _last; }
                if (had)
                {
                    try { handler(last); }     // late observer: the replay it missed
                    catch (Exception e) { Console.Error.WriteLine("dart variable observer: " + e); }
                }
            }
            return new Observer(list, handler);
        }

        private void Fan(List<Action<VariableUpdate>> list, bool change, VariableUpdate u)
        {
            Action<VariableUpdate>[] hs;
            lock (list)
            {
                if (change) { _last = u; _hasLast = true; }
                hs = list.ToArray();           // an observer may add or drop from inside
            }
            for (int i = 0; i < hs.Length; i++)
            {
                try { hs[i](u); }
                catch (Exception e) { Console.Error.WriteLine("dart variable observer: " + e); }
            }
        }

        private sealed class Observer : IDisposable
        {
            private List<Action<VariableUpdate>> _list;
            private Action<VariableUpdate> _fn;
            internal Observer(List<Action<VariableUpdate>> list, Action<VariableUpdate> fn)
            {
                _list = list; _fn = fn;
            }
            public void Dispose()
            {
                List<Action<VariableUpdate>> l = _list;
                Action<VariableUpdate> f = _fn;
                _list = null; _fn = null;
                if (l != null) lock (l) l.Remove(f);
            }
        }

        /// <summary>Retire the handle: park its channels and release the name, else a re created
        /// same name handle is shadowed. Unusable after, refused from a callback.</summary>
        public SendStatus Retire()
        {
            var rc = (SendStatus)Native.dart_variable_retire(Var);
            if (rc == SendStatus.Ok) Var = IntPtr.Zero;
            return rc;
        }

        /// <summary>A reflectFromMesh handle: re type every channel in place when the mesh
        /// moved. True when it was re typed. See docs/reflection.md.</summary>
        public bool Refresh() => Var != IntPtr.Zero && Native.dart_variable_refresh(Var) == 1;
    }

    /// <summary>A reference to a variable owned by another node (untyped): reads see
    /// the cached latest, writes go over the set channel (dumb writes, no response).</summary>
    public class RemoteVariable : VariableDefinition
    {
        public RemoteVariable(DartNode node, string name, Schema schema = null,
                              int catchUp = 0, int keepLast = 0, int backpressureWaitMs = 0,
                              bool reflectFromMesh = false)
            : base(node, name, schema, null, false, false, catchUp, keepLast,
                   backpressureWaitMs, reflectFromMesh, false) { }

        /// <summary>Owners currently matched.</summary>
        public int MatchCount => RemoteCount;
        public bool HasDefinition => RemoteCount > 0;
    }

    // ---- patterns: pub/sub handles ----------------------------------------------

    /// <summary>The publish-side handle over a (possibly shared) topic (untyped).
    /// Same-name handles on one node share the topic slot with a widened role.</summary>
    public class Publisher
    {
        internal readonly Topic T;

        public Publisher(DartNode node, string name, Schema schema = null, Qos qos = null)
        {
            T = new Topic(node, name, schema, Role.PubOnly, qos);
        }

        public SendStatus Send(byte[] data, long captureUs = 0) => T.Send(data, captureUs);
        public SendStatus Send(string text, long captureUs = 0) => T.Send(text, captureUs);
        public int MatchCount => T.MatchCount();
        public int PendingCount => T.PendingCount;
        public bool Ready => T.Ready;
        public Topic Topic => T;
    }

    /// <summary>The untyped subscribe side. A handler fires per message on the polling
    /// thread instead of the node wide onMessage, or consume with TryTake and Dispatch.</summary>
    public class Subscriber
    {
        internal readonly Topic T;

        public Subscriber(DartNode node, string name, Schema schema = null,
                          Action<DartMessage> handler = null, Qos qos = null)
        {
            T = new Topic(node, name, schema, Role.SubOnly, qos);
            if (handler != null) node.AddSubHandler(T.Index, handler);
        }

        public bool TryTake(out DartMessage message, int timeoutMs = 0) => T.TryTake(out message, timeoutMs);
        public int Dispatch(int maxMsgs = 0, int timeoutMs = 0) => T.Dispatch(maxMsgs, timeoutMs);
        public Topic Topic => T;
    }

    // ---- patterns: typed sugar --------------------------------------------------

    /// <summary>The typed request view inside a full-form function handler: reply with
    /// a typed value, Fail, or Defer. Valid only inside the handler callback.</summary>
    public sealed class DartRequest<TRsp>
    {
        private readonly DartRequest _core;
        private readonly Schema _rsp;

        internal DartRequest(DartRequest core, Schema rsp) { _core = core; _rsp = rsp; }

        public byte[] Data => _core.Data;
        public uint Caller => _core.Caller;
        public string CallerName => _core.CallerName;
        public string FunctionName => _core.FunctionName;
        public ulong RecvUs => _core.RecvUs;
        public bool Answered => _core.Answered;

        public void Reply(TRsp value) => _core.Reply(_rsp.Encode(value));
        public void Fail(string message = null) => _core.Fail(message);
        public Deferred<TRsp> Defer() => new Deferred<TRsp>(_core.Defer(), _rsp);
    }

    /// <summary>The typed parked reply: Complete(value)/Fail() exactly once, any thread.</summary>
    public sealed class Deferred<TRsp>
    {
        private readonly Deferred _core;
        private readonly Schema _rsp;

        internal Deferred(Deferred core, Schema rsp) { _core = core; _rsp = rsp; }

        public bool Valid => _core.Valid;
        public bool Complete(TRsp value, string message = null) => _core.Complete(_rsp.Encode(value), message);
        public bool Fail(string message = null) => _core.Fail(message);
    }

    /// <summary>The typed implementation side. Simple form: the return value is the reply
    /// and a thrown exception answers AppError. Full form: DartRequest&lt;TRsp&gt;.</summary>
    public sealed class FunctionDefinition<TReq, TRsp>
    {
        private readonly FunctionDefinition _core;
        private readonly Schema _req, _rsp;

        public FunctionDefinition(DartNode node, string name, Func<TReq, TRsp> handler,
                                  int backpressureWaitMs = 0, int timeoutMs = 0,
                                  bool reflectFromMesh = false)
        {
            _req = new Schema(typeof(TReq));
            _rsp = new Schema(typeof(TRsp));
            Action<DartRequest> h = null;
            if (handler != null)
            {
                Schema req = _req, rsp = _rsp;
                h = r =>
                {
                    object q;
                    if (!Patterns.TryDecode(req, r.SchemaPtr, r.Data, typeof(TReq), out q)) { r.Fail("request decode failed"); return; }
                    TRsp outv = handler((TReq)q);   // a throw answers AppError (trampoline catch)
                    if (!r.Answered) r.Reply(rsp.Encode(outv));
                };
            }
            _core = new FunctionDefinition(node, name, _req, _rsp, h, backpressureWaitMs, timeoutMs,
                                           reflectFromMesh: reflectFromMesh);
        }

        /// <summary>The async handler form: the Task's completion answers the call, its result
        /// Ok and an exception AppError. On the polling thread until the first await.</summary>
        public FunctionDefinition(DartNode node, string name, Func<TReq, Task<TRsp>> handler,
                                  int backpressureWaitMs = 0, int timeoutMs = 0,
                                  bool reflectFromMesh = false)
        {
            _req = new Schema(typeof(TReq));
            _rsp = new Schema(typeof(TRsp));
            Func<DartRequest, Task<byte[]>> h = null;
            if (handler != null)
            {
                Schema req = _req, rsp = _rsp;
                h = async r =>
                {
                    object q;
                    if (!Patterns.TryDecode(req, r.SchemaPtr, r.Data, typeof(TReq), out q))
                        throw new Exception("request decode failed");
                    TRsp outv = await handler((TReq)q).ConfigureAwait(false);
                    return rsp.Encode(outv);
                };
            }
            _core = new FunctionDefinition(node, name, _req, _rsp, h, backpressureWaitMs, timeoutMs,
                                           reflectFromMesh: reflectFromMesh);
        }

        public FunctionDefinition(DartNode node, string name, Action<TReq, DartRequest<TRsp>> handler,
                                  int backpressureWaitMs = 0, int timeoutMs = 0,
                                  bool reflectFromMesh = false)
        {
            _req = new Schema(typeof(TReq));
            _rsp = new Schema(typeof(TRsp));
            Action<DartRequest> h = null;
            if (handler != null)
            {
                Schema req = _req, rsp = _rsp;
                h = r =>
                {
                    object q;
                    if (!Patterns.TryDecode(req, r.SchemaPtr, r.Data, typeof(TReq), out q)) { r.Fail("request decode failed"); return; }
                    handler((TReq)q, new DartRequest<TRsp>(r, rsp));
                };
            }
            _core = new FunctionDefinition(node, name, _req, _rsp, h, backpressureWaitMs, timeoutMs,
                                           reflectFromMesh: reflectFromMesh);
        }

        public int CallerCount => _core.CallerCount;
        /// <summary>A reflectFromMesh handle: re type in place when the mesh moved. True when
        /// it was re typed. See docs/reflection.md.</summary>
        public bool Refresh() => _core.Refresh();
        public SendStatus Retire() => _core.Retire();
    }

    /// <summary>The typed owning call outcome. Reading Value when not Ok throws
    /// CallException. Status never throws.</summary>
    public sealed class DartResponse<TRsp>
    {
        internal DartResponse Core;
        internal Schema RspSchema;

        public CallStatus Status => Core.Status;
        public bool Ok => Core.Ok;
        public uint Provider => Core.Provider;
        public ulong WrittenUs => Core.WrittenUs;
        public SendStatus SendStatus => Core.SendStatus;
        /// <summary>Human-readable outcome text (see DartResponse.Message).</summary>
        public string Message => Core.Message;

        public TRsp Value
        {
            get
            {
                if (!Ok)
                    throw new CallException(Status, "call did not complete Ok: status " + Status
                        + (Core.Message.Length != 0 ? " (" + Core.Message + ")" : "")
                        + (Core.SendStatus != SendStatus.Ok ? " (send " + Core.SendStatus + ")" : ""));
                object v;
                if (!Patterns.TryDecode(RspSchema, Core.SchemaPtr, Core.Data, typeof(TRsp), out v))
                    throw new CallException(Status, "response payload failed to decode");
                return (TRsp)v;
            }
        }
    }

    /// <summary>The typed caller side of a function defined on another node.</summary>
    public sealed class RemoteFunction<TReq, TRsp>
    {
        private readonly RemoteFunction _core;
        private readonly Schema _req, _rsp;

        public RemoteFunction(DartNode node, string name, int backpressureWaitMs = 0, int timeoutMs = 0,
                              bool reflectFromMesh = false)
        {
            _req = new Schema(typeof(TReq));
            _rsp = new Schema(typeof(TRsp));
            _core = new RemoteFunction(node, name, _req, _rsp, backpressureWaitMs, timeoutMs,
                                       reflectFromMesh: reflectFromMesh);
        }

        /// <summary>BLOCKING call (see the untyped RemoteFunction.Call). provider directs it
        /// at one definition by peer id (0 = undirected, first answer wins).</summary>
        public DartResponse<TRsp> Call(TReq request, int timeoutMs = -1, uint provider = 0)
            => new DartResponse<TRsp> { Core = _core.Call(_req.Encode(request), timeoutMs, provider), RspSchema = _rsp };

        /// <summary>Async call: the Task NEVER faults, inspect Status.</summary>
        public async Task<DartResponse<TRsp>> CallAsync(TReq request, uint provider = 0)
        {
            DartResponse core = await _core.CallAsync(_req.Encode(request), provider).ConfigureAwait(false);
            return new DartResponse<TRsp> { Core = core, RspSchema = _rsp };
        }

        public int MatchCount => _core.MatchCount;
        public bool HasDefinition => _core.HasDefinition;
        /// <summary>A reflectFromMesh handle: re type in place when the mesh moved. True when
        /// it was re typed. See docs/reflection.md.</summary>
        public bool Refresh() => _core.Refresh();
        public SendStatus Retire() => _core.Retire();
    }

    /// <summary>The typed task handler context: typed Progress over the untyped surface.</summary>
    public sealed class TaskContext<TPrg>
    {
        private readonly TaskContext _core;
        private readonly Schema _prg;

        internal TaskContext(TaskContext core, Schema prg) { _core = core; _prg = prg; }

        public SendStatus Progress(TPrg value) => _core.Progress(_prg.Encode(value));
        public SendStatus Progress(byte[] value) => _core.Progress(value);
        public CancellationToken CancellationToken => _core.CancellationToken;
        public bool Cancelled => _core.Cancelled;
        public uint Caller => _core.Caller;
        public string CallerName => _core.CallerName;
        public ulong RecvUs => _core.RecvUs;
        public ulong WrittenUs => _core.WrittenUs;
    }

    /// <summary>The typed implementation side of a task. The async handler's completion
    /// answers the call (docs/csharp.md), on the polling thread until its first await.</summary>
    public sealed class TaskDefinition<TReq, TPrg, TRsp>
    {
        private readonly TaskDefinition _core;
        private readonly Schema _req, _prg, _rsp;

        public TaskDefinition(DartNode node, string name, Func<TReq, TaskContext<TPrg>, Task<TRsp>> handler,
                              bool progressBestEffort = false, int progressKeepLast = 0,
                              bool noCancel = false, bool exclusive = false, bool multi = false,
                              int backpressureWaitMs = 0, int timeoutMs = 0,
                              bool reflectFromMesh = false)
        {
            _req = new Schema(typeof(TReq));
            _prg = new Schema(typeof(TPrg));
            _rsp = new Schema(typeof(TRsp));
            Func<DartRequest, TaskContext, Task<byte[]>> h = null;
            if (handler != null)
            {
                Schema req = _req, prg = _prg, rsp = _rsp;
                h = async (r, ctx) =>
                {
                    object q;
                    if (!Patterns.TryDecode(req, r.SchemaPtr, r.Data, typeof(TReq), out q))
                        throw new Exception("request decode failed");
                    TRsp outv = await handler((TReq)q, new TaskContext<TPrg>(ctx, prg)).ConfigureAwait(false);
                    return rsp.Encode(outv);
                };
            }
            _core = new TaskDefinition(node, name, _req, _prg, _rsp, h, progressBestEffort,
                                       progressKeepLast, noCancel, exclusive, multi,
                                       backpressureWaitMs, timeoutMs,
                                       reflectFromMesh: reflectFromMesh);
        }

        public int CallerCount => _core.CallerCount;
        /// <summary>A reflectFromMesh handle: re type in place when the mesh moved. True when
        /// it was re typed. See docs/reflection.md.</summary>
        public bool Refresh() => _core.Refresh();
        public SendStatus Retire() => _core.Retire();
    }

    /// <summary>The typed caller side of a task defined on another node.</summary>
    public sealed class RemoteTask<TReq, TPrg, TRsp>
    {
        private readonly RemoteTask _core;
        private readonly Schema _req, _prg, _rsp;

        public RemoteTask(DartNode node, string name, bool progressBestEffort = false,
                          int progressKeepLast = 0, int backpressureWaitMs = 0, int timeoutMs = 0,
                          bool reflectFromMesh = false)
        {
            _req = new Schema(typeof(TReq));
            _prg = new Schema(typeof(TPrg));
            _rsp = new Schema(typeof(TRsp));
            _core = new RemoteTask(node, name, _req, _prg, _rsp, progressBestEffort,
                                   progressKeepLast, backpressureWaitMs, timeoutMs,
                                   reflectFromMesh: reflectFromMesh);
        }

        /// <summary>Start the task: the Task never faults. progress fires per typed update and
        /// skips the valueless RUNNING ack. The token requests cooperative cancellation.</summary>
        public Task<DartResponse<TRsp>> CallAsync(TReq request, IProgress<TPrg> progress = null,
                                                  CancellationToken cancellationToken = default,
                                                  uint provider = 0)
            => CallAsync(request, out _, progress, cancellationToken, provider);

        /// <summary>As above, and callId receives the call id at commit, the handle for Cancel.
        /// 0 when the request never committed.</summary>
        public Task<DartResponse<TRsp>> CallAsync(TReq request, out uint callId,
                                                  IProgress<TPrg> progress = null,
                                                  CancellationToken cancellationToken = default,
                                                  uint provider = 0)
        {
            Action<TaskProgress> sink = null;
            if (progress != null)
            {
                Schema prg = _prg;
                IProgress<TPrg> pr = progress;
                sink = info =>
                {
                    object v;
                    if (info.Value == null) return;   // the RUNNING ack: no TPrg to decode
                    if (Patterns.TryDecode(prg, info.SchemaPtr, info.Value, typeof(TPrg), out v))
                        pr.Report((TPrg)v);
                };
            }
            Task<DartResponse> core = _core.CallCore(_req.Encode(request), out callId, sink,
                                                     cancellationToken, provider);
            return Wrap(core);
        }

        private async Task<DartResponse<TRsp>> Wrap(Task<DartResponse> core)
            => new DartResponse<TRsp> { Core = await core.ConfigureAwait(false), RspSchema = _rsp };

        /// <summary>Cancel the call callId, see the untyped RemoteTask.Cancel.</summary>
        public SendStatus Cancel(uint callId) => _core.Cancel(callId);
        public int MatchCount => _core.MatchCount;
        public bool HasDefinition => _core.HasDefinition;
        /// <summary>A reflectFromMesh handle: re type in place when the mesh moved. True when
        /// it was re typed. See docs/reflection.md.</summary>
        public bool Refresh() => _core.Refresh();
        public SendStatus Retire() => _core.Retire();
    }

    /// <summary>The typed authoritative variable. Value get throws while no value exists,
    /// Value set throws DartException on a non Ok status. Set returns the status.</summary>
    public class VariableDefinition<T>
    {
        private protected VariableDefinition _core;
        private protected Schema _schema;

        private protected VariableDefinition() { }

        public VariableDefinition(DartNode node, string name, bool readOnly = false,
                                  bool allowForce = false, int catchUp = 0, int keepLast = 0,
                                  int backpressureWaitMs = 0, bool reflectFromMesh = false)
        {
            _schema = new Schema(typeof(T));
            _core = new VariableDefinition(node, name, _schema, null, readOnly, allowForce,
                                           catchUp, keepLast, backpressureWaitMs, reflectFromMesh);
        }

        /// <summary>Overload with an initial value (the value before any set).</summary>
        public VariableDefinition(DartNode node, string name, T initial, bool readOnly = false,
                                  bool allowForce = false, int catchUp = 0, int keepLast = 0,
                                  int backpressureWaitMs = 0, bool reflectFromMesh = false)
        {
            _schema = new Schema(typeof(T));
            _core = new VariableDefinition(node, name, _schema, _schema.Encode(initial),
                                           readOnly, allowForce, catchUp, keepLast,
                                           backpressureWaitMs, reflectFromMesh);
        }

        public T Value
        {
            get
            {
                T v;
                if (!TryGet(out v))
                    throw new InvalidOperationException("variable '" + _core.Name + "' has no value yet");
                return v;
            }
            set
            {
                SendStatus st = Set(value);
                if (st != SendStatus.Ok)
                    throw new DartException(st, "variable '" + _core.Name + "' set refused: " + st);
            }
        }

        public bool TryGet(out T value)
        {
            value = default(T);
            byte[] b;
            if (!_core.TryGet(out b)) return false;
            object v;
            if (!Patterns.TryDecode(_schema, IntPtr.Zero, b, typeof(T), out v)) return false;
            value = (T)v;
            return true;
        }

        public SendStatus Set(T value) => _core.Set(_schema.Encode(value));
        public SendStatus Force(T value) => _core.Force(_schema.Encode(value));
        public SendStatus Unforce() => _core.Unforce();
        public bool Forced => _core.Forced;
        public int RemoteCount => _core.RemoteCount;
        public bool Wait(int timeoutMs) => _core.Wait(timeoutMs);
        /// <summary>A reflectFromMesh handle: re type in place when the mesh moved. True when
        /// it was re typed. See docs/reflection.md.</summary>
        public bool Refresh() => _core.Refresh();
        public SendStatus Retire() => _core.Retire();

        /// <summary>Observe changes, typed. Handler forms: (T value) or (T value, VariableUpdate
        /// update). A null cast delegate clears.</summary>
        public IDisposable OnChange(Action<T> handler) => _core.OnChange(Adapt(handler, null));
        public IDisposable OnChange(Action<T, VariableUpdate> handler) => _core.OnChange(Adapt(null, handler));
        /// <summary>Observe every applied write, typed (no replay at registration).</summary>
        public IDisposable OnWrite(Action<T> handler) => _core.OnWrite(Adapt(handler, null));
        public IDisposable OnWrite(Action<T, VariableUpdate> handler) => _core.OnWrite(Adapt(null, handler));

        private Action<VariableUpdate> Adapt(Action<T> plain, Action<T, VariableUpdate> full)
        {
            if (plain == null && full == null) return null;
            Schema schema = _schema;
            return u =>
            {
                object v;
                if (!Patterns.TryDecode(schema, u.SchemaPtr, u.Data, typeof(T), out v)) return;
                if (plain != null) plain((T)v); else full((T)v, u);
            };
        }
    }

    /// <summary>The typed accessor of a variable owned elsewhere. The same surface as the
    /// definition plus HasDefinition and MatchCount.</summary>
    public sealed class RemoteVariable<T> : VariableDefinition<T>
    {
        public RemoteVariable(DartNode node, string name, int catchUp = 0, int keepLast = 0,
                              int backpressureWaitMs = 0, bool reflectFromMesh = false)
        {
            _schema = new Schema(typeof(T));
            _core = new RemoteVariable(node, name, _schema, catchUp, keepLast, backpressureWaitMs,
                                       reflectFromMesh);
        }

        /// <summary>Owners currently matched.</summary>
        public int MatchCount => _core.RemoteCount;
        public bool HasDefinition => _core.RemoteCount > 0;
    }

    /// <summary>The typed publish side.</summary>
    public sealed class Publisher<T>
    {
        private readonly Publisher _core;
        private readonly Schema _schema;

        public Publisher(DartNode node, string name, Qos qos = null)
        {
            _schema = new Schema(typeof(T));
            _core = new Publisher(node, name, _schema, qos);
        }

        public SendStatus Send(T value, long captureUs = 0)
            => _core.Send(_schema.Encode(value), captureUs);
        public int MatchCount => _core.MatchCount;
        public int PendingCount => _core.PendingCount;
        public bool Ready => _core.Ready;
        public Topic Topic => _core.Topic;
    }

    /// <summary>The typed subscribe side: a handler per message (on the polling
    /// thread), or the typed TryTake/Dispatch consumer-queue form.</summary>
    public sealed class Subscriber<T>
    {
        private readonly Subscriber _core;

        public Subscriber(DartNode node, string name, Action<T> handler = null, Qos qos = null)
        {
            _core = new Subscriber(node, name, new Schema(typeof(T)),
                handler == null ? (Action<DartMessage>)null : m => { if (m.Value is T v) handler(v); },
                qos);
        }

        public Subscriber(DartNode node, string name, Action<T, DartMessage> handler,
                          Qos qos = null)
        {
            if (handler == null) throw new ArgumentNullException(nameof(handler));
            _core = new Subscriber(node, name, new Schema(typeof(T)),
                m => { if (m.Value is T v) handler(v, m); }, qos);
        }

        /// <summary>Typed take: decodes straight from the queue.</summary>
        public bool TryTake(out T value, int timeoutMs = 0)
        {
            value = default(T);
            DartMessage m;
            if (!_core.TryTake(out m, timeoutMs) || !(m.Value is T)) return false;
            value = (T)m.Value;
            return true;
        }

        public int Dispatch(int maxMsgs = 0, int timeoutMs = 0) => _core.Dispatch(maxMsgs, timeoutMs);
        public Topic Topic => _core.Topic;
    }

    // ---- marshaling, allocators, schema codec + reflection ----------------------

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate IntPtr DartPageFn(IntPtr ptr, UIntPtr size);

    internal static class Codec
    {
        internal const byte U8 = 0, U16 = 1, U32 = 2, U64 = 3, I8 = 4, I16 = 5, I32 = 6,
            I64 = 7, F32 = 8, F64 = 9, BOOL = 10, ARR = 11, STRUCT = 12, STR = 13,
            VSTR = 14, VARR = 15, MAP = 16,   // the variable kinds (ride the message tail)
            ENUM = 17,                        // named integer (wire = its backing scalar)
            NAMED = 18;                       // a name on another type (reflection unwraps it)

        private static readonly string[] Token = { "u8", "u16", "u32", "u64", "i8", "i16",
            "i32", "i64", "f32", "f64", "bool" };
        private static readonly int[] ScalarSize = { 1, 2, 4, 8, 1, 2, 4, 8, 4, 8, 1 };

        private static readonly Dictionary<Type, byte> ScalarKind = new Dictionary<Type, byte>
        {
            { typeof(byte), U8 }, { typeof(ushort), U16 }, { typeof(uint), U32 }, { typeof(ulong), U64 },
            { typeof(sbyte), I8 }, { typeof(short), I16 }, { typeof(int), I32 }, { typeof(long), I64 },
            { typeof(float), F32 }, { typeof(double), F64 }, { typeof(bool), BOOL },
        };

        // --- allocators: managed realloc/free, works on plain .NET and Unity/IL2CPP ---
        private static readonly DartPageFn s_page = PageRealloc;
        internal static readonly DartAllocFn SchemaAlloc = SchemaReAlloc;

        [MonoPInvokeCallback(typeof(DartPageFn))]
        private static IntPtr PageRealloc(IntPtr ptr, UIntPtr size)
        {
            if ((ulong)size == 0) { if (ptr != IntPtr.Zero) Marshal.FreeHGlobal(ptr); return IntPtr.Zero; }
            IntPtr cb = (IntPtr)(long)(ulong)size;
            return ptr == IntPtr.Zero ? Marshal.AllocHGlobal(cb) : Marshal.ReAllocHGlobal(ptr, cb);
        }

        [MonoPInvokeCallback(typeof(DartAllocFn))]
        private static IntPtr SchemaReAlloc(IntPtr user, IntPtr ptr, UIntPtr size)
        {
            if ((ulong)size == 0) { if (ptr != IntPtr.Zero) Marshal.FreeHGlobal(ptr); return IntPtr.Zero; }
            IntPtr cb = (IntPtr)(long)(ulong)size;
            return ptr == IntPtr.Zero ? Marshal.AllocHGlobal(cb) : Marshal.ReAllocHGlobal(ptr, cb);
        }

        internal static DartAllocator DefaultAllocator()
        {
            return new DartAllocator
            {
                page_realloc = Marshal.GetFunctionPointerForDelegate(s_page),
                page_size = 64u * 1024u,
            };
        }

        // --- string / bytes marshaling ---
        internal static byte[] CStr(string s)
        {
            if (s == null) return null;
            byte[] b = Encoding.UTF8.GetBytes(s);
            byte[] r = new byte[b.Length + 1];
            Array.Copy(b, r, b.Length);
            return r;   // NUL-terminated
        }

        internal static IntPtr CStrPtr(string s)
        {
            if (string.IsNullOrEmpty(s)) return IntPtr.Zero;
            byte[] b = Encoding.UTF8.GetBytes(s);
            IntPtr p = Marshal.AllocHGlobal(b.Length + 1);
            Marshal.Copy(b, 0, p, b.Length);
            Marshal.WriteByte(p, b.Length, 0);
            return p;
        }

        internal static void FreeCStr(IntPtr p) { if (p != IntPtr.Zero) Marshal.FreeHGlobal(p); }

        internal static string Str(DartStringView s)
        {
            if (s.data == IntPtr.Zero || (ulong)s.len == 0) return "";
            int n = (int)(ulong)s.len;
            byte[] b = new byte[n];
            Marshal.Copy(s.data, b, 0, n);
            return Encoding.UTF8.GetString(b);
        }

        internal static byte[] Bytes(DartBytes d)
        {
            if (d.data == IntPtr.Zero || (ulong)d.len == 0) return Array.Empty<byte>();
            int n = (int)(ulong)d.len;
            byte[] b = new byte[n];
            Marshal.Copy(d.data, b, 0, n);
            return b;
        }

        internal static string PtrToStr(IntPtr p)
        {
            if (p == IntPtr.Zero) return "";
            int len = 0;
            while (Marshal.ReadByte(p, len) != 0) len++;
            byte[] b = new byte[len];
            Marshal.Copy(p, b, 0, len);
            return Encoding.UTF8.GetString(b);
        }

        internal static string CBufStr(byte[] buf)
        {
            int n = Array.IndexOf(buf, (byte)0);
            if (n < 0) n = buf.Length;
            return Encoding.UTF8.GetString(buf, 0, n);
        }

        // reflection: message type to field plan to DSL
        private sealed class FieldPlan
        {
            public FieldInfo Field;
            public string WireName;
            public byte Kind;
            public byte Elem;      // array element kind, or (enum) the backing scalar kind
            public int Count;
            public int StrCap;
            public Type Nested;
            public Type EnumType;  // enum fields: the C# enum type (names/values via reflection)
            public string TypeName; // a STANDARD type's name: the whole spelling
        }
        private sealed class TypeSpec { public string Name; public List<FieldPlan> Fields; }
        private static readonly Dictionary<Type, TypeSpec> s_specs = new Dictionary<Type, TypeSpec>();

        private static bool StructLike(Type t)
            => t != typeof(string) && !t.IsArray && !t.IsPrimitive && !t.IsEnum
               && (t.IsValueType || t.IsClass);

        // A `map` field: any dictionary (canonically Dictionary<string, object>).
        private static bool IsMapType(Type t)
            => typeof(System.Collections.IDictionary).IsAssignableFrom(t);

        // A bare type used directly as a handle's T is the whole schema, anonymous, so the same
        // bare type in any language is the same bytes and hash. A plain array is the variable one.
        private static bool IsValueType(Type t)
            => ScalarKind.ContainsKey(t) || t.IsEnum || t == typeof(string) || t.IsArray
               || IsMapType(t);

        // the one-field spec of a bare type: no root name, the field is anonymous
        private static TypeSpec ValueSpec(Type t)
        {
            var plan = new FieldPlan { Field = null, WireName = "" };
            byte k;
            if (t == typeof(string)) plan.Kind = VSTR;
            else if (t.IsArray)
            {
                Type et = t.GetElementType();
                if (et == null || !ScalarKind.TryGetValue(et, out k))
                    throw new SchemaException("bare array schema " + t
                        + " element must be a scalar (a capped-string array needs DSL text)");
                plan.Kind = VARR; plan.Elem = k;
            }
            else if (IsMapType(t)) plan.Kind = MAP;
            else if (t.IsEnum)
            {
                if (!ScalarKind.TryGetValue(Enum.GetUnderlyingType(t), out k) || k > I64)
                    throw new SchemaException("bare enum schema " + t + " must have an integer backing type");
                plan.Kind = ENUM; plan.Elem = k; plan.EnumType = t;
            }
            else plan.Kind = ScalarKind[t];
            return new TypeSpec { Name = null, Fields = new List<FieldPlan> { plan } };
        }

        private static TypeSpec Spec(Type t)
        {
            lock (s_specs)
            {
                TypeSpec cached;
                if (s_specs.TryGetValue(t, out cached)) return cached;
                if (IsValueType(t))
                {
                    cached = ValueSpec(t);
                    s_specs[t] = cached;
                    return cached;
                }
                var attr = (DartSchemaAttribute)Attribute.GetCustomAttribute(t, typeof(DartSchemaAttribute));
                string name = attr != null && !string.IsNullOrEmpty(attr.Name) ? attr.Name : t.Name;
                FieldInfo[] fields = t.GetFields(BindingFlags.Public | BindingFlags.Instance);
                Array.Sort(fields, (a, b) => a.MetadataToken.CompareTo(b.MetadataToken));
                var plans = new List<FieldPlan>();
                foreach (var f in fields)
                {
                    var fa = (DartFieldAttribute)Attribute.GetCustomAttribute(f, typeof(DartFieldAttribute));
                    var arr = (DartArrayAttribute)Attribute.GetCustomAttribute(f, typeof(DartArrayAttribute));
                    var str = (DartStringAttribute)Attribute.GetCustomAttribute(f, typeof(DartStringAttribute));
                    var plan = new FieldPlan { Field = f, WireName = fa != null ? fa.Name : f.Name };
                    byte k;
                    if (arr != null)   // [DartArray(N)]: a fixed array
                    {
                        Type et = f.FieldType.GetElementType();
                        if (et == typeof(string))
                        {
                            if (str == null)
                                throw new SchemaException("string array field " + f.Name
                                    + " needs [DartString(cap)] for its element capacity");
                            plan.Kind = ARR; plan.Elem = STR; plan.Count = arr.Count; plan.StrCap = str.Cap;
                        }
                        else if (et != null && ScalarKind.TryGetValue(et, out k))
                        { plan.Kind = ARR; plan.Elem = k; plan.Count = arr.Count; }
                        else throw new SchemaException("array field " + f.Name
                            + " element must be a scalar or a [DartString] string");
                    }
                    else if (f.FieldType.IsArray)   // T[] without [DartArray]: a variable array
                    {
                        Type et = f.FieldType.GetElementType();
                        if (et == typeof(string))
                        {
                            if (str == null)
                                throw new SchemaException("variable string array field " + f.Name
                                    + " needs [DartString(cap)] for its element capacity");
                            plan.Kind = VARR; plan.Elem = STR; plan.StrCap = str.Cap;
                        }
                        else if (et != null && ScalarKind.TryGetValue(et, out k))
                        { plan.Kind = VARR; plan.Elem = k; }
                        else throw new SchemaException("variable array field " + f.Name
                            + " element must be a scalar or a [DartString] string");
                    }
                    else if (f.FieldType == typeof(string))
                    {
                        if (str != null) { plan.Kind = STR; plan.StrCap = str.Cap; }   // capped
                        else plan.Kind = VSTR;   // variable, unbounded
                    }
                    else if (IsMapType(f.FieldType)) { plan.Kind = MAP; }
                    else if (f.FieldType.IsEnum)   // a named integer, backing from the enum
                    {
                        if (!ScalarKind.TryGetValue(Enum.GetUnderlyingType(f.FieldType), out k) || k > I64)
                            throw new SchemaException("enum field " + f.Name + " must have an integer backing type");
                        plan.Kind = ENUM; plan.Elem = k; plan.EnumType = f.FieldType;
                    }
                    else if (ScalarKind.TryGetValue(f.FieldType, out k)) { plan.Kind = k; }
                    else if (StructLike(f.FieldType)) { plan.Kind = STRUCT; plan.Nested = f.FieldType; }
                    else throw new SchemaException("unsupported field type " + f.FieldType + " on " + f.Name
                        + " (use scalars, strings ([DartString] = capped, plain = variable), arrays "
                        + "([DartArray] = fixed, plain = variable), a Dictionary<string,object> map, "
                        + "or nested structs)");
                    // a standard type names the field's type: on the field, or on its struct
                    var tn = (DartTypeNameAttribute)Attribute.GetCustomAttribute(f, typeof(DartTypeNameAttribute));
                    if (tn == null && plan.Nested != null)
                        tn = (DartTypeNameAttribute)Attribute.GetCustomAttribute(plan.Nested, typeof(DartTypeNameAttribute));
                    if (tn != null) plan.TypeName = tn.Name;
                    plans.Add(plan);
                }
                if (plans.Count == 0)
                    throw new SchemaException(t.Name + " has no public instance fields to map");
                cached = new TypeSpec { Name = name, Fields = plans };
                s_specs[t] = cached;
                return cached;
            }
        }

        internal static string TypeDsl(Type t)
        {
            var spec = Spec(t);
            if (spec.Name == null) return TypeToken(spec.Fields[0]) + "\n";   // a bare type
            var sb = new StringBuilder();
            sb.Append(spec.Name).Append("\n{\n");
            for (int i = 0; i < spec.Fields.Count; i++)
            {
                if (i > 0) sb.Append(",\n");
                sb.Append("    ").Append(FieldLine(spec.Fields[i]));
            }
            sb.Append("\n}\n");
            return sb.ToString();
        }

        private static string FieldLine(FieldPlan f) => f.WireName + ": " + TypeToken(f);

        // one field's type in DSL form (the whole schema when the root is a bare type)
        private static string TypeToken(FieldPlan f)
        {
            if (f.TypeName != null) return f.TypeName;   // a standard type spells as its name
            if (f.Kind == STRUCT)
            {
                var nested = Spec(f.Nested).Fields;
                var parts = new List<string>();
                foreach (var g in nested) parts.Add(FieldLine(g));
                return "{ " + string.Join(", ", parts) + " }";
            }
            if (f.Kind == ARR) return ElemToken(f.Elem, f.StrCap) + "[" + f.Count + "]";
            if (f.Kind == VARR) return ElemToken(f.Elem, f.StrCap) + "[]";
            if (f.Kind == STR) return "string<" + f.StrCap + ">";
            if (f.Kind == VSTR) return "string";
            if (f.Kind == MAP) return "map";
            if (f.Kind == ENUM) return "enum<" + Token[f.Elem] + "> " + EnumBody(f.EnumType);
            return Token[f.Kind];
        }

        private static string ElemToken(byte elem, int strCap)
            => elem == STR ? "string<" + strCap + ">" : Token[elem];

        // `{ Name=value, ... }` from a C# enum's members in DECLARATION order (GetFields, not
        // Enum.GetNames which sorts by value), so the wire/hash matches the other languages.
        private static string EnumBody(Type enumType)
        {
            var under = Enum.GetUnderlyingType(enumType);
            var parts = new List<string>();
            foreach (var fi in enumType.GetFields(BindingFlags.Public | BindingFlags.Static))
                parts.Add(fi.Name + "=" + Convert.ToString(Convert.ChangeType(fi.GetValue(null), under),
                                                           System.Globalization.CultureInfo.InvariantCulture));
            return "{ " + string.Join(", ", parts) + " }";
        }

        // The DSL text of any compiled schema, straight from the C printer, so there is exactly
        // one implementation of the spelling across every binding.
        internal static string SchemaDsl(IntPtr s)
        {
            uint need = Native.dart_schema_print(s, IntPtr.Zero, UIntPtr.Zero);
            if (need == 0) return "";
            IntPtr buf = Marshal.AllocHGlobal((int)need + 1);
            try
            {
                Native.dart_schema_print(s, buf, (UIntPtr)(need + 1));
                return Marshal.PtrToStringAnsi(buf) ?? "";
            }
            finally { Marshal.FreeHGlobal(buf); }
        }

        // True when a compiled schema is a BARE TYPE: an unnamed root of one anonymous field,
        // so its message is a single value (encode takes it, decode returns it).
        internal static bool IsValueRoot(IntPtr s)
        {
            if (s == IntPtr.Zero || Native.dart_schema_field_count(s) != 1) return false;
            if ((ulong)Native.dart_schema_name(s).len != 0) return false;
            DartSchemaFieldInfo info;
            if (Native.dart_schema_field_at(s, 0, out info) == 0) return false;
            return info.depth == 0 && (ulong)info.name.len == 0 && info.kind != STRUCT;
        }

        private static void EmitNodes(StringBuilder sb, List<object[]> nodes)
        {
            for (int i = 0; i < nodes.Count; i++)
            {
                if (i > 0) sb.Append(",\n");
                sb.Append("    ").Append(NodeLine(nodes[i]));
            }
        }

        private static string NodeLine(object[] node) => (string)node[0] + ": " + NodeType(node);

        private static string NodeType(object[] node)
        {
            byte kind = (byte)node[1], elem = (byte)node[2];
            int count = (int)node[3], strCap = (int)node[4];
            var children = (List<object[]>)node[5];
            if (kind == STRUCT)
            {
                var parts = new List<string>();
                foreach (var c in children) parts.Add(NodeLine(c));
                return "{ " + string.Join(", ", parts) + " }";
            }
            if (kind == ARR) return ElemToken(elem, strCap) + "[" + count + "]";
            if (kind == VARR) return ElemToken(elem, strCap) + "[]";
            if (kind == STR) return "string<" + strCap + ">";
            if (kind == VSTR) return "string";
            if (kind == MAP) return "map";
            if (kind == ENUM) return "enum<" + Token[elem] + "> " + (string)node[6];
            return Token[kind];
        }

        // `{ Name=value, ... }` reconstructed from a compiled schema's enum option table
        private static string EnumBodyFromSchema(IntPtr s, ushort field)
        {
            var parts = new List<string>();
            ushort n = Native.dart_schema_enum_count(s, field);
            for (ushort k = 0; k < n; k++)
            {
                long val; DartStringView nm;
                if (Native.dart_schema_enum_variant(s, field, k, out val, out nm) != 0)
                    parts.Add(Str(nm) + "=" + val.ToString(System.Globalization.CultureInfo.InvariantCulture));
            }
            return "{ " + string.Join(", ", parts) + " }";
        }

        // Encode: object to message bytes. One walk of the reflection spec resolves every value
        // and pre serializes the variable payloads, so the tail is sized before pinning.
        private sealed class SetOp
        {
            public byte[] Cpath;
            public string Path;      // for error text
            public byte Kind, Elem;
            public int Count, StrCap;
            public object Value;     // fixed fields / fixed string[] : the raw value
            public byte[] Prepared;  // variable fields: the pre-serialized payload bytes
        }

        // Encode a typed object by reflection, or a Dictionary by field name with the schema
        // driving the walk, like the Python wrapper.
        internal static byte[] Encode(IntPtr s, object value)
        {
            var ops = new List<SetOp>();
            long varBytes = 0;
            if (IsValueRoot(s))
                CollectRootValue(s, value, ops, ref varBytes);   // the bare root value
            else if (value is System.Collections.IDictionary dict)
                CollectFromDict(s, dict, ops, ref varBytes);
            else
                Collect(s, Spec(value.GetType()), value, "", ops, ref varBytes);

            // msg_min is the fixed section plus one empty frame per variable field, and each
            // variable frame then grows by exactly its payload length
            long cap = (long)Native.dart_schema_msg_min(s) + varBytes;
            byte[] buf = new byte[cap > 0 ? cap : 1];
            GCHandle gh = GCHandle.Alloc(buf, GCHandleType.Pinned);
            try
            {
                IntPtr p = gh.AddrOfPinnedObject();
                Native.dart_schema_message_default(s, p, (UIntPtr)buf.Length);
                foreach (var op in ops) ExecuteOp(s, p, (UIntPtr)buf.Length, op);
                uint n = Native.dart_schema_msg_len(s, p, (UIntPtr)buf.Length);
                if (n == buf.Length) return buf;
                byte[] outb = new byte[n];
                Array.Copy(buf, outb, n);
                return outb;
            }
            finally { gh.Free(); }
        }

        // a bare type root: one op on the empty path, the schema's single anonymous field
        private static void CollectRootValue(IntPtr s, object value, List<SetOp> ops, ref long varBytes)
        {
            DartSchemaFieldInfo info;
            if (value == null || Native.dart_schema_field_at(s, 0, out info) == 0) return;
            var op = new SetOp { Cpath = CStr(""), Path = "", Kind = info.kind, Elem = info.elem,
                                 Count = info.count, StrCap = info.str_cap };
            PrepareOp(op, value, ref varBytes);
            ops.Add(op);
        }

        // a reflected object to ops, recursing the type spec with dotted paths for nested structs
        private static void Collect(IntPtr s, TypeSpec spec, object obj, string prefix,
                                    List<SetOp> ops, ref long varBytes)
        {
            foreach (var fp in spec.Fields)
            {
                object val = fp.Field.GetValue(obj);
                if (val == null) continue;   // keep the zeroed default from message_default
                string path = prefix + fp.WireName;
                if (fp.Kind == STRUCT) { Collect(s, Spec(fp.Nested), val, path + ".", ops, ref varBytes); continue; }
                var op = new SetOp { Cpath = CStr(path), Path = path, Kind = fp.Kind, Elem = fp.Elem,
                                     Count = fp.Count, StrCap = fp.StrCap };
                PrepareOp(op, val, ref varBytes);
                ops.Add(op);
            }
        }

        // a Dictionary source to ops: walk the compiled schema's flat field table pulling values
        // by name, nested structs from a nested dictionary of the same shape
        private static void CollectFromDict(IntPtr s, System.Collections.IDictionary root,
                                            List<SetOp> ops, ref long varBytes)
        {
            var names = new List<string>();
            var srcs = new List<System.Collections.IDictionary> { root };
            ushort n = Native.dart_schema_field_count(s);
            for (ushort i = 0; i < n; i++)
            {
                DartSchemaFieldInfo info;
                Native.dart_schema_field_at(s, i, out info);
                string name = Str(info.name);
                int d = info.depth;
                while (names.Count <= d) names.Add(null);
                names[d] = name;
                var parent = d < srcs.Count ? srcs[d] : null;
                object val = (parent != null && parent.Contains(name)) ? parent[name] : null;
                if (info.kind == STRUCT)
                {
                    while (srcs.Count <= d + 1) srcs.Add(null);
                    srcs[d + 1] = val as System.Collections.IDictionary;
                    continue;
                }
                if (val == null) continue;
                string path = string.Join(".", names.GetRange(0, d + 1));
                var op = new SetOp { Cpath = CStr(path), Path = path, Kind = info.kind, Elem = info.elem,
                                     Count = info.count, StrCap = info.str_cap };
                PrepareOp(op, val, ref varBytes);
                ops.Add(op);
            }
        }

        // Pre serialize a field's variable payload for sizing. Fixed fields keep the raw value.
        private static void PrepareOp(SetOp op, object val, ref long varBytes)
        {
            if (op.Kind == VSTR) { op.Prepared = Encoding.UTF8.GetBytes(val as string ?? ""); varBytes += op.Prepared.Length; }
            else if (op.Kind == VARR && op.Elem == STR) { op.Prepared = PackStringSlots(AsStringArray(val), op.StrCap); varBytes += op.Prepared.Length; }
            else if (op.Kind == VARR) { op.Prepared = PackArray(op.Elem, val); varBytes += op.Prepared.Length; }
            else if (op.Kind == MAP) { op.Prepared = EncodeMapBody(val); varBytes += op.Prepared.Length; }
            else op.Value = val;
        }

        private static string[] AsStringArray(object val)
        {
            if (val is string[] sa) return sa;
            if (val is System.Collections.IEnumerable en)
            {
                var list = new List<string>();
                foreach (var x in en) list.Add(x?.ToString());
                return list.ToArray();
            }
            return Array.Empty<string>();
        }

        private static void ExecuteOp(IntPtr s, IntPtr buf, UIntPtr cap, SetOp op)
        {
            byte[] cpath = op.Cpath;
            if (op.Kind == STR)
            {
                if (!SetString(s, buf, cap, cpath, (string)op.Value))
                    throw new SchemaException("string too long for " + op.Path + " (cap " + op.StrCap + ")");
            }
            else if (op.Kind == VSTR) SetBytesString(s, buf, cap, cpath, op.Prepared);
            else if (op.Kind == ARR && op.Elem == STR)
            {
                var strs = AsStringArray(op.Value);
                for (ushort i = 0; i < strs.Length && i < op.Count; i++)
                    if (!SetStringAt(s, buf, cap, cpath, i, strs[i]))
                        throw new SchemaException("string too long for " + op.Path + "[" + i + "] (cap " + op.StrCap + ")");
            }
            else if (op.Kind == ARR) SetArrayBytes(s, buf, cap, cpath, PackArray(op.Elem, op.Value));
            else if (op.Kind == VARR) SetArrayBytes(s, buf, cap, cpath, op.Prepared);
            else if (op.Kind == MAP)
            {
                if (!SetMapBytes(s, buf, cap, cpath, op.Prepared))
                    throw new SchemaException("invalid map for " + op.Path);
            }
            else if (op.Kind == F32) Native.dart_set_f32(buf, cap, s, cpath, Convert.ToSingle(op.Value));
            else if (op.Kind == F64) Native.dart_set_f64(buf, cap, s, cpath, Convert.ToDouble(op.Value));
            else if (op.Kind == ENUM)   // the backing integer, signed or unsigned per Elem
            {
                object raw = op.Value is Enum ? Convert.ChangeType(op.Value, Enum.GetUnderlyingType(op.Value.GetType())) : op.Value;
                if (op.Elem >= I8 && op.Elem <= I64) Native.dart_set_int(buf, cap, s, cpath, Convert.ToInt64(raw));
                else Native.dart_set_uint(buf, cap, s, cpath, Convert.ToUInt64(raw));
            }
            else if (op.Kind >= I8 && op.Kind <= I64) Native.dart_set_int(buf, cap, s, cpath, Convert.ToInt64(op.Value));
            else if (op.Kind == BOOL) Native.dart_set_uint(buf, cap, s, cpath, (bool)op.Value ? 1UL : 0UL);
            else Native.dart_set_uint(buf, cap, s, cpath, Convert.ToUInt64(op.Value));
        }

        private static bool SetString(IntPtr s, IntPtr buf, UIntPtr cap, byte[] cpath, string v)
        {
            byte[] b = Encoding.UTF8.GetBytes(v ?? "");
            GCHandle gh = GCHandle.Alloc(b, GCHandleType.Pinned);
            try
            {
                var ds = new DartStringView { data = gh.AddrOfPinnedObject(), len = (UIntPtr)b.Length };
                return Native.dart_set_string(buf, cap, s, cpath, ds) != 0;
            }
            finally { gh.Free(); }
        }

        private static bool SetStringAt(IntPtr s, IntPtr buf, UIntPtr cap, byte[] cpath, ushort index, string v)
        {
            byte[] b = Encoding.UTF8.GetBytes(v ?? "");
            GCHandle gh = GCHandle.Alloc(b, GCHandleType.Pinned);
            try
            {
                var ds = new DartStringView { data = gh.AddrOfPinnedObject(), len = (UIntPtr)b.Length };
                return Native.dart_set_string_at(buf, cap, s, cpath, index, ds) != 0;
            }
            finally { gh.Free(); }
        }

        // Set a variable string / array / map frame from pre-serialized payload bytes.
        private static bool SetBytesString(IntPtr s, IntPtr buf, UIntPtr cap, byte[] cpath, byte[] b)
        {
            GCHandle gh = GCHandle.Alloc(b, GCHandleType.Pinned);
            try
            {
                var ds = new DartStringView { data = b.Length > 0 ? gh.AddrOfPinnedObject() : IntPtr.Zero, len = (UIntPtr)b.Length };
                return Native.dart_set_string(buf, cap, s, cpath, ds) != 0;
            }
            finally { gh.Free(); }
        }

        private static void SetArrayBytes(IntPtr s, IntPtr buf, UIntPtr cap, byte[] cpath, byte[] b)
        {
            GCHandle gh = GCHandle.Alloc(b, GCHandleType.Pinned);
            try
            {
                var db = new DartBytes { data = b.Length > 0 ? gh.AddrOfPinnedObject() : IntPtr.Zero, len = (UIntPtr)b.Length };
                Native.dart_set_array(buf, cap, s, cpath, db);
            }
            finally { gh.Free(); }
        }

        private static bool SetMapBytes(IntPtr s, IntPtr buf, UIntPtr cap, byte[] cpath, byte[] b)
        {
            GCHandle gh = GCHandle.Alloc(b, GCHandleType.Pinned);
            try
            {
                var db = new DartBytes { data = b.Length > 0 ? gh.AddrOfPinnedObject() : IntPtr.Zero, len = (UIntPtr)b.Length };
                return Native.dart_set_map(buf, cap, s, cpath, db) != 0;
            }
            finally { gh.Free(); }
        }

        // A variable string array's frame: whole [u16 len][cap bytes] slots, the layout
        // UnpackStringArray reads back, one per element. The live count is the element count.
        private static byte[] PackStringSlots(string[] strings, int cap)
        {
            if (strings == null) return Array.Empty<byte>();
            int slot = 2 + cap;
            byte[] outb = new byte[strings.Length * slot];
            for (int i = 0; i < strings.Length; i++)
            {
                byte[] sb = Encoding.UTF8.GetBytes(strings[i] ?? "");
                if (sb.Length > cap)
                    throw new SchemaException("string too long (cap " + cap + "): " + strings[i]);
                outb[i * slot] = (byte)(sb.Length & 0xff);
                outb[i * slot + 1] = (byte)(sb.Length >> 8);
                Buffer.BlockCopy(sb, 0, outb, i * slot + 2, sb.Length);
            }
            return outb;
        }

        // The map body, the self describing tagged value tree of a map field, built and parsed
        // here rather than mirroring the C writer. spec/schema.md has the wire, the C validates.
        private static void WriteLE(System.IO.MemoryStream m, ulong v, int nbytes)
        {
            for (int i = 0; i < nbytes; i++) m.WriteByte((byte)(v >> (8 * i)));
        }

        private static byte[] EncodeMapBody(object val)
        {
            if (!(val is System.Collections.IDictionary dict))
                throw new SchemaException("map field expects a Dictionary, got " + (val?.GetType().Name ?? "null"));
            var entries = new System.IO.MemoryStream();
            int n = 0;
            foreach (System.Collections.DictionaryEntry e in dict)
            {
                if (e.Value == null) continue;   // a map has no null kind, omit the key
                byte[] kb = Encoding.UTF8.GetBytes(e.Key.ToString());
                if (kb.Length > 255) throw new SchemaException("map key too long (max 255 bytes): " + e.Key);
                entries.WriteByte((byte)kb.Length);
                entries.Write(kb, 0, kb.Length);
                EncodeMapValue(entries, e.Value);
                n++;
            }
            var ms = new System.IO.MemoryStream();
            WriteLE(ms, (ulong)n, 2);
            entries.WriteTo(ms);
            return ms.ToArray();
        }

        private static void EncodeMapValue(System.IO.MemoryStream m, object v)
        {
            if (v is bool b) { m.WriteByte(BOOL); m.WriteByte((byte)(b ? 1 : 0)); return; }
            if (v is string s) { byte[] sb = Encoding.UTF8.GetBytes(s); m.WriteByte(VSTR); WriteLE(m, (ulong)sb.Length, 2); m.Write(sb, 0, sb.Length); return; }
            if (v is System.Collections.IDictionary) { m.WriteByte(MAP); byte[] body = EncodeMapBody(v); m.Write(body, 0, body.Length); return; }
            if (v is float f) { m.WriteByte(F64); WriteLE(m, (ulong)BitConverter.DoubleToInt64Bits(f), 8); return; }
            if (v is double d) { m.WriteByte(F64); WriteLE(m, (ulong)BitConverter.DoubleToInt64Bits(d), 8); return; }
            if (v is System.Collections.IEnumerable en)   // any array / list (string handled above)
            {
                var items = new List<object>();
                foreach (var x in en) items.Add(x);
                m.WriteByte(VARR); WriteLE(m, (ulong)items.Count, 2);
                foreach (var x in items) EncodeMapValue(m, x);
                return;
            }
            WriteMapInt(m, v);   // integral scalar in the smallest kind that fits
        }

        private static void WriteMapInt(System.IO.MemoryStream m, object v)
        {
            byte kind; int nbytes; ulong bits;
            if (v is ulong uu)
            {
                if (uu <= 0xffUL) { kind = U8; nbytes = 1; }
                else if (uu <= 0xffffUL) { kind = U16; nbytes = 2; }
                else if (uu <= 0xffffffffUL) { kind = U32; nbytes = 4; }
                else { kind = U64; nbytes = 8; }
                bits = uu;
            }
            else
            {
                long x;
                try { x = Convert.ToInt64(v); }
                catch (Exception) { throw new SchemaException("map value type not supported: " + v.GetType().Name); }
                if (x >= 0)
                {
                    if (x <= 0xff) { kind = U8; nbytes = 1; }
                    else if (x <= 0xffff) { kind = U16; nbytes = 2; }
                    else if (x <= 0xffffffffL) { kind = U32; nbytes = 4; }
                    else { kind = U64; nbytes = 8; }
                }
                else
                {
                    if (x >= -0x80L) { kind = I8; nbytes = 1; }
                    else if (x >= -0x8000L) { kind = I16; nbytes = 2; }
                    else if (x >= -0x80000000L) { kind = I32; nbytes = 4; }
                    else { kind = I64; nbytes = 8; }
                }
                bits = (ulong)x;   // two's-complement low bytes
            }
            m.WriteByte(kind);
            WriteLE(m, bits, nbytes);
        }

        private static byte[] PackArray(byte elem, object val)
        {
            int esz = ScalarSize[elem];
            if ((elem == U8 || elem == I8) && val is byte[] u8) return u8;   // raw-blob fast path
            // fast path: a primitive array whose element width already matches the wire
            if (val is Array a && a.GetType().GetElementType() is Type et && et.IsPrimitive
                && et != typeof(bool) && System.Runtime.InteropServices.Marshal.SizeOf(et) == esz)
            {
                byte[] outb = new byte[a.Length * esz];
                Buffer.BlockCopy(a, 0, outb, 0, outb.Length);
                return outb;
            }
            // general: any enumerable of numbers (dict-source lists, mismatched widths),
            // each converted to the wire element kind, little-endian
            var items = new List<object>();
            if (val is System.Collections.IEnumerable en) foreach (var x in en) items.Add(x);
            byte[] r = new byte[items.Count * esz];
            for (int i = 0; i < items.Count; i++) WriteScalarLE(r, i * esz, elem, items[i]);
            return r;
        }

        // Write one scalar of the given kind at buf[off..], little-endian, converting v.
        private static void WriteScalarLE(byte[] buf, int off, byte kind, object v)
        {
            byte[] b;
            switch (kind)
            {
                case U8:  buf[off] = (byte)Convert.ToUInt64(v); return;
                case I8:  buf[off] = unchecked((byte)(sbyte)Convert.ToInt64(v)); return;
                case BOOL: buf[off] = (byte)(Convert.ToBoolean(v) ? 1 : 0); return;
                case U16: b = BitConverter.GetBytes((ushort)Convert.ToUInt64(v)); break;
                case I16: b = BitConverter.GetBytes((short)Convert.ToInt64(v)); break;
                case U32: b = BitConverter.GetBytes((uint)Convert.ToUInt64(v)); break;
                case I32: b = BitConverter.GetBytes((int)Convert.ToInt64(v)); break;
                case U64: b = BitConverter.GetBytes(Convert.ToUInt64(v)); break;
                case I64: b = BitConverter.GetBytes(Convert.ToInt64(v)); break;
                case F32: b = BitConverter.GetBytes(Convert.ToSingle(v)); break;
                case F64: b = BitConverter.GetBytes(Convert.ToDouble(v)); break;
                default: return;
            }
            if (!BitConverter.IsLittleEndian) Array.Reverse(b);
            Buffer.BlockCopy(b, 0, buf, off, b.Length);
        }

        // decode: message bytes to a nested dict or a typed object
        internal static Dictionary<string, object> DecodeDict(IntPtr s, byte[] data)
        {
            GCHandle gh = GCHandle.Alloc(data, GCHandleType.Pinned);
            try
            {
                var mb = new DartBytes
                {
                    data = data.Length > 0 ? gh.AddrOfPinnedObject() : IntPtr.Zero,
                    len = (UIntPtr)data.Length
                };
                var root = new Dictionary<string, object>();
                var dests = new List<Dictionary<string, object>> { root };
                ushort n = Native.dart_schema_field_count(s);
                for (ushort i = 0; i < n; i++)
                {
                    DartSchemaFieldInfo info;
                    Native.dart_schema_field_at(s, i, out info);
                    string name = Str(info.name);
                    int d = info.depth;
                    var parent = dests[d];
                    if (info.kind == STRUCT)
                    {
                        var child = new Dictionary<string, object>();
                        parent[name] = child;
                        while (dests.Count <= d + 1) dests.Add(null);
                        dests[d + 1] = child;
                    }
                    else
                    {
                        DartValue v;
                        Native.dart_get_value(mb, s, i, out v);
                        parent[name] = ValueToObj(v);
                    }
                }
                return root;
            }
            finally { gh.Free(); }
        }

        private static object ValueToObj(DartValue v)
        {
            if (v.kind == STR || v.kind == VSTR)
                return Encoding.UTF8.GetString(Bytes(v.bytes));
            if (v.kind == ARR || v.kind == VARR)
            {
                byte[] raw = v.bytes.data != IntPtr.Zero && (ulong)v.bytes.len > 0
                    ? Bytes(v.bytes) : Array.Empty<byte>();
                if (v.elem == STR) return UnpackStringArray(raw, v.count, v.str_cap);
                return UnpackArray(v.elem, raw);
            }
            if (v.kind == MAP)
            {
                byte[] raw = v.bytes.data != IntPtr.Zero && (ulong)v.bytes.len > 0
                    ? Bytes(v.bytes) : Array.Empty<byte>();
                int off = 0;
                return DecodeMapBody(raw, ref off, raw.Length, 1);
            }
            if (v.kind == F32 || v.kind == F64) return v.v.f;
            if (v.kind == ENUM) return v.v.i;   // the number. ToObject casts it to the enum type
            if (v.kind >= I8 && v.kind <= I64) return v.v.i;
            if (v.kind == BOOL) return v.v.u != 0;
            return v.v.u;
        }

        private static ulong ReadLE(byte[] buf, int off, int n)
        {
            ulong r = 0;
            for (int i = 0; i < n; i++) r |= (ulong)buf[off + i] << (8 * i);
            return r;
        }

        // Parse a map body (see EncodeMapBody) to a Dictionary. Fully bounds-checked and
        // tolerant: a short/hostile body never over-reads, it just stops early.
        private static Dictionary<string, object> DecodeMapBody(byte[] buf, ref int off, int end, int depth)
        {
            var outd = new Dictionary<string, object>();
            if (depth > 8 || off + 2 > end) { off = end; return outd; }   // DART_SCHEMA_MAX_DEPTH
            int n = (int)ReadLE(buf, off, 2); off += 2;
            for (int e = 0; e < n; e++)
            {
                if (off >= end) break;
                int klen = buf[off]; off += 1;
                if (off + klen > end) break;
                string key = Encoding.UTF8.GetString(buf, off, klen); off += klen;
                outd[key] = DecodeMapValue(buf, ref off, end, depth);
            }
            return outd;
        }

        private static object DecodeMapValue(byte[] buf, ref int off, int end, int depth)
        {
            if (off >= end) return null;
            byte kind = buf[off]; off += 1;
            switch (kind)
            {
                case U8:  if (off + 1 > end) { off = end; return 0UL; } { ulong r = buf[off]; off += 1; return r; }
                case U16: if (off + 2 > end) { off = end; return 0UL; } { ulong r = ReadLE(buf, off, 2); off += 2; return r; }
                case U32: if (off + 4 > end) { off = end; return 0UL; } { ulong r = ReadLE(buf, off, 4); off += 4; return r; }
                case U64: if (off + 8 > end) { off = end; return 0UL; } { ulong r = ReadLE(buf, off, 8); off += 8; return r; }
                case I8:  if (off + 1 > end) { off = end; return 0L; }  { long r = (sbyte)buf[off]; off += 1; return r; }
                case I16: if (off + 2 > end) { off = end; return 0L; }  { long r = (short)(ushort)ReadLE(buf, off, 2); off += 2; return r; }
                case I32: if (off + 4 > end) { off = end; return 0L; }  { long r = (int)(uint)ReadLE(buf, off, 4); off += 4; return r; }
                case I64: if (off + 8 > end) { off = end; return 0L; }  { long r = (long)ReadLE(buf, off, 8); off += 8; return r; }
                case F32: if (off + 4 > end) { off = end; return 0.0; } { uint bits = (uint)ReadLE(buf, off, 4); off += 4; return (double)BitConverter.ToSingle(BitConverter.GetBytes(bits), 0); }
                case F64: if (off + 8 > end) { off = end; return 0.0; } { double r = BitConverter.Int64BitsToDouble((long)ReadLE(buf, off, 8)); off += 8; return r; }
                case BOOL: if (off + 1 > end) { off = end; return false; } { bool b = buf[off] != 0; off += 1; return b; }
                case VSTR:
                {
                    if (off + 2 > end) { off = end; return ""; }
                    int ln = (int)ReadLE(buf, off, 2); off += 2;
                    if (off + ln > end) ln = end - off;
                    string sres = Encoding.UTF8.GetString(buf, off, ln); off += ln; return sres;
                }
                case VARR:
                {
                    if (off + 2 > end) { off = end; return new List<object>(); }
                    int cnt = (int)ReadLE(buf, off, 2); off += 2;
                    var lst = new List<object>();
                    for (int i = 0; i < cnt && off < end; i++) lst.Add(DecodeMapValue(buf, ref off, end, depth));
                    return lst;
                }
                case MAP: return DecodeMapBody(buf, ref off, end, depth + 1);
                default: off = end; return null;
            }
        }

        // string array slots are [u16 len][cap bytes] each. Clamp len like the C reader so a
        // hostile message can never over read
        private static string[] UnpackStringArray(byte[] raw, int count, int cap)
        {
            var strs = new string[count];
            int slot = 2 + cap;
            for (int i = 0; i < count; i++)
            {
                int off = i * slot;
                if (off + 2 > raw.Length) { strs[i] = ""; continue; }
                int len = raw[off] | (raw[off + 1] << 8);
                if (len > cap) len = cap;
                if (off + 2 + len > raw.Length) len = raw.Length - off - 2;
                strs[i] = Encoding.UTF8.GetString(raw, off + 2, len);
            }
            return strs;
        }

        private static object UnpackArray(byte elem, byte[] raw)
        {
            if (elem == U8) return raw;
            int esz = ScalarSize[elem];
            int cnt = esz > 0 ? raw.Length / esz : 0;
            Array arr;
            switch (elem)
            {
                case I8: arr = new sbyte[cnt]; break;
                case U16: arr = new ushort[cnt]; break;
                case I16: arr = new short[cnt]; break;
                case U32: arr = new uint[cnt]; break;
                case I32: arr = new int[cnt]; break;
                case U64: arr = new ulong[cnt]; break;
                case I64: arr = new long[cnt]; break;
                case F32: arr = new float[cnt]; break;
                case F64: arr = new double[cnt]; break;
                case BOOL: arr = new byte[cnt]; break;
                default: return raw;
            }
            Buffer.BlockCopy(raw, 0, arr, 0, cnt * esz);
            return arr;
        }

        // The bare value out of a decoded bare-type message, coerced to T (an enum member, a
        // typed array, a string, a Dictionary, or a widened scalar).
        internal static object RootValue(Type t, object val)
        {
            if (val == null) return null;
            if (t.IsEnum) return Enum.ToObject(t, val);
            if (t.IsArray || t == typeof(string) || IsMapType(t)) return val;
            return Convert.ChangeType(val, t);
        }

        // What a decoded bare-type message is worth without a CLR type: the value itself.
        internal static object RootOrFields(IntPtr s, Dictionary<string, object> fields)
        {
            object v;
            return (IsValueRoot(s) && fields != null && fields.TryGetValue("", out v)) ? v : fields;
        }

        internal static object ToObject(Type t, Dictionary<string, object> dict)
        {
            var spec = Spec(t);
            if (spec.Name == null)          // a bare type: the whole message is one value
            {
                object rv;
                dict.TryGetValue("", out rv);
                return RootValue(t, rv);
            }
            object obj = Activator.CreateInstance(t);
            foreach (var fp in spec.Fields)
            {
                object val;
                if (!dict.TryGetValue(fp.WireName, out val) || val == null) continue;
                object set;
                if (fp.Kind == STRUCT) set = ToObject(fp.Nested, (Dictionary<string, object>)val);
                else if (fp.Kind == ARR || fp.Kind == VARR || fp.Kind == STR || fp.Kind == VSTR
                         || fp.Kind == MAP) set = val;   // already string / typed array / dict
                else if (fp.Kind == ENUM) set = Enum.ToObject(fp.Field.FieldType, val);
                else set = Convert.ChangeType(val, fp.Field.FieldType);
                fp.Field.SetValue(obj, set);
            }
            return obj;
        }
    }
}
