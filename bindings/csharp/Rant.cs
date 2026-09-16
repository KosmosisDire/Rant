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

namespace Rant
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
    internal enum Role { PubSub = 0, PubOnly = 1, SubOnly = 2, Inactive = 3 }
    public enum SendStatus
    {
        Ok = 0, NoTopic = -1, TooBig = -2, BadRole = -3, OutOfMemory = -4,
        State = -5,   // wrong state: Poll while started, or a call a handler may not make
        NoSys = -6,   // not compiled in (Start under RANT_NO_THREADS)
        Schema = -7   // the payload is not a message of the topic's schema
    }

    public enum EventKind
    {
        PeerUp = 0, PeerDown, PeerInterest, MessageLost, Error
    }

    // The error carried by an EventKind.Error event, RantEvent.Error and RantNode.LastError.
    public enum ErrorKind
    {
        None = 0,
        NameCollision, QosIncompatible, KindMismatch, SchemaMismatch, InterestOverflow,
        MetaTruncatedInterest, MetaTruncatedSchema, PeerMetaTooBig, MessageTooBig,
        PeerRefused, EvictedUnsent, UnmatchedSend, DuplicateAuthority,
        Oom, Platform, Socket, Bind, McastJoin, Send, Recv, Poll, Waker, BadAddress,
        BadName, State, BadSchema   // a create refused: the name, the moment, the schema
    }

    // Schema field kinds for reflection, the value is the wire kind byte. Named is a nominal
    // tag reflection unwraps into Field.TypeName, so a field never reports it as its Kind.
    public enum FieldType : byte
    {
        U8 = 0, U16, U32, U64, I8, I16, I32, I64, F32, F64, Bool, Array, Struct, String,
        VString, VArray, Map, Enum, Named
    }

    // A call's outcome, mirrors RantCallStatus. Timeout, PeerLost and NoProvider are
    // synthesized on the caller, and Cancelled also for calls still pending when the node closes.
    public enum CallStatus
    {
        Ok = 0, AppError = 1, NoHandler = 2, Timeout = 3, PeerLost = 4, Cancelled = 5,
        Running = 6,  // task, the one NON-terminal status: accepted and running
        NoProvider = 7   // the timeout passed with no definition ever matched
    }

    // Severity of a built-in @rant/log line. Mirrors RantLogLevel.
    public enum LogLevel { Error = 0, Warn = 1, Info = 2 }

    // A @rant/meta request's section mask, OR the bits. 0 = every section. Mirrors RANT_META_*.
    [Flags]
    public enum MetaSection : uint { Node = 0x1, Proc = 0x2, Topics = 0x4, Peers = 0x8, All = 0 }

    // Reflection: a peer's liveness and an entity's kind, mirrors RantPeerLiveness and
    // RantEntityKind. A dropped peer is still listed, gate on Active.
    public enum PeerLiveness { Active = 0, Dropped = 1 }
    public enum EntityKind { Topic = 0, Function = 1, Variable = 2, Task = 3 }

    // ---- native struct layouts (mirror the C exactly) ---------------------------

    [StructLayout(LayoutKind.Sequential)]
    internal struct RantBytes { public IntPtr data; public UIntPtr len; }

    // the C RantString, a length carrying view. Named View so the [RantString] attribute
    // owns the public name
    [StructLayout(LayoutKind.Sequential)]
    internal struct RantStringView { public IntPtr data; public UIntPtr len; }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RantQos
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
    internal struct RantTopicOpts { public RantQos qos; public byte reflect_from_mesh; }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RantNodeNet
    {
        public ushort data_port;
        public IntPtr discovery_group;         // const char*
        public ushort discovery_port;
        public IntPtr multicast_interface;     // const char*
        public byte multicast_ttl;
        public IntPtr seed_peers;              // const RantDiscoveryAddr*
        public ushort n_seed_peers;
        public byte unicast_only;
        public uint recv_buffer_bytes;
        public uint send_buffer_bytes;
        public ushort fragment_size;
        public IntPtr self_ip;                 // const char*
        public ushort advertise_port;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RantDiscoveryAddr
    {
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)] public byte[] ip;
        public byte ip_len;                    // 4 = IPv4, 16 = IPv6
        public ushort port;                    // host order, 0 = the discovery port
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RantNodeDiscovery
    {
        public uint announce_interval_us;
        public uint peer_timeout_us;
        public ushort max_peers;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RantNodeOpts
    {
        public ushort domain;
        public ushort max_topics;
        public IntPtr user_data;
        public byte disable_shm;
        public byte fetch_details;
        public int match_wait_ms;              // send path match wait, 0 = 1 s, negative = off
        public byte disable_logs;              // strip the built-in @rant/log topics
        public byte disable_meta;              // do not host the @rant/meta endpoint
        public byte disable_error_logs;   // no error mirroring onto @rant/log/error
        public RantNodeNet net;
        public RantNodeDiscovery discovery;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RantMsg
    {
        public IntPtr node;
        public IntPtr user;
        public ushort topic_index;
        public uint publisher_id;
        public RantStringView publisher_name;
        public RantStringView topic_name;
        public RantBytes header;     // the pattern header view, null on a plain topic
        public RantBytes data;
        public IntPtr schema;
        public ulong recv_us;
        public ulong written_us;
        public ulong capture_us;
    }

    // Optional per send config. capture_us 0 = unstated, and costs no wire bytes.
    [StructLayout(LayoutKind.Sequential)]
    internal struct RantSendOpts
    {
        public ulong capture_us;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RantEventNative
    {
        public int kind;
        public int error;                      // RantErrorKind (Error events)
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
    internal struct RantAllocator
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
    internal struct RantSchemaFieldInfo
    {
        public RantStringView name;
        public RantStringView type_name;     // the field type's NAME, empty when anonymous
        public RantStringView elem_name;     // an array ELEMENT type's name, empty when anonymous
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
    internal struct RantValueUnion
    {
        [FieldOffset(0)] public ulong u;
        [FieldOffset(0)] public long i;
        [FieldOffset(0)] public double f;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RantValue
    {
        public byte kind;
        public byte elem;
        public ushort count;
        public ushort str_cap;
        public RantValueUnion v;
        public RantBytes bytes;
    }

    // the reflection mirrors of src/node/core.h, field order and types exact
    [StructLayout(LayoutKind.Sequential)]
    internal struct RantIter { public uint a, b; public ushort c, d; }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RantPeerInfoNative
    {
        public uint id;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)] public byte[] uuid;
        public RantStringView name;
        public RantStringView address;
        public int liveness;
        public ulong last_heard_us;
        public uint epoch;
        public byte catching_up;
        public ushort fragment_size;
        public uint rtt_us;
        public uint rtt_jitter_us;
        public uint rtt_min_us;
        public uint rtt_samples;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RantEntityInfoNative
    {
        public int kind;
        public RantStringView name;
        public uint hash;
        public byte provides, consumes, reliable, writable, forceable, cancellable, exclusive, multi,
                    incomplete, conflict;
        public ushort providers;
        public ushort consumers;
        public uint provider;
        public RantStringView from;
        public IntPtr schema;
        public ulong schema_hash;
        public IntPtr rsp_schema;
        public ulong rsp_schema_hash;
        public IntPtr progress_schema;
        public ulong progress_schema_hash;
        public ulong generation;
    }

    // the pattern struct mirrors of src/patterns/core.h, field order and types exact

    // The public head of the C RequestCore, only ever read through the callback's pointer:
    // the reply machinery lives behind the struct, so the exact pointer is what reply takes.
    [StructLayout(LayoutKind.Sequential)]
    internal struct RantRequestNative
    {
        public IntPtr node;
        public RantStringView function_name;
        public RantBytes data;
        public IntPtr schema;                  // const RantSchema*
        public uint caller;
        public RantStringView caller_name;
        public ulong recv_us;
        public ulong written_us;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RantResponseNative
    {
        public int status;                     // RantCallStatus
        public RantBytes data;
        public IntPtr schema;                  // const RantSchema*
        public uint provider;
        public IntPtr user;
        public ulong written_us;
        public RantStringView message;           // outcome text (default status text if none sent)
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RantFunctionOpts
    {
        public uint backpressure_wait_us;
        public uint timeout_us;
        public ushort keep_last;        // req and rsp ring depth, 0 = 10
        public byte reflect_from_mesh;
        public byte multi;              // duplicate-authority diagnostic suppressed (@rant/meta)
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RantVariableOpts
    {
        public RantBytes initial;
        public byte access;                    // RantVarAccess
        public byte allow_force;
        public ushort catch_up;
        public ushort keep_last;
        public uint backpressure_wait_us;
        public byte reflect_from_mesh;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RantVariableUpdateNative
    {
        public IntPtr variable;
        public RantStringView name;
        public RantBytes value;
        public IntPtr schema;
        public byte forced;
        public uint write_seq;
        public uint source;
        public ulong recv_us;
        public ulong written_us;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RantCallOpts
    {
        public uint provider;        // direct a call at one definition by peer id (0 = undirected)
        public IntPtr on_progress;   // RantProgressFn for task calls, null = updates discarded
        public IntPtr progress_user; // handed back as RantProgress.user
        public IntPtr id_out;        // uint32_t*: filled with the call id at commit
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RantTaskOpts
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
    internal struct RantProgressNative
    {
        public uint call_id;
        public uint provider;
        public RantBytes data;
        public IntPtr schema;                  // const RantSchema*
        public ulong written_us;
        public ulong recv_us;
        public IntPtr user;                    // RantCallOpts.progress_user
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void RantMsgFn(IntPtr msg);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void RantEventFn(IntPtr ev);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate IntPtr RantAllocFn(IntPtr user, IntPtr ptr, UIntPtr size);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void RantRequestFn(IntPtr request, IntPtr user);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void RantResponseFn(IntPtr response);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void RantVariableUpdateFn(IntPtr update, IntPtr user);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void RantProgressFn(IntPtr progress);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void RantCancelFn(ulong token, IntPtr user);

    // ---- native entry points ----------------------------------------------------

    internal static class Native
    {
        internal const string LIB = "rant";
        private const CallingConvention CC = CallingConvention.Cdecl;

        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr rant_node_open(ref RantAllocator alloc, byte[] name,
            RantMsgFn on_message, RantEventFn on_event, ref RantNodeOpts opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern RantEventNative rant_last_error(IntPtr node);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_node_poll(IntPtr node, int timeout_ms);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_node_close(IntPtr node, int send_bye);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_node_start(IntPtr node);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_node_stop(IntPtr node);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_node_is_started(IntPtr node);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern uint rant_node_evicted_unsent(IntPtr node);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr rant_node_create_topic(IntPtr node, byte[] name, int role,
            IntPtr schema, ref RantTopicOpts opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr rant_node_topic(IntPtr node, ushort index);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_topic_send(IntPtr ch, RantBytes data,
                                                   ref RantSendOpts opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_topic_set_role(IntPtr ch, int role);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_topic_retire(IntPtr ch);

        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_topic_refresh(IntPtr ch);

        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr rant_topic_schema(IntPtr ch);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern ushort rant_topic_index(IntPtr ch);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_topic_match_count(IntPtr ch);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_topic_drain(IntPtr ch, int timeout_ms);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_topic_take(IntPtr ch, ref RantMsg msg, int timeout_ms);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_topic_dispatch(IntPtr ch, int max_msgs, int timeout_ms);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_node_dispatch(IntPtr node, int max_msgs, int timeout_ms);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern void rant_topic_queue_stats(IntPtr ch, out uint msgs,
            out uint bytes, out uint capacity, out uint dropped);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern void rant_node_mem_stats(IntPtr node, out UIntPtr in_use,
            out UIntPtr peak, out ulong alloc_calls);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern void rant_node_backpressure_stats(IntPtr node, out ulong waited_us,
            out uint waited_sends);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern void rant_topic_counts(IntPtr ch, out ulong tx_msgs, out ulong tx_bytes,
            out ulong rx_msgs, out ulong rx_bytes);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_node_log_text(IntPtr node, int level, byte[] text, int len);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr rant_node_log_topic(IntPtr node, int level);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr rant_node_meta_function(IntPtr node);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr rant_event_str(IntPtr ev, byte[] buf, UIntPtr cap);

        // serialize / schema
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr rant_schema_compile(RantAllocFn alloc, IntPtr user,
            byte[] text, out IntPtr err);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern void rant_schema_free(IntPtr s, RantAllocFn alloc, IntPtr user);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern RantBytes rant_schema_wire(IntPtr s);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern ulong rant_schema_hash(IntPtr s);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr rant_schema_copy(IntPtr s, RantAllocFn alloc, IntPtr user);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern RantStringView rant_schema_name(IntPtr s);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern uint rant_schema_print(IntPtr s, IntPtr buf, UIntPtr cap);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_schema_subset(IntPtr sub, IntPtr pub);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_std_recognize(IntPtr s, RantAllocFn alloc, IntPtr user);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_std_recognize_field(IntPtr s, ushort field,
                                                            RantAllocFn alloc, IntPtr user);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern long rant_timestamp_now();
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern uint rant_schema_size(IntPtr s);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern ushort rant_schema_field_count(IntPtr s);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_schema_field_at(IntPtr s, ushort i, out RantSchemaFieldInfo info);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern ushort rant_schema_enum_count(IntPtr s, ushort field);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_schema_enum_variant(IntPtr s, ushort field, ushort i,
            out long value, out RantStringView name);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_schema_message_default(IntPtr s, IntPtr buf, UIntPtr cap);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_set_uint(IntPtr buf, UIntPtr cap, IntPtr s, byte[] field, ulong v);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_set_int(IntPtr buf, UIntPtr cap, IntPtr s, byte[] field, long v);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_set_f64(IntPtr buf, UIntPtr cap, IntPtr s, byte[] field, double v);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_set_f32(IntPtr buf, UIntPtr cap, IntPtr s, byte[] field, float v);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_set_array(IntPtr buf, UIntPtr cap, IntPtr s, byte[] field, RantBytes elems);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_set_string(IntPtr buf, UIntPtr cap, IntPtr s, byte[] field, RantStringView v);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_set_string_at(IntPtr buf, UIntPtr cap, IntPtr s, byte[] field,
            ushort index, RantStringView v);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_get_value(RantBytes msg, IntPtr s, ushort field, out RantValue outv);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern uint rant_schema_msg_min(IntPtr s);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern uint rant_schema_msg_len(IntPtr s, IntPtr buf, UIntPtr cap);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_set_map(IntPtr buf, UIntPtr cap, IntPtr s, byte[] field, RantBytes map);

        // node lock (bracket zero-copy views) + match-wait companions
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern void rant_node_lock(IntPtr node);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern void rant_node_unlock(IntPtr node);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_node_peers_next(IntPtr node, ref RantIter it, out RantPeerInfoNative info);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_node_entities_next(IntPtr node, uint peer, ref RantIter it,
            out RantEntityInfoNative info);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_node_mesh_next(IntPtr node, ref RantIter it, out RantEntityInfoNative info);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_node_mesh_find(IntPtr node, int kind, byte[] name, out RantEntityInfoNative info);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern uint rant_node_mesh_epoch(IntPtr node);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_node_settle(IntPtr node, int timeout_ms);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_topic_ready(IntPtr ch);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_topic_pending_count(IntPtr ch);

        // patterns: functions
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr rant_node_create_function_definition(IntPtr node, byte[] name,
            IntPtr req_schema, IntPtr rsp_schema, RantRequestFn on_request, IntPtr user,
            ref RantFunctionOpts opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr rant_node_create_remote_function(IntPtr node, byte[] name,
            IntPtr req_schema, IntPtr rsp_schema, ref RantFunctionOpts opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_function_call(IntPtr fn, RantBytes req,
            out RantResponseNative response, int timeout_ms, IntPtr opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_function_call_async(IntPtr fn, RantBytes req,
            RantResponseFn on_response, IntPtr user, IntPtr opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_function_match_count(IntPtr fn);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_function_retire(IntPtr fn);

        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_function_refresh(IntPtr fn);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern void rant_request_reply(IntPtr request, RantBytes rsp);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern void rant_request_fail(IntPtr request, byte[] message, RantBytes rsp);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern ulong rant_request_defer(IntPtr request);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_function_complete(IntPtr fn, ulong token, int status, byte[] message, RantBytes rsp);

        // patterns: tasks
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr rant_node_create_task_definition(IntPtr node, byte[] name,
            IntPtr req_schema, IntPtr prg_schema, IntPtr rsp_schema, RantRequestFn on_request,
            IntPtr user, ref RantTaskOpts opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr rant_node_create_remote_task(IntPtr node, byte[] name,
            IntPtr req_schema, IntPtr prg_schema, IntPtr rsp_schema, ref RantTaskOpts opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_request_start(IntPtr request);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_function_progress(IntPtr fn, ulong token, RantBytes progress);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_function_cancelled(IntPtr fn, ulong token);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_function_on_cancel(IntPtr fn, RantCancelFn on_cancel, IntPtr user);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_function_cancel(IntPtr fn, uint call_id);

        // patterns: variables
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr rant_node_create_variable_definition(IntPtr node, byte[] name,
            IntPtr schema, ref RantVariableOpts opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr rant_node_create_remote_variable(IntPtr node, byte[] name,
            IntPtr schema, ref RantVariableOpts opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_variable_get(IntPtr var, out RantBytes value);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_variable_set(IntPtr var, RantBytes value);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_variable_force(IntPtr var, RantBytes value);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_variable_unforce(IntPtr var);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_variable_forced(IntPtr var);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_variable_wait(IntPtr var, int timeout_ms);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_variable_match_count(IntPtr var);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_variable_retire(IntPtr var);

        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_variable_refresh(IntPtr var);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_variable_on_change(IntPtr var, RantVariableUpdateFn on_change, IntPtr user);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int rant_variable_on_write(IntPtr var, RantVariableUpdateFn on_write, IntPtr user);
    }

    // ---- config + reflection attributes -----------------------------------------

    /// <summary>Per topic QoS, the C RantQos as a class. Every field zero means the default,
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

        internal RantQos ToNative()
        {
            return new RantQos
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

    /// <summary>Who drives the node's loop: the C service thread, started at construction,
    /// or your own thread calling Poll().</summary>
    public enum Threading { ServiceThread = 0, Manual = 1 }

    /// <summary>The node options, the C RantNodeOpts under C# names. 0, false or null is the
    /// C default. docs/node.md and docs/discovery.md explain each.</summary>
    public sealed class NodeOptions
    {
        /// <summary>Nodes only see peers on the same domain.</summary>
        public ushort Domain;
        /// <summary>Topics this node may create, 0 = 8.</summary>
        public ushort MaxTopics;
        /// <summary>Never use the same host shared memory path.</summary>
        public bool DisableShm;
        /// <summary>Fetch and cache every schema every peer advertises, for observer tools.</summary>
        public bool FetchDetails;
        /// <summary>The send path match wait bound. 0 = 1 s, negative = off, and such a send
        /// fires UnmatchedSend instead of waiting.</summary>
        public int MatchWaitMs;
        /// <summary>No @rant/log topics: Log returns NoSys.</summary>
        public bool DisableLogs;
        /// <summary>No built in @rant/meta function.</summary>
        public bool DisableMeta;
        /// <summary>Do not mirror this node's errors onto @rant/log/error.</summary>
        public bool DisableErrorLogs;
        /// <summary>The data socket port, 0 = OS assigned.</summary>
        public ushort DataPort;
        /// <summary>The discovery multicast group, null = 239.255.0.[domain].</summary>
        public string DiscoveryGroup;
        /// <summary>0 = 7400.</summary>
        public ushort DiscoveryPort;
        /// <summary>Pin discovery to this interface IP, null = every interface. "127.0.0.1"
        /// keeps a node on one host.</summary>
        public string MulticastInterface;
        /// <summary>0 = 1 hop.</summary>
        public byte MulticastTtl;
        /// <summary>"ip" or "ip:port" unicast announce targets.</summary>
        public string[] SeedPeers;
        /// <summary>No group join, the seeds relay this node.</summary>
        public bool UnicastOnly;
        /// <summary>UDP payload bytes per fragment, 0 = the default.</summary>
        public ushort FragmentSize;
        /// <summary>Data socket OS buffers, 0 = the OS default. Raise both for big payloads.</summary>
        public uint RecvBufferBytes;
        public uint SendBufferBytes;
        /// <summary>Advertise this locator to every peer instead of letting each learn it from
        /// the datagram source, for a cloud IP or a published container port.</summary>
        public string SelfIp;
        public ushort AdvertisePort;
        /// <summary>Discovery cadence: 0 = 3 s between announces, 12 s peer timeout, 16 peers.</summary>
        public uint AnnounceIntervalUs;
        public uint PeerTimeoutUs;
        public ushort MaxPeers;
        /// <summary>ServiceThread starts the C service thread at construction and handlers fire
        /// on it. Manual leaves the loop to your Poll() calls, so handlers fire there.</summary>
        public Threading Threading = Threading.ServiceThread;
        /// <summary>Where callbacks run. Null runs them inline on the service or polling thread.
        /// Set it to a delegate that posts to your thread and every event, pattern handler,
        /// variable observer, progress report and awaited call result runs there instead.
        /// Messages are unaffected, they have the consumer queue (docs/node.md).</summary>
        public Action<Action> Dispatcher;
    }

    /// <summary>Function options, the C RantFunctionOpts. 0 is the default.</summary>
    public sealed class FunctionOptions
    {
        /// <summary>Reliable send pause for a slow peer, 0 = 1 s.</summary>
        public uint BackpressureWaitUs;
        /// <summary>The remote call timeout, 0 = 5 s.</summary>
        public uint TimeoutUs;
        /// <summary>Request and response ring depth, 0 = 10.</summary>
        public ushort KeepLast;
        /// <summary>Redundant definitions on purpose: no duplicate authority diagnostic.</summary>
        public bool Multi;
        /// <summary>A byte[] handle with no schema takes the entity's from the mesh, and
        /// Refresh() re types it later (docs/reflection.md).</summary>
        public bool ReflectFromMesh;

        internal RantFunctionOpts ToNative() => new RantFunctionOpts
        {
            backpressure_wait_us = BackpressureWaitUs,
            timeout_us = TimeoutUs,
            keep_last = KeepLast,
            multi = (byte)(Multi ? 1 : 0),
            reflect_from_mesh = (byte)(ReflectFromMesh ? 1 : 0),
        };
    }

    /// <summary>Task options, the C RantTaskOpts (docs/tasks.md). 0 is the default.</summary>
    public sealed class TaskOptions
    {
        /// <summary>Best effort progress: the definition offers it, a remote requests it.</summary>
        public bool ProgressBestEffort;
        /// <summary>Progress ring depth, 0 = the pattern default.</summary>
        public ushort ProgressKeepLast;
        /// <summary>The definition will not honor a cancel: remotes get BadRole.</summary>
        public bool NoCancel;
        /// <summary>Declared serialization, enforced by the handler.</summary>
        public bool Exclusive;
        /// <summary>Redundant providers, one executor each.</summary>
        public bool Multi;
        /// <summary>Remote: the bound until the first response, 0 = 5 s.</summary>
        public uint TimeoutUs;
        /// <summary>Reliable send pause for a slow peer, 0 = 1 s.</summary>
        public uint BackpressureWaitUs;
        /// <summary>Request and response ring depth, 0 = 10.</summary>
        public ushort KeepLast;
        /// <summary>As FunctionOptions.ReflectFromMesh, for all three channels.</summary>
        public bool ReflectFromMesh;

        internal RantTaskOpts ToNative() => new RantTaskOpts
        {
            progress_best_effort = (byte)(ProgressBestEffort ? 1 : 0),
            progress_keep_last = ProgressKeepLast,
            no_cancel = (byte)(NoCancel ? 1 : 0),
            exclusive = (byte)(Exclusive ? 1 : 0),
            multi = (byte)(Multi ? 1 : 0),
            timeout_us = TimeoutUs,
            backpressure_wait_us = BackpressureWaitUs,
            keep_last = KeepLast,
            reflect_from_mesh = (byte)(ReflectFromMesh ? 1 : 0),
        };
    }

    /// <summary>Variable options, the C RantVariableOpts. 0 is the default. ReadOnly and
    /// AllowForce are the definition's declarations and mean nothing on a remote.</summary>
    public sealed class VariableOptions
    {
        /// <summary>No set channel: a remote set gets BadRole.</summary>
        public bool ReadOnly;
        /// <summary>Permit Force, local and remote.</summary>
        public bool AllowForce;
        /// <summary>Value channel catch up, 0 = 1.</summary>
        public ushort CatchUp;
        /// <summary>Both channels' repair window, 0 = 10.</summary>
        public ushort KeepLast;
        /// <summary>Reliable send pause for a slow peer, 0 = 1 s.</summary>
        public uint BackpressureWaitUs;
        /// <summary>A byte[] handle with no schema takes the owner's from the mesh, and
        /// Refresh() re types it later (docs/reflection.md).</summary>
        public bool ReflectFromMesh;

        internal RantVariableOpts ToNative() => new RantVariableOpts
        {
            access = (byte)(ReadOnly ? 1 : 0),
            allow_force = (byte)(AllowForce ? 1 : 0),
            catch_up = CatchUp,
            keep_last = KeepLast,
            backpressure_wait_us = BackpressureWaitUs,
            reflect_from_mesh = (byte)(ReflectFromMesh ? 1 : 0),
        };
    }

    /// <summary>Name the wire type of a message struct or class. Without it the class name is
    /// the wire name, and peers must match it.</summary>
    [AttributeUsage(AttributeTargets.Struct | AttributeTargets.Class)]
    public sealed class RantSchemaAttribute : Attribute
    {
        public string Name;
        public RantSchemaAttribute(string name) { Name = name; }
    }

    /// <summary>A fixed length array field with this element count. Without it an array
    /// field is a variable array whose length rides the message tail.</summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class RantArrayAttribute : Attribute
    {
        public int Count;
        public RantArrayAttribute(int count) { Count = count; }
    }

    /// <summary>A capped string field, the max UTF-8 byte length. Without it a string is
    /// unbounded. On a string[] it makes a variable array, with [RantArray] a fixed one.</summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class RantStringAttribute : Attribute
    {
        public int Cap;
        public RantStringAttribute(int cap) { Cap = cap; }
    }

    /// <summary>Name a field's type with a standard type (docs/stdtypes.md), so the name
    /// narrows matching. The shape must be the canonical one or compiling fails.</summary>
    [AttributeUsage(AttributeTargets.Field | AttributeTargets.Struct | AttributeTargets.Class)]
    public sealed class RantTypeNameAttribute : Attribute
    {
        public string Name;
        public RantTypeNameAttribute(string name) { Name = name; }
    }

    // The standard composites as plain mirrors of their wire shape (docs/stdtypes.md). The
    // [RantField] overrides give the canonical lowercase wire names every language agrees on.
    [RantTypeName("Float2")] public struct Float2
    { [RantField("x")] public float X; [RantField("y")] public float Y; }
    [RantTypeName("Float3")] public struct Float3
    { [RantField("x")] public float X; [RantField("y")] public float Y;
      [RantField("z")] public float Z; }
    [RantTypeName("Float4")] public struct Float4
    { [RantField("x")] public float X; [RantField("y")] public float Y;
      [RantField("z")] public float Z; [RantField("w")] public float W; }
    [RantTypeName("Double2")] public struct Double2
    { [RantField("x")] public double X; [RantField("y")] public double Y; }
    [RantTypeName("Double3")] public struct Double3
    { [RantField("x")] public double X; [RantField("y")] public double Y;
      [RantField("z")] public double Z; }
    [RantTypeName("Double4")] public struct Double4
    { [RantField("x")] public double X; [RantField("y")] public double Y;
      [RantField("z")] public double Z; [RantField("w")] public double W; }
    [RantTypeName("Int2")] public struct Int2
    { [RantField("x")] public int X; [RantField("y")] public int Y; }
    [RantTypeName("Int3")] public struct Int3
    { [RantField("x")] public int X; [RantField("y")] public int Y; [RantField("z")] public int Z; }
    [RantTypeName("Int4")] public struct Int4
    { [RantField("x")] public int X; [RantField("y")] public int Y;
      [RantField("z")] public int Z; [RantField("w")] public int W; }
    [RantTypeName("Quaternion")] public struct Quaternion    // stored x, y, z, w
    { [RantField("x")] public double X; [RantField("y")] public double Y;
      [RantField("z")] public double Z; [RantField("w")] public double W; }
    [RantTypeName("Color")] public struct Color              // sRGB, straight alpha
    { [RantField("r")] public byte R; [RantField("g")] public byte G;
      [RantField("b")] public byte B; [RantField("a")] public byte A; }
    [RantTypeName("Rect")] public struct Rect
    { [RantField("x")] public float X; [RantField("y")] public float Y;
      [RantField("w")] public float W; [RantField("h")] public float H; }
    [RantTypeName("RectI")] public struct RectI
    { [RantField("x")] public int X; [RantField("y")] public int Y;
      [RantField("w")] public int W; [RantField("h")] public int H; }
    // Meters and radians. Parent "" = unstated, the cap keeps the packed 88 bytes 8 aligned.
    [RantTypeName("Transform")] public struct Transform
    { [RantField("translation")] public Double3 Translation;
      [RantField("rotation")] public Quaternion Rotation;
      [RantField("parent")] [RantString(30)] public string Parent; }
    [RantTypeName("Twist")] public struct Twist               // m/s and rad/s
    { [RantField("linear")] public Double3 Linear; [RantField("angular")] public Double3 Angular; }
    [RantTypeName("GeoPoint")] public struct GeoPoint         // degrees, degrees, meters
    { [RantField("lat")] public double Lat; [RantField("lon")] public double Lon;
      [RantField("alt")] public double Alt; }

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
    [RantTypeName("Image")] public struct Image               // stride 0 = packed rows
    { [RantField("width")] public uint Width; [RantField("height")] public uint Height;
      [RantField("stride")] public uint Stride;
      [RantField("format")] public ImageFormat Format;
      [RantField("data")] public byte[] Data; }               // pixels, or the file bytes
    [RantTypeName("VideoFrame")] public struct VideoFrame     // width/height 0 = unstated
    { [RantField("codec")] public VideoCodec Codec;
      [RantField("width")] public uint Width; [RantField("height")] public uint Height;
      [RantField("keyframe")] public bool Keyframe;
      [RantField("pts")] [RantTypeName("Timestamp")] public long Pts;   // the Timestamp clock
      [RantField("data")] public byte[] Data; }
    // Fully fixed, so it works as a latched variable: hand a viewer a URL, not pixels. Codec,
    // Width and Height are hints for pickers, the stream stays authoritative once connected.
    [RantTypeName("ExternalVideoStream")] public struct ExternalVideoStream
    { [RantField("kind")] public VideoStreamKind Kind;
      [RantField("codec")] public VideoCodec Codec;
      [RantField("width")] public uint Width; [RantField("height")] public uint Height;
      [RantField("url")] [RantTypeName("Uri")] [RantString(256)] public string Url;
      [RantField("name")] [RantString(32)] public string Name; }

    /// <summary>A lens distortion model. NoDistortion is an ideal pinhole.</summary>
    public enum DistortionModel : byte
    { NoDistortion = 0, BrownConrady = 1, Fisheye = 2, Rational = 3 }
    // The pinhole model and its lens distortion. Coeffs is zero filled past the model's count.
    [RantTypeName("CameraIntrinsics")] public struct CameraIntrinsics
    { [RantField("width")] public uint Width; [RantField("height")] public uint Height;
      [RantField("fx")] public double Fx; [RantField("fy")] public double Fy;
      [RantField("cx")] public double Cx; [RantField("cy")] public double Cy;
      [RantField("model")] public DistortionModel Model;
      [RantField("coeffs")] [RantArray(8)] public double[] Coeffs; }
    // SI: radians or meters, per second, and newtons or newton meters. Velocity and Effort
    // may be empty. The names ride a JointNames variable, not every sample.
    [RantTypeName("JointState")] public struct JointState
    { [RantField("position")] public double[] Position;
      [RantField("velocity")] public double[] Velocity;
      [RantField("effort")] public double[] Effort; }
    // Published once as a variable. The order every JointState array follows.
    [RantTypeName("JointNames")] public struct JointNames
    { [RantField("name")] [RantString(32)] public string[] Name; }

    /// <summary>The Timestamp clock: microseconds since the Unix epoch UTC, the units of a
    /// message's WrittenUs and of a Send's captureUs.</summary>
    public static class Timestamp
    {
        public static long Now() => Native.rant_timestamp_now();
    }

    /// <summary>Override a field's wire name (must match peers, like a topic name).</summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class RantFieldAttribute : Attribute
    {
        public string Name;
        public RantFieldAttribute(string name) { Name = name; }
    }

    public class SchemaException : Exception
    {
        public SchemaException(string m) : base(m) { }
    }

    /// <summary>A call was refused: a non Ok SendStatus surfaced through a throwing surface
    /// such as the Variable&lt;T&gt;.Value setter.</summary>
    public class RantException : Exception
    {
        public SendStatus Status;
        public RantException(SendStatus status, string m) : base(m) { Status = status; }
    }

    /// <summary>Reading RantResponse&lt;TRsp&gt;.Value when the call did not complete Ok.</summary>
    public class CallException : Exception
    {
        public CallStatus Status;
        public CallException(CallStatus status, string m) : base(m) { Status = status; }
    }

    // ---- schema: DSL compile, reflection, encode/decode -------------------------

    /// <summary>A compiled message schema: the wire shape of a topic, request, response or
    /// variable. Compile DSL text, or reflect a type's public fields.</summary>
    public sealed class Schema : IDisposable
    {
        internal IntPtr Handle;
        internal Type ClrType;   // set for reflection-built schemas (decode target)

        /// <summary>Compile schema DSL text such as "Pose { x: f32 }".</summary>
        public Schema(string text)
        {
            IntPtr err;
            IntPtr h = Native.rant_schema_compile(Codec.SchemaAlloc, IntPtr.Zero, Codec.CStr(text), out err);
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

        // The schema a typed handle uses: the given one bound to T, else T reflected. A byte[]
        // T carries the encoded message as is, so it binds no type and reflects nothing.
        internal static Schema For(Type t, Schema given)
        {
            if (Codec.IsRaw(t)) return given == null ? null : Copy(given, null);
            return given == null ? new Schema(t) : Copy(given, t);
        }

        private static Schema Copy(Schema given, Type t)
        {
            IntPtr h = Native.rant_schema_copy(given.Handle, Codec.SchemaAlloc, IntPtr.Zero);
            if (h == IntPtr.Zero) throw new SchemaException("schema copy failed");
            return new Schema(h) { ClrType = t };
        }

        /// <summary>The wire type name, "" for an anonymous root.</summary>
        public string Name => Codec.Str(Native.rant_schema_name(Handle));
        /// <summary>The identity of the wire shape, the same value in every language.</summary>
        public ulong Hash => Native.rant_schema_hash(Handle);

        /// <summary>The DSL text reconstructed from the compiled schema (works for any
        /// schema, including one parsed from a peer). Paste into a C node for interop.</summary>
        public string Dsl => Codec.SchemaDsl(Handle);

        /// <summary>Can a reader declaring this schema read messages written with pub? Type
        /// names narrow: an anonymous type reads a named one, never the reverse.</summary>
        public bool CanRead(Schema pub) => Native.rant_schema_subset(Handle, pub.Handle) != 0;

        /// <summary>The flat depth first field table (spec/schema.md). A struct's members
        /// follow it one level deeper, a struct array's element template likewise with
        /// ArrayParent pointing back. A bare root is one field named "".</summary>
        public SchemaField[] Fields
        {
            get
            {
                ushort n = Native.rant_schema_field_count(Handle);
                var fields = new SchemaField[n];
                for (ushort i = 0; i < n; i++)
                {
                    RantSchemaFieldInfo info;
                    if (Native.rant_schema_field_at(Handle, i, out info) == 0)
                        throw new SchemaException("field " + i + " unreadable");
                    fields[i] = new SchemaField
                    {
                        Index = i, Name = Codec.Str(info.name), TypeName = Codec.Str(info.type_name),
                        ElemName = Codec.Str(info.elem_name), Kind = (FieldType)info.kind,
                        Elem = (FieldType)info.elem, Count = info.count, Depth = info.depth,
                        StrCap = info.str_cap,
                        ArrayParent = info.arr_parent == 0xFFFF ? -1 : info.arr_parent,
                        Offset = info.offset, Size = info.size, ElemSize = info.elem_size,
                    };
                }
                return fields;
            }
        }

        /// <summary>An enum field's options by flat index, empty for any other field.</summary>
        public SchemaEnumVariant[] EnumVariants(int field)
        {
            ushort n = Native.rant_schema_enum_count(Handle, (ushort)field);
            var list = new SchemaEnumVariant[n];
            for (ushort i = 0; i < n; i++)
            {
                long value; RantStringView name;
                if (Native.rant_schema_enum_variant(Handle, (ushort)field, i, out value, out name) == 0)
                    throw new SchemaException("enum variant " + i + " unreadable");
                list[i] = new SchemaEnumVariant { Name = Codec.Str(name), Value = value };
            }
            return list;
        }

        /// <summary>Encode a value: an instance of the reflected type, or a Dictionary by field
        /// name against any schema. A field left null keeps the zero default.</summary>
        public byte[] Encode(object value) => Codec.Encode(Handle, value);
        /// <summary>Decode message bytes: an instance of the reflected type, the one value of a
        /// bare type, else the fields by name as a Dictionary.</summary>
        public object Decode(byte[] data)
        {
            var d = Codec.DecodeDict(Handle, data);
            return ClrType != null ? Codec.ToObject(ClrType, d) : Codec.RootOrFields(Handle, d);
        }

        public void Dispose()
        {
            if (Handle != IntPtr.Zero)
            {
                Native.rant_schema_free(Handle, Codec.SchemaAlloc, IntPtr.Zero);
                Handle = IntPtr.Zero;
            }
            GC.SuppressFinalize(this);
        }
        ~Schema() { Dispose(); }
    }

    // ---- delivered message / event ----------------------------------------------

    /// <summary>One row of a schema's flat field table, mirrors RantSchemaFieldInfo.</summary>
    public sealed class SchemaField
    {
        public int Index;
        public string Name;
        public string TypeName;     // the field type's name, "" when anonymous
        public string ElemName;     // an array element type's name, "" when anonymous
        public FieldType Kind;
        public FieldType Elem;      // an array's element kind, or an enum's backing scalar
        public int Count;           // a fixed array's element count
        public int Depth;
        public int StrCap;          // a capped string's byte capacity
        public int ArrayParent;     // the enclosing struct array's flat index, -1 for none
        public uint Offset, Size, ElemSize;
    }

    public sealed class SchemaEnumVariant { public string Name; public long Value; }

    /// <summary>A delivered message: the envelope beside the payload. The payload is copied
    /// out so it outlives the callback, and the decode happens on the first read of Value and
    /// never if it is not read. Read it from one thread, as handlers do.</summary>
    public sealed class RantMessage
    {
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

        /// <summary>The typed instance on a typed subscriber, the one value of a bare type,
        /// else the fields by name as a Dictionary. Null on a raw topic or when the decode
        /// failed. Decodes on the first read.</summary>
        public object Value { get { Decode(); return _value; } }

        // Decode failures stay where they were: reported once, leaving Value null, so one bad
        // publisher never throws out of a handler that only wanted the bytes.
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
            catch (Exception e) { Console.Error.WriteLine("rant decode: " + e); }
        }

        internal static RantMessage FromNative(ref RantMsg m, Type clrType, Schema ownedSchema)
        {
            return new RantMessage
            {
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
            => $"RantMessage(topic={TopicName}, from={PublisherName}, {Data.Length} bytes)";
    }

    /// <summary>One node event: a peer came or went, a peer's interest changed, messages were
    /// lost, or an error. ToString() is the one line diagnostic.</summary>
    public sealed class RantEvent
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

        // format an event returned BY VALUE (rant_last_error): rant_event_str wants a
        // pointer, so briefly marshal the struct to unmanaged memory.
        internal static RantEvent FromValue(RantEventNative e)
        {
            IntPtr p = Marshal.AllocHGlobal(Marshal.SizeOf<RantEventNative>());
            try { Marshal.StructureToPtr(e, p, false); return FromNative(p, ref e); }
            finally { Marshal.FreeHGlobal(p); }
        }

        internal static RantEvent FromNative(IntPtr evPtr, ref RantEventNative e)
        {
            var buf = new byte[192];
            Native.rant_event_str(evPtr, buf, (UIntPtr)buf.Length);
            return new RantEvent
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

    /// <summary>A node's counters as one snapshot, from RantNode.Stats.</summary>
    public sealed class NodeStats
    {
        /// <summary>Sends that evicted never sent history after the bounded wait, the
        /// overload indicator (the ErrorKind.EvictedUnsent count).</summary>
        public uint EvictedUnsent;
        /// <summary>Bytes of the node's arena in use now and at the peak, and how many
        /// allocation calls it made.</summary>
        public ulong MemInUse, MemPeak, AllocCalls;
        /// <summary>Time reliable sends spent paused for a slow subscriber, and how many did.</summary>
        public ulong BackpressureWaitedUs;
        public uint BackpressureWaits;
    }

    /// <summary>One decoded @rant/log line for a RantNode.OnLog handler. WallUs is epoch us,
    /// MonoUs the publisher's monotonic clock, RecvUs this node's clock at receipt.</summary>
    public sealed class RantLogLine
    {
        public LogLevel Level;
        public string Node;      // the publishing node's name
        public uint NodeId;      // the publishing peer id
        public ulong WallUs;
        public ulong MonoUs;
        public ulong RecvUs;
        /// <summary>The carrying message's source stamp (see RantMessage.WrittenUs).</summary>
        public ulong WrittenUs;
        public string Text;

        public override string ToString() => $"[{Level}] {Node}: {Text}";
    }

    /// <summary>A decoded @rant/meta reply. The node and proc scalars are fields, the full
    /// body stays in Info. Absent sections leave zeros and HaveProc false.</summary>
    public sealed class RantMetaSnapshot
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

        internal static RantMetaSnapshot FromResponse(ResponseCore r)
        {
            var s = new RantMetaSnapshot { Status = r.Status, Provider = r.Provider };
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
    // zeroes every handle there: the C refuses a NULL one (RANT_ERR_NO_TOPIC) instead of
    // reading freed memory.
    internal interface INodeHandle { void Invalidate(); }

    // ---- reflection snapshots (docs/reflection.md) ----------------------------

    /// <summary>A discovered peer, copied out of the node so it outlives the walk. A dropped
    /// peer is still listed since the same uuid may return: gate on Active.</summary>
    public sealed class RantPeer
    {
        public uint Id;
        public byte[] Uuid;
        public string Name;
        public string Address;
        public PeerLiveness Liveness;
        public bool Active => Liveness == PeerLiveness.Active;
        public ulong LastHeardUs;
        public uint Epoch;
        public bool CatchingUp;
        public ushort FragmentSize;
        public uint RttUs, RttJitterUs, RttMinUs, RttSamples;

        internal static RantPeer Read(ref RantPeerInfoNative p) => new RantPeer
        {
            Id = p.id, Uuid = p.uuid, Name = Codec.Str(p.name), Address = Codec.Str(p.address),
            Liveness = (PeerLiveness)p.liveness, LastHeardUs = p.last_heard_us, Epoch = p.epoch,
            CatchingUp = p.catching_up != 0, FragmentSize = p.fragment_size, RttUs = p.rtt_us,
            RttJitterUs = p.rtt_jitter_us, RttMinUs = p.rtt_min_us, RttSamples = p.rtt_samples,
        };
    }

    /// <summary>One entity of the mesh: a topic, a function, a variable or a task, never a
    /// channel. The schemas are owned copies, null when untyped or not fetched yet.</summary>
    public sealed class RantEntity
    {
        public EntityKind Kind;
        public string Name;
        public uint Hash;
        public bool Provides, Consumes, Reliable;
        public bool Writable, Forceable;
        public bool Cancellable, Exclusive;
        public bool Multi, Incomplete, Conflict;
        public ushort Providers, Consumers;
        public uint Provider;
        public string From;
        public Schema Schema;
        public ulong SchemaHash;
        public Schema ResponseSchema;
        public ulong ResponseSchemaHash;
        public Schema ProgressSchema;
        public ulong ProgressSchemaHash;
        public ulong Generation;

        internal static RantEntity Read(ref RantEntityInfoNative e) => new RantEntity
        {
            Kind = (EntityKind)e.kind,
            Name = e.name.data != IntPtr.Zero ? Codec.Str(e.name) : "0x" + e.hash.ToString("x8"),
            Hash = e.hash, Provides = e.provides != 0, Consumes = e.consumes != 0,
            Reliable = e.reliable != 0, Writable = e.writable != 0, Forceable = e.forceable != 0,
            Cancellable = e.cancellable != 0, Exclusive = e.exclusive != 0, Multi = e.multi != 0,
            Incomplete = e.incomplete != 0, Conflict = e.conflict != 0,
            Providers = e.providers, Consumers = e.consumers, Provider = e.provider,
            From = Codec.Str(e.from),
            Schema = Own(e.schema), SchemaHash = e.schema_hash,
            ResponseSchema = Own(e.rsp_schema), ResponseSchemaHash = e.rsp_schema_hash,
            ProgressSchema = Own(e.progress_schema), ProgressSchemaHash = e.progress_schema_hash,
            Generation = e.generation,
        };

        // the node's view is good only until the next poll, so every schema is copied out
        private static Schema Own(IntPtr view)
        {
            if (view == IntPtr.Zero) return null;
            IntPtr h = Native.rant_schema_copy(view, Codec.SchemaAlloc, IntPtr.Zero);
            return h == IntPtr.Zero ? null : new Schema(h);
        }
    }

    // ---- topic ----------------------------------------------------------------

    // The native topic slot behind Publisher<T> and Subscriber<T>. Same name handles on one
    // node share it, held per role in the node's registry, and the last one out retires it.
    internal sealed class TopicCore : INodeHandle
    {
        internal readonly RantNode _node;
        internal IntPtr _handle;   // zeroed by Release, and by the node at Close
        internal readonly Schema Schema;
        internal readonly Role Role;

        void INodeHandle.Invalidate() { _handle = IntPtr.Zero; }

        internal TopicCore(RantNode node, string name, Schema schema, Role role, Qos qos)
        {
            _node = node;
            Schema = schema;
            Role = role;
            _handle = node.AcquireTopic(name, role, schema, qos);
            node.RegisterHandle(this);
        }

        internal SendStatus Send(byte[] data, long captureUs)
        {
            int r;
            var h = GCHandle.Alloc(data, GCHandleType.Pinned);
            try
            {
                var b = new RantBytes
                {
                    data = data != null && data.Length > 0 ? h.AddrOfPinnedObject() : IntPtr.Zero,
                    len = (UIntPtr)(data?.Length ?? 0)
                };
                var o = new RantSendOpts { capture_us = (ulong)captureUs };
                r = Native.rant_topic_send(_handle, b, ref o);
            }
            finally { h.Free(); }
            return (SendStatus)r;
        }

        // Drop this handle's hold on the slot. The native topic is retired with the last one.
        internal void Release() => _node.ReleaseTopic(this);
        internal bool Refresh() => _handle != IntPtr.Zero && Native.rant_topic_refresh(_handle) == 1;
        internal ushort Index => Native.rant_topic_index(_handle);
        internal int MatchCount => Native.rant_topic_match_count(_handle);
        internal bool Ready => Native.rant_topic_ready(_handle) == 1;

        // The first take or dispatch queues the topic (docs/node.md).
        internal bool TryTake(out RantMessage message, int timeoutMs)
        {
            message = null;
            var m = new RantMsg();
            if (Native.rant_topic_take(_handle, ref m, timeoutMs) != 1) return false;
            message = RantMessage.FromNative(ref m, _node.ClrTypeOf(m.topic_index),
                                             _node.OwnedSchema(m.schema));
            return true;
        }

        internal int Dispatch(int maxMsgs, int timeoutMs)
            => Native.rant_topic_dispatch(_handle, maxMsgs, timeoutMs);

        internal (uint Messages, uint Bytes, uint Capacity, uint Dropped) QueueStats()
        {
            Native.rant_topic_queue_stats(_handle, out uint m, out uint b, out uint c, out uint d);
            return (m, b, c, d);
        }

        internal (ulong TxMsgs, ulong TxBytes, ulong RxMsgs, ulong RxBytes) Counts()
        {
            Native.rant_topic_counts(_handle, out ulong tm, out ulong tb, out ulong rm, out ulong rb);
            return (tm, tb, rm, rb);
        }
    }

    // ---- node -------------------------------------------------------------------

    /// <summary>One participant on the mesh. Its Publisher, Subscriber, function, task and
    /// variable methods create the handles. The service thread runs from construction unless
    /// NodeOptions.Threading is Manual, where Poll() drives it. Every call is thread safe.</summary>
    public sealed class RantNode : IDisposable
    {
        private IntPtr _handle;
        private long _id;
        private RantAllocator _alloc;
        private IntPtr _discGroup;   // native strings the node retains for its lifetime
        private IntPtr _mcastIf;
        internal readonly Action<Action> Dispatcher;   // NodeOptions.Dispatcher
        private readonly Dictionary<ushort, Type> _topicTypes = new Dictionary<ushort, Type>();
        private readonly List<Schema> _schemas = new List<Schema>();
        // A publisher's schema is a node owned view good only until the next poll, so a
        // message that decodes later needs our own copy. Keyed by the schema hash, so one
        // copy serves every message on every topic that carries it.
        private readonly Dictionary<ulong, Schema> _msgSchemas = new Dictionary<ulong, Schema>();
        // its own lock: the delivery path reaches it while the node lock is held, and
        // _createLock is held across a create, which takes the node lock the other way round
        private readonly object _msgSchemaLock = new object();
        // same name topic sharing: one native slot per name and a hold count per role, so the
        // role follows the live handles and the last release retires the slot
        private sealed class TopicRec { public IntPtr Handle; public ulong SchemaHash; public int Pubs, Subs; }
        private readonly Dictionary<string, TopicRec> _topicsByName = new Dictionary<string, TopicRec>();
        private readonly object _createLock = new object();
        // per topic subscriber handlers, copy on write arrays so the poll thread read never
        // takes more than a volatile fetch
        private volatile Dictionary<ushort, Action<RantMessage>[]> _subHandlers = new Dictionary<ushort, Action<RantMessage>[]>();
        private readonly object _subLock = new object();
        // pattern handler boxes + in-flight async calls this node owns (reaped at Close)
        private readonly List<long> _patternBoxes = new List<long>();
        private readonly List<WeakReference<INodeHandle>> _handles = new List<WeakReference<INodeHandle>>();
        private readonly HashSet<long> _asyncLive = new HashSet<long>();
        internal readonly object PatternLock = new object();

        // rooted so the GC never collects the trampolines handed to native code.
        private static readonly RantMsgFn s_onMsg = OnMessageTramp;
        private static readonly RantEventFn s_onEvt = OnEventTramp;
        // Read once per delivered message and per event, written only at open and close, so
        // it is replaced whole under s_reg and read without a lock.
        private static volatile Dictionary<long, RantNode> s_nodes = new Dictionary<long, RantNode>();
        private static readonly object s_reg = new object();
        private static long s_nextId = 1;

        /// <summary>Peer lifecycle, loss and error events, on the service or polling thread or
        /// the Dispatcher. Optional: LastError records the last error either way.</summary>
        public event Action<RantEvent> OnEvent;

        /// <summary>Who drives the loop, as opened.</summary>
        public Threading Threading { get; }

        /// <summary>Open a node and join the mesh. Null options are the C defaults. The service
        /// thread runs from here unless options.Threading is Manual, and OnEvent may attach
        /// after: the first peer takes longer to appear than the next statement.</summary>
        public RantNode(string name = null, NodeOptions options = null)
        {
            NodeOptions o = options ?? new NodeOptions();
            Dispatcher = o.Dispatcher;
            Threading = o.Threading;
            lock (s_reg)
            {
                _id = s_nextId++;
                var next = new Dictionary<long, RantNode>(s_nodes) { [_id] = this };
                s_nodes = next;
            }

            var co = new RantNodeOpts
            {
                domain = o.Domain,
                max_topics = o.MaxTopics,
                disable_shm = (byte)(o.DisableShm ? 1 : 0),
                fetch_details = (byte)(o.FetchDetails ? 1 : 0),
                match_wait_ms = o.MatchWaitMs,
                disable_logs = (byte)(o.DisableLogs ? 1 : 0),
                disable_meta = (byte)(o.DisableMeta ? 1 : 0),
                disable_error_logs = (byte)(o.DisableErrorLogs ? 1 : 0),
                user_data = (IntPtr)_id,
            };
            // The node retains these pointers for its lifetime, so keep them alive
            // (freed in Close), matching the C++ wrapper.
            _discGroup = Codec.CStrPtr(o.DiscoveryGroup);
            _mcastIf = Codec.CStrPtr(o.MulticastInterface);
            co.net.data_port = o.DataPort;
            co.net.discovery_group = _discGroup;
            co.net.discovery_port = o.DiscoveryPort;
            co.net.multicast_interface = _mcastIf;
            co.net.multicast_ttl = o.MulticastTtl;
            ushort nSeeds;
            IntPtr seedBlock = SeedArray(o.SeedPeers, out nSeeds);   // copied by open, freed below
            co.net.seed_peers = seedBlock;
            co.net.n_seed_peers = nSeeds;
            co.net.unicast_only = (byte)(o.UnicastOnly ? 1 : 0);
            // parsed into 4 bytes during open, never retained: free it right after (unlike
            // the group/interface strings, which the wrapper keeps for the node's life)
            IntPtr selfIpPtr = Codec.CStrPtr(o.SelfIp);
            co.net.self_ip = selfIpPtr;
            co.net.advertise_port = o.AdvertisePort;
            co.net.fragment_size = o.FragmentSize;
            co.net.recv_buffer_bytes = o.RecvBufferBytes;
            co.net.send_buffer_bytes = o.SendBufferBytes;
            co.discovery.announce_interval_us = o.AnnounceIntervalUs;
            co.discovery.peer_timeout_us = o.PeerTimeoutUs;
            co.discovery.max_peers = o.MaxPeers;

            _alloc = Codec.DefaultAllocator();
            byte[] cname = string.IsNullOrEmpty(name) ? null : Codec.CStr(name);
            IntPtr h = Native.rant_node_open(ref _alloc, cname, s_onMsg, s_onEvt, ref co);
            if (seedBlock != IntPtr.Zero) Marshal.FreeHGlobal(seedBlock);
            Codec.FreeCStr(selfIpPtr);

            if (h == IntPtr.Zero)
            {
                lock (s_reg) { var next = new Dictionary<long, RantNode>(s_nodes); next.Remove(_id); s_nodes = next; }
                Codec.FreeCStr(_discGroup); Codec.FreeCStr(_mcastIf);
                // the node does not exist, so read the reason from the process-global slot
                RantEvent err = LastOpenError();
                throw new InvalidOperationException("rant_node_open failed: " + err);
            }
            _handle = h;
            Reflection = new RantReflection(this);
            if (o.Threading == Threading.ServiceThread)
            {
                int rc = Native.rant_node_start(h);
                if (rc != 0)
                {
                    Close(false);
                    throw new InvalidOperationException("service thread start failed: " + (SendStatus)rc
                        + " (open with Threading.Manual and Poll() the node)");
                }
            }
        }

        // Marshal "ip" / "ip:port" seeds into one unmanaged array. The node COPIES it at
        // open, so the caller frees the block as soon as open returns.
        static IntPtr SeedArray(string[] seeds, out ushort count)
        {
            count = 0;
            if (seeds == null || seeds.Length == 0) return IntPtr.Zero;
            int stride = Marshal.SizeOf<RantDiscoveryAddr>();
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
                    var a = new RantDiscoveryAddr { ip = new byte[16], ip_len = 4 };
                    for (int k = 0; k < 4; k++) a.ip[k] = byte.Parse(oct[k]);
                    if (colon >= 0) a.port = ushort.Parse(s.Substring(colon + 1));
                    Marshal.StructureToPtr(a, IntPtr.Add(block, i * stride), false);
                }
            }
            catch { Marshal.FreeHGlobal(block); throw; }
            count = (ushort)seeds.Length;
            return block;
        }

        // Every callback funnels through here so a dispatcher is honored in one place.
        internal void RunCallback(Action a)
        {
            Action<Action> d = Dispatcher;
            if (d == null) { a(); return; }
            try { d(a); }
            catch (Exception e) { Console.Error.WriteLine("rant callback dispatcher: " + e); }
        }
        // The native create behind Publisher<T> and Subscriber<T>. A same name handle on this
        // node shares the slot, widening its role, and a different schema is refused.
        internal IntPtr AcquireTopic(string name, Role role, Schema schema, Qos qos)
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
                            + " (dispose every handle on it to retype the name)");
                    Role before = RoleOf(rec);
                    Hold(rec, role, 1);
                    Role after = RoleOf(rec);
                    if (after != before) Native.rant_topic_set_role(rec.Handle, (int)after);
                    if (schema != null) _schemas.Add(schema);
                    return rec.Handle;
                }

                qos = qos ?? new Qos();
                var co = new RantTopicOpts
                {
                    qos = qos.ToNative(),
                    reflect_from_mesh = (byte)(qos.ReflectFromMesh ? 1 : 0),
                };
                IntPtr h = Native.rant_node_create_topic(_handle, Codec.CStr(name), (int)role,
                    schema != null ? schema.Handle : IntPtr.Zero, ref co);
                if (h == IntPtr.Zero)
                    throw new InvalidOperationException("topic create failed: " + LastError);
                ushort idx = Native.rant_topic_index(h);
                if (schema != null) { _schemas.Add(schema); _topicTypes[idx] = schema.ClrType; }
                rec = new TopicRec { Handle = h, SchemaHash = sh };
                Hold(rec, role, 1);
                _topicsByName[name] = rec;
                return h;
            }
        }

        // Release one hold. The role narrows to what is still held, and the last release
        // retires the native slot and forgets every wrapper registration for the index, since
        // a reused slot may carry a different topic.
        internal void ReleaseTopic(TopicCore t)
        {
            lock (_createLock)
            {
                if (t._handle == IntPtr.Zero) return;
                string name = null;
                TopicRec rec = null;
                foreach (var kv in _topicsByName)
                    if (kv.Value.Handle == t._handle) { name = kv.Key; rec = kv.Value; break; }
                if (rec == null) { t._handle = IntPtr.Zero; return; }
                Role before = RoleOf(rec);
                Hold(rec, t.Role, -1);
                Role after = RoleOf(rec);
                int r = 0;
                if (after == Role.Inactive)
                {
                    ushort idx = Native.rant_topic_index(t._handle);
                    r = Native.rant_topic_retire(t._handle);
                    if (r == 0)
                    {
                        _topicsByName.Remove(name);
                        _topicTypes.Remove(idx);
                        lock (_subLock)
                        {
                            var next = new Dictionary<ushort, Action<RantMessage>[]>(_subHandlers);
                            next.Remove(idx);
                            _subHandlers = next;
                        }
                    }
                }
                else if (after != before)
                {
                    r = Native.rant_topic_set_role(rec.Handle, (int)after);
                }
                if (r != 0)
                {
                    Hold(rec, t.Role, 1);   // still held: the C refused from a callback
                    throw new InvalidOperationException("topic release refused: " + (SendStatus)r);
                }
                t._handle = IntPtr.Zero;
            }
        }

        private static void Hold(TopicRec rec, Role role, int delta)
        {
            if (role == Role.PubOnly || role == Role.PubSub) rec.Pubs += delta;
            if (role == Role.SubOnly || role == Role.PubSub) rec.Subs += delta;
        }

        private static Role RoleOf(TopicRec rec)
            => rec.Pubs > 0 && rec.Subs > 0 ? Role.PubSub : rec.Pubs > 0 ? Role.PubOnly
             : rec.Subs > 0 ? Role.SubOnly : Role.Inactive;

        // Subscriber handlers per topic index, copy on write.
        internal void AddSubHandler(ushort index, Action<RantMessage> fn)
        {
            lock (_subLock)
            {
                var next = new Dictionary<ushort, Action<RantMessage>[]>(_subHandlers);
                Action<RantMessage>[] cur;
                if (!next.TryGetValue(index, out cur)) cur = Array.Empty<Action<RantMessage>>();
                var nv = new Action<RantMessage>[cur.Length + 1];
                Array.Copy(cur, nv, cur.Length);
                nv[cur.Length] = fn;
                next[index] = nv;
                _subHandlers = next;   // published whole, so a reader never sees a torn map
            }
        }

        internal void RemoveSubHandler(ushort index, Action<RantMessage> fn)
        {
            lock (_subLock)
            {
                Action<RantMessage>[] cur;
                if (!_subHandlers.TryGetValue(index, out cur)) return;
                int at = Array.IndexOf(cur, fn);
                if (at < 0) return;
                var next = new Dictionary<ushort, Action<RantMessage>[]>(_subHandlers);
                if (cur.Length == 1) next.Remove(index);
                else
                {
                    var nv = new Action<RantMessage>[cur.Length - 1];
                    Array.Copy(cur, 0, nv, 0, at);
                    Array.Copy(cur, at + 1, nv, at, cur.Length - at - 1);
                    next[index] = nv;
                }
                _subHandlers = next;
            }
        }

        // No lock: the map is replaced whole on every write, so this volatile read gets one
        // consistent version. Runs once per delivered message.
        private Action<RantMessage>[] SubHandlersOf(ushort index)
        {
            Action<RantMessage>[] hs;
            _subHandlers.TryGetValue(index, out hs);
            return hs;
        }

        // Our copy of a publisher's schema, made once per distinct schema. Called on the
        // delivering thread, where the view passed in is still valid.
        internal Schema OwnedSchema(IntPtr view)
        {
            if (view == IntPtr.Zero) return null;
            ulong hash = Native.rant_schema_hash(view);
            lock (_msgSchemaLock)
            {
                Schema owned;
                if (_msgSchemas.TryGetValue(hash, out owned)) return owned;
                IntPtr h = Native.rant_schema_copy(view, Codec.SchemaAlloc, IntPtr.Zero);
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

        /// <summary>One loop tick of a Manual node: discovery, receive, timers and queued
        /// sends. Blocks up to timeoutMs in the socket wait, 0 = non blocking. State under the
        /// service thread.</summary>
        public int Poll(int timeoutMs = 0)
        {
            return Native.rant_node_poll(_handle, timeoutMs);
        }

        /// <summary>Block until discovery and matching settle, so everything sent now reaches
        /// everyone. Call after creating the topics. timeoutMs &lt; 0 = 3 intervals.</summary>
        public bool Settle(int timeoutMs = -1) => Native.rant_node_settle(_handle, timeoutMs) == 1;

        internal IntPtr Handle => _handle;

        /// <summary>Dispatch every queued topic on the calling thread, waiting up to timeoutMs
        /// for any to hold data. Per frame in Unity, so every queued handler runs there.</summary>
        public int Dispatch(int maxMsgs = 0, int timeoutMs = 0)
            => Native.rant_node_dispatch(_handle, maxMsgs, timeoutMs);

        // ---- handles ----------------------------------------------------------------

        /// <summary>The publishing side of a topic. T's public fields are the schema, or the
        /// given one, and a byte[] T sends the message bytes as is.</summary>
        public Publisher<T> Publisher<T>(string name, Qos qos = null, Schema schema = null)
            => new Publisher<T>(this, name, qos, schema);

        /// <summary>The subscribing side of a topic. Add an OnMessage handler, or TryTake and
        /// Dispatch from a queue.</summary>
        public Subscriber<T> Subscriber<T>(string name, Qos qos = null, Schema schema = null)
            => new Subscriber<T>(this, name, qos, schema);

        /// <summary>Define a function, one definition per name on the mesh. The return value
        /// is the reply and a thrown exception answers AppError with its message.</summary>
        public FunctionDefinition<TReq, TRsp> FunctionDefinition<TReq, TRsp>(string name,
                Func<TReq, TRsp> handler, FunctionOptions options = null,
                Schema requestSchema = null, Schema responseSchema = null)
            => new FunctionDefinition<TReq, TRsp>(this, name, handler, options, requestSchema, responseSchema);

        /// <summary>The async form: the Task's result answers Ok and an exception AppError. It
        /// runs on the service thread until its first await.</summary>
        public FunctionDefinition<TReq, TRsp> FunctionDefinition<TReq, TRsp>(string name,
                Func<TReq, Task<TRsp>> handler, FunctionOptions options = null,
                Schema requestSchema = null, Schema responseSchema = null)
            => new FunctionDefinition<TReq, TRsp>(this, name, handler, options, requestSchema, responseSchema);

        /// <summary>The full form: the handler replies, fails or defers through the request.</summary>
        public FunctionDefinition<TReq, TRsp> FunctionDefinition<TReq, TRsp>(string name,
                Action<TReq, RantRequest<TRsp>> handler, FunctionOptions options = null,
                Schema requestSchema = null, Schema responseSchema = null)
            => new FunctionDefinition<TReq, TRsp>(this, name, handler, options, requestSchema, responseSchema);

        /// <summary>The caller side of a function defined on another node.</summary>
        public RemoteFunction<TReq, TRsp> RemoteFunction<TReq, TRsp>(string name,
                FunctionOptions options = null, Schema requestSchema = null, Schema responseSchema = null)
            => new RemoteFunction<TReq, TRsp>(this, name, options, requestSchema, responseSchema);

        /// <summary>Define a task: a long running call that streams progress and can be
        /// cancelled. The async handler's completion answers the call.</summary>
        public TaskDefinition<TReq, TPrg, TRsp> TaskDefinition<TReq, TPrg, TRsp>(string name,
                Func<TReq, TaskContext<TPrg>, Task<TRsp>> handler, TaskOptions options = null,
                Schema requestSchema = null, Schema progressSchema = null, Schema responseSchema = null)
            => new TaskDefinition<TReq, TPrg, TRsp>(this, name, handler, options,
                                                    requestSchema, progressSchema, responseSchema);

        /// <summary>The caller side of a task defined on another node.</summary>
        public RemoteTask<TReq, TPrg, TRsp> RemoteTask<TReq, TPrg, TRsp>(string name,
                TaskOptions options = null, Schema requestSchema = null, Schema progressSchema = null,
                Schema responseSchema = null)
            => new RemoteTask<TReq, TPrg, TRsp>(this, name, options, requestSchema, progressSchema, responseSchema);

        /// <summary>Own a variable: this node holds the value and publishes every applied
        /// write. One definition per name on the mesh.</summary>
        public VariableDefinition<T> VariableDefinition<T>(string name, VariableOptions options = null,
                                                           Schema schema = null)
            => new VariableDefinition<T>(this, name, false, default(T), options, schema);

        /// <summary>With the value it holds before any set.</summary>
        public VariableDefinition<T> VariableDefinition<T>(string name, T initial,
                                                           VariableOptions options = null, Schema schema = null)
            => new VariableDefinition<T>(this, name, true, initial, options, schema);

        /// <summary>A reference to a variable owned by another node: reads see the cached
        /// latest, writes go to the owner and come back as a change.</summary>
        public RemoteVariable<T> RemoteVariable<T>(string name, VariableOptions options = null,
                                                   Schema schema = null)
            => new RemoteVariable<T>(this, name, options, schema);

        // ---- built-in logs (the @rant/log/{error,warn,info} topics) --------------------

        /// <summary>Publish a log line at a level (already formatted text, truncated at
        /// RANT_LOG_MAX). SendStatus.NoSys when logs are disabled. Thread safe.</summary>
        public SendStatus Log(LogLevel level, string text)
        {
            byte[] b = Encoding.UTF8.GetBytes(text ?? "");
            return (SendStatus)Native.rant_node_log_text(_handle, (int)level, b, b.Length);
        }

        private Action<RantLogLine> _onLog;
        private bool _logBound;

        /// <summary>Every other node's log lines at every level as a RantLogLine, on the
        /// service or polling thread. Nothing arrives when logs are disabled on this node.</summary>
        public event Action<RantLogLine> OnLog
        {
            add
            {
                if (value == null) return;
                lock (_subLock)
                {
                    _onLog += value;
                    if (_logBound) return;
                    _logBound = true;
                }
                BindLog();
            }
            remove { lock (_subLock) _onLog -= value; }
        }

        // One subscription per level topic for the node's life, fanning into the event.
        private void BindLog()
        {
            foreach (LogLevel level in new[] { LogLevel.Error, LogLevel.Warn, LogLevel.Info })
            {
                IntPtr ch = Native.rant_node_log_topic(_handle, (int)level);
                if (ch == IntPtr.Zero) return;                                   // logs disabled
                if (Native.rant_topic_set_role(ch, (int)Role.PubSub) != 0) return;
                LogLevel lv = level;
                AddSubHandler(Native.rant_topic_index(ch), m =>
                {
                    Action<RantLogLine> h = _onLog;
                    if (h == null) return;
                    var f = m.Value as Dictionary<string, object>;
                    h(new RantLogLine
                    {
                        Level = lv, Node = m.PublisherName, NodeId = m.PublisherId, RecvUs = m.RecvUs,
                        WrittenUs = m.WrittenUs,
                        WallUs = LogFieldU(f, "wall_us"), MonoUs = LogFieldU(f, "mono_us"),
                        Text = f != null && f.TryGetValue("text", out var t) ? t as string ?? "" : "",
                    });
                });
            }
        }

        private static ulong LogFieldU(Dictionary<string, object> f, string k)
            => f != null && f.TryGetValue(k, out var o)
               ? (o is ulong u ? u : o is long l ? (ulong)l : 0UL) : 0UL;

        /// <summary>The mesh as this node sees it: peers, their entities, the folded mesh
        /// and the @rant/meta snapshots (docs/reflection.md).</summary>
        public RantReflection Reflection { get; }

        internal Type ClrTypeOf(ushort index)
        {
            Type t;
            _topicTypes.TryGetValue(index, out t);
            return t;
        }

        /// <summary>The most recent error this node reported (also delivered via OnEvent), or
        /// an Error event whose Error is None, printing "no error", before any.</summary>
        public RantEvent LastError => RantEvent.FromValue(Native.rant_last_error(_handle));

        // Why the most recent node open failed, from the process global slot.
        private static RantEvent LastOpenError() => RantEvent.FromValue(Native.rant_last_error(IntPtr.Zero));

        /// <summary>The node's counters, read at the call.</summary>
        public NodeStats Stats
        {
            get
            {
                UIntPtr u, p; ulong c, us; uint n;
                Native.rant_node_mem_stats(_handle, out u, out p, out c);
                Native.rant_node_backpressure_stats(_handle, out us, out n);
                return new NodeStats
                {
                    EvictedUnsent = Native.rant_node_evicted_unsent(_handle),
                    MemInUse = (ulong)u, MemPeak = (ulong)p, AllocCalls = c,
                    BackpressureWaitedUs = us, BackpressureWaits = n,
                };
            }
        }

        /// <summary>Stop the service thread and tear the node down. False when refused from a
        /// handler, where the node stays live: close from another thread.</summary>
        public bool Close(bool sendBye = true)
        {
            if (_handle != IntPtr.Zero)
            {
                if (Native.rant_node_close(_handle, sendBye ? 1 : 0) != 0) return false;
                _handle = IntPtr.Zero;
            }
            lock (s_reg) { var next = new Dictionary<long, RantNode>(s_nodes); next.Remove(_id); s_nodes = next; }
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
            // Not disposed: a RantMessage taken before Close may still decode against one.
            // Dropping the node's reference leaves each to its finalizer.
            lock (_msgSchemaLock) _msgSchemas.Clear();
            Codec.FreeCStr(_discGroup); _discGroup = IntPtr.Zero;
            Codec.FreeCStr(_mcastIf); _mcastIf = IntPtr.Zero;
            return true;
        }

        public void Dispose() { Close(); GC.SuppressFinalize(this); }
        ~RantNode() { try { Close(); } catch { } }

        [MonoPInvokeCallback(typeof(RantMsgFn))]
        private static void OnMessageTramp(IntPtr msgPtr)
        {
            try
            {
                var m = Marshal.PtrToStructure<RantMsg>(msgPtr);
                RantNode node; Type clr = null;
                s_nodes.TryGetValue((long)m.user, out node);
                if (node == null) return;
                var hs = node.SubHandlersOf(m.topic_index);
                if (hs == null) return;
                node._topicTypes.TryGetValue(m.topic_index, out clr);
                // the payload is copied and the schema is ours, so a later decode is safe
                var msg = RantMessage.FromNative(ref m, clr, node.OwnedSchema(m.schema));
                foreach (var h in hs) h(msg);
            }
            catch (Exception e) { Console.Error.WriteLine("rant on_message: " + e); }
        }

        [MonoPInvokeCallback(typeof(RantEventFn))]
        private static void OnEventTramp(IntPtr evPtr)
        {
            try
            {
                var e = Marshal.PtrToStructure<RantEventNative>(evPtr);
                RantNode node;
                s_nodes.TryGetValue((long)e.user, out node);
                if (node == null) return;
                Action<RantEvent> fn = node.OnEvent;
                if (fn == null) return;
                RantEvent ev = RantEvent.FromNative(evPtr, ref e);       // copied past the callback
                node.RunCallback(() => fn(ev));
            }
            catch (Exception ex) { Console.Error.WriteLine("rant on_event: " + ex); }
        }
    }

    /// <summary>The reflection walks of docs/reflection.md, from RantNode.Reflection. Every
    /// result is a copied snapshot, so it outlives the poll and needs no lock.</summary>
    public sealed class RantReflection
    {
        private readonly RantNode _node;
        internal RantReflection(RantNode node) { _node = node; }

        /// <summary>Every discovered peer, dropped ones included: gate on Active.</summary>
        public List<RantPeer> Peers()
        {
            var list = new List<RantPeer>();
            Native.rant_node_lock(_node.Handle);
            try
            {
                var it = new RantIter();
                RantPeerInfoNative p;
                while (Native.rant_node_peers_next(_node.Handle, ref it, out p) != 0) list.Add(RantPeer.Read(ref p));
            }
            finally { Native.rant_node_unlock(_node.Handle); }
            return list;
        }

        /// <summary>What one node offers, peer 0 for this one. A dropped peer's last known view
        /// is served as a ghost. Schemas need FetchDetails on the node.</summary>
        public List<RantEntity> Entities(uint peer = 0)
        {
            var list = new List<RantEntity>();
            Native.rant_node_lock(_node.Handle);
            try
            {
                var it = new RantIter();
                RantEntityInfoNative e;
                while (Native.rant_node_entities_next(_node.Handle, peer, ref it, out e) != 0)
                    list.Add(RantEntity.Read(ref e));
            }
            finally { Native.rant_node_unlock(_node.Handle); }
            return list;
        }

        /// <summary>The whole mesh folded: one entity per kind and name across every active
        /// peer and this node. Schemas need FetchDetails on the node.</summary>
        public List<RantEntity> Mesh()
        {
            var list = new List<RantEntity>();
            Native.rant_node_lock(_node.Handle);
            try
            {
                var it = new RantIter();
                RantEntityInfoNative e;
                while (Native.rant_node_mesh_next(_node.Handle, ref it, out e) != 0) list.Add(RantEntity.Read(ref e));
            }
            finally { Native.rant_node_unlock(_node.Handle); }
            return list;
        }

        /// <summary>One folded entity by kind and name, null when the mesh has none.</summary>
        public RantEntity Find(EntityKind kind, string name)
        {
            Native.rant_node_lock(_node.Handle);
            try
            {
                RantEntityInfoNative e;
                if (Native.rant_node_mesh_find(_node.Handle, (int)kind, Codec.CStr(name), out e) == 0) return null;
                return RantEntity.Read(ref e);
            }
            finally { Native.rant_node_unlock(_node.Handle); }
        }

        /// <summary>Bumps whenever the folded mesh view changed, so a tool knows when to walk
        /// again.</summary>
        public uint Epoch => Native.rant_node_mesh_epoch(_node.Handle);

        /// <summary>Fetch a peer's snapshot: a directed @rant/meta call decoded into a
        /// RantMetaSnapshot. The Task never faults. sections is a MetaSection mask.</summary>
        public async Task<RantMetaSnapshot> MetaAsync(uint peer, MetaSection sections = MetaSection.All)
        {
            // the local @rant/meta caller handle, node owned: callable, never created or
            // destroyed here, and absent when meta is disabled
            IntPtr fn = Native.rant_node_meta_function(_node.Handle);
            if (fn == IntPtr.Zero) return new RantMetaSnapshot { Status = CallStatus.NoHandler };
            byte[] req = sections == MetaSection.All
                ? Array.Empty<byte>() : BitConverter.GetBytes((uint)sections);
            ResponseCore r = await new RemoteFunctionCore(_node, fn).CallAsync(req, peer).ConfigureAwait(false);
            return RantMetaSnapshot.FromResponse(r);
        }
    }

    // ---- patterns: shared plumbing ----------------------------------------------

    // Pins a byte[] for the duration of one native call ({NULL,0} for null/empty).
    internal struct PinnedBytes : IDisposable
    {
        private GCHandle _h;
        internal RantBytes B;
        internal PinnedBytes(byte[] d)
        {
            if (d == null || d.Length == 0)
            {
                _h = default(GCHandle);
                B = new RantBytes { data = IntPtr.Zero, len = UIntPtr.Zero };
            }
            else
            {
                _h = GCHandle.Alloc(d, GCHandleType.Pinned);
                B = new RantBytes { data = _h.AddrOfPinnedObject(), len = (UIntPtr)d.Length };
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
            public Action<RequestCore> Handler;
            public IntPtr Fn;   // set right after create (the callback cannot fire before poll)
            public RantNode Node;
        }
        internal sealed class VarBox
        {
            public Action<VariableUpdate> Handler;
            public RantNode Node;
        }
        internal sealed class AsyncCall
        {
            public TaskCompletionSource<ResponseCore> Tcs;
            public RantNode RantNode;
            public Action<ProgressCore> OnProgress;   // task calls only, else null
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
                        catch (Exception e) { Console.Error.WriteLine("rant on_cancel: " + e); }
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
            if (c != null) c.Tcs.TrySetResult(new ResponseCore { Status = CallStatus.Cancelled, Message = "cancelled" });
        }

        // rooted delegates handed to native code
        internal static readonly RantRequestFn OnRequest = OnRequestTramp;
        internal static readonly RantResponseFn OnResponse = OnResponseTramp;
        internal static readonly RantVariableUpdateFn OnVarUpdate = OnVarUpdateTramp;
        internal static readonly RantProgressFn OnProgress = OnProgressTramp;
        internal static readonly RantCancelFn OnCancel = OnCancelTramp;

        // A thrown handler's response message.
        internal static string FailText(Exception e)
            => string.IsNullOrEmpty(e.Message) ? "handler threw" : e.Message;

        [MonoPInvokeCallback(typeof(RantRequestFn))]
        private static void OnRequestTramp(IntPtr reqPtr, IntPtr user)
        {
            try
            {
                var box = GetBox((long)user) as RequestBox;
                if (box == null) return;
                var r = new RequestCore(reqPtr, box.Fn);
                // The handler runs after this callback returns, where the native request is
                // dead, so park the reply now and answer through the DeferredCore instead.
                if (box.Node != null && box.Node.Dispatcher != null)
                {
                    r.Rebind(r.Defer());
                    RequestBox b = box;
                    box.Node.RunCallback(() =>
                    {
                        try { b.Handler(r); }
                        catch (Exception e)
                        {
                            r.FailQuiet(string.IsNullOrEmpty(e.Message) ? "handler threw" : e.Message);
                            Console.Error.WriteLine("rant on_request: " + e);
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
                    Console.Error.WriteLine("rant on_request: " + e);
                }
                finally { r.Expire(); }
            }
            catch (Exception e) { Console.Error.WriteLine("rant on_request: " + e); }
        }

        [MonoPInvokeCallback(typeof(RantResponseFn))]
        private static void OnResponseTramp(IntPtr rspPtr)
        {
            try
            {
                var o = Marshal.PtrToStructure<RantResponseNative>(rspPtr);
                AsyncCall call = TakeAsync((long)o.user);
                if (call == null) return;
                call.RantNode.UnregisterAsync((long)o.user);
                var r = new ResponseCore
                {
                    Status = (CallStatus)o.status,
                    Provider = o.provider,
                    WrittenUs = o.written_us,
                    SchemaPtr = o.schema,
                    Data = Codec.Bytes(o.data),   // copied out: the view dies with the callback
                    Message = Codec.Str(o.message),
                };
                if (call.RantNode != null) call.RantNode.RunCallback(() => call.Tcs.TrySetResult(r));
                else call.Tcs.TrySetResult(r);
            }
            catch (Exception e) { Console.Error.WriteLine("rant on_response: " + e); }
        }

        [MonoPInvokeCallback(typeof(RantProgressFn))]
        private static void OnProgressTramp(IntPtr prgPtr)
        {
            try
            {
                var p = Marshal.PtrToStructure<RantProgressNative>(prgPtr);
                AsyncCall call = PeekAsync((long)p.user);
                if (call == null || call.OnProgress == null) return;
                var prg = new ProgressCore
                {
                    CallId = p.call_id,
                    Provider = p.provider,
                    Value = (ulong)p.data.len != 0 ? Codec.Bytes(p.data) : null,
                    WrittenUs = p.written_us,
                    RecvUs = p.recv_us,
                    SchemaPtr = p.schema,
                };
                Action<ProgressCore> sink = call.OnProgress;
                if (call.RantNode != null) call.RantNode.RunCallback(() => sink(prg));
                else sink(prg);
            }
            catch (Exception e) { Console.Error.WriteLine("rant on_progress: " + e); }
        }

        [MonoPInvokeCallback(typeof(RantCancelFn))]
        private static void OnCancelTramp(ulong token, IntPtr user)
        {
            try
            {
                var box = GetBox((long)user) as TaskCancelBox;
                if (box != null) box.Cancel(token);
            }
            catch (Exception e) { Console.Error.WriteLine("rant on_cancel: " + e); }
        }

        [MonoPInvokeCallback(typeof(RantVariableUpdateFn))]
        private static void OnVarUpdateTramp(IntPtr updPtr, IntPtr user)
        {
            try
            {
                var box = GetBox((long)user) as VarBox;
                if (box == null) return;
                var u = Marshal.PtrToStructure<RantVariableUpdateNative>(updPtr);
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
            catch (Exception e) { Console.Error.WriteLine("rant on_variable_update: " + e); }
        }

        // The bytes a typed handle sends for a value: T encoded with its schema, or a byte[]
        // T as is (the encoded message).
        internal static byte[] Encode<T>(Schema s, T value)
            => Codec.IsRaw(typeof(T)) ? (byte[])(object)value ?? Array.Empty<byte>() : s.Encode(value);

        // A delivered message as T: the decoded value, or the bytes as is for a byte[] T.
        internal static bool TryValue<T>(RantMessage m, out T value)
        {
            if (Codec.IsRaw(typeof(T))) { value = (T)(object)m.Data; return true; }
            if (m.Value is T v) { value = v; return true; }
            value = default(T);
            return false;
        }

        // Decode wire bytes to a typed value: prefer the wire schema (the publisher's
        // layout bound to ours), fall back to the local one. A byte[] target takes the bytes.
        internal static bool TryDecode(Schema local, IntPtr wireSchema, byte[] data, Type t, out object v)
        {
            v = null;
            if (Codec.IsRaw(t)) { v = data; return true; }
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

    // The request seen by a definition handler, valid only inside the callback. Reply there,
    // or Defer() and complete later. No reply acknowledges Ok.
    internal sealed class RequestCore
    {
        private IntPtr _ptr;          // the exact native pointer, zeroed when the callback returns
        private readonly IntPtr _fn;
        private DeferredCore _deferred;   // set when the reply was parked for another thread
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

        internal RequestCore(IntPtr ptr, IntPtr fn)
        {
            _ptr = ptr;
            _fn = fn;
            var r = Marshal.PtrToStructure<RantRequestNative>(ptr);
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
            using (var p = new PinnedBytes(rsp)) Native.rant_request_reply(_ptr, p.B);
        }

        /// <summary>Answer AppError. message is the text shown on the caller, truncated at 255
        /// bytes, empty = the default. rsp may still carry structured failure data.</summary>
        public void Fail(string message = null, byte[] rsp = null)
        {
            Guard();
            _done = true;
            if (_deferred != null) { _deferred.Fail(message, rsp); return; }
            using (var p = new PinnedBytes(rsp))
                Native.rant_request_fail(_ptr, string.IsNullOrEmpty(message) ? null : Codec.CStr(message), p.B);
        }

        /// <summary>Park the reply and return now. The DeferredCore completes the call later from
        /// any thread.</summary>
        public DeferredCore Defer()
        {
            Guard();
            _done = true;
            if (_deferred != null) return _deferred;
            ulong token = Native.rant_request_defer(_ptr);
            return new DeferredCore(_fn, token);
        }

        // The reply is parked so the handler can run on another thread. Every field is already
        // copied out, so only the answer has to move off the dead native pointer.
        internal void Rebind(DeferredCore d) { _ptr = IntPtr.Zero; _deferred = d; _done = false; }

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

    // A parked reply (from RequestCore.Defer): complete exactly once, from any thread.
    // Dropping it leaves the caller to its timeout.
    internal sealed class DeferredCore
    {
        private readonly IntPtr _fn;
        private long _token;

        internal DeferredCore(IntPtr fn, ulong token) { _fn = fn; _token = (long)token; }

        public bool Valid => _fn != IntPtr.Zero && Interlocked.Read(ref _token) != 0;

        /// <summary>message as in RequestCore.Fail, also carried on Ok as debug text.</summary>
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
                return Native.rant_function_complete(_fn, (ulong)token, (int)status,
                    string.IsNullOrEmpty(message) ? null : Codec.CStr(message), p.B) == 0;
        }
    }

    // The engine of FunctionDefinition<TReq, TRsp>: one reply per call, one definition per
    // name. A null handler answers NoHandler.
    internal sealed class FunctionDefinitionCore : INodeHandle
    {
        internal IntPtr Fn;   // zeroed by Dispose, and by the node at Close
        internal readonly RantNode RantNode;

        void INodeHandle.Invalidate() { Fn = IntPtr.Zero; }

        public FunctionDefinitionCore(RantNode node, string name, Schema requestSchema, Schema responseSchema,
                                  Action<RequestCore> handler, FunctionOptions options)
        {
            RantNode = node;
            RantFunctionOpts co = (options ?? new FunctionOptions()).ToNative();
            long id = 0;
            Patterns.RequestBox box = null;
            if (handler != null)
            {
                box = new Patterns.RequestBox { Handler = handler, Node = node };
                id = Patterns.AddBox(box);
            }
            Fn = Native.rant_node_create_function_definition(node.Handle, Codec.CStr(name),
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
        public FunctionDefinitionCore(RantNode node, string name, Schema requestSchema, Schema responseSchema,
                                  Func<RequestCore, Task<byte[]>> handler, FunctionOptions options)
            : this(node, name, requestSchema, responseSchema, AsyncAdapter(handler), options) { }

        // Defer FIRST (a continuation may finish before the invocation returns), then the
        // Task's completion answers through the DeferredCore.
        internal static Action<RequestCore> AsyncAdapter(Func<RequestCore, Task<byte[]>> handler)
        {
            if (handler == null) return null;
            return r =>
            {
                DeferredCore d = r.Defer();
                Task<byte[]> t;
                try { t = handler(r); }
                catch (Exception e) { d.Fail(Patterns.FailText(e)); return; }
                _ = FinishAsync(t, d);
            };
        }

        private static async Task FinishAsync(Task<byte[]> t, DeferredCore d)
        {
            try { d.Complete(await t.ConfigureAwait(false)); }
            catch (Exception e) { d.Fail(Patterns.FailText(e)); }
        }

        public int MatchCount => Native.rant_function_match_count(Fn);

        // Retire the handle: park its channels and release the name. No op once dead.
        public void Dispose()
        {
            if (Fn == IntPtr.Zero) return;
            var rc = (SendStatus)Native.rant_function_retire(Fn);
            if (rc != SendStatus.Ok) throw new InvalidOperationException("retire refused: " + rc);
            Fn = IntPtr.Zero;
        }

        public bool Refresh() => Fn != IntPtr.Zero && Native.rant_function_refresh(Fn) == 1;
    }

    // An owning call outcome, the payload copied out. SendStatus carries a synchronous
    // refusal, and Status stays Timeout then.
    internal sealed class ResponseCore
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

    // The engine of RemoteFunction<TReq, TRsp>, and the @rant/meta caller.
    internal sealed class RemoteFunctionCore : INodeHandle
    {
        internal IntPtr Fn;   // zeroed by Dispose, and by the node at Close
        internal readonly RantNode RantNode;

        void INodeHandle.Invalidate() { Fn = IntPtr.Zero; }

        public RemoteFunctionCore(RantNode node, string name, Schema requestSchema, Schema responseSchema,
                              FunctionOptions options)
        {
            RantNode = node;
            RantFunctionOpts co = (options ?? new FunctionOptions()).ToNative();
            Fn = Native.rant_node_create_remote_function(node.Handle, Codec.CStr(name),
                requestSchema != null ? requestSchema.Handle : IntPtr.Zero,
                responseSchema != null ? responseSchema.Handle : IntPtr.Zero, ref co);
            if (Fn == IntPtr.Zero)
                throw new InvalidOperationException("remote function create failed: " + node.LastError);
            node.RetainSchema(requestSchema);
            node.RetainSchema(responseSchema);
            node.RegisterHandle(this);
        }

        // Wrap an existing node-owned function handle (the @rant/meta endpoint): callable,
        // never created or destroyed here.
        internal RemoteFunctionCore(RantNode node, IntPtr fn)
        {
            RantNode = node; Fn = fn;
            node.RegisterHandle(this);
        }

        // Pin a RantCallOpts for one native call (IntPtr.Zero when undirected).
        private static GCHandle OptsHandle(uint provider, out IntPtr ptr)
        {
            if (provider == 0) { ptr = IntPtr.Zero; return default(GCHandle); }
            var g = GCHandle.Alloc(new RantCallOpts[] { new RantCallOpts { provider = provider } },
                                   GCHandleType.Pinned);
            ptr = g.AddrOfPinnedObject();
            return g;
        }

        /// <summary>Blocking call: waits for the response or timeoutMs, negative = the default,
        /// on the service thread's progress under Start() and driving the loop otherwise.
        /// Refused from a callback. Never throws.</summary>
        public ResponseCore Call(byte[] request, int timeoutMs = -1, uint provider = 0)
        {
            var r = new ResponseCore();
            RantResponseNative o;
            int rc;
            GCHandle og = OptsHandle(provider, out IntPtr optp);
            try
            {
                using (var p = new PinnedBytes(request))
                    rc = Native.rant_function_call(Fn, p.B, out o, timeoutMs, optp);
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
        public Task<ResponseCore> CallAsync(byte[] request, uint provider = 0)
        {
            // A dispatcher already completes on the thread the caller chose, so continuations
            // belong there. With none, keep them off the polling thread.
            var tcs = new TaskCompletionSource<ResponseCore>(
                RantNode.Dispatcher != null ? TaskCreationOptions.None
                                                    : TaskCreationOptions.RunContinuationsAsynchronously);
            long id = Patterns.AddAsync(new Patterns.AsyncCall { Tcs = tcs, RantNode = RantNode });
            RantNode.RegisterAsync(id);
            int rc;
            GCHandle og = OptsHandle(provider, out IntPtr optp);
            try
            {
                using (var p = new PinnedBytes(request))
                    rc = Native.rant_function_call_async(Fn, p.B, Patterns.OnResponse, (IntPtr)id, optp);
            }
            finally { if (og.IsAllocated) og.Free(); }
            if (rc != 0)
            {
                Patterns.TakeAsync(id);
                RantNode.UnregisterAsync(id);
                tcs.TrySetResult(new ResponseCore { SendStatus = (SendStatus)rc });
            }
            return tcs.Task;
        }

        public int MatchCount => Native.rant_function_match_count(Fn);

        // Retire the handle: park its channels and release the name. No op once dead.
        public void Dispose()
        {
            if (Fn == IntPtr.Zero) return;
            var rc = (SendStatus)Native.rant_function_retire(Fn);
            if (rc != SendStatus.Ok) throw new InvalidOperationException("retire refused: " + rc);
            Fn = IntPtr.Zero;
        }

        public bool Refresh() => Fn != IntPtr.Zero && Native.rant_function_refresh(Fn) == 1;
    }

    // ---- patterns: tasks --------------------------------------------------------

    // What a task handler works through: stream progress, observe cancellation. Thread safe
    // across awaits. Once the call completed, Progress returns State.
    internal sealed class TaskContextCore
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

        internal TaskContextCore(IntPtr fn, ulong token, CancellationToken ct, RequestCore r)
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
                return (SendStatus)Native.rant_function_progress(_fn, _token, p.B);
        }

        /// <summary>Convenience view of CancellationToken (with the native flag as a
        /// backstop).</summary>
        public bool Cancelled => CancellationToken.IsCancellationRequested
            || Native.rant_function_cancelled(_fn, _token) == 1;
    }

    // The engine of TaskDefinition<TReq, TPrg, TRsp>. The handler is an async delegate whose
    // completion answers the call. Null answers NoHandler.
    internal sealed class TaskDefinitionCore : INodeHandle
    {
        internal IntPtr Fn;   // zeroed by Dispose, and by the node at Close
        internal readonly RantNode RantNode;

        void INodeHandle.Invalidate() { Fn = IntPtr.Zero; }

        public TaskDefinitionCore(RantNode node, string name, Schema requestSchema, Schema progressSchema,
                              Schema responseSchema, Func<RequestCore, TaskContextCore, Task<byte[]>> handler,
                              TaskOptions options)
        {
            RantNode = node;
            RantTaskOpts co = (options ?? new TaskOptions()).ToNative();
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
            Fn = Native.rant_node_create_task_definition(node.Handle, Codec.CStr(name),
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
                Native.rant_function_on_cancel(Fn, Patterns.OnCancel, (IntPtr)cancelId);
            }
            node.RetainSchema(requestSchema);
            node.RetainSchema(progressSchema);
            node.RetainSchema(responseSchema);
            node.RegisterHandle(this);
        }

        // Poll thread: defer, which implies RUNNING, arm the per call CancellationTokenSource,
        // invoke the async delegate. Wherever its completion lands answers the call.
        private static void RunCall(Patterns.RequestBox box, Patterns.TaskCancelBox cancels,
                                    Func<RequestCore, TaskContextCore, Task<byte[]>> handler, RequestCore r)
        {
            DeferredCore d = r.Defer();
            ulong token = d.Token;
            var cts = new CancellationTokenSource();
            cancels.Add(token, cts);
            var ctx = new TaskContextCore(box.Fn, token, cts.Token, r);
            Task<byte[]> t;
            try { t = handler(r, ctx); }
            catch (OperationCanceledException) { cancels.Drop(token); d.CompleteCancelled(); return; }
            catch (Exception e) { cancels.Drop(token); d.Fail(Patterns.FailText(e)); return; }
            _ = FinishCall(t, d, cancels, token);
        }

        private static async Task FinishCall(Task<byte[]> t, DeferredCore d,
                                             Patterns.TaskCancelBox cancels, ulong token)
        {
            try { d.Complete(await t.ConfigureAwait(false)); }
            catch (OperationCanceledException) { d.CompleteCancelled(); }
            catch (Exception e) { d.Fail(Patterns.FailText(e)); }
            finally { cancels.Drop(token); }
        }

        public int MatchCount => Native.rant_function_match_count(Fn);

        // Retire the handle: park its channels and release the name. No op once dead.
        public void Dispose()
        {
            if (Fn == IntPtr.Zero) return;
            var rc = (SendStatus)Native.rant_function_retire(Fn);
            if (rc != SendStatus.Ok) throw new InvalidOperationException("retire refused: " + rc);
            Fn = IntPtr.Zero;
        }

        public bool Refresh() => Fn != IntPtr.Zero && Native.rant_function_refresh(Fn) == 1;
    }

    // One task progress update as delivered. Value is the payload copied out, null = the
    // RUNNING acknowledgment.
    internal sealed class ProgressCore
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

    // The engine of RemoteTask<TReq, TPrg, TRsp>. A request is always directed at one
    // provider, and the timeout bounds only the first response.
    internal sealed class RemoteTaskCore : INodeHandle
    {
        internal IntPtr Fn;   // zeroed by Dispose, and by the node at Close
        internal readonly RantNode RantNode;

        void INodeHandle.Invalidate() { Fn = IntPtr.Zero; }

        public RemoteTaskCore(RantNode node, string name, Schema requestSchema, Schema progressSchema,
                          Schema responseSchema, TaskOptions options)
        {
            RantNode = node;
            RantTaskOpts co = (options ?? new TaskOptions()).ToNative();
            Fn = Native.rant_node_create_remote_task(node.Handle, Codec.CStr(name),
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

        // Start the task: the Task completes with the terminal outcome and never faults. sink
        // fires per update, null Value for RUNNING. The token cancels through Cancel.
        internal Task<ResponseCore> CallCore(byte[] request, out uint callId, Action<ProgressCore> sink,
                                             CancellationToken cancellationToken, uint provider)
        {
            // A dispatcher already completes on the thread the caller chose, so continuations
            // belong there. With none, keep them off the polling thread.
            var tcs = new TaskCompletionSource<ResponseCore>(
                RantNode.Dispatcher != null ? TaskCreationOptions.None
                                                    : TaskCreationOptions.RunContinuationsAsynchronously);
            long id = Patterns.AddAsync(new Patterns.AsyncCall
            {
                Tcs = tcs, RantNode = RantNode, OnProgress = sink,
            });
            RantNode.RegisterAsync(id);
            var opts = new RantCallOpts[1];
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
                    rc = Native.rant_function_call_async(Fn, p.B, Patterns.OnResponse, (IntPtr)id,
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
                RantNode.UnregisterAsync(id);
                tcs.TrySetResult(new ResponseCore { SendStatus = (SendStatus)rc });
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

        // Cooperative and never acked, the terminal status answers. BadRole when the provider
        // declared noCancel, State if done.
        public SendStatus Cancel(uint callId) => (SendStatus)Native.rant_function_cancel(Fn, callId);

        public int MatchCount => Native.rant_function_match_count(Fn);

        // Retire the handle: park its channels and release the name. No op once dead.
        public void Dispose()
        {
            if (Fn == IntPtr.Zero) return;
            var rc = (SendStatus)Native.rant_function_retire(Fn);
            if (rc != SendStatus.Ok) throw new InvalidOperationException("retire refused: " + rc);
            Fn = IntPtr.Zero;
        }

        public bool Refresh() => Fn != IntPtr.Zero && Native.rant_function_refresh(Fn) == 1;
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

    // The engine of Variable<T>, both sides: definition = true holds the authoritative value,
    // false caches the owner's latest and writes over the set channel.
    internal sealed class VariableCore : INodeHandle
    {
        internal IntPtr Var;   // zeroed by Dispose, and by the node at Close
        internal readonly RantNode RantNode;

        void INodeHandle.Invalidate() { Var = IntPtr.Zero; }
        internal readonly string Name;

        internal VariableCore(RantNode node, string name, Schema schema, byte[] initial,
                                    VariableOptions options, bool definition)
        {
            RantNode = node;
            Name = name;
            RantVariableOpts co = (options ?? new VariableOptions()).ToNative();
            using (var p = new PinnedBytes(initial))
            {
                co.initial = p.B;
                Var = definition
                    ? Native.rant_node_create_variable_definition(node.Handle, Codec.CStr(name),
                          schema != null ? schema.Handle : IntPtr.Zero, ref co)
                    : Native.rant_node_create_remote_variable(node.Handle, Codec.CStr(name),
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
            Native.rant_node_lock(RantNode.Handle);
            try
            {
                RantBytes b;
                if (Native.rant_variable_get(Var, out b) != 1) return false;
                value = Codec.Bytes(b);
                return true;
            }
            finally { Native.rant_node_unlock(RantNode.Handle); }
        }

        /// <summary>Set the value: apply and publish, or send over the set channel. BadRole =
        /// the owner advertises no set channel.</summary>
        public SendStatus Set(byte[] value)
        {
            using (var p = new PinnedBytes(value)) return (SendStatus)Native.rant_variable_set(Var, p.B);
        }

        /// <summary>Force the value: writes are absorbed into the shadow source until Unforce
        /// restores the latest absorbed set. Needs allowForce on the definition: State on the
        /// owner without it, BadRole on a remote whose owner advertises none.</summary>
        public SendStatus Force(byte[] value)
        {
            using (var p = new PinnedBytes(value)) return (SendStatus)Native.rant_variable_force(Var, p.B);
        }
        public SendStatus Unforce() => (SendStatus)Native.rant_variable_unforce(Var);
        public bool Forced => Native.rant_variable_forced(Var) == 1;

        public int MatchCount => Native.rant_variable_match_count(Var);

        /// <summary>Block until a value exists or timeoutMs elapses, on the service thread's
        /// progress under Start() and driving the loop otherwise. Refused from a callback.</summary>
        public bool Wait(int timeoutMs) => Native.rant_variable_wait(Var, timeoutMs) == 1;

        private bool _changeBound, _writeBound;
        // the last change delivered, replayed to a handler added after the first
        internal VariableUpdate LastChange;

        // One native registration per kind for the handle's life, fanning into the typed
        // events. True when this call bound it: the C then replayed the current value into a
        // change binding before returning, so a later handler is replayed LastChange instead.
        internal bool Bind(bool change, Action<VariableUpdate> fan)
        {
            if (change ? _changeBound : _writeBound) return false;
            Action<VariableUpdate> h = change ? (u => { LastChange = u; fan(u); }) : fan;
            long id = Patterns.AddBox(new Patterns.VarBox { Handler = h, Node = RantNode });
            RantNode.RegisterPatternBox(id);
            if (change) { Native.rant_variable_on_change(Var, Patterns.OnVarUpdate, (IntPtr)id); _changeBound = true; }
            else { Native.rant_variable_on_write(Var, Patterns.OnVarUpdate, (IntPtr)id); _writeBound = true; }
            return true;
        }

        // Retire the handle: park its channels and release the name. No op once dead.
        public void Dispose()
        {
            if (Var == IntPtr.Zero) return;
            var rc = (SendStatus)Native.rant_variable_retire(Var);
            if (rc != SendStatus.Ok) throw new InvalidOperationException("retire refused: " + rc);
            Var = IntPtr.Zero;
        }

        /// <summary>A reflectFromMesh handle: re type every channel in place when the mesh
        /// moved. True when it was re typed. See docs/reflection.md.</summary>
        public bool Refresh() => Var != IntPtr.Zero && Native.rant_variable_refresh(Var) == 1;
    }

    // ---- the typed handles --------------------------------------------------------

    /// <summary>The request inside a full form function handler: reply with a typed value,
    /// Fail, or Defer. Valid only inside the handler callback.</summary>
    public sealed class RantRequest<TRsp>
    {
        private readonly RequestCore _core;
        private readonly Schema _rsp;

        internal RantRequest(RequestCore core, Schema rsp) { _core = core; _rsp = rsp; }

        /// <summary>The request bytes as sent.</summary>
        public byte[] Data => _core.Data;
        /// <summary>The calling peer's id and node name.</summary>
        public uint Caller => _core.Caller;
        public string CallerName => _core.CallerName;
        public string FunctionName => _core.FunctionName;
        public ulong RecvUs => _core.RecvUs;
        /// <summary>The caller's wall clock when it sent the request, 0 = unstamped.</summary>
        public ulong WrittenUs => _core.WrittenUs;
        /// <summary>True once Reply, Fail or Defer has been called.</summary>
        public bool Answered => _core.Answered;

        /// <summary>Answer CallStatus.Ok with value.</summary>
        public void Reply(TRsp value) => _core.Reply(Patterns.Encode(_rsp, value));
        /// <summary>Answer AppError. message is the text the caller sees, empty = the default.</summary>
        public void Fail(string message = null) => _core.Fail(message);
        /// <summary>Park the reply and return now. The Deferred completes the call later from
        /// any thread.</summary>
        public Deferred<TRsp> Defer() => new Deferred<TRsp>(_core.Defer(), _rsp);
    }

    /// <summary>A parked reply from RantRequest&lt;TRsp&gt;.Defer: Complete or Fail exactly once,
    /// from any thread. Dropping it leaves the caller to its timeout.</summary>
    public sealed class Deferred<TRsp>
    {
        private readonly DeferredCore _core;
        private readonly Schema _rsp;

        internal Deferred(DeferredCore core, Schema rsp) { _core = core; _rsp = rsp; }

        /// <summary>False once completed, or after the node closed.</summary>
        public bool Valid => _core.Valid;
        /// <summary>Answer Ok with value. message rides along as debug text.</summary>
        public bool Complete(TRsp value, string message = null) => _core.Complete(Patterns.Encode(_rsp, value), message);
        /// <summary>Answer AppError with message as the text the caller sees.</summary>
        public bool Fail(string message = null) => _core.Fail(message);
    }

    /// <summary>The implementation of a function, from RantNode.FunctionDefinition. One
    /// definition per name on the mesh. A byte[] type carries the message bytes as is.</summary>
    public sealed class FunctionDefinition<TReq, TRsp> : IDisposable
    {
        private readonly FunctionDefinitionCore _core;
        private readonly Schema _req, _rsp;

        internal FunctionDefinition(RantNode node, string name, Func<TReq, TRsp> handler,
                                    FunctionOptions options, Schema requestSchema, Schema responseSchema)
        {
            _req = Schema.For(typeof(TReq), requestSchema);
            _rsp = Schema.For(typeof(TRsp), responseSchema);
            Action<RequestCore> h = null;
            if (handler != null)
            {
                Schema req = _req, rsp = _rsp;
                h = r =>
                {
                    object q;
                    if (!Patterns.TryDecode(req, r.SchemaPtr, r.Data, typeof(TReq), out q)) { r.Fail("request decode failed"); return; }
                    TRsp outv = handler((TReq)q);   // a throw answers AppError (trampoline catch)
                    if (!r.Answered) r.Reply(Patterns.Encode(rsp, outv));
                };
            }
            _core = new FunctionDefinitionCore(node, name, _req, _rsp, h, options);
        }

        internal FunctionDefinition(RantNode node, string name, Func<TReq, Task<TRsp>> handler,
                                    FunctionOptions options, Schema requestSchema, Schema responseSchema)
        {
            _req = Schema.For(typeof(TReq), requestSchema);
            _rsp = Schema.For(typeof(TRsp), responseSchema);
            Func<RequestCore, Task<byte[]>> h = null;
            if (handler != null)
            {
                Schema req = _req, rsp = _rsp;
                h = async r =>
                {
                    object q;
                    if (!Patterns.TryDecode(req, r.SchemaPtr, r.Data, typeof(TReq), out q))
                        throw new Exception("request decode failed");
                    TRsp outv = await handler((TReq)q).ConfigureAwait(false);
                    return Patterns.Encode(rsp, outv);
                };
            }
            _core = new FunctionDefinitionCore(node, name, _req, _rsp, h, options);
        }

        internal FunctionDefinition(RantNode node, string name, Action<TReq, RantRequest<TRsp>> handler,
                                    FunctionOptions options, Schema requestSchema, Schema responseSchema)
        {
            _req = Schema.For(typeof(TReq), requestSchema);
            _rsp = Schema.For(typeof(TRsp), responseSchema);
            Action<RequestCore> h = null;
            if (handler != null)
            {
                Schema req = _req, rsp = _rsp;
                h = r =>
                {
                    object q;
                    if (!Patterns.TryDecode(req, r.SchemaPtr, r.Data, typeof(TReq), out q)) { r.Fail("request decode failed"); return; }
                    handler((TReq)q, new RantRequest<TRsp>(r, rsp));
                };
            }
            _core = new FunctionDefinitionCore(node, name, _req, _rsp, h, options);
        }

        /// <summary>Callers currently matched to this definition.</summary>
        public int MatchCount => _core.MatchCount;
        /// <summary>A ReflectFromMesh handle: re type in place when the mesh moved. True when
        /// it was re typed. See docs/reflection.md.</summary>
        public bool Refresh() => _core.Refresh();
        /// <summary>Retire the definition and release the name. Refused from a callback.</summary>
        public void Dispose() => _core.Dispose();
    }

    /// <summary>A call outcome. Reading Value when the call did not complete Ok throws
    /// CallException. Status never throws.</summary>
    public sealed class RantResponse<TRsp>
    {
        internal ResponseCore Core;
        internal Schema RspSchema;

        public CallStatus Status => Core.Status;
        public bool Ok => Core.Ok;
        /// <summary>The peer that answered.</summary>
        public uint Provider => Core.Provider;
        /// <summary>The provider's wall clock when it sent the response, 0 = synthesized.</summary>
        public ulong WrittenUs => Core.WrittenUs;
        /// <summary>A synchronous refusal of the send. Status stays Timeout then.</summary>
        public SendStatus SendStatus => Core.SendStatus;
        /// <summary>The outcome text to display on a failure: the definition's message, else
        /// the default status text.</summary>
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

    /// <summary>The caller side of a function defined on another node, from
    /// RantNode.RemoteFunction. A byte[] type carries the message bytes as is, with an
    /// explicit Schema or none.</summary>
    public sealed class RemoteFunction<TReq, TRsp> : IDisposable
    {
        private readonly RemoteFunctionCore _core;
        private readonly Schema _req, _rsp;

        internal RemoteFunction(RantNode node, string name, FunctionOptions options,
                                Schema requestSchema, Schema responseSchema)
        {
            _req = Schema.For(typeof(TReq), requestSchema);
            _rsp = Schema.For(typeof(TRsp), responseSchema);
            _core = new RemoteFunctionCore(node, name, _req, _rsp, options);
        }

        /// <summary>Blocking call: waits for the response or timeoutMs, negative = the default,
        /// on the service thread's progress or driving a Manual node's loop. Refused from a
        /// callback. Never throws. provider directs it at one definition by peer id, 0 =
        /// undirected, first answer wins.</summary>
        public RantResponse<TRsp> Call(TReq request, int timeoutMs = -1, uint provider = 0)
            => new RantResponse<TRsp> { Core = _core.Call(Patterns.Encode(_req, request), timeoutMs, provider), RspSchema = _rsp };

        /// <summary>Async call: the Task completes with the outcome and never faults, inspect
        /// Status. Continuations run off the service thread.</summary>
        public async Task<RantResponse<TRsp>> CallAsync(TReq request, uint provider = 0)
        {
            ResponseCore core = await _core.CallAsync(Patterns.Encode(_req, request), provider).ConfigureAwait(false);
            return new RantResponse<TRsp> { Core = core, RspSchema = _rsp };
        }

        /// <summary>Definitions currently matched, 0 = no provider present.</summary>
        public int MatchCount => _core.MatchCount;
        /// <summary>A ReflectFromMesh handle: re type in place when the mesh moved. True when
        /// it was re typed. See docs/reflection.md.</summary>
        public bool Refresh() => _core.Refresh();
        /// <summary>Retire the remote: every outstanding call completes Cancelled. Refused
        /// from a callback.</summary>
        public void Dispose() => _core.Dispose();
    }

    /// <summary>What a task handler works through: stream typed Progress and observe
    /// cancellation. Thread safe across awaits.</summary>
    public sealed class TaskContext<TPrg>
    {
        private readonly TaskContextCore _core;
        private readonly Schema _prg;

        internal TaskContext(TaskContextCore core, Schema prg) { _core = core; _prg = prg; }

        /// <summary>Broadcast one progress update. A reliable subscriber backpressures end to
        /// end. State once the call completed.</summary>
        public SendStatus Progress(TPrg value) => _core.Progress(Patterns.Encode(_prg, value));
        /// <summary>Cancelled the moment a cancel arrives. Honor it by throwing
        /// OperationCanceledException, or run to completion anyway.</summary>
        public CancellationToken CancellationToken => _core.CancellationToken;
        public bool Cancelled => _core.Cancelled;
        public uint Caller => _core.Caller;
        public string CallerName => _core.CallerName;
        public ulong RecvUs => _core.RecvUs;
        public ulong WrittenUs => _core.WrittenUs;
    }

    /// <summary>The implementation of a task, from RantNode.TaskDefinition: a long running
    /// call that streams progress and can be cancelled. The async handler's completion
    /// answers the call, on the service thread until its first await (docs/csharp.md).</summary>
    public sealed class TaskDefinition<TReq, TPrg, TRsp> : IDisposable
    {
        private readonly TaskDefinitionCore _core;
        private readonly Schema _req, _prg, _rsp;

        internal TaskDefinition(RantNode node, string name, Func<TReq, TaskContext<TPrg>, Task<TRsp>> handler,
                                TaskOptions options, Schema requestSchema, Schema progressSchema,
                                Schema responseSchema)
        {
            _req = Schema.For(typeof(TReq), requestSchema);
            _prg = Schema.For(typeof(TPrg), progressSchema);
            _rsp = Schema.For(typeof(TRsp), responseSchema);
            Func<RequestCore, TaskContextCore, Task<byte[]>> h = null;
            if (handler != null)
            {
                Schema req = _req, prg = _prg, rsp = _rsp;
                h = async (r, ctx) =>
                {
                    object q;
                    if (!Patterns.TryDecode(req, r.SchemaPtr, r.Data, typeof(TReq), out q))
                        throw new Exception("request decode failed");
                    TRsp outv = await handler((TReq)q, new TaskContext<TPrg>(ctx, prg)).ConfigureAwait(false);
                    return Patterns.Encode(rsp, outv);
                };
            }
            _core = new TaskDefinitionCore(node, name, _req, _prg, _rsp, h, options);
        }

        /// <summary>Callers currently matched to this definition.</summary>
        public int MatchCount => _core.MatchCount;
        /// <summary>A ReflectFromMesh handle: re type in place when the mesh moved. True when
        /// it was re typed. See docs/reflection.md.</summary>
        public bool Refresh() => _core.Refresh();
        /// <summary>Retire the definition: every live deferred call answers Cancelled, a later
        /// completion is refused. Refused from a callback.</summary>
        public void Dispose() => _core.Dispose();
    }

    /// <summary>The caller side of a task defined on another node, from RantNode.RemoteTask.
    /// A request is always directed at one provider, and the timeout bounds only the first
    /// response.</summary>
    public sealed class RemoteTask<TReq, TPrg, TRsp> : IDisposable
    {
        private readonly RemoteTaskCore _core;
        private readonly Schema _req, _prg, _rsp;

        internal RemoteTask(RantNode node, string name, TaskOptions options,
                            Schema requestSchema, Schema progressSchema, Schema responseSchema)
        {
            _req = Schema.For(typeof(TReq), requestSchema);
            _prg = Schema.For(typeof(TPrg), progressSchema);
            _rsp = Schema.For(typeof(TRsp), responseSchema);
            _core = new RemoteTaskCore(node, name, _req, _prg, _rsp, options);
        }

        /// <summary>Start the task: the Task completes with the terminal outcome and never
        /// faults. progress fires per update, skipping the valueless RUNNING ack. Cancelling
        /// the token requests cooperative cancellation, the terminal status answers.</summary>
        public Task<RantResponse<TRsp>> CallAsync(TReq request, IProgress<TPrg> progress = null,
                                                  CancellationToken cancellationToken = default,
                                                  uint provider = 0)
        {
            Action<ProgressCore> sink = null;
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
            uint callId;
            Task<ResponseCore> core = _core.CallCore(Patterns.Encode(_req, request), out callId, sink,
                                                     cancellationToken, provider);
            return Wrap(core);
        }

        private async Task<RantResponse<TRsp>> Wrap(Task<ResponseCore> core)
            => new RantResponse<TRsp> { Core = await core.ConfigureAwait(false), RspSchema = _rsp };

        /// <summary>Definitions currently matched, 0 = no provider present.</summary>
        public int MatchCount => _core.MatchCount;
        /// <summary>A ReflectFromMesh handle: re type in place when the mesh moved. True when
        /// it was re typed. See docs/reflection.md.</summary>
        public bool Refresh() => _core.Refresh();
        /// <summary>Retire the remote: every outstanding call completes Cancelled. Refused
        /// from a callback.</summary>
        public void Dispose() => _core.Dispose();
    }

    /// <summary>The surface shared by VariableDefinition&lt;T&gt; and RemoteVariable&lt;T&gt;: read
    /// the latest value, write, force and observe. Value get throws while no value exists,
    /// Value set throws RantException on a non Ok status, and Set returns the status.</summary>
    public abstract class Variable<T> : IDisposable
    {
        private protected VariableCore _core;
        private protected Schema _schema;

        private protected Variable() { }

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
                    throw new RantException(st, "variable '" + _core.Name + "' set refused: " + st);
            }
        }

        /// <summary>Read the current value copied out, the store or the cached latest. False
        /// when no value exists yet.</summary>
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

        /// <summary>Write the value: apply and publish on the owner, or send it to the owner
        /// from a remote. BadRole = the owner advertises no set channel.</summary>
        public SendStatus Set(T value) => _core.Set(Patterns.Encode(_schema, value));
        /// <summary>Force the value: writes are absorbed until Unforce restores the latest.
        /// Needs AllowForce on the definition: State on the owner without it, BadRole on a
        /// remote whose owner advertises none.</summary>
        public SendStatus Force(T value) => _core.Force(Patterns.Encode(_schema, value));
        public SendStatus Unforce() => _core.Unforce();
        public bool Forced => _core.Forced;
        /// <summary>Handles matched on the other side: remotes for a definition, owners for a
        /// remote, 0 = no owner present.</summary>
        public int MatchCount => _core.MatchCount;
        /// <summary>Block until a value exists or timeoutMs elapses, on the service thread's
        /// progress or driving a Manual node's loop. Refused from a callback.</summary>
        public bool Wait(int timeoutMs) => _core.Wait(timeoutMs);
        /// <summary>A ReflectFromMesh handle: re type in place when the mesh moved. True when
        /// it was re typed. See docs/reflection.md.</summary>
        public bool Refresh() => _core.Refresh();
        /// <summary>Retire the handle: park its channels and release the name. Refused from a
        /// callback.</summary>
        public void Dispose() => _core.Dispose();

        private readonly object _lock = new object();
        private Action<T, VariableUpdate> _onChange, _onWrite;

        /// <summary>Fires on every state change with the value and its update envelope, and
        /// replays the current value to a handler as it is added. Inline on the thread that
        /// applied the write, or on the Dispatcher.</summary>
        public event Action<T, VariableUpdate> OnChange
        {
            add
            {
                if (value == null) return;
                bool first;
                lock (_lock) { first = _onChange == null; _onChange += value; }
                if (first && _core.Bind(true, u => Fan(_onChange, u))) return;
                VariableUpdate last = _core.LastChange;
                if (last != null) Fan(value, last);   // the replay the others already saw
            }
            remove { lock (_lock) _onChange -= value; }
        }

        /// <summary>Fires on every applied write, identical bytes or not, with no replay. The
        /// same threading as OnChange.</summary>
        public event Action<T, VariableUpdate> OnWrite
        {
            add
            {
                if (value == null) return;
                bool first;
                lock (_lock) { first = _onWrite == null; _onWrite += value; }
                if (first) _core.Bind(false, u => Fan(_onWrite, u));
            }
            remove { lock (_lock) _onWrite -= value; }
        }

        private void Fan(Action<T, VariableUpdate> hs, VariableUpdate u)
        {
            if (hs == null) return;
            object v;
            if (!Patterns.TryDecode(_schema, u.SchemaPtr, u.Data, typeof(T), out v)) return;
            foreach (Delegate d in hs.GetInvocationList())
            {
                try { ((Action<T, VariableUpdate>)d)((T)v, u); }
                catch (Exception e) { Console.Error.WriteLine("rant variable observer: " + e); }
            }
        }
    }

    /// <summary>The authoritative variable, from RantNode.VariableDefinition: this node owns
    /// the value and publishes every applied write. One definition per name on the mesh.</summary>
    public sealed class VariableDefinition<T> : Variable<T>
    {
        internal VariableDefinition(RantNode node, string name, bool hasInitial, T initial,
                                    VariableOptions options, Schema schema)
        {
            _schema = Schema.For(typeof(T), schema);
            byte[] first = hasInitial ? Patterns.Encode(_schema, initial) : null;
            _core = new VariableCore(node, name, _schema, first, options, true);
        }
    }

    /// <summary>A reference to a variable owned by another node, from RantNode.RemoteVariable:
    /// reads see the cached latest, writes go to the owner and come back as a change.</summary>
    public sealed class RemoteVariable<T> : Variable<T>
    {
        internal RemoteVariable(RantNode node, string name, VariableOptions options, Schema schema)
        {
            _schema = Schema.For(typeof(T), schema);
            _core = new VariableCore(node, name, _schema, null, options, false);
        }
    }

    /// <summary>The publishing side of a topic, from RantNode.Publisher. Send encodes T with
    /// the topic's schema: T's public fields by reflection, or the given Schema. A byte[] T
    /// sends the message bytes as is. Same name handles on one node share the topic.</summary>
    public sealed class Publisher<T> : IDisposable
    {
        private readonly TopicCore _topic;
        internal TopicCore Topic => _topic;

        internal Publisher(RantNode node, string name, Qos qos, Schema schema)
        {
            _topic = new TopicCore(node, name, Schema.For(typeof(T), schema), Role.PubOnly, qos);
        }

        /// <summary>Publish one message to every matched subscriber. captureUs is when the
        /// data was true rather than when it was sent, in Timestamp.Now() units, 0 = unstated.</summary>
        public SendStatus Send(T value, long captureUs = 0)
            => _topic.Send(Patterns.Encode(_topic.Schema, value), captureUs);

        /// <summary>Subscribers currently matched.</summary>
        public int MatchCount => _topic.MatchCount;
        /// <summary>True when a send would not wait on the match wait: a subscriber is matched
        /// or matching has converged. For a GUI: park payloads while false.</summary>
        public bool Ready => _topic.Ready;
        /// <summary>The cumulative traffic this node committed to the topic and delivered from
        /// it.</summary>
        public (ulong TxMsgs, ulong TxBytes, ulong RxMsgs, ulong RxBytes) Counts() => _topic.Counts();
        /// <summary>A ReflectFromMesh topic: re read the mesh and re type in place when the
        /// provider moved. True when it was re typed. See docs/reflection.md.</summary>
        public bool Refresh() => _topic.Refresh();
        /// <summary>Stop publishing. The node stops advertising the role no handle holds, and
        /// the last handle on the name retires the topic. Refused from a callback.</summary>
        public void Dispose() => _topic.Release();
    }

    /// <summary>The subscribing side of a topic, from RantNode.Subscriber. OnMessage fires per
    /// message on the service or polling thread, or TryTake and Dispatch consume from a queue
    /// instead. T decodes against the publisher's schema, a byte[] T is the bytes as is.</summary>
    public sealed class Subscriber<T> : IDisposable
    {
        private readonly TopicCore _topic;
        private readonly Action<RantMessage> _deliver;
        private readonly object _lock = new object();
        private Action<T, RantMessage> _onMessage;
        private bool _bound;
        internal TopicCore Topic => _topic;

        internal Subscriber(RantNode node, string name, Qos qos, Schema schema)
        {
            _topic = new TopicCore(node, name, Schema.For(typeof(T), schema), Role.SubOnly, qos);
            _deliver = Deliver;
        }

        /// <summary>A message as the decoded value and its envelope: sender, clocks, raw
        /// bytes. The first handler binds the topic to handler delivery.</summary>
        public event Action<T, RantMessage> OnMessage
        {
            add
            {
                if (value == null) return;
                lock (_lock)
                {
                    _onMessage += value;
                    if (_bound || _topic._handle == IntPtr.Zero) return;
                    _bound = true;
                    _topic._node.AddSubHandler(_topic.Index, _deliver);
                }
            }
            remove { lock (_lock) _onMessage -= value; }
        }

        private void Deliver(RantMessage m)
        {
            Action<T, RantMessage> hs = _onMessage;
            if (hs == null) return;
            T v;
            if (!Patterns.TryValue(m, out v)) return;
            hs(v, m);
        }

        /// <summary>Pop the next queued message. The first TryTake or Dispatch switches the
        /// topic to queued delivery (docs/node.md). timeoutMs 0 = check, negative = forever.</summary>
        public bool TryTake(out T value, int timeoutMs = 0)
        {
            value = default(T);
            RantMessage m;
            return _topic.TryTake(out m, timeoutMs) && Patterns.TryValue(m, out value);
        }

        /// <summary>Drain the queue by running OnMessage on the calling thread, oldest first,
        /// up to maxMsgs (0 = all), waiting like TryTake. Runs without the node lock.</summary>
        public int Dispatch(int maxMsgs = 0, int timeoutMs = 0) => _topic.Dispatch(maxMsgs, timeoutMs);

        /// <summary>Consumer queue observability, all zeros when not queued.</summary>
        public (uint Messages, uint Bytes, uint Capacity, uint Dropped) QueueStats() => _topic.QueueStats();
        /// <summary>The cumulative traffic this node committed to the topic and delivered from
        /// it.</summary>
        public (ulong TxMsgs, ulong TxBytes, ulong RxMsgs, ulong RxBytes) Counts() => _topic.Counts();
        /// <summary>A ReflectFromMesh topic: re read the mesh and re type in place when the
        /// provider moved. True when it was re typed. See docs/reflection.md.</summary>
        public bool Refresh() => _topic.Refresh();
        /// <summary>Stop receiving: the handlers are dropped, the node stops advertising the
        /// role no handle holds, and the last handle on the name retires the topic. Refused
        /// from a callback.</summary>
        public void Dispose()
        {
            lock (_lock)
            {
                if (_bound && _topic._handle != IntPtr.Zero) _topic._node.RemoveSubHandler(_topic.Index, _deliver);
                _bound = false;
                _onMessage = null;
            }
            _topic.Release();
        }
    }

    // ---- marshaling, allocators, schema codec + reflection ----------------------

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate IntPtr RantPageFn(IntPtr ptr, UIntPtr size);

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
        private static readonly RantPageFn s_page = PageRealloc;
        internal static readonly RantAllocFn SchemaAlloc = SchemaReAlloc;

        [MonoPInvokeCallback(typeof(RantPageFn))]
        private static IntPtr PageRealloc(IntPtr ptr, UIntPtr size)
        {
            if ((ulong)size == 0) { if (ptr != IntPtr.Zero) Marshal.FreeHGlobal(ptr); return IntPtr.Zero; }
            IntPtr cb = (IntPtr)(long)(ulong)size;
            return ptr == IntPtr.Zero ? Marshal.AllocHGlobal(cb) : Marshal.ReAllocHGlobal(ptr, cb);
        }

        [MonoPInvokeCallback(typeof(RantAllocFn))]
        private static IntPtr SchemaReAlloc(IntPtr user, IntPtr ptr, UIntPtr size)
        {
            if ((ulong)size == 0) { if (ptr != IntPtr.Zero) Marshal.FreeHGlobal(ptr); return IntPtr.Zero; }
            IntPtr cb = (IntPtr)(long)(ulong)size;
            return ptr == IntPtr.Zero ? Marshal.AllocHGlobal(cb) : Marshal.ReAllocHGlobal(ptr, cb);
        }

        internal static RantAllocator DefaultAllocator()
        {
            return new RantAllocator
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

        internal static string Str(RantStringView s)
        {
            if (s.data == IntPtr.Zero || (ulong)s.len == 0) return "";
            int n = (int)(ulong)s.len;
            byte[] b = new byte[n];
            Marshal.Copy(s.data, b, 0, n);
            return Encoding.UTF8.GetString(b);
        }

        internal static byte[] Bytes(RantBytes d)
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

        // A byte[] as a handle's T is the encoded message as is, never a reflected u8[].
        internal static bool IsRaw(Type t) => t == typeof(byte[]);

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
                var attr = (RantSchemaAttribute)Attribute.GetCustomAttribute(t, typeof(RantSchemaAttribute));
                string name = attr != null && !string.IsNullOrEmpty(attr.Name) ? attr.Name : t.Name;
                FieldInfo[] fields = t.GetFields(BindingFlags.Public | BindingFlags.Instance);
                Array.Sort(fields, (a, b) => a.MetadataToken.CompareTo(b.MetadataToken));
                var plans = new List<FieldPlan>();
                foreach (var f in fields)
                {
                    var fa = (RantFieldAttribute)Attribute.GetCustomAttribute(f, typeof(RantFieldAttribute));
                    var arr = (RantArrayAttribute)Attribute.GetCustomAttribute(f, typeof(RantArrayAttribute));
                    var str = (RantStringAttribute)Attribute.GetCustomAttribute(f, typeof(RantStringAttribute));
                    var plan = new FieldPlan { Field = f, WireName = fa != null ? fa.Name : f.Name };
                    byte k;
                    if (arr != null)   // [RantArray(N)]: a fixed array
                    {
                        Type et = f.FieldType.GetElementType();
                        if (et == typeof(string))
                        {
                            if (str == null)
                                throw new SchemaException("string array field " + f.Name
                                    + " needs [RantString(cap)] for its element capacity");
                            plan.Kind = ARR; plan.Elem = STR; plan.Count = arr.Count; plan.StrCap = str.Cap;
                        }
                        else if (et != null && ScalarKind.TryGetValue(et, out k))
                        { plan.Kind = ARR; plan.Elem = k; plan.Count = arr.Count; }
                        else throw new SchemaException("array field " + f.Name
                            + " element must be a scalar or a [RantString] string");
                    }
                    else if (f.FieldType.IsArray)   // T[] without [RantArray]: a variable array
                    {
                        Type et = f.FieldType.GetElementType();
                        if (et == typeof(string))
                        {
                            if (str == null)
                                throw new SchemaException("variable string array field " + f.Name
                                    + " needs [RantString(cap)] for its element capacity");
                            plan.Kind = VARR; plan.Elem = STR; plan.StrCap = str.Cap;
                        }
                        else if (et != null && ScalarKind.TryGetValue(et, out k))
                        { plan.Kind = VARR; plan.Elem = k; }
                        else throw new SchemaException("variable array field " + f.Name
                            + " element must be a scalar or a [RantString] string");
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
                        + " (use scalars, strings ([RantString] = capped, plain = variable), arrays "
                        + "([RantArray] = fixed, plain = variable), a Dictionary<string,object> map, "
                        + "or nested structs)");
                    // a standard type names the field's type: on the field, or on its struct
                    var tn = (RantTypeNameAttribute)Attribute.GetCustomAttribute(f, typeof(RantTypeNameAttribute));
                    if (tn == null && plan.Nested != null)
                        tn = (RantTypeNameAttribute)Attribute.GetCustomAttribute(plan.Nested, typeof(RantTypeNameAttribute));
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
            uint need = Native.rant_schema_print(s, IntPtr.Zero, UIntPtr.Zero);
            if (need == 0) return "";
            IntPtr buf = Marshal.AllocHGlobal((int)need + 1);
            try
            {
                Native.rant_schema_print(s, buf, (UIntPtr)(need + 1));
                return Marshal.PtrToStringAnsi(buf) ?? "";
            }
            finally { Marshal.FreeHGlobal(buf); }
        }

        // True when a compiled schema is a BARE TYPE: an unnamed root of one anonymous field,
        // so its message is a single value (encode takes it, decode returns it).
        internal static bool IsValueRoot(IntPtr s)
        {
            if (s == IntPtr.Zero || Native.rant_schema_field_count(s) != 1) return false;
            if ((ulong)Native.rant_schema_name(s).len != 0) return false;
            RantSchemaFieldInfo info;
            if (Native.rant_schema_field_at(s, 0, out info) == 0) return false;
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
            ushort n = Native.rant_schema_enum_count(s, field);
            for (ushort k = 0; k < n; k++)
            {
                long val; RantStringView nm;
                if (Native.rant_schema_enum_variant(s, field, k, out val, out nm) != 0)
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
            long cap = (long)Native.rant_schema_msg_min(s) + varBytes;
            byte[] buf = new byte[cap > 0 ? cap : 1];
            GCHandle gh = GCHandle.Alloc(buf, GCHandleType.Pinned);
            try
            {
                IntPtr p = gh.AddrOfPinnedObject();
                Native.rant_schema_message_default(s, p, (UIntPtr)buf.Length);
                foreach (var op in ops) ExecuteOp(s, p, (UIntPtr)buf.Length, op);
                uint n = Native.rant_schema_msg_len(s, p, (UIntPtr)buf.Length);
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
            RantSchemaFieldInfo info;
            if (value == null || Native.rant_schema_field_at(s, 0, out info) == 0) return;
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
            ushort n = Native.rant_schema_field_count(s);
            for (ushort i = 0; i < n; i++)
            {
                RantSchemaFieldInfo info;
                Native.rant_schema_field_at(s, i, out info);
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
            else if (op.Kind == F32) Native.rant_set_f32(buf, cap, s, cpath, Convert.ToSingle(op.Value));
            else if (op.Kind == F64) Native.rant_set_f64(buf, cap, s, cpath, Convert.ToDouble(op.Value));
            else if (op.Kind == ENUM)   // the backing integer, signed or unsigned per Elem
            {
                object raw = op.Value is Enum ? Convert.ChangeType(op.Value, Enum.GetUnderlyingType(op.Value.GetType())) : op.Value;
                if (op.Elem >= I8 && op.Elem <= I64) Native.rant_set_int(buf, cap, s, cpath, Convert.ToInt64(raw));
                else Native.rant_set_uint(buf, cap, s, cpath, Convert.ToUInt64(raw));
            }
            else if (op.Kind >= I8 && op.Kind <= I64) Native.rant_set_int(buf, cap, s, cpath, Convert.ToInt64(op.Value));
            else if (op.Kind == BOOL) Native.rant_set_uint(buf, cap, s, cpath, (bool)op.Value ? 1UL : 0UL);
            else Native.rant_set_uint(buf, cap, s, cpath, Convert.ToUInt64(op.Value));
        }

        private static bool SetString(IntPtr s, IntPtr buf, UIntPtr cap, byte[] cpath, string v)
        {
            byte[] b = Encoding.UTF8.GetBytes(v ?? "");
            GCHandle gh = GCHandle.Alloc(b, GCHandleType.Pinned);
            try
            {
                var ds = new RantStringView { data = gh.AddrOfPinnedObject(), len = (UIntPtr)b.Length };
                return Native.rant_set_string(buf, cap, s, cpath, ds) != 0;
            }
            finally { gh.Free(); }
        }

        private static bool SetStringAt(IntPtr s, IntPtr buf, UIntPtr cap, byte[] cpath, ushort index, string v)
        {
            byte[] b = Encoding.UTF8.GetBytes(v ?? "");
            GCHandle gh = GCHandle.Alloc(b, GCHandleType.Pinned);
            try
            {
                var ds = new RantStringView { data = gh.AddrOfPinnedObject(), len = (UIntPtr)b.Length };
                return Native.rant_set_string_at(buf, cap, s, cpath, index, ds) != 0;
            }
            finally { gh.Free(); }
        }

        // Set a variable string / array / map frame from pre-serialized payload bytes.
        private static bool SetBytesString(IntPtr s, IntPtr buf, UIntPtr cap, byte[] cpath, byte[] b)
        {
            GCHandle gh = GCHandle.Alloc(b, GCHandleType.Pinned);
            try
            {
                var ds = new RantStringView { data = b.Length > 0 ? gh.AddrOfPinnedObject() : IntPtr.Zero, len = (UIntPtr)b.Length };
                return Native.rant_set_string(buf, cap, s, cpath, ds) != 0;
            }
            finally { gh.Free(); }
        }

        private static void SetArrayBytes(IntPtr s, IntPtr buf, UIntPtr cap, byte[] cpath, byte[] b)
        {
            GCHandle gh = GCHandle.Alloc(b, GCHandleType.Pinned);
            try
            {
                var db = new RantBytes { data = b.Length > 0 ? gh.AddrOfPinnedObject() : IntPtr.Zero, len = (UIntPtr)b.Length };
                Native.rant_set_array(buf, cap, s, cpath, db);
            }
            finally { gh.Free(); }
        }

        private static bool SetMapBytes(IntPtr s, IntPtr buf, UIntPtr cap, byte[] cpath, byte[] b)
        {
            GCHandle gh = GCHandle.Alloc(b, GCHandleType.Pinned);
            try
            {
                var db = new RantBytes { data = b.Length > 0 ? gh.AddrOfPinnedObject() : IntPtr.Zero, len = (UIntPtr)b.Length };
                return Native.rant_set_map(buf, cap, s, cpath, db) != 0;
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
                var mb = new RantBytes
                {
                    data = data.Length > 0 ? gh.AddrOfPinnedObject() : IntPtr.Zero,
                    len = (UIntPtr)data.Length
                };
                var root = new Dictionary<string, object>();
                var dests = new List<Dictionary<string, object>> { root };
                ushort n = Native.rant_schema_field_count(s);
                for (ushort i = 0; i < n; i++)
                {
                    RantSchemaFieldInfo info;
                    Native.rant_schema_field_at(s, i, out info);
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
                        RantValue v;
                        Native.rant_get_value(mb, s, i, out v);
                        parent[name] = ValueToObj(v);
                    }
                }
                return root;
            }
            finally { gh.Free(); }
        }

        private static object ValueToObj(RantValue v)
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
            if (depth > 8 || off + 2 > end) { off = end; return outd; }   // RANT_SCHEMA_MAX_DEPTH
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
