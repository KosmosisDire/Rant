// DART C# wrapper: a thin P/Invoke layer over the prebuilt native library.
//
// DART = Discovery And Realtime Transport, a dependency-free C99 middleware.
// The API mirrors the C++/Python wrappers (Node / Channel / Schema / Qos ...),
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
//   var node = new Dart.Node("robot1",
//                            onMessage: m => Console.WriteLine(m.Value),  // decoded Pose for a typed channel
//                            onEvent: e => Console.Error.WriteLine(e));   // wired up before the ctor returns
//   var ch = new Dart.Channel<Pose>(node, "pose",
//                                   qos: new Dart.Qos { Reliability = Dart.Reliability.Reliable });
//   node.Start();                                      // C-level service thread owns the loop
//   ch.Send(new Pose { X = 1, Frame = "map" });        // thread-safe from any thread
//
// Schemas come straight from the type: public fields become the wire fields, in
// declaration order. [DartArray(n)] fixes an array's element count, [DartString(cap)]
// fixes a string's byte capacity, [DartField("name")] overrides a wire name, and
// [DartSchema("Name")] optionally overrides the wire type name (the class name by
// default). Nested structs/classes just work.
//
// Threading: every Node/Channel call is thread-safe (a node-level lock in the C
// core serializes them). Drive a node either with Start() (a C background service
// thread runs the loop; handlers fire on it, never two at once) or by calling
// Poll() from your own loop (Unity: Poll(0) from Update() keeps handlers on the
// main thread). From inside OnMessage/OnEvent, Channel.Send and read-only
// queries are allowed; Poll/channel create/SetRole/Drain/Start/Stop/Close are
// refused (SendStatus.State / exception), never corrupting.

using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;

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
        Ok = 0, NoChannel = -1, TooBig = -2, BadRole = -3, OutOfMemory = -4,
        State = -5,   // wrong state: Poll while started, or a call a handler may not make
        NoSys = -6    // not compiled in (Start under DART_NO_THREADS)
    }

    public enum EventKind
    {
        PeerUp = 0, PeerDown, PeerInterest, MessageLost, Error
    }

    // The specific error carried by an EventKind.Error event (Event.Error / Node.LastError).
    // Mirrors DartErrorKind in node/core.h.
    public enum ErrorKind
    {
        None = 0,
        NameCollision, QosIncompatible, SchemaMismatch, InterestOverflow,
        MetaTruncatedInterest, MetaTruncatedSchema, PeerMetaTooBig, MessageTooBig,
        PeerRefused, EvictedUnsent,
        Oom, Platform, Socket, Bind, McastJoin, Send, Recv, Poll, Waker
    }

    public enum FieldType : byte
    {
        U8 = 0, U16, U32, U64, I8, I16, I32, I64, F32, F64, Bool, Array, Struct, String
    }

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
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartChannelOpts { public DartQos qos; }

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
        public ushort max_channels;
        public IntPtr user_data;
        public byte disable_shm;
        public byte fetch_details;
        public DartNodeNet net;
        public DartNodeDiscovery discovery;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartMsg
    {
        public IntPtr node;
        public IntPtr user;
        public ushort channel_id;
        public uint sender_id;
        public DartStringView sender_name;
        public DartStringView channel_name;
        public DartBytes data;
        public IntPtr schema;
        public ulong recv_us;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartEvent
    {
        public int kind;
        public int error;                      // DartErrorKind (Error events)
        public IntPtr channel_name;            // const char* (channel-scoped events; else null)
        public IntPtr user;
        public uint peer;
        public ushort channel;
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

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void DartMsgFn(IntPtr msg);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void DartEventFn(IntPtr ev);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate IntPtr DartAllocFn(IntPtr user, IntPtr ptr, UIntPtr size);

    // ---- native entry points ----------------------------------------------------

    internal static class Native
    {
        internal const string LIB = "dart";
        private const CallingConvention CC = CallingConvention.Cdecl;

        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr dart_node_open(ref DartAllocator alloc, byte[] name,
            DartMsgFn on_message, DartEventFn on_event, ref DartNodeOpts opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern DartEvent dart_last_error(IntPtr node);
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
        internal static extern IntPtr dart_node_create_channel(IntPtr node, byte[] name, int role,
            IntPtr schema, ref DartChannelOpts opts);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern IntPtr dart_node_channel(IntPtr node, ushort index);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_channel_send(IntPtr ch, DartBytes data);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_channel_set_role(IntPtr ch, int role);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern ushort dart_channel_index(IntPtr ch);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_channel_match_count(IntPtr ch);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_channel_drain(IntPtr ch, int timeout_ms);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_channel_take(IntPtr ch, ref DartMsg msg, int timeout_ms);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_channel_dispatch(IntPtr ch, int max_msgs, int timeout_ms);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern int dart_node_dispatch(IntPtr node, int max_msgs, int timeout_ms);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern void dart_channel_queue_stats(IntPtr ch, out uint msgs,
            out uint bytes, out uint capacity, out uint dropped);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern void dart_node_mem_stats(IntPtr node, out UIntPtr in_use,
            out UIntPtr peak, out ulong alloc_calls);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern void dart_node_backpressure_stats(IntPtr node, out ulong waited_us,
            out uint waited_sends);
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
    }

    // ---- config + reflection attributes -----------------------------------------

    public sealed class Qos
    {
        public Reliability Reliability = Reliability.BestEffort;
        public ushort KeepLast = 0;
        public ushort CatchUp = 0;
        public uint MaxMessageBytes = 0;
        public uint HeartbeatUs = 0;
        public uint RepairDelayUs = 0;
        public uint BackpressureWaitUs = 0;
        public uint ShmMaxBytes = 0;
        /// <summary>Consumer-queue cap in bytes for TryTake/Dispatch; 0 = the queue
        /// appears lazily on first use and grows on demand to 1 MB.</summary>
        public uint QueueBytes = 0;
    }

    public sealed class NodeOptions
    {
        public ushort Domain = 0;
        public ushort MaxChannels = 0;
        public bool DisableShm = false;
        /// <summary>Greedily fetch every peer topic's name + schema (observer/debugger
        /// UIs); costs memory in proportion to the peers' topic counts.</summary>
        public bool FetchDetails = false;
        public ushort DataPort = 0;
        public string DiscoveryGroup = null;
        public ushort DiscoveryPort = 0;
        public string MulticastInterface = null;
        public byte MulticastTtl = 0;
        public ushort FragmentSize = 0;
        public uint AnnounceIntervalUs = 0;
        public uint PeerTimeoutUs = 0;
        public ushort MaxPeers = 0;
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

    public sealed class Message
    {
        public ushort ChannelId;
        public uint SenderId;
        public string SenderName;
        public string ChannelName;
        public byte[] Data;
        /// <summary>Node monotonic clock (microseconds) when the poll RECEIVED the message
        /// (a queued channel stamps at enqueue), so a frame-paced consumer measures true
        /// arrival times, never its own cadence.</summary>
        public ulong RecvUs;
        public Dictionary<string, object> Fields;   // decoded (schema'd messages), else null
        public object Value;                          // typed instance for a typed channel, else Fields

        public string Text => Encoding.UTF8.GetString(Data);
        public T As<T>() => (T)Value;

        internal static Message FromNative(ref DartMsg m, Type clrType)
        {
            var msg = new Message
            {
                ChannelId = m.channel_id,
                SenderId = m.sender_id,
                SenderName = Codec.Str(m.sender_name),
                ChannelName = Codec.Str(m.channel_name),
                Data = Codec.Bytes(m.data),
                RecvUs = m.recv_us,
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
            => $"Message(channel={ChannelName}, from={SenderName}, {Data.Length} bytes)";
    }

    public sealed class Event
    {
        public EventKind Kind;
        public ErrorKind Error;         // the specific error when Kind == EventKind.Error, else None
        public string ChannelName;      // our channel's name for channel-scoped events, else null
        public uint Peer;
        public ushort Channel;
        public int OsError;             // errno / WSAGetLastError for socket failures, else 0
        public ulong LostFirst;
        public ulong LostCount;
        public ulong TooBigBytes;
        public string SchemaDetail;     // SchemaMismatch: what exactly was incompatible, else null
        private string _line;

        /// <summary>True if this event reports something going wrong (Kind == EventKind.Error).</summary>
        public bool IsError => Kind == EventKind.Error;

        // format an event returned BY VALUE (dart_last_error): dart_event_str wants a
        // pointer, so briefly marshal the struct to unmanaged memory.
        internal static Event FromValue(DartEvent e)
        {
            IntPtr p = Marshal.AllocHGlobal(Marshal.SizeOf<DartEvent>());
            try { Marshal.StructureToPtr(e, p, false); return FromNative(p, ref e); }
            finally { Marshal.FreeHGlobal(p); }
        }

        internal static Event FromNative(IntPtr evPtr, ref DartEvent e)
        {
            var buf = new byte[192];
            Native.dart_event_str(evPtr, buf, (UIntPtr)buf.Length);
            return new Event
            {
                Kind = (EventKind)e.kind,
                Error = (ErrorKind)e.error,
                ChannelName = e.channel_name != IntPtr.Zero ? Codec.PtrToStr(e.channel_name) : null,
                Peer = e.peer,
                Channel = e.channel,
                OsError = e.os_error,
                LostFirst = e.lost_first,
                LostCount = e.lost_count,
                TooBigBytes = e.too_big_bytes,
                SchemaDetail = e.schema_detail != IntPtr.Zero ? Codec.PtrToStr(e.schema_detail) : null,
                _line = Codec.CBufStr(buf),
            };
        }

        public override string ToString() => _line;
    }

    // ---- channel ----------------------------------------------------------------

    public class Channel
    {
        private readonly Node _node;
        private readonly IntPtr _handle;
        internal readonly Schema Schema;

        /// <summary>Create a raw (schemaless) topic on the node: send/receive bytes or
        /// UTF-8 strings.</summary>
        public Channel(Node node, string name, Role role = Role.PubSub, Qos qos = null)
            : this(node, name, (Schema)null, role, qos) { }

        /// <summary>Create a typed topic with an explicit Schema (compiled from DSL or
        /// reflected). Channel&lt;T&gt; is the shorthand for the reflected case.</summary>
        public Channel(Node node, string name, Schema schema, Role role = Role.PubSub, Qos qos = null)
        {
            _node = node;
            Schema = schema;
            _handle = node.CreateNativeChannel(name, role, schema, qos);
        }

        /// <summary>Publish bytes/string (raw) or a message object (encoded via the
        /// channel schema). Returns a SendStatus.</summary>
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
                r = Native.dart_channel_send(_handle, b);
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
                    "channel has no schema; send bytes/string, or create the channel with a schema");
            return Send(Schema.Encode(value));
        }

        public SendStatus SetRole(Role role)
        {
            return (SendStatus)Native.dart_channel_set_role(_handle, (int)role);
        }

        public ushort Index => Native.dart_channel_index(_handle);

        public int MatchCount()
        {
            return Native.dart_channel_match_count(_handle);
        }

        public bool Drain(int timeoutMs)
        {
            return Native.dart_channel_drain(_handle, timeoutMs) == 1;
        }

        /// <summary>Pop the next queued message, fully copied out. The FIRST
        /// TryTake/Dispatch switches this channel to QUEUED delivery: its messages then
        /// queue instead of firing the node handler on the poll thread, and exactly one
        /// thread of your choosing consumes them here (per channel). The queue grows on
        /// demand to Qos.QueueBytes (0 = 1 MB); at the cap a best-effort channel
        /// overwrites oldest (EventKind.MsgLost fires), a reliable one backpressures the
        /// publisher. timeoutMs: 0 = just check, &gt;0 = wait up to that long, negative =
        /// wait indefinitely (the wait sleeps beside a running service thread and drives
        /// the poll loop itself otherwise).</summary>
        public bool TryTake(out Message message, int timeoutMs = 0)
        {
            message = null;
            var m = new DartMsg();
            if (Native.dart_channel_take(_handle, ref m, timeoutMs) != 1) return false;
            message = Message.FromNative(ref m, _node.ClrTypeOf(m.channel_id));
            return true;
        }

        /// <summary>Drain the queue by running the node's OnMessage handler on the
        /// CALLING thread, oldest first: up to maxMsgs of those queued at entry (0 =
        /// all), first waiting up to timeoutMs like TryTake. Returns the number
        /// dispatched. Unlike poll-thread callbacks these run without the node lock,
        /// so they may use the whole API.</summary>
        public int Dispatch(int maxMsgs = 0, int timeoutMs = 0)
            => Native.dart_channel_dispatch(_handle, maxMsgs, timeoutMs);

        /// <summary>Consumer-queue observability; all zeros when not queued.</summary>
        public (uint Messages, uint Bytes, uint Capacity, uint Dropped) QueueStats()
        {
            Native.dart_channel_queue_stats(_handle, out uint m, out uint b, out uint c, out uint d);
            return (m, b, c, d);
        }
    }

    /// <summary>A typed topic: T's public fields are the schema ([DartArray] /
    /// [DartString] / [DartField] refine them). Delivered messages decode to T
    /// (Message.Value / Message.As&lt;T&gt;()).</summary>
    public sealed class Channel<T> : Channel
    {
        public Channel(Node node, string name, Role role = Role.PubSub, Qos qos = null)
            : base(node, name, new Schema(typeof(T)), role, qos) { }

        public SendStatus Send(T value) => Send((object)value);

        /// <summary>Typed take: decodes straight from the queue.</summary>
        public bool TryTake(out T value, int timeoutMs = 0)
        {
            value = default(T);
            Message m;
            if (!TryTake(out m, timeoutMs) || !(m.Value is T)) return false;
            value = (T)m.Value;
            return true;
        }
    }

    // ---- node -------------------------------------------------------------------

    public sealed class Node : IDisposable
    {
        private IntPtr _handle;
        private long _id;
        private DartAllocator _alloc;
        private IntPtr _discGroup;   // native strings the node retains for its lifetime
        private IntPtr _mcastIf;
        private Action<Message> _onMsg;
        private Action<Event> _onEvt;
        private readonly Dictionary<ushort, Type> _channelTypes = new Dictionary<ushort, Type>();
        private readonly List<Schema> _schemas = new List<Schema>();

        // rooted so the GC never collects the trampolines handed to native code.
        private static readonly DartMsgFn s_onMsg = OnMessageTramp;
        private static readonly DartEventFn s_onEvt = OnEventTramp;
        private static readonly Dictionary<long, Node> s_nodes = new Dictionary<long, Node>();
        private static readonly object s_reg = new object();
        private static long s_nextId = 1;

        /// <summary>Open a node. onMessage/onEvent are required (pass null for either if
        /// truly not needed) so they are wired in before the constructor even returns --
        /// no early peer/error event is ever missed. OnMessage/OnEvent below can still
        /// rebind them later.</summary>
        public Node(string name, Action<Message> onMessage, Action<Event> onEvent,
                    NodeOptions options = null)
        {
            options = options ?? new NodeOptions();
            _onMsg = onMessage;
            _onEvt = onEvent;
            lock (s_reg) { _id = s_nextId++; s_nodes[_id] = this; }

            var co = new DartNodeOpts
            {
                domain = options.Domain,
                max_channels = options.MaxChannels,
                disable_shm = (byte)(options.DisableShm ? 1 : 0),
                fetch_details = (byte)(options.FetchDetails ? 1 : 0),
                user_data = (IntPtr)_id,
            };
            // The node retains these pointers for its lifetime, so keep them alive
            // (freed in Close), matching the C++ wrapper.
            _discGroup = Codec.CStrPtr(options.DiscoveryGroup);
            _mcastIf = Codec.CStrPtr(options.MulticastInterface);
            co.net.data_port = options.DataPort;
            co.net.discovery_group = _discGroup;
            co.net.discovery_port = options.DiscoveryPort;
            co.net.multicast_interface = _mcastIf;
            co.net.multicast_ttl = options.MulticastTtl;
            co.net.fragment_size = options.FragmentSize;
            co.discovery.announce_interval_us = options.AnnounceIntervalUs;
            co.discovery.peer_timeout_us = options.PeerTimeoutUs;
            co.discovery.max_peers = options.MaxPeers;

            _alloc = Codec.DefaultAllocator();
            byte[] cname = string.IsNullOrEmpty(name) ? null : Codec.CStr(name);
            IntPtr h = Native.dart_node_open(ref _alloc, cname, s_onMsg, s_onEvt, ref co);

            if (h == IntPtr.Zero)
            {
                lock (s_reg) s_nodes.Remove(_id);
                Codec.FreeCStr(_discGroup); Codec.FreeCStr(_mcastIf);
                // the node does not exist, so read the reason from the process-global slot
                Event err = LastOpenError();
                throw new InvalidOperationException("dart_node_open failed: " + err);
            }
            _handle = h;
        }

        /// <summary>Rebind the message handler set at construction. Rarely needed: the
        /// constructor already requires an initial one.</summary>
        public Node OnMessage(Action<Message> fn) { _onMsg = fn; return this; }
        /// <summary>Rebind the event handler set at construction. Rarely needed: the
        /// constructor already requires an initial one.</summary>
        public Node OnEvent(Action<Event> fn) { _onEvt = fn; return this; }

        /// <summary>Convenience helper: construct and return a raw (schemaless) Channel
        /// on this node -- exactly new Channel(this, name, role, qos).</summary>
        public Channel CreateChannel(string name, Role role = Role.PubSub, Qos qos = null)
            => new Channel(this, name, role, qos);

        /// <summary>Convenience helper: construct and return a typed Channel with an
        /// explicit Schema -- exactly new Channel(this, name, schema, role, qos).</summary>
        public Channel CreateChannel(string name, Schema schema, Role role = Role.PubSub, Qos qos = null)
            => new Channel(this, name, schema, role, qos);

        /// <summary>Convenience helper: construct and return a typed Channel&lt;T&gt; on
        /// this node -- exactly new Channel&lt;T&gt;(this, name, role, qos).</summary>
        public Channel<T> CreateChannel<T>(string name, Role role = Role.PubSub, Qos qos = null)
            => new Channel<T>(this, name, role, qos);

        // the native create behind the Channel constructors: makes the handle and
        // registers the schema/decode type against the channel index.
        internal IntPtr CreateNativeChannel(string name, Role role, Schema schema, Qos qos)
        {
            qos = qos ?? new Qos();
            var co = new DartChannelOpts();
            co.qos.reliability = (int)qos.Reliability;
            co.qos.keep_last = qos.KeepLast;
            co.qos.catch_up = qos.CatchUp;
            co.qos.max_message_bytes = qos.MaxMessageBytes;
            co.qos.heartbeat_us = qos.HeartbeatUs;
            co.qos.repair_delay_us = qos.RepairDelayUs;
            co.qos.backpressure_wait_us = qos.BackpressureWaitUs;
            co.qos.shm_max_bytes = qos.ShmMaxBytes;
            co.qos.queue_bytes = qos.QueueBytes;

            IntPtr h = Native.dart_node_create_channel(_handle, Codec.CStr(name), (int)role,
                schema != null ? schema.Handle : IntPtr.Zero, ref co);
            if (h == IntPtr.Zero)
                throw new InvalidOperationException("channel create failed (reserve full, bad name, or OOM)");
            ushort idx = Native.dart_channel_index(h);
            if (schema != null) { _schemas.Add(schema); _channelTypes[idx] = schema.ClrType; }
            return h;
        }

        /// <summary>One loop tick: drives discovery, RX, timers, and flushes queued TX.
        /// timeoutMs blocks up to that long in the socket wait (0 = non-blocking; a send
        /// from another thread wakes it early). Returns SendStatus.State (as int) while
        /// Start() runs -- the service thread owns the loop then.</summary>
        public int Poll(int timeoutMs = 0)
        {
            return Native.dart_node_poll(_handle, timeoutMs);
        }

        /// <summary>Run the C-level background service thread: it owns the loop and fires
        /// the handlers (never two at once for one node); every Node/Channel call stays
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

        /// <summary>Dispatch every already-queued channel on the calling thread (see
        /// Channel.TryTake/Dispatch): with Start() running, this in a Unity Update() (or
        /// any UI frame) keeps every queued handler on that thread while the service
        /// thread owns the network. Waits up to timeoutMs for any queued channel to hold
        /// data; returns the number of messages dispatched.</summary>
        public int Dispatch(int maxMsgs = 0, int timeoutMs = 0)
            => Native.dart_node_dispatch(_handle, maxMsgs, timeoutMs);

        internal Type ClrTypeOf(ushort index)
        {
            Type t;
            _channelTypes.TryGetValue(index, out t);
            return t;
        }

        /// <summary>The most recent error this node reported (also delivered via OnEvent).
        /// Event.Kind is PeerUp with Error == None if none has occurred yet.</summary>
        public Event LastError => Event.FromValue(Native.dart_last_error(_handle));

        /// <summary>Why the most recent node open failed, from the process-global slot
        /// (there is no node handle on failure). The constructor already throws with
        /// this message.</summary>
        public static Event LastOpenError() => Event.FromValue(Native.dart_last_error(IntPtr.Zero));

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
            foreach (var s in _schemas) s.Dispose();
            _schemas.Clear();
            Codec.FreeCStr(_discGroup); _discGroup = IntPtr.Zero;
            Codec.FreeCStr(_mcastIf); _mcastIf = IntPtr.Zero;
            return true;
        }

        public void Dispose() { Close(); GC.SuppressFinalize(this); }
        ~Node() { try { Close(); } catch { } }

        [MonoPInvokeCallback(typeof(DartMsgFn))]
        private static void OnMessageTramp(IntPtr msgPtr)
        {
            try
            {
                var m = Marshal.PtrToStructure<DartMsg>(msgPtr);
                Node node; Type clr = null;
                lock (s_reg) s_nodes.TryGetValue((long)m.user, out node);
                if (node == null || node._onMsg == null) return;
                node._channelTypes.TryGetValue(m.channel_id, out clr);
                node._onMsg(Message.FromNative(ref m, clr));   // fully copied: safe past the callback
            }
            catch (Exception e) { Console.Error.WriteLine("dart on_message: " + e); }
        }

        [MonoPInvokeCallback(typeof(DartEventFn))]
        private static void OnEventTramp(IntPtr evPtr)
        {
            try
            {
                var e = Marshal.PtrToStructure<DartEvent>(evPtr);
                Node node;
                lock (s_reg) s_nodes.TryGetValue((long)e.user, out node);
                if (node == null || node._onEvt == null) return;
                node._onEvt(Event.FromNative(evPtr, ref e));    // fully copied: safe past the callback
            }
            catch (Exception ex) { Console.Error.WriteLine("dart on_event: " + ex); }
        }
    }

    // ---- marshaling, allocators, schema codec + reflection ----------------------

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate IntPtr DartPageFn(IntPtr ptr, UIntPtr size);

    internal static class Codec
    {
        internal const byte U8 = 0, U16 = 1, U32 = 2, U64 = 3, I8 = 4, I16 = 5, I32 = 6,
            I64 = 7, F32 = 8, F64 = 9, BOOL = 10, ARR = 11, STRUCT = 12, STR = 13,
            VSTR = 14, VARR = 15, MAP = 16;   // the variable kinds (ride the message tail)

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
            public byte Elem;
            public int Count;
            public int StrCap;
            public Type Nested;
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
            return f.WireName + ": " + Token[f.Kind];
        }

        private static string ElemToken(byte elem, int strCap)
            => elem == STR ? "string<" + strCap + ">" : Token[elem];

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
                                          (int)info.str_cap, null };
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
            return name + ": " + Token[kind];
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
                else set = Convert.ChangeType(val, fp.Field.FieldType);
                fp.Field.SetValue(obj, set);
            }
            return obj;
        }
    }
}
