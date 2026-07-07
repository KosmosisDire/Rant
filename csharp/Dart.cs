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
//   var node = Dart.Node.Open("robot1", new Dart.NodeOptions { Domain = 7 });
//   var ch = node.CreateChannel("pose", Dart.Role.PubSub, typeof(Pose),
//                               new Dart.Qos { Reliability = Dart.Reliability.Reliable });
//   node.OnMessage(m => Console.WriteLine(m.Value));   // decoded Pose for a typed channel
//   while (true) { node.Poll(1); /* send/receive */ }  // single-threaded: drive poll yourself
//
// Do NOT call node/channel methods from inside OnMessage/OnEvent (the poll loop is
// mid-callback).

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
    public enum SendStatus { Ok = 0, NoChannel = -1, TooBig = -2, BadRole = -3, OutOfMemory = -4 }

    public enum EventKind
    {
        PeerUp = 0, PeerDown, PeerInterest, MessageLost, MessageTooBig, NameCollision,
        QosIncompatible, SchemaMismatch, PeerRefused, InterestOverflow, MetaTruncated,
        PeerMetaTooBig
    }

    public enum FieldType : byte
    {
        U8 = 0, U16, U32, U64, I8, I16, I32, I64, F32, F64, Bool, Array, Struct
    }

    // ---- native struct layouts (mirror the C exactly) ---------------------------

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartBytes { public IntPtr data; public UIntPtr len; }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartString { public IntPtr data; public UIntPtr len; }

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
        public DartString sender_name;
        public DartString channel_name;
        public DartBytes data;
        public IntPtr schema;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartEvent
    {
        public int kind;
        public IntPtr detail;                  // const char*
        public IntPtr user;
        public uint peer;
        public ushort channel;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)] public byte[] ip;
        public byte ip_len;
        public ushort port;
        public ulong lost_first;
        public ulong lost_count;
        public ulong too_big_bytes;
        public ulong identity;
        public ushort publish_topics;
        public ushort receive_topics;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartAllocator
    {
        public IntPtr page_realloc;
        public IntPtr shared;
        public IntPtr owned;
        public uint page_size;
        public UIntPtr max_bytes;
        public UIntPtr in_use;
        public UIntPtr peak;
        public ulong alloc_calls;
        public ulong pages_live;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DartSchemaFieldInfo
    {
        public DartString name;
        public byte kind;
        public byte elem;
        public ushort count;
        public ushort depth;
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
        internal static extern int dart_node_poll(IntPtr node, int timeout_ms);
        [DllImport(LIB, CallingConvention = CC)]
        internal static extern void dart_node_close(IntPtr node, int send_bye);
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
        internal static extern DartString dart_schema_name(IntPtr s);
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
        internal static extern int dart_get_value(DartBytes msg, IntPtr s, ushort field, out DartValue outv);
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
    }

    public sealed class NodeOptions
    {
        public ushort Domain = 0;
        public ushort MaxChannels = 0;
        public bool DisableShm = false;
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

    /// <summary>Mark a struct/class as a DART message: its fields become the schema.</summary>
    [AttributeUsage(AttributeTargets.Struct | AttributeTargets.Class)]
    public sealed class DartSchemaAttribute : Attribute
    {
        public string Name;   // wire type name; defaults to the class name
        public DartSchemaAttribute(string name = null) { Name = name; }
    }

    /// <summary>A fixed-length array field: the element count on the wire.</summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class DartArrayAttribute : Attribute
    {
        public int Count;
        public DartArrayAttribute(int count) { Count = count; }
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

        private Schema(IntPtr h) { Handle = h; }

        public static Schema Compile(string text)
        {
            IntPtr err;
            IntPtr h = Native.dart_schema_compile(Codec.SchemaAlloc, IntPtr.Zero, Codec.CStr(text), out err);
            if (h == IntPtr.Zero)
                throw new SchemaException("schema compile failed near: " + Codec.PtrToStr(err));
            return new Schema(h);
        }

        /// <summary>Reflect a [DartSchema] type into a compiled schema (class -> DSL -> compile).</summary>
        public static Schema FromType(Type t)
        {
            var s = Compile(Codec.TypeDsl(t));
            s.ClrType = t;
            return s;
        }

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
        public string Detail;
        public uint Peer;
        public ushort Channel;
        public ulong LostFirst;
        public ulong LostCount;
        private string _line;

        internal static Event FromNative(IntPtr evPtr, ref DartEvent e)
        {
            var buf = new byte[192];
            Native.dart_event_str(evPtr, buf, (UIntPtr)buf.Length);
            return new Event
            {
                Kind = (EventKind)e.kind,
                Detail = e.detail != IntPtr.Zero ? Codec.PtrToStr(e.detail) : "",
                Peer = e.peer,
                Channel = e.channel,
                LostFirst = e.lost_first,
                LostCount = e.lost_count,
                _line = Codec.CBufStr(buf),
            };
        }

        public override string ToString() => _line;
    }

    // ---- channel ----------------------------------------------------------------

    public sealed class Channel
    {
        private readonly Node _node;
        private readonly IntPtr _handle;
        internal readonly Schema Schema;

        internal Channel(Node node, IntPtr handle, Schema schema)
        {
            _node = node; _handle = handle; Schema = schema;
        }

        /// <summary>Publish bytes/string (raw) or a [DartSchema] object (encoded via the
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

        private Node() { }

        public static Node Open(string name = null, NodeOptions options = null,
                                Action<Message> onMessage = null, Action<Event> onEvent = null)
        {
            options = options ?? new NodeOptions();
            var node = new Node { _onMsg = onMessage, _onEvt = onEvent };
            lock (s_reg) { node._id = s_nextId++; s_nodes[node._id] = node; }

            var co = new DartNodeOpts
            {
                domain = options.Domain,
                max_channels = options.MaxChannels,
                disable_shm = (byte)(options.DisableShm ? 1 : 0),
                user_data = (IntPtr)node._id,
            };
            // The node retains these pointers for its lifetime, so keep them alive
            // (freed in Close), matching the C++ wrapper.
            node._discGroup = Codec.CStrPtr(options.DiscoveryGroup);
            node._mcastIf = Codec.CStrPtr(options.MulticastInterface);
            co.net.data_port = options.DataPort;
            co.net.discovery_group = node._discGroup;
            co.net.discovery_port = options.DiscoveryPort;
            co.net.multicast_interface = node._mcastIf;
            co.net.multicast_ttl = options.MulticastTtl;
            co.net.fragment_size = options.FragmentSize;
            co.discovery.announce_interval_us = options.AnnounceIntervalUs;
            co.discovery.peer_timeout_us = options.PeerTimeoutUs;
            co.discovery.max_peers = options.MaxPeers;

            node._alloc = Codec.DefaultAllocator();
            byte[] cname = string.IsNullOrEmpty(name) ? null : Codec.CStr(name);
            IntPtr h = Native.dart_node_open(ref node._alloc, cname, s_onMsg, s_onEvt, ref co);

            if (h == IntPtr.Zero)
            {
                lock (s_reg) s_nodes.Remove(node._id);
                Codec.FreeCStr(node._discGroup); Codec.FreeCStr(node._mcastIf);
                throw new InvalidOperationException("dart_node_open failed (bad options or OOM)");
            }
            node._handle = h;
            return node;
        }

        public Node OnMessage(Action<Message> fn) { _onMsg = fn; return this; }
        public Node OnEvent(Action<Event> fn) { _onEvt = fn; return this; }

        /// <summary>Create a topic. schema may be null (raw), a Schema, a [DartSchema]
        /// Type, or DSL text.</summary>
        public Channel CreateChannel(string name, Role role = Role.PubSub, object schema = null, Qos qos = null)
        {
            qos = qos ?? new Qos();
            Schema sch = AsSchema(schema);
            var co = new DartChannelOpts();
            co.qos.reliability = (int)qos.Reliability;
            co.qos.keep_last = qos.KeepLast;
            co.qos.catch_up = qos.CatchUp;
            co.qos.max_message_bytes = qos.MaxMessageBytes;
            co.qos.heartbeat_us = qos.HeartbeatUs;
            co.qos.repair_delay_us = qos.RepairDelayUs;
            co.qos.backpressure_wait_us = qos.BackpressureWaitUs;
            co.qos.shm_max_bytes = qos.ShmMaxBytes;

            IntPtr h = Native.dart_node_create_channel(_handle, Codec.CStr(name), (int)role,
                sch != null ? sch.Handle : IntPtr.Zero, ref co);
            if (h == IntPtr.Zero)
                throw new InvalidOperationException("create_channel failed (reserve full, bad name, or OOM)");
            ushort idx = Native.dart_channel_index(h);
            if (sch != null) { _schemas.Add(sch); _channelTypes[idx] = sch.ClrType; }
            return new Channel(this, h, sch);
        }

        private Schema AsSchema(object schema)
        {
            if (schema == null) return null;
            if (schema is Schema s) return s;
            if (schema is Type t) return Schema.FromType(t);
            if (schema is string text) return Schema.Compile(text);
            throw new ArgumentException("schema must be null, a Schema, a [DartSchema] Type, or DSL text");
        }

        /// <summary>One loop tick: drives discovery, RX, timers, and flushes queued TX.
        /// timeoutMs blocks up to that long in the socket wait (0 = non-blocking). Drive
        /// this from your own loop -- background threading was removed for now, so a node
        /// is single-threaded: call Poll, send, and handlers all on one thread.</summary>
        public int Poll(int timeoutMs = 0)
        {
            return Native.dart_node_poll(_handle, timeoutMs);
        }

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

        public void Close(bool sendBye = true)
        {
            if (_handle != IntPtr.Zero)
            {
                Native.dart_node_close(_handle, sendBye ? 1 : 0);
                _handle = IntPtr.Zero;
            }
            lock (s_reg) s_nodes.Remove(_id);
            foreach (var s in _schemas) s.Dispose();
            _schemas.Clear();
            Codec.FreeCStr(_discGroup); _discGroup = IntPtr.Zero;
            Codec.FreeCStr(_mcastIf); _mcastIf = IntPtr.Zero;
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
                node._onMsg(Message.FromNative(ref m, clr));
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
                node._onEvt(Event.FromNative(evPtr, ref e));
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
            I64 = 7, F32 = 8, F64 = 9, BOOL = 10, ARR = 11, STRUCT = 12;

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

        internal static string Str(DartString s)
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

        // --- reflection: [DartSchema] type -> field plan -> DSL ---
        private sealed class FieldPlan
        {
            public FieldInfo Field;
            public string WireName;
            public byte Kind;
            public byte Elem;
            public int Count;
            public Type Nested;
        }
        private sealed class TypeSpec { public string Name; public List<FieldPlan> Fields; }
        private static readonly Dictionary<Type, TypeSpec> s_specs = new Dictionary<Type, TypeSpec>();

        private static TypeSpec Spec(Type t)
        {
            lock (s_specs)
            {
                TypeSpec cached;
                if (s_specs.TryGetValue(t, out cached)) return cached;
                var attr = (DartSchemaAttribute)Attribute.GetCustomAttribute(t, typeof(DartSchemaAttribute));
                if (attr == null) throw new SchemaException(t.Name + " is missing [DartSchema]");
                string name = string.IsNullOrEmpty(attr.Name) ? t.Name : attr.Name;
                FieldInfo[] fields = t.GetFields(BindingFlags.Public | BindingFlags.Instance);
                Array.Sort(fields, (a, b) => a.MetadataToken.CompareTo(b.MetadataToken));  // declaration order
                var plans = new List<FieldPlan>();
                foreach (var f in fields)
                {
                    var fa = (DartFieldAttribute)Attribute.GetCustomAttribute(f, typeof(DartFieldAttribute));
                    var arr = (DartArrayAttribute)Attribute.GetCustomAttribute(f, typeof(DartArrayAttribute));
                    var plan = new FieldPlan { Field = f, WireName = fa != null ? fa.Name : f.Name };
                    byte k;
                    if (arr != null)
                    {
                        Type et = f.FieldType.GetElementType();
                        if (et == null || !ScalarKind.TryGetValue(et, out k))
                            throw new SchemaException("array field " + f.Name + " element must be a scalar");
                        plan.Kind = ARR; plan.Elem = k; plan.Count = arr.Count;
                    }
                    else if (ScalarKind.TryGetValue(f.FieldType, out k)) { plan.Kind = k; }
                    else if (Attribute.GetCustomAttribute(f.FieldType, typeof(DartSchemaAttribute)) != null)
                    { plan.Kind = STRUCT; plan.Nested = f.FieldType; }
                    else throw new SchemaException("unsupported field type " + f.FieldType + " on " + f.Name
                        + " (use scalars, [DartArray] arrays, or nested [DartSchema] types)");
                    plans.Add(plan);
                }
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
            if (f.Kind == ARR) return f.WireName + ": " + Token[f.Elem] + "[" + f.Count + "]";
            return f.WireName + ": " + Token[f.Kind];
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
                var node = new object[] { Str(info.name), info.kind, info.elem, (int)info.count, null };
                int d = info.depth;
                stack[d].Add(node);
                if (info.kind == STRUCT)
                {
                    var ch = new List<object[]>();
                    node[4] = ch;
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
            int count = (int)node[3];
            var children = (List<object[]>)node[4];
            if (kind == STRUCT)
            {
                var parts = new List<string>();
                foreach (var c in children) parts.Add(NodeLine(c));
                return name + ": { " + string.Join(", ", parts) + " }";
            }
            if (kind == ARR) return name + ": " + Token[elem] + "[" + count + "]";
            return name + ": " + Token[kind];
        }

        // --- encode: object -> message bytes (walks the reflection spec) ---
        internal static byte[] Encode(IntPtr s, object value)
        {
            uint size = Native.dart_schema_size(s);
            byte[] buf = new byte[size];
            GCHandle gh = GCHandle.Alloc(buf, GCHandleType.Pinned);
            try
            {
                IntPtr p = size > 0 ? gh.AddrOfPinnedObject() : IntPtr.Zero;
                Native.dart_schema_message_default(s, p, (UIntPtr)size);
                EncodeInto(s, p, (UIntPtr)size, Spec(value.GetType()), value, "");
                return buf;
            }
            finally { gh.Free(); }
        }

        private static void EncodeInto(IntPtr s, IntPtr buf, UIntPtr cap, TypeSpec spec, object obj, string prefix)
        {
            foreach (var fp in spec.Fields)
            {
                object val = fp.Field.GetValue(obj);
                string path = prefix + fp.WireName;
                byte[] cpath = CStr(path);
                if (fp.Kind == STRUCT) { EncodeInto(s, buf, cap, Spec(fp.Nested), val, path + "."); }
                else if (fp.Kind == ARR)
                {
                    byte[] packed = PackArray(fp.Elem, val);
                    GCHandle ah = GCHandle.Alloc(packed, GCHandleType.Pinned);
                    try
                    {
                        var b = new DartBytes
                        {
                            data = packed.Length > 0 ? ah.AddrOfPinnedObject() : IntPtr.Zero,
                            len = (UIntPtr)packed.Length
                        };
                        Native.dart_set_array(buf, cap, s, cpath, b);
                    }
                    finally { ah.Free(); }
                }
                else if (fp.Kind == F32) Native.dart_set_f32(buf, cap, s, cpath, Convert.ToSingle(val));
                else if (fp.Kind == F64) Native.dart_set_f64(buf, cap, s, cpath, Convert.ToDouble(val));
                else if (fp.Kind >= I8 && fp.Kind <= I64) Native.dart_set_int(buf, cap, s, cpath, Convert.ToInt64(val));
                else if (fp.Kind == BOOL) Native.dart_set_uint(buf, cap, s, cpath, (bool)val ? 1UL : 0UL);
                else Native.dart_set_uint(buf, cap, s, cpath, Convert.ToUInt64(val));
            }
        }

        private static byte[] PackArray(byte elem, object val)
        {
            if (val is byte[] u8) return u8;
            var a = (Array)val;
            int esz = ScalarSize[elem];
            byte[] outb = new byte[a.Length * esz];
            Buffer.BlockCopy(a, 0, outb, 0, outb.Length);
            return outb;
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
            if (v.kind == ARR)
            {
                byte[] raw = v.bytes.data != IntPtr.Zero && (ulong)v.bytes.len > 0
                    ? Bytes(v.bytes) : Array.Empty<byte>();
                return UnpackArray(v.elem, raw);
            }
            if (v.kind == F32 || v.kind == F64) return v.v.f;
            if (v.kind >= I8 && v.kind <= I64) return v.v.i;
            if (v.kind == BOOL) return v.v.u != 0;
            return v.v.u;
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
                else if (fp.Kind == ARR) set = val;                       // already the element-typed array
                else set = Convert.ChangeType(val, fp.Field.FieldType);
                fp.Field.SetValue(obj, set);
            }
            return obj;
        }
    }
}
