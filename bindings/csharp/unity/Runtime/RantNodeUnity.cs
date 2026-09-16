// The RantNodeUnity component, the scene's shared node. It lives in its own file because
// Unity registers only a MonoBehaviour whose class name matches the file name.
#if UNITY_5_3_OR_NEWER
using System;
using System.Collections.Generic;
using System.Threading;
using UnityEngine;

namespace Rant
{
    /// <summary>The scene's shared node. Add exactly one, every other script reaches it through
    /// the static API. Runs in edit mode too when Run In Edit Mode is on.</summary>
    [ExecuteAlways]
    [DefaultExecutionOrder(-1000)]   // dispatch before other scripts' Update
    [DisallowMultipleComponent]
    [AddComponentMenu("Rant/RantNode")]
    public sealed class RantNodeUnity : MonoBehaviour
    {
        [Tooltip("Human-readable node name, synced to peers; empty = auto node-XXXXXXXX.")]
        [SerializeField] private string nodeName = "";
        [Tooltip("Discovery domain: nodes only see peers on the same domain.")]
        [SerializeField] private int domain = 0;
        [Tooltip("Max topics this node can create (the core default of 8 is small for a scene of components).")]
        [SerializeField] private int maxTopics = 32;
        [Tooltip("Multihomed hosts (VPN adapters, WSL, docker bridges): this machine's LAN IP, so discovery uses the right interface. Empty = auto probe.")]
        [SerializeField] private string multicastInterface = "";
        [Tooltip("Per topic consumer queue in bytes. The wire fills it off thread, the frame drains it.")]
        [SerializeField] private int queueBytes = 1 << 20;
        [Tooltip("Most messages dispatched per frame across all topics, 0 = drain everything. A budget bounds the frame under a burst.")]
        [SerializeField] private int dispatchBudget = 0;
        [Tooltip("Keep the node live in the editor outside play mode.")]
        [SerializeField] private bool runInEditMode = true;
        [Tooltip("Log peer lifecycle, message loss, and errors to the Console.")]
        [SerializeField] private bool logEvents = true;
        [Tooltip("Play mode: survive scene loads (DontDestroyOnLoad).")]
        [SerializeField] private bool persistAcrossScenes = true;

        // A frame that has to run more than this many parked callbacks is a symptom, not a
        // budget: nothing is dropped, but the console says so.
        private const int CallbackBacklogWarn = 4096;

        private static RantNodeUnity s_main;

        private RantNode _node;
        private bool _pollFallback;      // service thread unavailable: pump polls instead
        private bool _configDirty;
        private int _frame;
        private readonly Dictionary<string, RantTopicBase> _topics = new Dictionary<string, RantTopicBase>();
        private readonly Dictionary<string, SharedEntry> _shared = new Dictionary<string, SharedEntry>();
        private readonly List<Action> _callbacks = new List<Action>();   // node threads to the frame
        private readonly List<Action> _drain = new List<Action>();
        private readonly object _cbLock = new object();
        private string _openName; private int _openDomain; private int _openMax; private string _openIf;

        /// <summary>The scene's RantNodeUnity (found lazily), or null if none exists.</summary>
        public static RantNodeUnity Main
        {
            get
            {
                if (s_main == null)
                {
#if UNITY_2023_1_OR_NEWER
                    s_main = FindAnyObjectByType<RantNodeUnity>();
#else
                    s_main = FindObjectOfType<RantNodeUnity>();
#endif
                }
                return s_main;
            }
        }

        /// <summary>The underlying wrapper RantNode, null while closed. The escape hatch to the
        /// full API.</summary>
        public RantNode Raw => _node;
        public bool IsOpen => _node != null;
        internal RantNode NativeNode => _node;

        /// <summary>Peer lifecycle + errors, delivered on the main thread.</summary>
        public static event Action<RantEvent> OnEvent;

        // ---- topics -----------------------------------------------------------------

        /// <summary>The shared topic of this name on the scene node, created on first request.
        /// The QoS applies only to that first request, later callers share it.</summary>
        public static RantTopic<T> Topic<T>(string name, Qos qos = null)
            => RequireMain().GetTopic<T>(name, qos);

        /// <summary>The shared raw (bytes) topic of this name.</summary>
        public static RantTopic Topic(string name, Qos qos = null)
            => RequireMain().GetTopic(name, qos);

        /// <summary>The instance form of the static Topic&lt;T&gt;().</summary>
        public RantTopic<T> GetTopic<T>(string name, Qos qos = null)
        {
            RantTopicBase ch = LookupOrNull(name, qos != null);
            if (ch != null)
            {
                var typed = ch as RantTopic<T>;
                if (typed == null) throw ShapeMismatch(name, ch, "RantTopic<" + typeof(T).Name + ">");
                return typed;
            }
            var c = new RantTopic<T>(this, name, EffectiveQos(qos));
            _topics.Add(name, c);
            return c;
        }

        /// <summary>Instance form of the static raw Topic().</summary>
        public RantTopic GetTopic(string name, Qos qos = null)
        {
            RantTopicBase ch = LookupOrNull(name, qos != null);
            if (ch != null)
            {
                var raw = ch as RantTopic;
                if (raw == null) throw ShapeMismatch(name, ch, "a raw RantTopic");
                return raw;
            }
            var c = new RantTopic(this, name, EffectiveQos(qos));
            _topics.Add(name, c);
            return c;
        }

        private RantTopicBase LookupOrNull(string name, bool customQos)
        {
            if (string.IsNullOrEmpty(name)) throw new ArgumentException("topic name required", nameof(name));
            RantTopicBase ch;
            if (!_topics.TryGetValue(name, out ch)) return null;
            if (customQos)
                Debug.LogWarning("[Rant] topic '" + name + "' already exists: the QoS passed here is ignored (first request wins)", this);
            return ch;
        }

        private static InvalidOperationException ShapeMismatch(string name, RantTopicBase have, string want)
            => new InvalidOperationException("topic '" + name + "' already exists as " + have.GetType().Name
                + ", requested as " + want + ": one name = one message type per node");

        private static RantNodeUnity RequireMain()
        {
            RantNodeUnity m = Main;
            if (m == null)
                throw new InvalidOperationException(
                    "no RantNodeUnity in the scene: add the RantNodeUnity component to a GameObject (it owns the shared node)");
            return m;
        }

        // Unity's one QoS rule: handlers run on the frame, so every topic is queued. Everything
        // else is the C default, best effort included, as in every other binding.
        private Qos EffectiveQos(Qos qos)
        {
            var e = new Qos(qos);
            if (e.QueueBytes == 0) e.QueueBytes = (uint)Mathf.Max(queueBytes, 1);
            return e;
        }

        // ---- patterns: the core handles, shared by name -------------------------------

        /// <summary>The scene shared handle of this key, built on first request against the open
        /// node. Use it for a pattern that needs options the shorthands below do not take.</summary>
        public static T Shared<T>(string key, Func<RantNode, T> make) where T : class
            => RequireMain().GetShared(key, make);

        // The recipe is kept beside the handle: the handle belongs to the native node, so a
        // reopen has to build a new one, exactly as a topic re creates itself.
        private sealed class SharedEntry
        {
            internal Func<RantNode, object> Make;
            internal object Handle;
        }

        public T GetShared<T>(string key, Func<RantNode, T> make) where T : class
        {
            if (string.IsNullOrEmpty(key)) throw new ArgumentException("name required", nameof(key));
            if (make == null) throw new ArgumentNullException(nameof(make));
            string slot = typeof(T).Name + ":" + key;
            SharedEntry have;
            if (_shared.TryGetValue(slot, out have))
            {
                var typed = have.Handle as T;
                if (typed == null)
                    throw new InvalidOperationException("'" + key + "' already exists as "
                        + have.Handle.GetType().Name + ", requested as " + typeof(T).Name
                        + ": one name = one kind per node");
                return typed;
            }
            if (_node == null) Reconcile();     // acquiring one is a reason to open
            if (_node == null)
                throw new InvalidOperationException(
                    "Rant node is not open: '" + key + "' cannot be created (component disabled, "
                    + "Run In Edit Mode off, or open failed)");
            var e = new SharedEntry { Make = n => make(n) };
            e.Handle = make(_node);
            _shared.Add(slot, e);
            return (T)e.Handle;
        }

        /// <summary>The scene shared authoritative variable of this name.</summary>
        public static VariableDefinition<T> VariableDefinition<T>(string name)
            => Shared(name, n => n.VariableDefinition<T>(name));

        /// <summary>Overload with the value it holds before any set.</summary>
        public static VariableDefinition<T> VariableDefinition<T>(string name, T initial)
            => Shared(name, n => n.VariableDefinition<T>(name, initial));

        /// <summary>The scene shared reference to a variable owned by another node.</summary>
        public static RemoteVariable<T> RemoteVariable<T>(string name)
            => Shared(name, n => n.RemoteVariable<T>(name));

        /// <summary>The scene shared function definition, one per name on the network.</summary>
        public static FunctionDefinition<TReq, TRsp> FunctionDefinition<TReq, TRsp>(
                string name, Func<TReq, TRsp> handler)
            => Shared(name, n => n.FunctionDefinition<TReq, TRsp>(name, handler));

        /// <summary>The scene shared reference to a function defined on another node.</summary>
        public static RemoteFunction<TReq, TRsp> RemoteFunction<TReq, TRsp>(string name)
            => Shared(name, n => n.RemoteFunction<TReq, TRsp>(name));

        /// <summary>The scene shared task definition, one per name.</summary>
        public static TaskDefinition<TReq, TPrg, TRsp> TaskDefinition<TReq, TPrg, TRsp>(
                string name, Func<TReq, TaskContext<TPrg>, System.Threading.Tasks.Task<TRsp>> handler)
            => Shared(name, n => n.TaskDefinition<TReq, TPrg, TRsp>(name, handler));

        /// <summary>The scene shared reference to a task defined on another node.</summary>
        public static RemoteTask<TReq, TPrg, TRsp> RemoteTask<TReq, TPrg, TRsp>(string name)
            => Shared(name, n => n.RemoteTask<TReq, TPrg, TRsp>(name));

        // ---- lifecycle ------------------------------------------------------------

        private void OnEnable()
        {
            if (s_main != null && s_main != this)
            {
                Debug.LogWarning("[Rant] a RantNodeUnity already exists on '" + s_main.gameObject.name
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

        // The service thread must never survive into a new domain, since its trampolines point
        // at delegates the reload collects. OnDisable covers the normal paths, this backstops.
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
            var options = new NodeOptions
            {
                Domain = (ushort)Mathf.Clamp(domain, 0, ushort.MaxValue),
                MaxTopics = (ushort)Mathf.Clamp(maxTopics, 0, ushort.MaxValue),
                // A frame must never block: publishing into an unresolved match returns at
                // once and topic.Ready is the check (docs/topics.md).
                MatchWaitMs = -1,
                MulticastInterface = string.IsNullOrEmpty(multicastInterface) ? null : multicastInterface,
                // Everything that is not a message arrives on the frame: events, pattern
                // handlers, variable observers, awaited call results. Messages ride the C queue.
                Dispatcher = PostToFrame,
            };
            string name = string.IsNullOrEmpty(nodeName) ? null : nodeName;
            _pollFallback = false;
            try { _node = new RantNode(name, options); }
            catch (Exception first)
            {
                // A RANT_NO_THREADS build refuses the service thread: the pump polls instead.
                options.Threading = Threading.Manual;
                try { _node = new RantNode(name, options); _pollFallback = true; }
                catch (Exception)
                {
                    Debug.LogError("[Rant] node open failed: " + first.Message, this);
                    _node = null;
                    return;
                }
            }
            _node.OnEvent += OnNodeEvent;
            _openName = nodeName; _openDomain = domain; _openMax = maxTopics; _openIf = multicastInterface;
            foreach (RantTopicBase ch in _topics.Values) ch.OnNodeOpened();
            foreach (KeyValuePair<string, SharedEntry> kv in _shared)
            {
                try { kv.Value.Handle = kv.Value.Make(_node); }
                catch (Exception e)
                {
                    Debug.LogError("[Rant] '" + kv.Key + "' could not be re created: " + e.Message, this);
                }
            }
        }

        private void CloseNativeNode()
        {
            if (_node == null) return;
            foreach (RantTopicBase ch in _topics.Values) ch.OnNodeClosed();
            _node.Close();
            _node = null;
            lock (_cbLock) _callbacks.Clear();
        }

        // ---- per-frame pump ---------------------------------------------------------

        private void Pump()
        {
            if (_configDirty) Reconcile();
            if (_node == null) return;
            if (_pollFallback) _node.Poll(0);
            _node.Dispatch(Mathf.Max(dispatchBudget, 0));   // every queued topic, up to the budget
            DrainCallbacks();
            if ((++_frame & 0xFF) == 0)
            {
                foreach (RantTopicBase ch in _topics.Values) ch.PruneDeadOwners();
            }
            foreach (RantTopicBase ch in _topics.Values)
                if (ch.Dirty) ch.OnNodeOpened();    // drop the side nobody holds, off the dispatch
        }

        // The node's threads park work here. Nothing is dropped: a deep backlog is reported and
        // still run, since a swallowed callback is a silent fault.
        private void PostToFrame(Action a)
        {
            lock (_cbLock) _callbacks.Add(a);
        }

        private void DrainCallbacks()
        {
            lock (_cbLock)
            {
                if (_callbacks.Count == 0) return;
                _drain.AddRange(_callbacks);
                _callbacks.Clear();
            }
            if (_drain.Count > CallbackBacklogWarn)
                Debug.LogWarning("[Rant] " + _drain.Count + " callbacks parked for one frame: "
                    + "the frame is behind the wire", this);
            for (int i = 0; i < _drain.Count; i++)
            {
                try { _drain[i](); }
                catch (Exception e) { Debug.LogException(e); }
            }
            _drain.Clear();
        }

        // Already on the frame: the node hands its events here through the dispatcher.
        private void OnNodeEvent(RantEvent e)
        {
            if (logEvents)
            {
                if (e.IsError) Debug.LogError("[Rant] " + e, this);
                else if (e.Kind == EventKind.MessageLost) Debug.LogWarning("[Rant] " + e, this);
                else Debug.Log("[Rant] " + e, this);
            }
            Action<RantEvent> handler = OnEvent;
            if (handler == null) return;
            try { handler(e); }
            catch (Exception ex) { Debug.LogException(ex); }
        }
    }
}
#endif
