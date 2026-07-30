// DART C# wrapper: a thin P/Invoke layer over the prebuilt native library.
//
// DART = Discovery And Realtime Transport, a dependency-free C99 middleware.
// The API mirrors the C++/Python wrappers (DartNode / Topic / Schema / Qos ...),
// is IL2CPP-safe (static callbacks dispatched by id, blittable structs), and
// P/Invokes a native library named "dart" (dart.dll / libdart.so / libdart.dylib):
//
//   - Plain .NET: get it from the NuGet package (bundled under runtimes/<rid>/native/)
//     or place it next to the assembly. Build it with csharp/native/build.{ps1,sh}.
//   - Unity (UNITY_5_3_OR_NEWER): the same native library, placed in Assets/Plugins;
//     the only Unity-specific bit is [MonoPInvokeCallback] on the callbacks (AOT).
//
//   struct Pose { public double X; [DartString(16)] public string Frame; }
//
//   var node = new Dart.DartNode("robot1",
//                            onMessage: m => Console.WriteLine(m.Value),  // decoded Pose for a typed topic
//                            onEvent: e => Console.Error.WriteLine(e));   // wired up before the ctor returns
//   var ch = new Dart.Topic<Pose>(node, "pose", reliable: true);
//   node.Start();                                      // C-level service thread owns the loop
//   ch.Send(new Pose { X = 1, Frame = "map" });        // thread-safe from any thread
//
// All optional configuration is named parameters (there are no options classes).
// Beyond plain topics, the patterns layer is bound too: FunctionDefinition /
// RemoteFunction (request/response), VariableDefinition / RemoteVariable
// (replicated state, one owner), Signal (reliable fire-and-forget event), and
// Publisher/Subscriber (side-named topic handles); each has an untyped (Schema +
// byte[]) core and a typed generic layered on it.
//
// Schemas come straight from the type: public fields become the wire fields, in
// declaration order. [DartArray(n)] fixes an array's element count, [DartString(cap)]
// fixes a string's byte capacity, [DartField("name")] overrides a wire name, and
// [DartSchema("Name")] optionally overrides the wire type name (the class name by
// default). Nested structs/classes just work.
//
// Threading: every DartNode/Topic call is thread-safe (a node-level lock in the C
// core serializes them). Drive a node either with Start() (a C background service
// thread runs the loop; handlers fire on it, never two at once) or by calling
// Poll() from your own loop (Unity: Poll(0) from Update() keeps handlers on the
// main thread). From inside OnMessage/OnEvent, Topic.Send and read-only
// queries are allowed; Poll/topic create/SetRole/Drain/Start/Stop/Close are
// refused (SendStatus.State / exception), never corrupting.

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
    // Off Unity we synthesize this attribute; it is a no-op but keeps the callback
    // methods annotated identically to the Unity/IL2CPP build.
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

    // The specific error carried by an EventKind.Error event (DartEvent.Error / DartNode.LastError).
    // Mirrors DartErrorKind in node/core.h.
    public enum ErrorKind
    {
        None = 0,
        NameCollision, QosIncompatible, KindMismatch, SchemaMismatch, InterestOverflow,
        MetaTruncatedInterest, MetaTruncatedSchema, PeerMetaTooBig, MessageTooBig,
        PeerRefused, EvictedUnsent, UnmatchedSend, DuplicateAuthority,
        Oom, Platform, Socket, Bind, McastJoin, Send, Recv, Poll, Waker
    }

    public enum FieldType : byte
    {
        U8 = 0, U16, U32, U64, I8, I16, I32, I64, F32, F64, Bool, Array, Struct, String
    }

    // A function call's outcome. Ok/AppError/NoHandler travel on the wire;
    // Timeout/PeerLost are synthesized client-side; Cancelled is synthesized for
    // calls still pending when the local node closes. Mirrors DartCallStatus.
    public enum CallStatus
    {
        Ok = 0, AppError = 1, NoHandler = 2, Timeout = 3, PeerLost = 4, Cancelled = 5
    }

    // Severity of a built-in @dart/log line. Mirrors DartLogLevel.
    public enum LogLevel { Error = 0, Warn = 1, Info = 2 }

    // A @dart/meta request's section mask (OR the bits; 0 = every section). Mirrors DART_META_*.
    [Flags]
    public enum MetaSection : uint { Node = 0x1, Proc = 0x2, Topics = 0x4, Peers = 0x8, All = 0 }

    // ---- native struct layouts (mirror the C exactly) ---------------------------

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartBytes { public IntPtr data; public UIntPtr len; }

    // the C DartString (a non-NUL length-carrying view); named *View here so the
    // [DartString] attribute owns the public name
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
    internal struct DartTopicOpts { public DartQos qos; }

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
        public uint recv_buffer_bytes;
        public uint send_buffer_bytes;
        public ushort fragment_size;
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
        public int match_wait_ms;              // send-path match wait; 0 = default (1s), <0 = off
        public byte disable_logs;              // strip the built-in @dart/log topics
        public byte disable_meta;              // do not host the @dart/meta endpoint
        public byte disable_error_logs;        // suppress default error mirroring onto @dart/log/error
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
        public DartBytes header;   // pattern-header prefix view ({null,0} on a plain topic); layout mirror of the C DartMsg
        public DartBytes data;
        public IntPtr schema;
        public ulong recv_us;
        public ulong sent_us;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartEventNative
    {
        public int kind;
        public int error;                      // DartErrorKind (Error events)
        public IntPtr topic_name;            // const char* (topic-scoped events; else null)
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
        public IntPtr schema_detail;           // const char* (SchemaMismatch: what was incompatible; else null)
        public IntPtr peer_name;               // const char* (peer-scoped events: the peer's node name; else null)
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
        public byte kind;
        public byte elem;
        public ushort count;
        public ushort depth;
        public ushort str_cap;
        public uint offset;
        public uint size;
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

    // ---- pattern struct mirrors (src/patterns/core.h; field order/types EXACT) --

    // The public head of the C DartRequestNative. Only ever read through the callback's
    // pointer; the reply machinery lives BEHIND the struct, so the exact pointer
    // (never a copy) is what dart_request_reply/fail/defer take.
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
        public ulong sent_us;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartResponseNative
    {
        public int status;                     // DartCallStatus
        public DartBytes data;
        public IntPtr schema;                  // const DartSchema*
        public uint provider;
        public IntPtr user;
        public ulong sent_us;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartFunctionOpts
    {
        public uint backpressure_wait_us;
        public uint timeout_us;
        public byte multi;              // duplicate-authority diagnostic suppressed (@dart/meta)
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartVariableOpts
    {
        public DartBytes initial;
        public byte access;                    // DartVarAccess
        public byte allow_force;
        public ushort catch_up;
        public uint backpressure_wait_us;
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
        public ulong sent_us;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartSignalOpts
    {
        public uint backpressure_wait_us;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartCallOpts
    {
        public uint provider;   // direct a call at one definition by peer id (0 = undirected)
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
    internal delegate void DartSignalFn(IntPtr msg, IntPtr user);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void DartVariableUpdateFn(IntPtr update, IntPtr user);

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
        internal static extern int dart_topic_send(IntPtr ch, DartBytes data);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_topic_set_role(IntPtr ch, int role);
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
        internal static extern DartStringView dart_schema_name(IntPtr s);
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
        internal static extern void dart_request_reply(IntPtr request, DartBytes rsp);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern void dart_request_fail(IntPtr request, DartBytes rsp);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern ulong dart_request_defer(IntPtr request);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_function_complete(IntPtr fn, ulong token, int status, DartBytes rsp);

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
        internal static extern int dart_variable_on_change(IntPtr var, DartVariableUpdateFn on_change, IntPtr user);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_variable_on_write(IntPtr var, DartVariableUpdateFn on_write, IntPtr user);

        // patterns: signals
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr dart_node_create_signal(IntPtr node, byte[] name,
            IntPtr schema, DartSignalFn on_signal, IntPtr user, ref DartSignalOpts opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_signal_emit(IntPtr sig, DartBytes payload);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_signal_listener_count(IntPtr sig);
    }

    // ---- config + reflection attributes -----------------------------------------

    // Internal QoS carrier: the PUBLIC surface is named constructor parameters
    // (reliable, keepLast, ...); this just plumbs them to the native DartQos.
    internal sealed class Qos
    {
        public Reliability Reliability = Reliability.BestEffort;
        public ushort KeepLast = 0;
        public ushort CatchUp = 0;
        public uint MaxMessageBytes = 0;
        public uint HeartbeatUs = 0;
        public uint RepairDelayUs = 0;
        public uint BackpressureWaitUs = 0;
        public uint ShmMaxBytes = 0;
        public uint QueueBytes = 0;
        public ushort MaxRateHz = 0;
        public bool NoTimestamp = false;

        internal static Qos FromParams(bool reliable, int keepLast, int catchUp,
            int maxMessageBytes, int heartbeatUs, int repairDelayUs, int backpressureWaitMs,
            int shmMaxBytes, int queueBytes, int maxRateHz = 0, bool noTimestamp = false)
        {
            return new Qos
            {
                Reliability = reliable ? Reliability.Reliable : Reliability.BestEffort,
                KeepLast = (ushort)keepLast,
                CatchUp = (ushort)catchUp,
                MaxMessageBytes = (uint)maxMessageBytes,
                HeartbeatUs = (uint)heartbeatUs,
                RepairDelayUs = (uint)repairDelayUs,
                BackpressureWaitUs = (uint)backpressureWaitMs * 1000u,
                ShmMaxBytes = (uint)shmMaxBytes,
                QueueBytes = (uint)queueBytes,
                MaxRateHz = (ushort)maxRateHz,
                NoTimestamp = noTimestamp,
            };
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

    /// <summary>Optional: override the wire type name of a message struct/class
    /// (defaults to the class name). Any struct/class with public fields works as
    /// a message type; this attribute is never required.</summary>
    [AttributeUsage(AttributeTargets.Struct | AttributeTargets.Class)]
    public sealed class DartSchemaAttribute : Attribute
    {
        public string Name;
        public DartSchemaAttribute(string name = null) { Name = name; }
    }

    /// <summary>A fixed-length array field: the element count on the wire. Omit it on
    /// an array field to get a VARIABLE array (`elem[]`), whose length rides the message
    /// tail (a live element count).</summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class DartArrayAttribute : Attribute
    {
        public int Count;
        public DartArrayAttribute(int count) { Count = count; }
    }

    /// <summary>A capped string field: the max UTF-8 byte length on the wire. Omit it on
    /// a plain string field to get a VARIABLE string (`string`, unbounded, in the message
    /// tail). On a string[] its presence (without [DartArray]) makes a variable
    /// `string&lt;cap&gt;[]`; with [DartArray] it makes a fixed array of capped strings.</summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class DartStringAttribute : Attribute
    {
        public int Cap;
        public DartStringAttribute(int cap) { Cap = cap; }
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

    /// <summary>A DART call was refused (a non-Ok SendStatus surfaced through a
    /// throwing surface, e.g. the VariableDefinition&lt;T&gt;.Value setter).</summary>
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

        /// <summary>Compile schema DSL text (e.g. "Pose { x: f32, frame: string&lt;16&gt; }").</summary>
        public Schema(string text)
        {
            IntPtr err;
            IntPtr h = Native.dart_schema_compile(Codec.SchemaAlloc, IntPtr.Zero, Codec.CStr(text), out err);
            if (h == IntPtr.Zero)
                throw new SchemaException("schema compile failed near: " + Codec.PtrToStr(err));
            Handle = h;
        }

        /// <summary>Reflect a type into a compiled schema: public fields become the
        /// wire fields (class -> DSL -> compile).</summary>
        public Schema(Type t) : this(Codec.TypeDsl(t)) { ClrType = t; }

        public string Name => Codec.Str(Native.dart_schema_name(Handle));
        public uint Size => Native.dart_schema_size(Handle);
        public ulong Hash => Native.dart_schema_hash(Handle);
        public ushort FieldCount => Native.dart_schema_field_count(Handle);
        public byte[] Wire => Codec.Bytes(Native.dart_schema_wire(Handle));

        /// <summary>The DSL text reconstructed from the compiled schema (works for any
        /// schema, including one parsed from a peer). Paste into a C node for interop.</summary>
        public string Dsl => Codec.SchemaDsl(Handle);

        public byte[] Encode(object value) => Codec.Encode(Handle, value);
        public Dictionary<string, object> DecodeFields(byte[] data) => Codec.DecodeDict(Handle, data);
        public object Decode(byte[] data)
        {
            var d = Codec.DecodeDict(Handle, data);
            return ClrType != null ? Codec.ToObject(ClrType, d) : d;
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

    public sealed class DartMessage
    {
        public ushort TopicIndex;
        public uint PublisherId;
        public string PublisherName;
        public string TopicName;
        public byte[] Data;
        /// <summary>DartNode monotonic clock (microseconds) when the poll RECEIVED the message
        /// (a queued topic stamps at enqueue), so a frame-paced consumer measures true
        /// arrival times, never its own cadence.</summary>
        public ulong RecvUs;
        /// <summary>The SENDER's wall clock (UTC microseconds) when its send committed: a
        /// source timestamp, kept across repair and replay. 0 = the publisher opted out
        /// (noTimestamp). Never mix it with the monotonic RecvUs.</summary>
        public ulong SentUs;
        public Dictionary<string, object> Fields;   // decoded (schema'd messages), else null
        public object Value;                          // typed instance for a typed topic, else Fields

        public string Text => Encoding.UTF8.GetString(Data);
        public T As<T>() => (T)Value;

        internal static DartMessage FromNative(ref DartMsg m, Type clrType)
        {
            var msg = new DartMessage
            {
                TopicIndex = m.topic_index,
                PublisherId = m.publisher_id,
                PublisherName = Codec.Str(m.publisher_name),
                TopicName = Codec.Str(m.topic_name),
                Data = Codec.Bytes(m.data),
                RecvUs = m.recv_us,
                SentUs = m.sent_us,
            };
            if (m.schema != IntPtr.Zero)
            {
                try
                {
                    msg.Fields = Codec.DecodeDict(m.schema, msg.Data);
                    msg.Value = clrType != null ? Codec.ToObject(clrType, msg.Fields) : msg.Fields;
                }
                catch (Exception e) { Console.Error.WriteLine("dart decode: " + e); }
            }
            return msg;
        }

        public override string ToString()
            => $"DartMessage(topic={TopicName}, from={PublisherName}, {Data.Length} bytes)";
    }

    public sealed class DartEvent
    {
        public EventKind Kind;
        public ErrorKind Error;         // the specific error when Kind == EventKind.Error, else None
        public string TopicName;      // our topic's name for topic-scoped events, else null
        public uint Peer;
        public ushort Topic;
        public int OsError;             // errno / WSAGetLastError for socket failures, else 0
        public ulong LostFirst;
        public ulong LostCount;
        public ulong TooBigBytes;
        public string SchemaDetail;     // SchemaMismatch: what exactly was incompatible, else null
        public string PeerName;         // peer-scoped events: the peer's human-readable node name, else null
        private string _line;

        /// <summary>True if this event reports something going wrong (Kind == EventKind.Error).</summary>
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

    /// <summary>One decoded @dart/log line handed to a DartNode.OnLog handler. WallUs is
    /// epoch micros (comparable across nodes); MonoUs is the publisher's monotonic clock
    /// (orders within one node); RecvUs is this node's clock when the poll received it.</summary>
    public sealed class DartLogLine
    {
        public LogLevel Level;
        public string Node;      // the publishing node's name
        public uint NodeId;      // the publishing peer id
        public ulong WallUs;
        public ulong MonoUs;
        public ulong RecvUs;
        public string Text;

        public override string ToString() => $"[{Level}] {Node}: {Text}";
    }

    /// <summary>A decoded @dart/meta reply (see DartNode.MetaAsync). The common "node" and
    /// "proc" scalars are pulled out as fields; Info holds the full self-describing body
    /// (including the topics/peers arrays) for anything else. Absent sections leave their
    /// fields zero (HaveProc stays false where the platform can't measure).</summary>
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

        // proc section (per process; HaveProc false where unmeasured)
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

    // ---- topic ----------------------------------------------------------------

    public class Topic
    {
        internal readonly DartNode _node;
        internal readonly IntPtr _handle;
        internal readonly Schema Schema;

        /// <summary>Create a raw (schemaless) topic on the node: send/receive bytes or
        /// UTF-8 strings. QoS rides as named parameters (all zero = defaults).</summary>
        public Topic(DartNode node, string name, Role role = Role.PubSub,
                     bool reliable = false, int keepLast = 0, int catchUp = 0,
                     int maxMessageBytes = 0, int heartbeatUs = 0, int repairDelayUs = 0,
                     int backpressureWaitMs = 0, int shmMaxBytes = 0, int queueBytes = 0, int maxRateHz = 0, bool noTimestamp = false)
            : this(node, name, (Schema)null, role, Qos.FromParams(reliable, keepLast, catchUp,
                   maxMessageBytes, heartbeatUs, repairDelayUs, backpressureWaitMs,
                   shmMaxBytes, queueBytes, maxRateHz, noTimestamp)) { }

        /// <summary>Create a typed topic with an explicit Schema (compiled from DSL or
        /// reflected). Topic&lt;T&gt; is the shorthand for the reflected case.</summary>
        public Topic(DartNode node, string name, Schema schema, Role role = Role.PubSub,
                     bool reliable = false, int keepLast = 0, int catchUp = 0,
                     int maxMessageBytes = 0, int heartbeatUs = 0, int repairDelayUs = 0,
                     int backpressureWaitMs = 0, int shmMaxBytes = 0, int queueBytes = 0, int maxRateHz = 0, bool noTimestamp = false)
            : this(node, name, schema, role, Qos.FromParams(reliable, keepLast, catchUp,
                   maxMessageBytes, heartbeatUs, repairDelayUs, backpressureWaitMs,
                   shmMaxBytes, queueBytes, maxRateHz, noTimestamp)) { }

        // The plumbing constructor every path funnels through: same-name topics on
        // one node share the native slot with a widened role (registry in DartNode).
        internal Topic(DartNode node, string name, Schema schema, Role role, Qos qos)
        {
            _node = node;
            Schema = schema;
            _handle = node.CreateOrShareTopic(name, role, schema, qos);
        }

        // Wrap an already-existing native handle (e.g. a @dart/log topic from the node):
        // no name registry entry, no schema. Query/send/set-role like any topic.
        internal Topic(DartNode node, IntPtr handle)
        {
            _node = node;
            Schema = null;
            _handle = handle;
        }

        /// <summary>Publish bytes/string (raw) or a message object (encoded via the
        /// topic schema). Returns a SendStatus.</summary>
        public SendStatus Send(byte[] data)
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
                r = Native.dart_topic_send(_handle, b);
            }
            finally { h.Free(); }
            return (SendStatus)r;
        }

        public SendStatus Send(string text) => Send(Encoding.UTF8.GetBytes(text));

        public SendStatus Send(object value)
        {
            if (value is byte[] b) return Send(b);
            if (value is string s) return Send(s);
            if (Schema == null)
                throw new InvalidOperationException(
                    "topic has no schema; send bytes/string, or create the topic with a schema");
            return Send(Schema.Encode(value));
        }

        public SendStatus SetRole(Role role)
        {
            return (SendStatus)Native.dart_topic_set_role(_handle, (int)role);
        }

        public ushort Index => Native.dart_topic_index(_handle);

        public int MatchCount()
        {
            return Native.dart_topic_match_count(_handle);
        }

        /// <summary>1-liner form of the send-path match wait for GUIs: true when a send
        /// would not wait (a matched subscriber exists, or matching has converged so
        /// there is nobody to wait for). Park payloads while false, flush on true.</summary>
        public bool Ready => Native.dart_topic_ready(_handle) == 1;

        /// <summary>Unresolved candidate matches right now: peers whose announces
        /// nominate this topic but whose name/schema verdicts are still in flight.
        /// 0 = matching has converged for everyone currently known.</summary>
        public int PendingCount => Native.dart_topic_pending_count(_handle);

        public bool Drain(int timeoutMs)
        {
            return Native.dart_topic_drain(_handle, timeoutMs) == 1;
        }

        /// <summary>Pop the next queued message, fully copied out. The FIRST
        /// TryTake/Dispatch switches this topic to QUEUED delivery: its messages then
        /// queue instead of firing the node handler on the poll thread, and exactly one
        /// thread of your choosing consumes them here (per topic). The queue grows on
        /// demand to Qos.QueueBytes (0 = 1 MB); at the cap a best-effort topic
        /// overwrites oldest (EventKind.MsgLost fires), a reliable one backpressures the
        /// publisher. timeoutMs: 0 = just check, &gt;0 = wait up to that long, negative =
        /// wait indefinitely (the wait sleeps beside a running service thread and drives
        /// the poll loop itself otherwise).</summary>
        public bool TryTake(out DartMessage message, int timeoutMs = 0)
        {
            message = null;
            var m = new DartMsg();
            if (Native.dart_topic_take(_handle, ref m, timeoutMs) != 1) return false;
            message = DartMessage.FromNative(ref m, _node.ClrTypeOf(m.topic_index));
            return true;
        }

        /// <summary>Drain the queue by running the node's OnMessage handler on the
        /// CALLING thread, oldest first: up to maxMsgs of those queued at entry (0 =
        /// all), first waiting up to timeoutMs like TryTake. Returns the number
        /// dispatched. Unlike poll-thread callbacks these run without the node lock,
        /// so they may use the whole API.</summary>
        public int Dispatch(int maxMsgs = 0, int timeoutMs = 0)
            => Native.dart_topic_dispatch(_handle, maxMsgs, timeoutMs);

        /// <summary>Consumer-queue observability; all zeros when not queued.</summary>
        public (uint Messages, uint Bytes, uint Capacity, uint Dropped) QueueStats()
        {
            Native.dart_topic_queue_stats(_handle, out uint m, out uint b, out uint c, out uint d);
            return (m, b, c, d);
        }

        /// <summary>Cumulative traffic counters (always on): messages/bytes this node
        /// committed to the topic (Tx) and delivered from it (Rx). Also in the @dart/meta
        /// snapshot.</summary>
        public (ulong TxMsgs, ulong TxBytes, ulong RxMsgs, ulong RxBytes) Counts()
        {
            Native.dart_topic_counts(_handle, out ulong tm, out ulong tb, out ulong rm, out ulong rb);
            return (tm, tb, rm, rb);
        }
    }

    /// <summary>A typed topic: T's public fields are the schema ([DartArray] /
    /// [DartString] / [DartField] refine them). Delivered messages decode to T
    /// (DartMessage.Value / DartMessage.As&lt;T&gt;()).</summary>
    public sealed class Topic<T> : Topic
    {
        public Topic(DartNode node, string name, Role role = Role.PubSub,
                     bool reliable = false, int keepLast = 0, int catchUp = 0,
                     int maxMessageBytes = 0, int heartbeatUs = 0, int repairDelayUs = 0,
                     int backpressureWaitMs = 0, int shmMaxBytes = 0, int queueBytes = 0, int maxRateHz = 0, bool noTimestamp = false)
            : base(node, name, new Schema(typeof(T)), role, Qos.FromParams(reliable, keepLast,
                   catchUp, maxMessageBytes, heartbeatUs, repairDelayUs, backpressureWaitMs,
                   shmMaxBytes, queueBytes, maxRateHz, noTimestamp)) { }

        internal Topic(DartNode node, string name, Role role, Qos qos)
            : base(node, name, new Schema(typeof(T)), role, qos) { }

        public SendStatus Send(T value) => Send((object)value);

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
        // same-name topic sharing (role widening); serialized by the ctor path's lock
        private sealed class TopicRec { public IntPtr Handle; public byte Bits; public ulong SchemaHash; }
        private readonly Dictionary<string, TopicRec> _topicsByName = new Dictionary<string, TopicRec>();
        private readonly object _createLock = new object();
        // per-topic subscriber handlers (Subscriber ctor); copy-on-write arrays so the
        // poll-thread read never takes more than a volatile fetch
        private readonly Dictionary<ushort, Action<DartMessage>[]> _subHandlers = new Dictionary<ushort, Action<DartMessage>[]>();
        private readonly object _subLock = new object();
        // pattern handler boxes + in-flight async calls this node owns (reaped at Close)
        private readonly List<long> _patternBoxes = new List<long>();
        private readonly HashSet<long> _asyncLive = new HashSet<long>();
        internal readonly object PatternLock = new object();

        // rooted so the GC never collects the trampolines handed to native code.
        private static readonly DartMsgFn s_onMsg = OnMessageTramp;
        private static readonly DartEventFn s_onEvt = OnEventTramp;
        private static readonly Dictionary<long, DartNode> s_nodes = new Dictionary<long, DartNode>();
        private static readonly object s_reg = new object();
        private static long s_nextId = 1;

        /// <summary>Open a node. onMessage may be null (per-Subscriber handlers and
        /// TryTake/Dispatch still deliver); onEvent is REQUIRED (it carries the
        /// diagnostics: null throws) and is wired in before the constructor returns,
        /// so no early peer/error event is ever missed. Everything else is optional
        /// named parameters (0/null = the C default).</summary>
        public DartNode(string name, Action<DartMessage> onMessage, Action<DartEvent> onEvent,
                    int domain = 0, int maxTopics = 0, bool disableShm = false,
                    bool fetchDetails = false, int matchWaitMs = 0,
                    bool disableLogs = false, bool disableMeta = false, bool disableErrorLogs = false,
                    int dataPort = 0, string discoveryGroup = null, int discoveryPort = 0,
                    string multicastInterface = null, int multicastTtl = 0,
                    int fragmentSize = 0, int announceIntervalUs = 0, int peerTimeoutUs = 0,
                    int maxPeers = 0)
        {
            if (onEvent == null)
                throw new ArgumentNullException(nameof(onEvent),
                    "onEvent carries the node's diagnostics (errors, peer lifecycle) and must not be null");
            _onMsg = onMessage;
            _onEvt = onEvent;
            lock (s_reg) { _id = s_nextId++; s_nodes[_id] = this; }

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
            co.net.fragment_size = (ushort)fragmentSize;
            co.discovery.announce_interval_us = (uint)announceIntervalUs;
            co.discovery.peer_timeout_us = (uint)peerTimeoutUs;
            co.discovery.max_peers = (ushort)maxPeers;

            _alloc = Codec.DefaultAllocator();
            byte[] cname = string.IsNullOrEmpty(name) ? null : Codec.CStr(name);
            IntPtr h = Native.dart_node_open(ref _alloc, cname, s_onMsg, s_onEvt, ref co);

            if (h == IntPtr.Zero)
            {
                lock (s_reg) s_nodes.Remove(_id);
                Codec.FreeCStr(_discGroup); Codec.FreeCStr(_mcastIf);
                // the node does not exist, so read the reason from the process-global slot
                DartEvent err = LastOpenError();
                throw new InvalidOperationException("dart_node_open failed: " + err);
            }
            _handle = h;
        }

        /// <summary>Rebind the message handler set at construction. Rarely needed: the
        /// constructor already requires an initial one.</summary>
        public DartNode OnMessage(Action<DartMessage> fn) { _onMsg = fn; return this; }
        /// <summary>Rebind the event handler set at construction. Rarely needed: the
        /// constructor already requires an initial one.</summary>
        public DartNode OnEvent(Action<DartEvent> fn) { _onEvt = fn; return this; }

        // The native create behind the Topic constructors. Same-name creates on this
        // node SHARE the native slot: the role is widened (SetRole re-advertises, peers
        // rematch from cached verdicts) and a different schema is refused.
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
                            "topic '" + name + "' already exists on this node with a different schema");
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
                var co = new DartTopicOpts { qos = qos.ToNative() };
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

        // role <-> pub/sub bit pair (bit 0 = pub, bit 1 = sub) for role widening
        private static byte RoleBits(Role r)
            => r == Role.PubSub ? (byte)3 : r == Role.PubOnly ? (byte)1
             : r == Role.SubOnly ? (byte)2 : (byte)0;
        private static Role RoleFromBits(byte b)
            => b == 3 ? Role.PubSub : b == 1 ? Role.PubOnly : b == 2 ? Role.SubOnly : Role.Inactive;

        // Subscriber handlers: per-topic-index, copy-on-write; when any exist for an
        // index they receive the message INSTEAD of the node-wide onMessage.
        internal void AddSubHandler(ushort index, Action<DartMessage> fn)
        {
            lock (_subLock)
            {
                Action<DartMessage>[] cur;
                if (!_subHandlers.TryGetValue(index, out cur)) cur = Array.Empty<Action<DartMessage>>();
                var nv = new Action<DartMessage>[cur.Length + 1];
                Array.Copy(cur, nv, cur.Length);
                nv[cur.Length] = fn;
                _subHandlers[index] = nv;
            }
        }

        private Action<DartMessage>[] SubHandlersOf(ushort index)
        {
            lock (_subLock)
            {
                Action<DartMessage>[] hs;
                _subHandlers.TryGetValue(index, out hs);
                return hs;
            }
        }

        // pattern-layer bookkeeping (reaped at Close)
        internal void RetainSchema(Schema s) { if (s != null) lock (_createLock) _schemas.Add(s); }
        internal void RegisterPatternBox(long id) { lock (PatternLock) _patternBoxes.Add(id); }
        internal void RegisterAsync(long id) { lock (PatternLock) _asyncLive.Add(id); }
        internal void UnregisterAsync(long id) { lock (PatternLock) _asyncLive.Remove(id); }

        /// <summary>One loop tick: drives discovery, RX, timers, and flushes queued TX.
        /// timeoutMs blocks up to that long in the socket wait (0 = non-blocking; a send
        /// from another thread wakes it early). Returns SendStatus.State (as int) while
        /// Start() runs -- the service thread owns the loop then.</summary>
        public int Poll(int timeoutMs = 0)
        {
            return Native.dart_node_poll(_handle, timeoutMs);
        }

        /// <summary>Run the C-level background service thread: it owns the loop and fires
        /// the handlers (never two at once for one node); every DartNode/Topic call stays
        /// safe from any thread, and a send is flushed immediately. Handlers run on the
        /// service thread -- to keep them on a specific thread (Unity: the main thread),
        /// skip Start() and call Poll() from that thread instead. Returns false if
        /// already started or threads are compiled out.</summary>
        public bool Start()
        {
            return Native.dart_node_start(_handle) == 0;
        }

        /// <summary>Stop and join the service thread (idempotent; implied by Close).</summary>
        public void Stop() => Native.dart_node_stop(_handle);

        public bool IsStarted => Native.dart_node_is_started(_handle) == 1;

        /// <summary>Block until discovery + matching settle: everything now sent reaches
        /// everyone already on the network. Call AFTER creating your topics. Most apps
        /// never need it (the per-send match wait covers the same window lazily).
        /// timeoutMs &lt; 0 = 3 announce intervals. True when settled.</summary>
        public bool Settle(int timeoutMs = -1) => Native.dart_node_settle(_handle, timeoutMs) == 1;

        internal IntPtr Handle => _handle;

        /// <summary>Dispatch every already-queued topic on the calling thread (see
        /// Topic.TryTake/Dispatch): with Start() running, this in a Unity Update() (or
        /// any UI frame) keeps every queued handler on that thread while the service
        /// thread owns the network. Waits up to timeoutMs for any queued topic to hold
        /// data; returns the number of messages dispatched.</summary>
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

        /// <summary>Subscribe to a level's mesh-wide log stream: widens this node's own log
        /// handle to PubSub and delivers every OTHER node's lines at that level (never your
        /// own), decoded to a DartLogLine. Late-join history replays on match. Handlers fire
        /// on the polling thread like any subscription. False when logs are disabled.</summary>
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
                WallUs = LogFieldU(m, "wall_us"), MonoUs = LogFieldU(m, "mono_us"),
                Text = m.Fields != null && m.Fields.TryGetValue("text", out var t) ? t as string ?? "" : "",
            }));
            return true;
        }

        private static ulong LogFieldU(DartMessage m, string k)
            => m.Fields != null && m.Fields.TryGetValue(k, out var o)
               ? (o is ulong u ? u : o is long l ? (ulong)l : 0UL) : 0UL;

        // ---- @dart/meta introspection --------------------------------------------------

        /// <summary>The local @dart/meta caller handle (null when meta is disabled). Call it
        /// directed at a peer id, e.g. MetaFunction().Call(req, -1, peerId). Most callers
        /// want MetaAsync.</summary>
        public RemoteFunction MetaFunction()
        {
            IntPtr fn = Native.dart_node_meta_function(_handle);
            return fn == IntPtr.Zero ? null : new RemoteFunction(this, fn);
        }

        /// <summary>Fetch a peer's snapshot: directs a @dart/meta call at `peer` and decodes
        /// the reply into a DartMetaSnapshot. The Task never faults (inspect Status). Works
        /// under Start() (unlike a blocking call). sections = OR of MetaSection (All = every
        /// section).</summary>
        public async Task<DartMetaSnapshot> MetaAsync(uint peer, MetaSection sections = MetaSection.All)
        {
            RemoteFunction fn = MetaFunction();
            if (fn == null) return new DartMetaSnapshot { Status = CallStatus.NoHandler };
            byte[] req = sections == MetaSection.All
                ? Array.Empty<byte>() : BitConverter.GetBytes((uint)sections);   // LE, as the C wire wants
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

        /// <summary>Why the most recent node open failed, from the process-global slot
        /// (there is no node handle on failure). The constructor already throws with
        /// this message.</summary>
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

        /// <summary>Stop the service thread (if running) and tear the node down. Returns
        /// false when refused from inside a handler (the node and this wrapper stay
        /// fully live): close from another thread instead.</summary>
        public bool Close(bool sendBye = true)
        {
            if (_handle != IntPtr.Zero)
            {
                if (Native.dart_node_close(_handle, sendBye ? 1 : 0) != 0) return false;
                _handle = IntPtr.Zero;
            }
            lock (s_reg) s_nodes.Remove(_id);
            // reap this node's pattern handler boxes and complete any still-pending
            // async calls (the C never fires their callbacks after close)
            lock (PatternLock)
            {
                foreach (long id in _patternBoxes) Patterns.DropBox(id);
                _patternBoxes.Clear();
                foreach (long id in _asyncLive) Patterns.AbandonAsync(id);
                _asyncLive.Clear();
            }
            foreach (var s in _schemas) s.Dispose();
            _schemas.Clear();
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
                lock (s_reg) s_nodes.TryGetValue((long)m.user, out node);
                if (node == null) return;
                var hs = node.SubHandlersOf(m.topic_index);
                if (hs == null && node._onMsg == null) return;
                node._topicTypes.TryGetValue(m.topic_index, out clr);
                var msg = DartMessage.FromNative(ref m, clr);      // fully copied: safe past the callback
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
                lock (s_reg) s_nodes.TryGetValue((long)e.user, out node);
                if (node == null || node._onEvt == null) return;
                node._onEvt(DartEvent.FromNative(evPtr, ref e));    // fully copied: safe past the callback
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
        }
        internal sealed class SignalBox
        {
            public Action<DartMessage> Handler;
            public Type ClrType;
        }
        internal sealed class VarBox
        {
            public Action<VariableUpdate> Handler;
        }
        internal sealed class AsyncCall
        {
            public TaskCompletionSource<DartResponse> Tcs;
            public DartNode DartNode;
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
        // Backstop only: the C fires every pending callback with CANCELLED during
        // dart_node_close, so this normally finds nothing. Completes any straggler
        // the same way instead of hanging its Task.
        internal static void AbandonAsync(long id)
        {
            AsyncCall c = TakeAsync(id);
            if (c != null) c.Tcs.TrySetResult(new DartResponse { Status = CallStatus.Cancelled });
        }

        // rooted delegates handed to native code
        internal static readonly DartRequestFn OnRequest = OnRequestTramp;
        internal static readonly DartResponseFn OnResponse = OnResponseTramp;
        internal static readonly DartSignalFn OnSignal = OnSignalTramp;
        internal static readonly DartVariableUpdateFn OnVarUpdate = OnVarUpdateTramp;

        [MonoPInvokeCallback(typeof(DartRequestFn))]
        private static void OnRequestTramp(IntPtr reqPtr, IntPtr user)
        {
            try
            {
                var box = GetBox((long)user) as RequestBox;
                if (box == null) return;
                var r = new DartRequest(reqPtr, box.Fn);
                try { box.Handler(r); }
                catch (Exception e)
                {
                    // a thrown handler answers AppError; the exception never crosses into C
                    r.FailQuiet();
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
                    SentUs = o.sent_us,
                    SchemaPtr = o.schema,
                    Data = Codec.Bytes(o.data),   // copied out: the view dies with the callback
                };
                call.Tcs.TrySetResult(r);
            }
            catch (Exception e) { Console.Error.WriteLine("dart on_response: " + e); }
        }

        [MonoPInvokeCallback(typeof(DartSignalFn))]
        private static void OnSignalTramp(IntPtr msgPtr, IntPtr user)
        {
            try
            {
                var box = GetBox((long)user) as SignalBox;
                if (box == null) return;
                var m = Marshal.PtrToStructure<DartMsg>(msgPtr);
                box.Handler(DartMessage.FromNative(ref m, box.ClrType));
            }
            catch (Exception e) { Console.Error.WriteLine("dart on_signal: " + e); }
        }

        [MonoPInvokeCallback(typeof(DartVariableUpdateFn))]
        private static void OnVarUpdateTramp(IntPtr updPtr, IntPtr user)
        {
            try
            {
                var box = GetBox((long)user) as VarBox;
                if (box == null) return;
                var u = Marshal.PtrToStructure<DartVariableUpdateNative>(updPtr);
                box.Handler(new VariableUpdate
                {
                    Name = Codec.Str(u.name),
                    Data = Codec.Bytes(u.value),   // copied out: the view dies with the callback
                    Forced = u.forced != 0,
                    WriteSeq = u.write_seq,
                    Source = u.source,
                    RecvUs = u.recv_us,
                    SentUs = u.sent_us,
                    SchemaPtr = u.schema,
                });
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

    /// <summary>The request as seen by a FunctionDefinition handler. Valid only inside
    /// the handler callback: reply there (Reply/Fail), or Defer() and complete later
    /// from any thread. Returning without answering auto-acks CallStatus.Ok.</summary>
    public sealed class DartRequest
    {
        private IntPtr _ptr;          // the EXACT native pointer; zeroed when the callback returns
        private readonly IntPtr _fn;
        private bool _done;
        internal readonly IntPtr SchemaPtr;

        public byte[] Data { get; private set; }
        public uint Caller { get; private set; }
        public string CallerName { get; private set; }
        public string FunctionName { get; private set; }
        public ulong RecvUs { get; private set; }
        /// <summary>The caller's wall clock when it sent the request (0 = unstamped).</summary>
        public ulong SentUs { get; private set; }
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
            SentUs = r.sent_us;
            SchemaPtr = r.schema;
        }

        /// <summary>Answer CallStatus.Ok with rsp.</summary>
        public void Reply(byte[] rsp)
        {
            Guard();
            using (var p = new PinnedBytes(rsp)) Native.dart_request_reply(_ptr, p.B);
            _done = true;
        }

        /// <summary>Answer CallStatus.AppError (with an optional error payload).</summary>
        public void Fail(byte[] rsp = null)
        {
            Guard();
            using (var p = new PinnedBytes(rsp)) Native.dart_request_fail(_ptr, p.B);
            _done = true;
        }

        /// <summary>Park the reply: suppresses the auto-ack and lets the handler return
        /// now; the returned Deferred completes the call later, from any thread.</summary>
        public Deferred Defer()
        {
            Guard();
            ulong token = Native.dart_request_defer(_ptr);
            _done = true;
            return new Deferred(_fn, token);
        }

        internal void FailQuiet() { if (_ptr != IntPtr.Zero && !_done) Fail(); }
        internal void Expire() { _ptr = IntPtr.Zero; }
        private void Guard()
        {
            if (_ptr == IntPtr.Zero)
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

        public bool Complete(byte[] rsp = null) => Finish(CallStatus.Ok, rsp);
        public bool Fail(byte[] rsp = null) => Finish(CallStatus.AppError, rsp);

        private bool Finish(CallStatus status, byte[] rsp)
        {
            long token = Interlocked.Exchange(ref _token, 0);   // single-shot
            if (_fn == IntPtr.Zero || token == 0) return false;
            using (var p = new PinnedBytes(rsp))
                return Native.dart_function_complete(_fn, (ulong)token, (int)status, p.B) == 0;
        }
    }

    /// <summary>The implementation side of a request/response function (untyped:
    /// Schema + byte[]). Exactly one reply per call; ONE definition per name on the
    /// network (a rival fires ErrorKind.DuplicateAuthority on both). A null handler
    /// answers every call CallStatus.NoHandler (a declared stub).</summary>
    public class FunctionDefinition
    {
        internal readonly IntPtr Fn;
        internal readonly DartNode DartNode;

        public FunctionDefinition(DartNode node, string name, Schema requestSchema, Schema responseSchema,
                                  Action<DartRequest> handler, int backpressureWaitMs = 0, int timeoutMs = 0)
        {
            DartNode = node;
            var co = new DartFunctionOpts
            {
                backpressure_wait_us = (uint)backpressureWaitMs * 1000u,
                timeout_us = (uint)timeoutMs * 1000u,
            };
            long id = 0;
            Patterns.RequestBox box = null;
            if (handler != null)
            {
                box = new Patterns.RequestBox { Handler = handler };
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
        }

        /// <summary>Callers currently matched to this definition.</summary>
        public int CallerCount => Native.dart_function_match_count(Fn);
    }

    /// <summary>An owning function-call outcome: the payload is copied out, so it
    /// outlives the call. SendStatus carries a synchronous refusal (Status stays
    /// Timeout then: the call never launched).</summary>
    public sealed class DartResponse
    {
        public CallStatus Status { get; internal set; } = CallStatus.Timeout;
        public SendStatus SendStatus { get; internal set; } = SendStatus.Ok;
        public uint Provider { get; internal set; }
        /// <summary>The provider's wall clock when it sent the response (0 = synthesized).</summary>
        public ulong SentUs { get; internal set; }
        public byte[] Data { get; internal set; } = Array.Empty<byte>();
        internal IntPtr SchemaPtr;

        public bool Ok => Status == CallStatus.Ok;
    }

    /// <summary>A reference to a function definition on another node (untyped).</summary>
    public class RemoteFunction
    {
        internal readonly IntPtr Fn;
        internal readonly DartNode DartNode;

        public RemoteFunction(DartNode node, string name, Schema requestSchema = null,
                              Schema responseSchema = null, int backpressureWaitMs = 0, int timeoutMs = 0)
        {
            DartNode = node;
            var co = new DartFunctionOpts
            {
                backpressure_wait_us = (uint)backpressureWaitMs * 1000u,
                timeout_us = (uint)timeoutMs * 1000u,
            };
            Fn = Native.dart_node_create_remote_function(node.Handle, Codec.CStr(name),
                requestSchema != null ? requestSchema.Handle : IntPtr.Zero,
                responseSchema != null ? responseSchema.Handle : IntPtr.Zero, ref co);
            if (Fn == IntPtr.Zero)
                throw new InvalidOperationException("remote function create failed: " + node.LastError);
            node.RetainSchema(requestSchema);
            node.RetainSchema(responseSchema);
        }

        // Wrap an existing node-owned function handle (the @dart/meta endpoint): callable,
        // never created or destroyed here.
        internal RemoteFunction(DartNode node, IntPtr fn) { DartNode = node; Fn = fn; }

        // Pin a DartCallOpts for one native call (IntPtr.Zero when undirected).
        private static GCHandle OptsHandle(uint provider, out IntPtr ptr)
        {
            if (provider == 0) { ptr = IntPtr.Zero; return default(GCHandle); }
            var g = GCHandle.Alloc(new DartCallOpts[] { new DartCallOpts { provider = provider } },
                                   GCHandleType.Pinned);
            ptr = g.AddrOfPinnedObject();
            return g;
        }

        /// <summary>BLOCKING call: drives the node loop until the response arrives or
        /// timeoutMs elapses (negative = the function's default timeout). Refused
        /// (SendStatus.State) from inside a callback or while a service thread owns
        /// this node's loop; use CallAsync there. Inspect Status, never throws.</summary>
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
                r.SentUs = o.sent_us;
                r.SchemaPtr = o.schema;
                r.Data = Codec.Bytes(o.data);   // the view is only valid until the next call: copy now
            }
            else if (rc < 0)
            {
                r.SendStatus = (SendStatus)rc;  // Status stays Timeout: never answered
            }
            return r;
        }

        /// <summary>Async call over dart_function_call_async: the Task completes with
        /// the outcome and NEVER faults (inspect Status/SendStatus). The response fires
        /// from whichever thread polls this node, continuations run off it.</summary>
        public Task<DartResponse> CallAsync(byte[] request, uint provider = 0)
        {
            var tcs = new TaskCompletionSource<DartResponse>(TaskCreationOptions.RunContinuationsAsynchronously);
            long id = Patterns.AddAsync(new Patterns.AsyncCall { Tcs = tcs, DartNode = DartNode });
            DartNode.RegisterAsync(id);
            int rc;
            GCHandle og = OptsHandle(provider, out IntPtr optp);   // committed synchronously; freed after
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
        /// <summary>The writer's wall clock for this write (this node's own for a local one).</summary>
        public ulong SentUs { get; internal set; }
        internal IntPtr SchemaPtr;
    }

    /// <summary>Replicated state, ONE owner: this node holds the authoritative value
    /// (untyped: Schema + byte[]). Remotes cache the latest published value.</summary>
    public class VariableDefinition
    {
        internal readonly IntPtr Var;
        internal readonly DartNode DartNode;
        internal readonly string Name;

        public VariableDefinition(DartNode node, string name, Schema schema, byte[] initial = null,
                                  bool readOnly = false, bool allowForce = false,
                                  int catchUp = 0, int backpressureWaitMs = 0)
            : this(node, name, schema, initial, readOnly, allowForce, catchUp, backpressureWaitMs, true) { }

        private protected VariableDefinition(DartNode node, string name, Schema schema, byte[] initial,
                                             bool readOnly, bool allowForce, int catchUp,
                                             int backpressureWaitMs, bool definition)
        {
            DartNode = node;
            Name = name;
            var co = new DartVariableOpts
            {
                access = (byte)(readOnly ? 1 : 0),
                allow_force = (byte)(allowForce ? 1 : 0),
                catch_up = (ushort)catchUp,
                backpressure_wait_us = (uint)backpressureWaitMs * 1000u,
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
        }

        /// <summary>Read the current value fully copied out (definition: the store;
        /// remote: the cached latest). False when no value exists yet.</summary>
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

        /// <summary>Set the value (definition: apply + publish; remote: send over the
        /// set channel). SendStatus.BadRole = the owner advertises no set channel.</summary>
        public SendStatus Set(byte[] value)
        {
            using (var p = new PinnedBytes(value)) return (SendStatus)Native.dart_variable_set(Var, p.B);
        }

        /// <summary>Force the value: writes are absorbed into the shadow source until
        /// Unforce, which restores the latest absorbed set. The definition must have
        /// been created with allowForce (SendStatus.State otherwise).</summary>
        public SendStatus Force(byte[] value)
        {
            using (var p = new PinnedBytes(value)) return (SendStatus)Native.dart_variable_force(Var, p.B);
        }
        public SendStatus Unforce() => (SendStatus)Native.dart_variable_unforce(Var);
        public bool Forced => Native.dart_variable_forced(Var) == 1;

        /// <summary>Remotes matched to this definition (on a RemoteVariable: owners
        /// matched, 0 = no owner present).</summary>
        public int RemoteCount => Native.dart_variable_match_count(Var);

        /// <summary>Block driving the node loop until a value exists or timeoutMs
        /// elapses (negative = forever-ish). Refused (false) from a callback or while
        /// a service thread owns this node's loop.</summary>
        public bool Wait(int timeoutMs) => Native.dart_variable_wait(Var, timeoutMs) == 1;

        /// <summary>Observe changes. Fires only when the observed state actually
        /// changes (the first value, different bytes, or a forced flip; a
        /// byte-identical re-set stays silent), and replays the current value once
        /// at registration so it can never be missed. Runs inline on the thread that
        /// applied the write (the service thread for anything off the wire), with the
        /// usual from-a-callback restrictions. One handler; null clears.</summary>
        public void OnChange(Action<VariableUpdate> handler) => Observe(handler, true);

        /// <summary>Observe every applied write, byte-identical or not (no replay at
        /// registration: writes are events, not state). Same threading as OnChange;
        /// one handler, null clears.</summary>
        public void OnWrite(Action<VariableUpdate> handler) => Observe(handler, false);

        private void Observe(Action<VariableUpdate> handler, bool change)
        {
            if (handler == null)
            {
                if (change) Native.dart_variable_on_change(Var, null, IntPtr.Zero);
                else Native.dart_variable_on_write(Var, null, IntPtr.Zero);
                return;
            }
            long id = Patterns.AddBox(new Patterns.VarBox { Handler = handler });
            DartNode.RegisterPatternBox(id);
            if (change) Native.dart_variable_on_change(Var, Patterns.OnVarUpdate, (IntPtr)id);
            else Native.dart_variable_on_write(Var, Patterns.OnVarUpdate, (IntPtr)id);
        }
    }

    /// <summary>A reference to a variable owned by another node (untyped): reads see
    /// the cached latest, writes go over the set channel (dumb writes, no response).</summary>
    public class RemoteVariable : VariableDefinition
    {
        public RemoteVariable(DartNode node, string name, Schema schema = null,
                              int catchUp = 0, int backpressureWaitMs = 0)
            : base(node, name, schema, null, false, false, catchUp, backpressureWaitMs, false) { }

        /// <summary>Owners currently matched.</summary>
        public int MatchCount => RemoteCount;
        public bool HasDefinition => RemoteCount > 0;
    }

    // ---- patterns: signals ------------------------------------------------------

    /// <summary>A reliable fire-and-forget event: N emitters / N listeners, NEVER
    /// latched (a late joiner receives nothing emitted before it joined). Passing a
    /// handler IS the subscription; every handle may emit.</summary>
    public class Signal
    {
        internal readonly IntPtr Sig;
        internal readonly DartNode DartNode;

        public Signal(DartNode node, string name, Schema schema = null,
                      Action<DartMessage> handler = null, int backpressureWaitMs = 0)
            : this(node, name, schema, handler, null, backpressureWaitMs) { }

        internal Signal(DartNode node, string name, Schema schema, Action<DartMessage> handler,
                        Type clrType, int backpressureWaitMs)
        {
            DartNode = node;
            var co = new DartSignalOpts { backpressure_wait_us = (uint)backpressureWaitMs * 1000u };
            long id = 0;
            Patterns.SignalBox box = null;
            if (handler != null)
            {
                box = new Patterns.SignalBox { Handler = handler, ClrType = clrType };
                id = Patterns.AddBox(box);
            }
            Sig = Native.dart_node_create_signal(node.Handle, Codec.CStr(name),
                schema != null ? schema.Handle : IntPtr.Zero,
                box != null ? Patterns.OnSignal : null, (IntPtr)id, ref co);
            if (Sig == IntPtr.Zero)
            {
                if (box != null) Patterns.DropBox(id);
                throw new InvalidOperationException("signal create failed: " + node.LastError);
            }
            if (box != null) node.RegisterPatternBox(id);
            node.RetainSchema(schema);
        }

        /// <summary>Emit to every matched listener (payload may be null/empty).</summary>
        public SendStatus Emit(byte[] payload = null)
        {
            using (var p = new PinnedBytes(payload)) return (SendStatus)Native.dart_signal_emit(Sig, p.B);
        }

        /// <summary>Listeners currently matched (other nodes subscribed).</summary>
        public int ListenerCount => Native.dart_signal_listener_count(Sig);
    }

    // ---- patterns: pub/sub handles ----------------------------------------------

    /// <summary>The publish-side handle over a (possibly shared) topic (untyped).
    /// Same-name handles on one node share the topic slot with a widened role.</summary>
    public class Publisher
    {
        internal readonly Topic T;

        public Publisher(DartNode node, string name, Schema schema = null,
                         bool reliable = false, int keepLast = 0, int catchUp = 0,
                         int maxMessageBytes = 0, int heartbeatUs = 0, int repairDelayUs = 0,
                         int backpressureWaitMs = 0, int shmMaxBytes = 0, int queueBytes = 0, int maxRateHz = 0, bool noTimestamp = false)
        {
            T = new Topic(node, name, schema, Role.PubOnly, Qos.FromParams(reliable, keepLast,
                    catchUp, maxMessageBytes, heartbeatUs, repairDelayUs, backpressureWaitMs,
                    shmMaxBytes, queueBytes, maxRateHz, noTimestamp));
        }

        public SendStatus Send(byte[] data) => T.Send(data);
        public SendStatus Send(string text) => T.Send(text);
        public int MatchCount => T.MatchCount();
        public int PendingCount => T.PendingCount;
        public bool Ready => T.Ready;
        public Topic Topic => T;
    }

    /// <summary>The subscribe-side handle (untyped). A handler fires per message on
    /// the polling thread; or consume with TryTake/Dispatch on a thread of your
    /// choosing. Handlers on a topic replace the node-wide onMessage for it.</summary>
    public class Subscriber
    {
        internal readonly Topic T;

        public Subscriber(DartNode node, string name, Schema schema = null,
                          Action<DartMessage> handler = null,
                          bool reliable = false, int keepLast = 0, int catchUp = 0,
                          int maxMessageBytes = 0, int heartbeatUs = 0, int repairDelayUs = 0,
                          int backpressureWaitMs = 0, int shmMaxBytes = 0, int queueBytes = 0, int maxRateHz = 0, bool noTimestamp = false)
        {
            T = new Topic(node, name, schema, Role.SubOnly, Qos.FromParams(reliable, keepLast,
                    catchUp, maxMessageBytes, heartbeatUs, repairDelayUs, backpressureWaitMs,
                    shmMaxBytes, queueBytes, maxRateHz, noTimestamp));
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
        public void Fail() => _core.Fail();
        public Deferred<TRsp> Defer() => new Deferred<TRsp>(_core.Defer(), _rsp);
    }

    /// <summary>The typed parked reply: Complete(value)/Fail() exactly once, any thread.</summary>
    public sealed class Deferred<TRsp>
    {
        private readonly Deferred _core;
        private readonly Schema _rsp;

        internal Deferred(Deferred core, Schema rsp) { _core = core; _rsp = rsp; }

        public bool Valid => _core.Valid;
        public bool Complete(TRsp value) => _core.Complete(_rsp.Encode(value));
        public bool Fail() => _core.Fail();
    }

    /// <summary>The typed implementation side. Simple form: the return value is the
    /// reply, a THROWN exception answers CallStatus.AppError (it never crosses into
    /// the C). Full form: reply/fail/defer explicitly through DartRequest&lt;TRsp&gt;.</summary>
    public sealed class FunctionDefinition<TReq, TRsp>
    {
        private readonly FunctionDefinition _core;
        private readonly Schema _req, _rsp;

        public FunctionDefinition(DartNode node, string name, Func<TReq, TRsp> handler,
                                  int backpressureWaitMs = 0, int timeoutMs = 0)
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
                    if (!Patterns.TryDecode(req, r.SchemaPtr, r.Data, typeof(TReq), out q)) { r.Fail(); return; }
                    TRsp outv = handler((TReq)q);   // a throw answers AppError (trampoline catch)
                    if (!r.Answered) r.Reply(rsp.Encode(outv));
                };
            }
            _core = new FunctionDefinition(node, name, _req, _rsp, h, backpressureWaitMs, timeoutMs);
        }

        public FunctionDefinition(DartNode node, string name, Action<TReq, DartRequest<TRsp>> handler,
                                  int backpressureWaitMs = 0, int timeoutMs = 0)
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
                    if (!Patterns.TryDecode(req, r.SchemaPtr, r.Data, typeof(TReq), out q)) { r.Fail(); return; }
                    handler((TReq)q, new DartRequest<TRsp>(r, rsp));
                };
            }
            _core = new FunctionDefinition(node, name, _req, _rsp, h, backpressureWaitMs, timeoutMs);
        }

        public int CallerCount => _core.CallerCount;
    }

    /// <summary>The typed owning call outcome. Reading Value when !Ok throws
    /// CallException; Status/SendStatus never throw.</summary>
    public sealed class DartResponse<TRsp>
    {
        internal DartResponse Core;
        internal Schema RspSchema;

        public CallStatus Status => Core.Status;
        public bool Ok => Core.Ok;
        public uint Provider => Core.Provider;
        public ulong SentUs => Core.SentUs;
        public SendStatus SendStatus => Core.SendStatus;

        public TRsp Value
        {
            get
            {
                if (!Ok)
                    throw new CallException(Status, "call did not complete Ok: status " + Status
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

        public RemoteFunction(DartNode node, string name, int backpressureWaitMs = 0, int timeoutMs = 0)
        {
            _req = new Schema(typeof(TReq));
            _rsp = new Schema(typeof(TRsp));
            _core = new RemoteFunction(node, name, _req, _rsp, backpressureWaitMs, timeoutMs);
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
    }

    /// <summary>The typed authoritative variable. Value get throws
    /// InvalidOperationException while no value exists; Value set throws DartException
    /// on a non-Ok SendStatus (use Set for the status-returning form).</summary>
    public class VariableDefinition<T>
    {
        private protected VariableDefinition _core;
        private protected Schema _schema;

        private protected VariableDefinition() { }

        public VariableDefinition(DartNode node, string name, bool readOnly = false,
                                  bool allowForce = false, int catchUp = 0, int backpressureWaitMs = 0)
        {
            _schema = new Schema(typeof(T));
            _core = new VariableDefinition(node, name, _schema, null, readOnly, allowForce,
                                           catchUp, backpressureWaitMs);
        }

        /// <summary>Overload with an initial value (the value before any set).</summary>
        public VariableDefinition(DartNode node, string name, T initial, bool readOnly = false,
                                  bool allowForce = false, int catchUp = 0, int backpressureWaitMs = 0)
        {
            _schema = new Schema(typeof(T));
            _core = new VariableDefinition(node, name, _schema, _schema.Encode(initial),
                                           readOnly, allowForce, catchUp, backpressureWaitMs);
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

        /// <summary>Observe changes, typed (see the untyped OnChange for the change
        /// contract and threading). Handler forms: (T value) or
        /// (T value, VariableUpdate update); pass a null-cast delegate to clear.</summary>
        public void OnChange(Action<T> handler) => _core.OnChange(Adapt(handler, null));
        public void OnChange(Action<T, VariableUpdate> handler) => _core.OnChange(Adapt(null, handler));
        /// <summary>Observe every applied write, typed (no replay at registration).</summary>
        public void OnWrite(Action<T> handler) => _core.OnWrite(Adapt(handler, null));
        public void OnWrite(Action<T, VariableUpdate> handler) => _core.OnWrite(Adapt(null, handler));

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

    /// <summary>The typed accessor of a variable owned elsewhere. Same surface as the
    /// definition (the definition decides force/write permissions), plus
    /// HasDefinition/MatchCount.</summary>
    public sealed class RemoteVariable<T> : VariableDefinition<T>
    {
        public RemoteVariable(DartNode node, string name, int catchUp = 0, int backpressureWaitMs = 0)
        {
            _schema = new Schema(typeof(T));
            _core = new RemoteVariable(node, name, _schema, catchUp, backpressureWaitMs);
        }

        /// <summary>Owners currently matched.</summary>
        public int MatchCount => _core.RemoteCount;
        public bool HasDefinition => _core.RemoteCount > 0;
    }

    /// <summary>The typed signal. Constructing with a handler subscribes; every
    /// handle may emit.</summary>
    public sealed class Signal<T>
    {
        private readonly Signal _core;
        private readonly Schema _schema;

        /// <summary>Emit-only handle (no subscription).</summary>
        public Signal(DartNode node, string name, int backpressureWaitMs = 0)
        {
            _schema = new Schema(typeof(T));
            _core = new Signal(node, name, _schema, null, null, backpressureWaitMs);
        }

        public Signal(DartNode node, string name, Action<T> handler, int backpressureWaitMs = 0)
        {
            if (handler == null) throw new ArgumentNullException(nameof(handler));
            _schema = new Schema(typeof(T));
            _core = new Signal(node, name, _schema,
                m => { if (m.Value is T v) handler(v); }, typeof(T), backpressureWaitMs);
        }

        public Signal(DartNode node, string name, Action<T, DartMessage> handler, int backpressureWaitMs = 0)
        {
            if (handler == null) throw new ArgumentNullException(nameof(handler));
            _schema = new Schema(typeof(T));
            _core = new Signal(node, name, _schema,
                m => { if (m.Value is T v) handler(v, m); }, typeof(T), backpressureWaitMs);
        }

        public SendStatus Emit(T value) => _core.Emit(_schema.Encode(value));
        public int ListenerCount => _core.ListenerCount;
    }

    /// <summary>The typed publish side.</summary>
    public sealed class Publisher<T>
    {
        private readonly Publisher _core;
        private readonly Schema _schema;

        public Publisher(DartNode node, string name,
                         bool reliable = false, int keepLast = 0, int catchUp = 0,
                         int maxMessageBytes = 0, int heartbeatUs = 0, int repairDelayUs = 0,
                         int backpressureWaitMs = 0, int shmMaxBytes = 0, int queueBytes = 0, int maxRateHz = 0, bool noTimestamp = false)
        {
            _schema = new Schema(typeof(T));
            _core = new Publisher(node, name, _schema, reliable, keepLast, catchUp,
                maxMessageBytes, heartbeatUs, repairDelayUs, backpressureWaitMs,
                shmMaxBytes, queueBytes, maxRateHz, noTimestamp);
        }

        public SendStatus Send(T value) => _core.Send(_schema.Encode(value));
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

        public Subscriber(DartNode node, string name, Action<T> handler = null,
                          bool reliable = false, int keepLast = 0, int catchUp = 0,
                          int maxMessageBytes = 0, int heartbeatUs = 0, int repairDelayUs = 0,
                          int backpressureWaitMs = 0, int shmMaxBytes = 0, int queueBytes = 0, int maxRateHz = 0, bool noTimestamp = false)
        {
            _core = new Subscriber(node, name, new Schema(typeof(T)),
                handler == null ? (Action<DartMessage>)null : m => { if (m.Value is T v) handler(v); },
                reliable, keepLast, catchUp, maxMessageBytes, heartbeatUs, repairDelayUs,
                backpressureWaitMs, shmMaxBytes, queueBytes, maxRateHz, noTimestamp);
        }

        public Subscriber(DartNode node, string name, Action<T, DartMessage> handler,
                          bool reliable = false, int keepLast = 0, int catchUp = 0,
                          int maxMessageBytes = 0, int heartbeatUs = 0, int repairDelayUs = 0,
                          int backpressureWaitMs = 0, int shmMaxBytes = 0, int queueBytes = 0, int maxRateHz = 0, bool noTimestamp = false)
        {
            if (handler == null) throw new ArgumentNullException(nameof(handler));
            _core = new Subscriber(node, name, new Schema(typeof(T)),
                m => { if (m.Value is T v) handler(v, m); },
                reliable, keepLast, catchUp, maxMessageBytes, heartbeatUs, repairDelayUs,
                backpressureWaitMs, shmMaxBytes, queueBytes, maxRateHz, noTimestamp);
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
            ENUM = 17;                        // named integer (wire = its backing scalar)

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

        // --- reflection: message type -> field plan -> DSL ---
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
        }
        private sealed class TypeSpec { public string Name; public List<FieldPlan> Fields; }
        private static readonly Dictionary<Type, TypeSpec> s_specs = new Dictionary<Type, TypeSpec>();

        private static bool StructLike(Type t)
            => t != typeof(string) && !t.IsArray && !t.IsPrimitive && !t.IsEnum
               && (t.IsValueType || t.IsClass);

        // A `map` field: any dictionary (canonically Dictionary<string, object>).
        private static bool IsMapType(Type t)
            => typeof(System.Collections.IDictionary).IsAssignableFrom(t);

        private static TypeSpec Spec(Type t)
        {
            lock (s_specs)
            {
                TypeSpec cached;
                if (s_specs.TryGetValue(t, out cached)) return cached;
                var attr = (DartSchemaAttribute)Attribute.GetCustomAttribute(t, typeof(DartSchemaAttribute));
                string name = attr != null && !string.IsNullOrEmpty(attr.Name) ? attr.Name : t.Name;
                FieldInfo[] fields = t.GetFields(BindingFlags.Public | BindingFlags.Instance);
                Array.Sort(fields, (a, b) => a.MetadataToken.CompareTo(b.MetadataToken));  // declaration order
                var plans = new List<FieldPlan>();
                foreach (var f in fields)
                {
                    var fa = (DartFieldAttribute)Attribute.GetCustomAttribute(f, typeof(DartFieldAttribute));
                    var arr = (DartArrayAttribute)Attribute.GetCustomAttribute(f, typeof(DartArrayAttribute));
                    var str = (DartStringAttribute)Attribute.GetCustomAttribute(f, typeof(DartStringAttribute));
                    var plan = new FieldPlan { Field = f, WireName = fa != null ? fa.Name : f.Name };
                    byte k;
                    if (arr != null)   // [DartArray(N)] -> FIXED array
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
                    else if (f.FieldType.IsArray)   // T[] without [DartArray] -> VARIABLE array
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
                        else plan.Kind = VSTR;                                          // variable (unbounded)
                    }
                    else if (IsMapType(f.FieldType)) { plan.Kind = MAP; }
                    else if (f.FieldType.IsEnum)   // named integer; backing from the enum's underlying type
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

        private static string FieldLine(FieldPlan f)
        {
            if (f.Kind == STRUCT)
            {
                var nested = Spec(f.Nested).Fields;
                var parts = new List<string>();
                foreach (var g in nested) parts.Add(FieldLine(g));
                return f.WireName + ": { " + string.Join(", ", parts) + " }";
            }
            if (f.Kind == ARR)
                return f.WireName + ": " + ElemToken(f.Elem, f.StrCap) + "[" + f.Count + "]";
            if (f.Kind == VARR) return f.WireName + ": " + ElemToken(f.Elem, f.StrCap) + "[]";
            if (f.Kind == STR) return f.WireName + ": string<" + f.StrCap + ">";
            if (f.Kind == VSTR) return f.WireName + ": string";
            if (f.Kind == MAP) return f.WireName + ": map";
            if (f.Kind == ENUM) return f.WireName + ": enum<" + Token[f.Elem] + "> " + EnumBody(f.EnumType);
            return f.WireName + ": " + Token[f.Kind];
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

        // --- DSL reconstruction from ANY compiled schema (flat depth-first table) ---
        internal static string SchemaDsl(IntPtr s)
        {
            var root = new List<object[]>();
            var stack = new List<List<object[]>> { root };
            ushort n = Native.dart_schema_field_count(s);
            for (ushort i = 0; i < n; i++)
            {
                DartSchemaFieldInfo info;
                Native.dart_schema_field_at(s, i, out info);
                var node = new object[] { Str(info.name), info.kind, info.elem, (int)info.count,
                                          (int)info.str_cap, null,
                                          info.kind == ENUM ? EnumBodyFromSchema(s, i) : null };
                int d = info.depth;
                stack[d].Add(node);
                if (info.kind == STRUCT)
                {
                    var ch = new List<object[]>();
                    node[5] = ch;
                    while (stack.Count <= d + 1) stack.Add(null);
                    stack[d + 1] = ch;
                }
            }
            var sb = new StringBuilder();
            sb.Append(Str(Native.dart_schema_name(s))).Append("\n{\n");
            EmitNodes(sb, root);
            sb.Append("\n}\n");
            return sb.ToString();
        }

        private static void EmitNodes(StringBuilder sb, List<object[]> nodes)
        {
            for (int i = 0; i < nodes.Count; i++)
            {
                if (i > 0) sb.Append(",\n");
                sb.Append("    ").Append(NodeLine(nodes[i]));
            }
        }

        private static string NodeLine(object[] node)
        {
            string name = (string)node[0];
            byte kind = (byte)node[1], elem = (byte)node[2];
            int count = (int)node[3], strCap = (int)node[4];
            var children = (List<object[]>)node[5];
            if (kind == STRUCT)
            {
                var parts = new List<string>();
                foreach (var c in children) parts.Add(NodeLine(c));
                return name + ": { " + string.Join(", ", parts) + " }";
            }
            if (kind == ARR) return name + ": " + ElemToken(elem, strCap) + "[" + count + "]";
            if (kind == VARR) return name + ": " + ElemToken(elem, strCap) + "[]";
            if (kind == STR) return name + ": string<" + strCap + ">";
            if (kind == VSTR) return name + ": string";
            if (kind == MAP) return name + ": map";
            if (kind == ENUM) return name + ": enum<" + Token[elem] + "> " + (string)node[6];
            return name + ": " + Token[kind];
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

        // --- encode: object -> message bytes ---------------------------------------
        // One walk of the reflection spec resolves every value and pre-serializes the
        // variable-field payloads, so the tail can be sized before the buffer is pinned.
        private sealed class SetOp
        {
            public byte[] Cpath;
            public string Path;      // for error text
            public byte Kind, Elem;
            public int Count, StrCap;
            public object Value;     // fixed fields / fixed string[] : the raw value
            public byte[] Prepared;  // variable fields: the pre-serialized payload bytes
        }

        // Encode a typed object (fields by reflection) OR a Dictionary<string,object>
        // (values by field name, the schema drives the walk -- like the Python wrapper).
        internal static byte[] Encode(IntPtr s, object value)
        {
            var ops = new List<SetOp>();
            long varBytes = 0;
            if (value is System.Collections.IDictionary dict)
                CollectFromDict(s, dict, ops, ref varBytes);
            else
                Collect(s, Spec(value.GetType()), value, "", ops, ref varBytes);

            // msg_min is the fixed section plus one empty frame per variable field; each
            // variable frame then grows by exactly its payload length.
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

        // reflected object -> ops (recurses the type spec, dotted paths for nested structs)
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

        // Dictionary source -> ops (walks the compiled schema's flat field table, pulling
        // values by name; nested structs read from a nested dictionary of the same shape).
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
                    srcs[d + 1] = val as System.Collections.IDictionary;   // null subtree -> keep defaults
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

        // Pre-serialize a field's variable payload (for sizing); fixed fields keep the raw value.
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
            else if (op.Kind == VSTR) SetBytesString(s, buf, cap, cpath, op.Prepared);   // variable: never fails on length
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
            else if (op.Kind == ENUM)   // write the enum's backing integer (signed vs unsigned per Elem)
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

        // A variable string array's frame: whole [u16 len][cap bytes] slots (the layout
        // UnpackStringArray reads back), one per element; live count = element count.
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

        // ---- map: the self-describing tagged value tree (`map` fields) ----------------
        // Body wire (little-endian): [u16 n] entry*n; entry := [u8 keylen][key] value;
        // value := [u8 kind] payload -- scalars store their raw bytes, VSTR is [u16 len]
        // [bytes], VARR is [u16 n] value*n, MAP is a nested body. Built/parsed here
        // (managed) rather than mirroring the C DartMapWriter struct; dart_set_map
        // validates the body we build.
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
                if (e.Value == null) continue;   // a map has no null kind; omit the key
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

        // --- decode: message bytes -> nested dict / typed object ---
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
            if (v.kind == ENUM) return v.v.i;   // the number; ToObject casts it to the enum type
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

        // string-array slots are [u16 len][cap bytes] each; clamp len like the C reader
        // so a hostile message can never over-read
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

        internal static object ToObject(Type t, Dictionary<string, object> dict)
        {
            var spec = Spec(t);
            object obj = Activator.CreateInstance(t);
            foreach (var fp in spec.Fields)
            {
                object val;
                if (!dict.TryGetValue(fp.WireName, out val) || val == null) continue;
                object set;
                if (fp.Kind == STRUCT) set = ToObject(fp.Nested, (Dictionary<string, object>)val);
                else if (fp.Kind == ARR || fp.Kind == VARR || fp.Kind == STR || fp.Kind == VSTR
                         || fp.Kind == MAP) set = val;   // already string / typed array / dict
                else if (fp.Kind == ENUM) set = Enum.ToObject(fp.Field.FieldType, val);   // number -> enum
                else set = Convert.ChangeType(val, fp.Field.FieldType);
                fp.Field.SetValue(obj, set);
            }
            return obj;
        }
    }
}
