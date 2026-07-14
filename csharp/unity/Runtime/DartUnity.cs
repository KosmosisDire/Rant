// DART for Unity: a scene-level node component + shared, name-keyed channels.
//
// Put ONE DartNode component in the scene. It owns the process node (name, domain,
// lifecycle) and every other script publishes/subscribes through it, so components
// never each open a node of their own:
//
//   public struct Pose { public float X, Y, Z; }
//
//   // publish from any component
//   DartChannel<Pose> pose;
//   void Start()  { pose = DartNode.Channel<Pose>("player/pose"); }
//   void Update() { pose.Publish(new Pose { X = transform.position.x }); }
//
//   // subscribe from any other component: dies with the component, skipped while
//   // it is disabled, and always fires on the main thread
//   void Start() { DartNode.Subscribe<Pose>("player/pose", this, OnPose); }
//   void OnPose(Pose p) { transform.position = new Vector3(p.X, p.Y, p.Z); }
//
// Channels are shared by name: every script asking for "player/pose" gets the same
// DartChannel<Pose>, and its role is managed automatically (created inactive; the
// first Publish advertises pub, the first Subscribe advertises sub, the last
// unsubscribe withdraws it).
//
// Threading: the node runs the C service thread, so the wire never waits for a
// frame; every channel is queued from creation and DartNode dispatches once per
// frame, so ALL handlers fire on the main thread, where the Unity API is legal.
// Events (peer up/down, errors) reach the main thread the same way and are logged
// to the Console by default (DartNode.Events to observe them).
//
// Edit mode: the component is [ExecuteAlways]; with Run In Edit Mode on (default)
// the node is live in the editor outside play, pumped from EditorApplication.update.
// Whether publishers/subscribers exist at edit time is up to them: a channel
// acquired while the node is closed simply goes live when it opens.
//
// The low-level wrapper stays fully available: DartNode.Main.Raw is the Node, and a
// DartChannel's Raw is its Channel (TryTake, Drain, QueueStats...).
#if UNITY_5_3_OR_NEWER
using System;
using System.Collections.Generic;
using UnityEngine;

namespace Dart
{
    /// <summary>Per-message metadata for handlers that want more than the payload.</summary>
    public readonly struct MessageInfo
    {
        /// <summary>The sending node's name (never null; "unknown-peer" fallback).</summary>
        public readonly string Sender;
        /// <summary>Node monotonic clock (microseconds) at ARRIVAL on the poll side,
        /// not at dispatch: inter-arrival timing is real even when dispatch is
        /// frame-paced.</summary>
        public readonly ulong RecvUs;
        public MessageInfo(string sender, ulong recvUs) { Sender = sender; RecvUs = recvUs; }
    }

    /// <summary>A live subscription: Dispose() unsubscribes. Owner-bound
    /// subscriptions dispose themselves when the owner is destroyed.</summary>
    public sealed class DartSubscription : IDisposable
    {
        private DartChannelBase _channel;
        private DartChannelBase.Sub _sub;
        internal DartSubscription(DartChannelBase channel, DartChannelBase.Sub sub)
        {
            _channel = channel; _sub = sub;
        }
        public void Dispose()
        {
            if (_channel == null) return;
            _channel.RemoveSub(_sub);
            _channel = null; _sub = null;
        }
    }

    /// <summary>A shared, name-keyed channel on the scene's DartNode. One instance
    /// exists per name; it survives the native node closing and reopening (edit
    /// mode toggles, inspector changes) by re-creating its native channel lazily.</summary>
    public abstract class DartChannelBase
    {
        internal sealed class Sub
        {
            internal Action<Message> Fn;
            internal Component Owner;
            internal bool HasOwner;
            internal bool Dead;
        }

        private readonly DartNode _owner;
        private readonly string _name;
        private readonly Qos _qos;
        private readonly List<Sub> _subs = new List<Sub>();
        private Channel _raw;
        private Role _appliedRole = Role.Inactive;
        private int _live;          // subs not yet marked dead
        private bool _wantPub;
        private bool _warnedClosed;

        internal DartChannelBase(DartNode owner, string name, Qos qos)
        {
            _owner = owner; _name = name; _qos = qos;
        }

        public string Name => _name;
        /// <summary>The underlying wrapper Channel; null while the node is closed.</summary>
        public Channel Raw => _raw;
        /// <summary>Matched remote endpoints (0 while the node is closed).</summary>
        public int Matches => _raw != null ? _raw.MatchCount() : 0;
        /// <summary>Local handlers currently subscribed.</summary>
        public int SubscriberCount => _live;

        internal abstract Channel CreateRaw(Node node, string name, Role role, Qos qos);

        // The advertised role always mirrors actual local use: create the native
        // channel on first use, flip the role on later changes (SetRole re-advertises
        // immediately and peers rematch from cached verdicts, so this is cheap).
        internal void ApplyRole()
        {
            Role want = _wantPub ? (_live > 0 ? Role.PubSub : Role.PubOnly)
                                 : (_live > 0 ? Role.SubOnly : Role.Inactive);
            if (_raw != null)
            {
                if (want != _appliedRole) { _raw.SetRole(want); _appliedRole = want; }
                return;
            }
            if (want == Role.Inactive) return;
            Node node = _owner != null ? _owner.NativeNode : null;
            if (node == null) return;               // deferred until the node opens
            _raw = CreateRaw(node, _name, want, _qos);
            _appliedRole = want;
            _owner.RegisterIndex(_raw.Index, this);
        }

        internal void OnNodeOpened()
        {
            _warnedClosed = false;
            try { ApplyRole(); }
            catch (Exception e) { Debug.LogError("[DART] channel '" + _name + "' create failed: " + e.Message); }
        }

        internal void OnNodeClosed()
        {
            _raw = null;                            // native handle died with the node
            _appliedRole = Role.Inactive;
        }

        protected Channel PubRaw()
        {
            _wantPub = true;
            ApplyRole();
            if (_raw == null && !_warnedClosed)
            {
                _warnedClosed = true;
                Debug.LogWarning("[DART] publish on '" + _name + "' dropped: no open DartNode "
                    + "(component disabled, Run In Edit Mode off, or open failed)");
            }
            return _raw;
        }

        internal DartSubscription AddSub(Action<Message> fn, Component owner, bool hasOwner)
        {
            var s = new Sub { Fn = fn, Owner = owner, HasOwner = hasOwner };
            _subs.Add(s); _live++;
            ApplyRole();
            return new DartSubscription(this, s);
        }

        internal void RemoveSub(Sub s)
        {
            if (s == null || s.Dead) return;
            s.Dead = true; _live--;
            ApplyRole();
        }

        // Main thread, from DartNode's per-frame dispatch. Handlers may subscribe,
        // unsubscribe, and publish freely from inside a delivery.
        internal void Deliver(Message m)
        {
            bool sawDead = false;
            int n = _subs.Count;                    // additions during the loop wait for the next message
            for (int i = 0; i < n; i++)
            {
                Sub s = _subs[i];
                if (s.Dead) { sawDead = true; continue; }
                if (s.HasOwner && s.Owner == null)  // owner destroyed: auto-unsubscribe
                {
                    s.Dead = true; _live--; sawDead = true;
                    continue;
                }
                if (s.HasOwner && !OwnerActive(s.Owner)) continue;   // paused while disabled
                try { s.Fn(m); }
                catch (Exception e) { Debug.LogException(e); }
            }
            if (sawDead) { _subs.RemoveAll(IsDead); ApplyRole(); }
        }

        internal void PruneDeadOwners()
        {
            bool changed = false;
            for (int i = 0; i < _subs.Count; i++)
            {
                Sub s = _subs[i];
                if (!s.Dead && s.HasOwner && s.Owner == null) { s.Dead = true; _live--; changed = true; }
            }
            if (changed) { _subs.RemoveAll(IsDead); ApplyRole(); }
        }

        private static bool IsDead(Sub s) => s.Dead;

        private static bool OwnerActive(Component c)
        {
            var b = c as Behaviour;
            return b != null ? b.isActiveAndEnabled : c.gameObject.activeInHierarchy;
        }
    }

    /// <summary>A raw (schemaless) shared channel: bytes or UTF-8 strings.</summary>
    public sealed class DartChannel : DartChannelBase
    {
        internal DartChannel(DartNode owner, string name, Qos qos) : base(owner, name, qos) { }

        internal override Channel CreateRaw(Node node, string name, Role role, Qos qos)
            => new Channel(node, name, role, qos);

        public SendStatus Publish(byte[] data)
        {
            Channel r = PubRaw();
            return r != null ? r.Send(data) : SendStatus.NoChannel;
        }

        public SendStatus Publish(string text)
        {
            Channel r = PubRaw();
            return r != null ? r.Send(text) : SendStatus.NoChannel;
        }

        public DartSubscription Subscribe(Action<Message> handler)
        {
            if (handler == null) throw new ArgumentNullException(nameof(handler));
            return AddSub(handler, null, false);
        }

        /// <summary>Owner-bound: auto-unsubscribes when owner is destroyed, skipped
        /// while it is disabled.</summary>
        public DartSubscription Subscribe(Component owner, Action<Message> handler)
        {
            if (owner == null) throw new ArgumentNullException(nameof(owner));
            if (handler == null) throw new ArgumentNullException(nameof(handler));
            return AddSub(handler, owner, true);
        }
    }

    /// <summary>A typed shared channel: T's public fields are the schema, exactly as
    /// in the core wrapper's Channel&lt;T&gt;.</summary>
    public sealed class DartChannel<T> : DartChannelBase
    {
        internal DartChannel(DartNode owner, string name, Qos qos) : base(owner, name, qos) { }

        internal override Channel CreateRaw(Node node, string name, Role role, Qos qos)
            => new Channel<T>(node, name, role, qos);

        public SendStatus Publish(T message)
        {
            Channel r = PubRaw();
            return r != null ? ((Channel<T>)r).Send(message) : SendStatus.NoChannel;
        }

        public DartSubscription Subscribe(Action<T> handler)
        {
            if (handler == null) throw new ArgumentNullException(nameof(handler));
            return AddSub(m => { if (m.Value is T v) handler(v); }, null, false);
        }

        public DartSubscription Subscribe(Action<T, MessageInfo> handler)
        {
            if (handler == null) throw new ArgumentNullException(nameof(handler));
            return AddSub(m => { if (m.Value is T v) handler(v, new MessageInfo(m.SenderName, m.RecvUs)); },
                          null, false);
        }

        /// <summary>Owner-bound: auto-unsubscribes when owner is destroyed, skipped
        /// while it is disabled.</summary>
        public DartSubscription Subscribe(Component owner, Action<T> handler)
        {
            if (owner == null) throw new ArgumentNullException(nameof(owner));
            if (handler == null) throw new ArgumentNullException(nameof(handler));
            return AddSub(m => { if (m.Value is T v) handler(v); }, owner, true);
        }

        /// <summary>Owner-bound, with per-message metadata.</summary>
        public DartSubscription Subscribe(Component owner, Action<T, MessageInfo> handler)
        {
            if (owner == null) throw new ArgumentNullException(nameof(owner));
            if (handler == null) throw new ArgumentNullException(nameof(handler));
            return AddSub(m => { if (m.Value is T v) handler(v, new MessageInfo(m.SenderName, m.RecvUs)); },
                          owner, true);
        }
    }

    /// <summary>The scene's shared DART node. Add exactly one to the scene; every
    /// other script reaches it through the static API (DartNode.Channel&lt;T&gt;,
    /// DartNode.Subscribe). Runs in edit mode too when Run In Edit Mode is on.</summary>
    [ExecuteAlways]
    [DefaultExecutionOrder(-1000)]   // dispatch before other scripts' Update
    [DisallowMultipleComponent]
    [AddComponentMenu("DART/DART Node")]
    public sealed class DartNode : MonoBehaviour
    {
        [Tooltip("Human-readable node name, synced to peers; empty = auto node-XXXXXXXX.")]
        [SerializeField] private string nodeName = "";
        [Tooltip("Discovery domain: nodes only see peers on the same domain.")]
        [SerializeField] private int domain = 0;
        [Tooltip("Max channels this node can create (the core default of 8 is small for a scene of components).")]
        [SerializeField] private int maxChannels = 32;
        [Tooltip("Multihomed hosts (VPN adapters, WSL, docker bridges): this machine's LAN IP, so discovery uses the right interface. Empty = auto probe.")]
        [SerializeField] private string multicastInterface = "";
        [Tooltip("Keep the node live in the editor outside play mode.")]
        [SerializeField] private bool runInEditMode = true;
        [Tooltip("Log peer lifecycle, message loss, and errors to the Console.")]
        [SerializeField] private bool logEvents = true;
        [Tooltip("Play mode: survive scene loads (DontDestroyOnLoad).")]
        [SerializeField] private bool persistAcrossScenes = true;

        private static DartNode s_main;

        private Node _node;
        private bool _pollFallback;      // service thread unavailable: pump polls instead
        private bool _configDirty;
        private int _frame;
        private readonly Dictionary<string, DartChannelBase> _channels = new Dictionary<string, DartChannelBase>();
        private readonly Dictionary<ushort, DartChannelBase> _byIndex = new Dictionary<ushort, DartChannelBase>();
        private readonly List<Event> _pending = new List<Event>();   // service thread -> main
        private readonly List<Event> _drain = new List<Event>();
        private readonly object _pendingLock = new object();
        private string _openName; private int _openDomain; private int _openMax; private string _openIf;

        /// <summary>The scene's DartNode (found lazily), or null if none exists.</summary>
        public static DartNode Main
        {
            get
            {
                if (s_main == null)
                {
#if UNITY_2023_1_OR_NEWER
                    s_main = FindAnyObjectByType<DartNode>();
#else
                    s_main = FindObjectOfType<DartNode>();
#endif
                }
                return s_main;
            }
        }

        /// <summary>The underlying wrapper Node; null while closed. Escape hatch to
        /// the full API (peers via events, MemoryStats, extra channels...).</summary>
        public Node Raw => _node;
        public bool IsOpen => _node != null;
        internal Node NativeNode => _node;

        /// <summary>Peer lifecycle + errors, delivered on the main thread.</summary>
        public static event Action<Event> Events;

        /// <summary>The shared channel named <paramref name="name"/> on the scene
        /// node, created on first request. Qos applies only to that first request
        /// (null = reliable defaults); later callers share the existing channel.</summary>
        public static DartChannel<T> Channel<T>(string name, Qos qos = null)
            => RequireMain().GetChannel<T>(name, qos);

        /// <summary>The shared raw (bytes) channel named <paramref name="name"/>.</summary>
        public static DartChannel Channel(string name, Qos qos = null)
            => RequireMain().GetChannel(name, qos);

        /// <summary>One-liner subscribe on the scene node.</summary>
        public static DartSubscription Subscribe<T>(string name, Action<T> handler)
            => Channel<T>(name).Subscribe(handler);

        /// <summary>One-liner owner-bound subscribe: dies with owner, skipped while
        /// it is disabled.</summary>
        public static DartSubscription Subscribe<T>(string name, Component owner, Action<T> handler)
            => Channel<T>(name).Subscribe(owner, handler);

        /// <summary>Instance form of the static Channel&lt;T&gt;().</summary>
        public DartChannel<T> GetChannel<T>(string name, Qos qos = null)
        {
            DartChannelBase ch = LookupOrNull(name, qos);
            if (ch != null)
            {
                var typed = ch as DartChannel<T>;
                if (typed == null) throw ShapeMismatch(name, ch, "DartChannel<" + typeof(T).Name + ">");
                return typed;
            }
            var c = new DartChannel<T>(this, name, EffectiveQos(qos));
            _channels.Add(name, c);
            return c;
        }

        /// <summary>Instance form of the static raw Channel().</summary>
        public DartChannel GetChannel(string name, Qos qos = null)
        {
            DartChannelBase ch = LookupOrNull(name, qos);
            if (ch != null)
            {
                var raw = ch as DartChannel;
                if (raw == null) throw ShapeMismatch(name, ch, "a raw DartChannel");
                return raw;
            }
            var c = new DartChannel(this, name, EffectiveQos(qos));
            _channels.Add(name, c);
            return c;
        }

        private DartChannelBase LookupOrNull(string name, Qos qos)
        {
            if (string.IsNullOrEmpty(name)) throw new ArgumentException("channel name required", nameof(name));
            DartChannelBase ch;
            if (!_channels.TryGetValue(name, out ch)) return null;
            if (qos != null)
                Debug.LogWarning("[DART] channel '" + name + "' already exists: the Qos passed here is ignored (first request wins)", this);
            return ch;
        }

        private static InvalidOperationException ShapeMismatch(string name, DartChannelBase have, string want)
            => new InvalidOperationException("channel '" + name + "' already exists as " + have.GetType().Name
                + ", requested as " + want + ": one name = one message type per node");

        private static DartNode RequireMain()
        {
            DartNode m = Main;
            if (m == null)
                throw new InvalidOperationException(
                    "no DartNode in the scene: add the DartNode component to a GameObject (it owns the shared node)");
            return m;
        }

        // Reliable by default, and always queued from creation: with the service
        // thread owning the wire, only a queued channel keeps its handlers off that
        // thread (they then fire from the per-frame Dispatch, on the main thread).
        private static Qos EffectiveQos(Qos q)
        {
            Qos e = q == null
                ? new Qos { Reliability = Reliability.Reliable }
                : new Qos
                {
                    Reliability = q.Reliability, KeepLast = q.KeepLast, CatchUp = q.CatchUp,
                    MaxMessageBytes = q.MaxMessageBytes, HeartbeatUs = q.HeartbeatUs,
                    RepairDelayUs = q.RepairDelayUs, BackpressureWaitUs = q.BackpressureWaitUs,
                    ShmMaxBytes = q.ShmMaxBytes, QueueBytes = q.QueueBytes,
                };
            if (e.QueueBytes == 0) e.QueueBytes = 1 << 20;
            return e;
        }

        // ---- lifecycle ------------------------------------------------------------

        private void OnEnable()
        {
            if (s_main != null && s_main != this)
            {
                Debug.LogWarning("[DART] a DartNode already exists on '" + s_main.gameObject.name
                    + "'; the one on '" + gameObject.name + "' stays inactive", this);
                return;
            }
            s_main = this;
#if UNITY_EDITOR
            if (!Application.isPlaying) UnityEditor.EditorApplication.update += EditorPump;
#endif
            if (Application.isPlaying && persistAcrossScenes) DontDestroyOnLoad(gameObject);
            Reconcile();
        }

        private void OnDisable()
        {
#if UNITY_EDITOR
            if (!Application.isPlaying) UnityEditor.EditorApplication.update -= EditorPump;
#endif
            CloseNativeNode();
            if (s_main == this) s_main = null;
        }

        private void OnValidate() { _configDirty = true; }

        private void Update() { if (Application.isPlaying) Pump(); }

#if UNITY_EDITOR
        private void EditorPump()
        {
            if (this == null) return;   // destroyed but not yet unhooked
            Pump();
        }

        // The service thread must never survive into a new domain: its trampolines
        // point at delegates the reload collects. OnDisable covers the normal paths;
        // this is the backstop.
        [UnityEditor.InitializeOnLoadMethod]
        private static void EditorReloadGuard()
        {
            UnityEditor.AssemblyReloadEvents.beforeAssemblyReload += () =>
            {
                if (s_main != null) s_main.CloseNativeNode();
            };
        }
#endif

        // Idempotent: open/close/reopen the native node to match the inspector state.
        private void Reconcile()
        {
            _configDirty = false;
            bool shouldRun = isActiveAndEnabled && s_main == this
                && (Application.isPlaying || runInEditMode);
            bool changed = _node != null
                && (_openName != nodeName || _openDomain != domain
                    || _openMax != maxChannels || _openIf != multicastInterface);
            if (_node != null && (!shouldRun || changed)) CloseNativeNode();
            if (_node == null && shouldRun) OpenNativeNode();
        }

        private void OpenNativeNode()
        {
            try
            {
                _node = new Node(string.IsNullOrEmpty(nodeName) ? null : nodeName,
                                 RouteMessage, QueueEvent,
                                 new NodeOptions
                                 {
                                     Domain = (ushort)Mathf.Clamp(domain, 0, ushort.MaxValue),
                                     MaxChannels = (ushort)Mathf.Clamp(maxChannels, 0, ushort.MaxValue),
                                     MulticastInterface = string.IsNullOrEmpty(multicastInterface)
                                         ? null : multicastInterface,
                                 });
            }
            catch (Exception e)
            {
                Debug.LogError("[DART] node open failed: " + e.Message, this);
                _node = null;
                return;
            }
            _openName = nodeName; _openDomain = domain; _openMax = maxChannels; _openIf = multicastInterface;
            _pollFallback = !_node.Start();     // DART_NO_THREADS builds: pump polls
            foreach (DartChannelBase ch in _channels.Values) ch.OnNodeOpened();
        }

        private void CloseNativeNode()
        {
            if (_node == null) return;
            foreach (DartChannelBase ch in _channels.Values) ch.OnNodeClosed();
            lock (_byIndex) _byIndex.Clear();
            _node.Close();
            _node = null;
            lock (_pendingLock) _pending.Clear();
        }

        // ---- per-frame pump ---------------------------------------------------------

        private void Pump()
        {
            if (_configDirty) Reconcile();
            if (_node == null) return;
            DrainEvents();
            if (_pollFallback) _node.Poll(0);
            _node.Dispatch();                   // every queued channel -> this thread
            if ((++_frame & 0xFF) == 0)
                foreach (DartChannelBase ch in _channels.Values) ch.PruneDeadOwners();
        }

        internal void RegisterIndex(ushort index, DartChannelBase ch)
        {
            lock (_byIndex) _byIndex[index] = ch;
        }

        // Runs on whichever thread dispatches. Our channels are all queued, so this
        // is the main thread; a locked lookup keeps a user's own extra (non-queued)
        // channel on Raw from racing the table, it just isn't routed.
        private void RouteMessage(Message m)
        {
            DartChannelBase ch;
            lock (_byIndex) _byIndex.TryGetValue(m.ChannelId, out ch);
            if (ch != null) ch.Deliver(m);
        }

        // Service thread: park the event for the main-thread pump.
        private void QueueEvent(Event e)
        {
            lock (_pendingLock)
            {
                if (_pending.Count < 512) _pending.Add(e);
            }
        }

        private void DrainEvents()
        {
            lock (_pendingLock)
            {
                if (_pending.Count == 0) return;
                _drain.AddRange(_pending);
                _pending.Clear();
            }
            Action<Event> handler = Events;
            for (int i = 0; i < _drain.Count; i++)
            {
                Event e = _drain[i];
                if (logEvents)
                {
                    if (e.IsError) Debug.LogError("[DART] " + e, this);
                    else if (e.Kind == EventKind.MessageLost) Debug.LogWarning("[DART] " + e, this);
                    else Debug.Log("[DART] " + e, this);
                }
                if (handler != null)
                {
                    try { handler(e); }
                    catch (Exception ex) { Debug.LogException(ex); }
                }
            }
            _drain.Clear();
        }
    }
}
#endif
