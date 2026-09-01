// The DartNodeUnity component: the scene's shared DART node. Lives in its own file
// because Unity only registers a MonoBehaviour whose class name matches the file
// name. The topic/subscription API it hands out is in DartUnity.cs.
//
//   public struct Pose { public float X, Y, Z; }
//
//   // publish from any component
//   DartTopic<Pose> pose;
//   void Start()  { pose = DartNodeUnity.Topic<Pose>("player/pose"); }
//   void Update() { pose.Publish(new Pose { X = transform.position.x }); }
//
//   // subscribe from any other component: dies with the component, skipped while
//   // it is disabled, and always fires on the main thread
//   void Start() { DartNodeUnity.Subscribe<Pose>("player/pose", this, OnPose); }
//   void OnPose(Pose p) { transform.position = new Vector3(p.X, p.Y, p.Z); }
#if UNITY_5_3_OR_NEWER
using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using UnityEngine;

namespace Dart
{
    /// <summary>The scene's shared DART node. Add exactly one to the scene; every
    /// other script reaches it through the static API (DartNodeUnity.Topic&lt;T&gt;,
    /// DartNodeUnity.Subscribe). Runs in edit mode too when Run In Edit Mode is on.</summary>
    [ExecuteAlways]
    [DefaultExecutionOrder(-1000)]   // dispatch before other scripts' Update
    [DisallowMultipleComponent]
    [AddComponentMenu("DART/DartNode")]
    public sealed class DartNodeUnity : MonoBehaviour
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

        private static DartNodeUnity s_main;

        private DartNode _node;
        private bool _pollFallback;      // service thread unavailable: pump polls instead
        private bool _configDirty;
        private int _frame;
        private readonly Dictionary<string, DartTopicBase> _topics = new Dictionary<string, DartTopicBase>();
        private readonly Dictionary<string, DartPatternEntity> _patterns = new Dictionary<string, DartPatternEntity>();
        private readonly Dictionary<ushort, DartTopicBase> _byIndex = new Dictionary<ushort, DartTopicBase>();
        private readonly List<DartEvent> _pending = new List<DartEvent>();   // service thread -> main
        private readonly List<DartEvent> _drain = new List<DartEvent>();
        private readonly object _pendingLock = new object();
        private readonly List<Action> _mainActions = new List<Action>();     // pattern callbacks -> main
        private readonly List<Action> _mainDrain = new List<Action>();
        private readonly object _mainLock = new object();
        private int _mainThreadId;
        private string _openName; private int _openDomain; private int _openMax; private string _openIf;

        /// <summary>The scene's DartNodeUnity (found lazily), or null if none exists.</summary>
        public static DartNodeUnity Main
        {
            get
            {
                if (s_main == null)
                {
#if UNITY_2023_1_OR_NEWER
                    s_main = FindAnyObjectByType<DartNodeUnity>();
#else
                    s_main = FindObjectOfType<DartNodeUnity>();
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
        public static event Action<DartEvent> Events;

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

        /// <summary>One-liner subscribe on the scene node. The reliability applies only if
        /// this is the first request for the topic (first request sets its QoS).</summary>
        public static DartSubscription Subscribe<T>(string name, Action<T> handler, bool reliable = true)
            => Topic<T>(name, reliable).Subscribe(handler);

        /// <summary>One-liner owner-bound subscribe: dies with owner, skipped while
        /// it is disabled.</summary>
        public static DartSubscription Subscribe<T>(string name, Component owner, Action<T> handler,
                                                    bool reliable = true)
            => Topic<T>(name, reliable).Subscribe(owner, handler);

        /// <summary>One-liner publish on the scene node (creates/shares the topic by name).
        /// The reliability applies only if this is the first request for the topic.</summary>
        public static SendStatus Publish<T>(string name, T message, bool reliable = true)
            => RequireMain().GetTopic<T>(name, reliable).Publish(message);

        /// <summary>One-liner raw (bytes) publish.</summary>
        public static SendStatus Publish(string name, byte[] data, bool reliable = true)
            => RequireMain().GetTopic(name, reliable).Publish(data);

        /// <summary>One-liner raw (UTF-8 string) publish.</summary>
        public static SendStatus Publish(string name, string text, bool reliable = true)
            => RequireMain().GetTopic(name, reliable).Publish(text);

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

        private static DartNodeUnity RequireMain()
        {
            DartNodeUnity m = Main;
            if (m == null)
                throw new InvalidOperationException(
                    "no DartNodeUnity in the scene: add the DartNodeUnity component to a GameObject (it owns the shared node)");
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

        // ---- patterns: variables / functions --------------------------------------

        /// <summary>The scene-shared authoritative variable named <paramref name="name"/>
        /// (this node owns the value). One side per node: asking for a RemoteVariable of the
        /// same name throws.</summary>
        public static DartVariableDefinition<T> VariableDefinition<T>(string name,
                bool readOnly = false, bool allowForce = false, int catchUp = 0, int keepLast = 0)
            => RequireMain().GetVariableDefinition<T>(name, readOnly, allowForce, catchUp, keepLast);

        /// <summary>The scene-shared reference to a variable owned by another node.</summary>
        public static DartRemoteVariable<T> RemoteVariable<T>(string name, int catchUp = 0,
                int keepLast = 0)
            => RequireMain().GetRemoteVariable<T>(name, catchUp, keepLast);

        /// <summary>The scene-shared function definition (this node implements it; ONE per
        /// name on the network). The handler runs on the main thread.</summary>
        public static DartFunctionDefinition<TReq, TRsp> FunctionDefinition<TReq, TRsp>(
                string name, Func<TReq, TRsp> handler)
            => RequireMain().GetFunctionDefinition<TReq, TRsp>(name, handler);

        /// <summary>The scene-shared reference to a function defined on another node.</summary>
        public static DartRemoteFunction<TReq, TRsp> RemoteFunction<TReq, TRsp>(string name)
            => RequireMain().GetRemoteFunction<TReq, TRsp>(name);

        /// <summary>The scene-shared task definition (a function with progress and
        /// cancellation; ONE per name). The async handler runs on the main thread.</summary>
        public static DartTaskDefinition<TReq, TPrg, TRsp> TaskDefinition<TReq, TPrg, TRsp>(
                string name, Func<TReq, TaskContext<TPrg>, Task<TRsp>> handler,
                bool noCancel = false, bool exclusive = false)
            => RequireMain().GetTaskDefinition<TReq, TPrg, TRsp>(name, handler, noCancel, exclusive);

        /// <summary>The scene-shared reference to a task defined on another node.</summary>
        public static DartRemoteTask<TReq, TPrg, TRsp> RemoteTask<TReq, TPrg, TRsp>(string name)
            => RequireMain().GetRemoteTask<TReq, TPrg, TRsp>(name);

        public DartVariableDefinition<T> GetVariableDefinition<T>(string name,
                bool readOnly = false, bool allowForce = false, int catchUp = 0, int keepLast = 0)
            => GetOrCreatePattern("var:", name,
                   () => new DartVariableDefinition<T>(this, name, readOnly, allowForce, catchUp, keepLast));

        public DartRemoteVariable<T> GetRemoteVariable<T>(string name, int catchUp = 0, int keepLast = 0)
            => GetOrCreatePattern("var:", name,
                   () => new DartRemoteVariable<T>(this, name, catchUp, keepLast));

        public DartFunctionDefinition<TReq, TRsp> GetFunctionDefinition<TReq, TRsp>(
                string name, Func<TReq, TRsp> handler)
            => GetOrCreatePattern("fn:", name,
                   () => new DartFunctionDefinition<TReq, TRsp>(this, name, handler));

        public DartRemoteFunction<TReq, TRsp> GetRemoteFunction<TReq, TRsp>(string name)
            => GetOrCreatePattern("fn:", name, () => new DartRemoteFunction<TReq, TRsp>(this, name));

        public DartTaskDefinition<TReq, TPrg, TRsp> GetTaskDefinition<TReq, TPrg, TRsp>(
                string name, Func<TReq, TaskContext<TPrg>, Task<TRsp>> handler,
                bool noCancel = false, bool exclusive = false)
            => GetOrCreatePattern("task:", name,
                   () => new DartTaskDefinition<TReq, TPrg, TRsp>(this, name, handler, noCancel, exclusive));

        public DartRemoteTask<TReq, TPrg, TRsp> GetRemoteTask<TReq, TPrg, TRsp>(string name)
            => GetOrCreatePattern("task:", name, () => new DartRemoteTask<TReq, TPrg, TRsp>(this, name));

        // Share a pattern handle by (kind, name): the first request builds it (and creates
        // the native object now if the node is open), later ones return it, and a mismatched
        // kind/type on the same name is a hard error (as for topics).
        private TEntity GetOrCreatePattern<TEntity>(string kind, string name, Func<TEntity> make)
            where TEntity : DartPatternEntity
        {
            if (string.IsNullOrEmpty(name)) throw new ArgumentException("pattern name required", nameof(name));
            string key = kind + name;
            DartPatternEntity have;
            if (_patterns.TryGetValue(key, out have))
            {
                var typed = have as TEntity;
                if (typed == null)
                    throw new InvalidOperationException("'" + name + "' already exists as "
                        + have.GetType().Name + ", requested as " + typeof(TEntity).Name
                        + ": one name = one kind/type per node");
                return typed;
            }
            TEntity created = make();
            _patterns.Add(key, created);
            created.OnNodeOpened();   // create the native object now if the node is already open
            return created;
        }

        // ---- lifecycle ------------------------------------------------------------

        private void OnEnable()
        {
            _mainThreadId = Thread.CurrentThread.ManagedThreadId;
            if (s_main != null && s_main != this)
            {
                Debug.LogWarning("[DART] a DartNodeUnity already exists on '" + s_main.gameObject.name
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
            foreach (DartPatternEntity p in _patterns.Values) p.OnNodeOpened();
        }

        private void CloseNativeNode()
        {
            if (_node == null) return;
            foreach (DartTopicBase ch in _topics.Values) ch.OnNodeClosed();
            foreach (DartPatternEntity p in _patterns.Values) p.OnNodeClosed();
            lock (_byIndex) _byIndex.Clear();
            _node.Close();
            _node = null;
            lock (_pendingLock) _pending.Clear();
            lock (_mainLock) _mainActions.Clear();
        }

        // ---- per-frame pump ---------------------------------------------------------

        private void Pump()
        {
            if (_configDirty) Reconcile();
            if (_node == null) return;
            DrainEvents();
            if (_pollFallback) _node.Poll(0);
            _node.Dispatch();                   // every queued topic -> this thread
            DrainMainActions();                 // pattern callbacks parked by the service thread
            if ((++_frame & 0xFF) == 0)
            {
                foreach (DartTopicBase ch in _topics.Values) ch.PruneDeadOwners();
                foreach (DartPatternEntity p in _patterns.Values) p.PruneDeadOwners();
            }
        }

        internal void RegisterIndex(ushort index, DartTopicBase ch)
        {
            lock (_byIndex) _byIndex[index] = ch;
        }

        // Pattern callbacks fire on the service thread; hand them to the frame. When we are
        // already on the main thread (poll fallback, or the core's synchronous OnChange
        // replay at registration) run inline so ordering is preserved.
        internal bool OnMainThread => Thread.CurrentThread.ManagedThreadId == _mainThreadId;

        internal void Post(Action a)
        {
            lock (_mainLock) { if (_mainActions.Count < 4096) _mainActions.Add(a); }
        }

        internal void RunOnMain(Action a)
        {
            if (OnMainThread) a();
            else Post(a);
        }

        private void DrainMainActions()
        {
            lock (_mainLock)
            {
                if (_mainActions.Count == 0) return;
                _mainDrain.AddRange(_mainActions);
                _mainActions.Clear();
            }
            for (int i = 0; i < _mainDrain.Count; i++)
            {
                try { _mainDrain[i](); }
                catch (Exception e) { Debug.LogException(e); }
            }
            _mainDrain.Clear();
        }

        // Runs on whichever thread dispatches. Our topics are all queued, so this
        // is the main thread; a locked lookup keeps a user's own extra (non-queued)
        // topic on Raw from racing the table, it just isn't routed.
        private void RouteMessage(DartMessage m)
        {
            DartTopicBase ch;
            lock (_byIndex) _byIndex.TryGetValue(m.TopicIndex, out ch);
            if (ch != null) ch.Deliver(m);
        }

        // Service thread: park the event for the main-thread pump.
        private void QueueEvent(DartEvent e)
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
            Action<DartEvent> handler = Events;
            for (int i = 0; i < _drain.Count; i++)
            {
                DartEvent e = _drain[i];
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
