// The UnityDartNode component: the scene's shared DART node. Lives in its own file
// because Unity only registers a MonoBehaviour whose class name matches the file
// name. The topic/subscription API it hands out is in DartUnity.cs.
//
//   public struct Pose { public float X, Y, Z; }
//
//   // publish from any component
//   DartTopic<Pose> pose;
//   void Start()  { pose = UnityDartNode.Topic<Pose>("player/pose"); }
//   void Update() { pose.Publish(new Pose { X = transform.position.x }); }
//
//   // subscribe from any other component: dies with the component, skipped while
//   // it is disabled, and always fires on the main thread
//   void Start() { UnityDartNode.Subscribe<Pose>("player/pose", this, OnPose); }
//   void OnPose(Pose p) { transform.position = new Vector3(p.X, p.Y, p.Z); }
#if UNITY_5_3_OR_NEWER
using System;
using System.Collections.Generic;
using UnityEngine;

namespace Dart
{
    /// <summary>The scene's shared DART node. Add exactly one to the scene; every
    /// other script reaches it through the static API (UnityDartNode.Topic&lt;T&gt;,
    /// UnityDartNode.Subscribe). Runs in edit mode too when Run In Edit Mode is on.</summary>
    [ExecuteAlways]
    [DefaultExecutionOrder(-1000)]   // dispatch before other scripts' Update
    [DisallowMultipleComponent]
    [AddComponentMenu("DART/DART DartNode")]
    public sealed class UnityDartNode : MonoBehaviour
    {
        [Tooltip("Human-readable node name, synced to peers; empty = auto node-XXXXXXXX.")]
        [SerializeField] private string nodeName = "";
        [Tooltip("Discovery domain: nodes only see peers on the same domain.")]
        [SerializeField] private int domain = 0;
        [Tooltip("Max topics this node can create (the core default of 8 is small for a scene of components).")]
        [SerializeField] private int maxTopics = 32;
        [Tooltip("Multihomed hosts (VPN adapters, WSL, docker bridges): this machine's LAN IP, so discovery uses the right interface. Empty = auto probe.")]
        [SerializeField] private string multicastInterface = "";
        [Tooltip("Keep the node live in the editor outside play mode.")]
        [SerializeField] private bool runInEditMode = true;
        [Tooltip("Log peer lifecycle, message loss, and errors to the Console.")]
        [SerializeField] private bool logEvents = true;
        [Tooltip("Play mode: survive scene loads (DontDestroyOnLoad).")]
        [SerializeField] private bool persistAcrossScenes = true;

        private static UnityDartNode s_main;

        private DartNode _node;
        private bool _pollFallback;      // service thread unavailable: pump polls instead
        private bool _configDirty;
        private int _frame;
        private readonly Dictionary<string, DartTopicBase> _topics = new Dictionary<string, DartTopicBase>();
        private readonly Dictionary<ushort, DartTopicBase> _byIndex = new Dictionary<ushort, DartTopicBase>();
        private readonly List<Event> _pending = new List<Event>();   // service thread -> main
        private readonly List<Event> _drain = new List<Event>();
        private readonly object _pendingLock = new object();
        private string _openName; private int _openDomain; private int _openMax; private string _openIf;

        /// <summary>The scene's UnityDartNode (found lazily), or null if none exists.</summary>
        public static UnityDartNode Main
        {
            get
            {
                if (s_main == null)
                {
#if UNITY_2023_1_OR_NEWER
                    s_main = FindAnyObjectByType<UnityDartNode>();
#else
                    s_main = FindObjectOfType<UnityDartNode>();
#endif
                }
                return s_main;
            }
        }

        /// <summary>The underlying wrapper DartNode; null while closed. Escape hatch to
        /// the full API (peers via events, MemoryStats, extra topics...).</summary>
        public DartNode Raw => _node;
        public bool IsOpen => _node != null;
        internal DartNode NativeNode => _node;

        /// <summary>Peer lifecycle + errors, delivered on the main thread.</summary>
        public static event Action<Event> Events;

        /// <summary>The shared topic named <paramref name="name"/> on the scene
        /// node, created on first request. The QoS parameters apply only to that
        /// first request (defaults = reliable + queued); later callers share the
        /// existing topic.</summary>
        public static DartTopic<T> Topic<T>(string name, bool reliable = true, int keepLast = 0,
                                            int catchUp = 0, int queueBytes = 0)
            => RequireMain().GetTopic<T>(name, reliable, keepLast, catchUp, queueBytes);

        /// <summary>The shared raw (bytes) topic named <paramref name="name"/>.</summary>
        public static DartTopic Topic(string name, bool reliable = true, int keepLast = 0,
                                      int catchUp = 0, int queueBytes = 0)
            => RequireMain().GetTopic(name, reliable, keepLast, catchUp, queueBytes);

        /// <summary>One-liner subscribe on the scene node.</summary>
        public static DartSubscription Subscribe<T>(string name, Action<T> handler)
            => Topic<T>(name).Subscribe(handler);

        /// <summary>One-liner owner-bound subscribe: dies with owner, skipped while
        /// it is disabled.</summary>
        public static DartSubscription Subscribe<T>(string name, Component owner, Action<T> handler)
            => Topic<T>(name).Subscribe(owner, handler);

        /// <summary>Instance form of the static Topic&lt;T&gt;().</summary>
        public DartTopic<T> GetTopic<T>(string name, bool reliable = true, int keepLast = 0,
                                        int catchUp = 0, int queueBytes = 0)
        {
            bool custom = !reliable || keepLast != 0 || catchUp != 0 || queueBytes != 0;
            DartTopicBase ch = LookupOrNull(name, custom);
            if (ch != null)
            {
                var typed = ch as DartTopic<T>;
                if (typed == null) throw ShapeMismatch(name, ch, "DartTopic<" + typeof(T).Name + ">");
                return typed;
            }
            var c = new DartTopic<T>(this, name, EffectiveQos(reliable, keepLast, catchUp, queueBytes));
            _topics.Add(name, c);
            return c;
        }

        /// <summary>Instance form of the static raw Topic().</summary>
        public DartTopic GetTopic(string name, bool reliable = true, int keepLast = 0,
                                  int catchUp = 0, int queueBytes = 0)
        {
            bool custom = !reliable || keepLast != 0 || catchUp != 0 || queueBytes != 0;
            DartTopicBase ch = LookupOrNull(name, custom);
            if (ch != null)
            {
                var raw = ch as DartTopic;
                if (raw == null) throw ShapeMismatch(name, ch, "a raw DartTopic");
                return raw;
            }
            var c = new DartTopic(this, name, EffectiveQos(reliable, keepLast, catchUp, queueBytes));
            _topics.Add(name, c);
            return c;
        }

        private DartTopicBase LookupOrNull(string name, bool customQos)
        {
            if (string.IsNullOrEmpty(name)) throw new ArgumentException("topic name required", nameof(name));
            DartTopicBase ch;
            if (!_topics.TryGetValue(name, out ch)) return null;
            if (customQos)
                Debug.LogWarning("[DART] topic '" + name + "' already exists: the QoS passed here is ignored (first request wins)", this);
            return ch;
        }

        private static InvalidOperationException ShapeMismatch(string name, DartTopicBase have, string want)
            => new InvalidOperationException("topic '" + name + "' already exists as " + have.GetType().Name
                + ", requested as " + want + ": one name = one message type per node");

        private static UnityDartNode RequireMain()
        {
            UnityDartNode m = Main;
            if (m == null)
                throw new InvalidOperationException(
                    "no UnityDartNode in the scene: add the UnityDartNode component to a GameObject (it owns the shared node)");
            return m;
        }

        // Reliable by default, and always queued from creation: with the service
        // thread owning the wire, only a queued topic keeps its handlers off that
        // thread (they then fire from the per-frame Dispatch, on the main thread).
        private static Qos EffectiveQos(bool reliable, int keepLast, int catchUp, int queueBytes)
        {
            var e = new Qos
            {
                Reliability = reliable ? Reliability.Reliable : Reliability.BestEffort,
                KeepLast = (ushort)keepLast,
                CatchUp = (ushort)catchUp,
                QueueBytes = (uint)queueBytes,
            };
            if (e.QueueBytes == 0) e.QueueBytes = 1 << 20;
            return e;
        }

        // ---- lifecycle ------------------------------------------------------------

        private void OnEnable()
        {
            if (s_main != null && s_main != this)
            {
                Debug.LogWarning("[DART] a UnityDartNode already exists on '" + s_main.gameObject.name
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
                    || _openMax != maxTopics || _openIf != multicastInterface);
            if (_node != null && (!shouldRun || changed)) CloseNativeNode();
            if (_node == null && shouldRun) OpenNativeNode();
        }

        private void OpenNativeNode()
        {
            try
            {
                _node = new DartNode(string.IsNullOrEmpty(nodeName) ? null : nodeName,
                                 RouteMessage, QueueEvent,
                                 domain: Mathf.Clamp(domain, 0, ushort.MaxValue),
                                 maxTopics: Mathf.Clamp(maxTopics, 0, ushort.MaxValue),
                                 multicastInterface: string.IsNullOrEmpty(multicastInterface)
                                     ? null : multicastInterface);
            }
            catch (Exception e)
            {
                Debug.LogError("[DART] node open failed: " + e.Message, this);
                _node = null;
                return;
            }
            _openName = nodeName; _openDomain = domain; _openMax = maxTopics; _openIf = multicastInterface;
            _pollFallback = !_node.Start();     // DART_NO_THREADS builds: pump polls
            foreach (DartTopicBase ch in _topics.Values) ch.OnNodeOpened();
        }

        private void CloseNativeNode()
        {
            if (_node == null) return;
            foreach (DartTopicBase ch in _topics.Values) ch.OnNodeClosed();
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
            _node.Dispatch();                   // every queued topic -> this thread
            if ((++_frame & 0xFF) == 0)
                foreach (DartTopicBase ch in _topics.Values) ch.PruneDeadOwners();
        }

        internal void RegisterIndex(ushort index, DartTopicBase ch)
        {
            lock (_byIndex) _byIndex[index] = ch;
        }

        // Runs on whichever thread dispatches. Our topics are all queued, so this
        // is the main thread; a locked lookup keeps a user's own extra (non-queued)
        // topic on Raw from racing the table, it just isn't routed.
        private void RouteMessage(Message m)
        {
            DartTopicBase ch;
            lock (_byIndex) _byIndex.TryGetValue(m.TopicIndex, out ch);
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
