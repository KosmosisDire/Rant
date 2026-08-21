// DART for Unity: shared, name-keyed topics handed out by the scene's DartNodeUnity
// component (DartNodeUnity.cs: Unity only registers a MonoBehaviour whose class name
// matches its file name, so the component lives there and this file holds the rest).
//
// Put ONE DartNodeUnity component in the scene. It owns the process node (name, domain,
// lifecycle) and every other script publishes/subscribes through it, so components
// never each open a node of their own:
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
//
// Topics are shared by name: every script asking for "player/pose" gets the same
// DartTopic<Pose>, and its role is managed automatically (created inactive; the
// first Publish advertises pub, the first Subscribe advertises sub, the last
// unsubscribe withdraws it).
//
// Threading: the node runs the C service thread, so the wire never waits for a
// frame; every topic is queued from creation and DartNodeUnity dispatches once per
// frame, so ALL handlers fire on the main thread, where the Unity API is legal.
// Events (peer up/down, errors) reach the main thread the same way and are logged
// to the Console by default (DartNodeUnity.Events to observe them).
//
// Edit mode: the component is [ExecuteAlways]; with Run In Edit Mode on (default)
// the node is live in the editor outside play, pumped from EditorApplication.update.
// Whether publishers/subscribers exist at edit time is up to them: a topic
// acquired while the node is closed simply goes live when it opens.
//
// The low-level wrapper stays fully available: DartNodeUnity.Main.Raw is the DartNode, and a
// DartTopic's Raw is the underlying Dart.Topic (TryTake, Drain, QueueStats...).
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
        /// <summary>DartNode monotonic clock (microseconds) at ARRIVAL on the poll side,
        /// not at dispatch: inter-arrival timing is real even when dispatch is
        /// frame-paced.</summary>
        public readonly ulong RecvUs;
        public MessageInfo(string sender, ulong recvUs) { Sender = sender; RecvUs = recvUs; }
    }

    /// <summary>A live subscription (to a topic or variable observer):
    /// Dispose() unsubscribes. Owner-bound subscriptions dispose themselves when the
    /// owner is destroyed.</summary>
    public sealed class DartSubscription : IDisposable
    {
        private Action _unsub;
        internal DartSubscription(Action unsub) { _unsub = unsub; }
        public void Dispose()
        {
            Action u = _unsub;
            _unsub = null;
            if (u != null) u();
        }
    }

    /// <summary>A shared, name-keyed topic on the scene's DartNodeUnity. One instance
    /// exists per name; it survives the native node closing and reopening (edit
    /// mode toggles, inspector changes) by re-creating its native topic lazily.</summary>
    public abstract class DartTopicBase
    {
        internal sealed class Sub
        {
            internal Action<DartMessage> Fn;
            internal Component Owner;
            internal bool HasOwner;
            internal bool Dead;
        }

        private readonly DartNodeUnity _owner;
        private readonly string _name;
        private readonly Qos _qos;
        private readonly List<Sub> _subs = new List<Sub>();
        private Topic _raw;
        private Role _appliedRole = Role.Inactive;
        private int _live;          // subs not yet marked dead
        private bool _wantPub;
        private bool _warnedClosed;

        internal DartTopicBase(DartNodeUnity owner, string name, Qos qos)
        {
            _owner = owner; _name = name; _qos = qos;
        }

        public string Name => _name;
        /// <summary>The underlying wrapper Topic; null while the node is closed.</summary>
        public Topic Raw => _raw;
        /// <summary>Matched remote endpoints (0 while the node is closed).</summary>
        public int Matches => _raw != null ? _raw.MatchCount() : 0;
        /// <summary>Local handlers currently subscribed.</summary>
        public int SubscriberCount => _live;

        internal abstract Topic CreateRaw(DartNode node, string name, Role role, Qos qos);

        // The advertised role always mirrors actual local use: create the native
        // topic on first use, flip the role on later changes (SetRole re-advertises
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
            DartNode node = _owner != null ? _owner.NativeNode : null;
            if (node == null) return;               // deferred until the node opens
            _raw = CreateRaw(node, _name, want, _qos);
            _appliedRole = want;
            _owner.RegisterIndex(_raw.Index, this);
        }

        internal void OnNodeOpened()
        {
            _warnedClosed = false;
            try { ApplyRole(); }
            catch (Exception e) { Debug.LogError("[DART] topic '" + _name + "' create failed: " + e.Message); }
        }

        internal void OnNodeClosed()
        {
            _raw = null;                            // native handle died with the node
            _appliedRole = Role.Inactive;
        }

        protected Topic PubRaw()
        {
            _wantPub = true;
            ApplyRole();
            if (_raw == null && !_warnedClosed)
            {
                _warnedClosed = true;
                Debug.LogWarning("[DART] publish on '" + _name + "' dropped: no open DartNodeUnity "
                    + "(component disabled, Run In Edit Mode off, or open failed)");
            }
            return _raw;
        }

        internal DartSubscription AddSub(Action<DartMessage> fn, Component owner, bool hasOwner)
        {
            var s = new Sub { Fn = fn, Owner = owner, HasOwner = hasOwner };
            _subs.Add(s); _live++;
            ApplyRole();
            return new DartSubscription(() => RemoveSub(s));
        }

        internal void RemoveSub(Sub s)
        {
            if (s == null || s.Dead) return;
            s.Dead = true; _live--;
            ApplyRole();
        }

        // Main thread, from DartNodeUnity's per-frame dispatch. Handlers may subscribe,
        // unsubscribe, and publish freely from inside a delivery.
        internal void Deliver(DartMessage m)
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

    /// <summary>A raw (schemaless) shared topic: bytes or UTF-8 strings.</summary>
    public sealed class DartTopic : DartTopicBase
    {
        internal DartTopic(DartNodeUnity owner, string name, Qos qos) : base(owner, name, qos) { }

        internal override Topic CreateRaw(DartNode node, string name, Role role, Qos qos)
            => new Topic(node, name, (Schema)null, role, qos);

        public SendStatus Publish(byte[] data)
        {
            Topic r = PubRaw();
            return r != null ? r.Send(data) : SendStatus.NoTopic;
        }

        public SendStatus Publish(string text)
        {
            Topic r = PubRaw();
            return r != null ? r.Send(text) : SendStatus.NoTopic;
        }

        public DartSubscription Subscribe(Action<DartMessage> handler)
        {
            if (handler == null) throw new ArgumentNullException(nameof(handler));
            return AddSub(handler, null, false);
        }

        /// <summary>Owner-bound: auto-unsubscribes when owner is destroyed, skipped
        /// while it is disabled.</summary>
        public DartSubscription Subscribe(Component owner, Action<DartMessage> handler)
        {
            if (owner == null) throw new ArgumentNullException(nameof(owner));
            if (handler == null) throw new ArgumentNullException(nameof(handler));
            return AddSub(handler, owner, true);
        }
    }

    /// <summary>A typed shared topic: T's public fields are the schema, exactly as
    /// in the core wrapper's Topic&lt;T&gt;.</summary>
    public sealed class DartTopic<T> : DartTopicBase
    {
        internal DartTopic(DartNodeUnity owner, string name, Qos qos) : base(owner, name, qos) { }

        internal override Topic CreateRaw(DartNode node, string name, Role role, Qos qos)
            => new Topic<T>(node, name, role, qos);   // the internal (role, qos) plumbing ctor

        public SendStatus Publish(T message)
        {
            Topic r = PubRaw();
            return r != null ? ((Topic<T>)r).Send(message) : SendStatus.NoTopic;
        }

        public DartSubscription Subscribe(Action<T> handler)
        {
            if (handler == null) throw new ArgumentNullException(nameof(handler));
            return AddSub(m => { if (m.Value is T v) handler(v); }, null, false);
        }

        public DartSubscription Subscribe(Action<T, MessageInfo> handler)
        {
            if (handler == null) throw new ArgumentNullException(nameof(handler));
            return AddSub(m => { if (m.Value is T v) handler(v, new MessageInfo(m.PublisherName, m.RecvUs)); },
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
            return AddSub(m => { if (m.Value is T v) handler(v, new MessageInfo(m.PublisherName, m.RecvUs)); },
                          owner, true);
        }
    }
}
#endif
