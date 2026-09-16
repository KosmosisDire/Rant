// Rant for Unity: shared, name keyed topics handed out by the scene's RantNodeUnity
// component, which lives in its own file. bindings/csharp/unity/README.md explains the model.
#if UNITY_5_3_OR_NEWER
using System;
using System.Collections.Generic;
using UnityEngine;

namespace Rant
{
    /// <summary>A live subscription. Dispose() unsubscribes, and an owner bound one disposes
    /// itself when the owner is destroyed.</summary>
    public sealed class RantSubscription : IDisposable
    {
        private Action _unsub;
        internal RantSubscription(Action unsub) { _unsub = unsub; }
        public void Dispose()
        {
            Action u = _unsub;
            _unsub = null;
            if (u != null) u();
        }
    }

    /// <summary>A shared, name keyed topic on the scene's RantNodeUnity, one instance per name.
    /// It survives the native node closing and reopening by re creating its handles lazily.</summary>
    public abstract class RantTopicBase
    {
        internal sealed class Sub
        {
            internal Action<RantMessage> Fn;
            internal Component Owner;
            internal bool HasOwner;
            internal bool Dead;
        }

        private readonly RantNodeUnity _owner;
        private readonly string _name;
        private readonly Qos _qos;
        private readonly List<Sub> _subs = new List<Sub>();
        private int _live;          // subs not yet marked dead
        private bool _wantPub;
        private bool _havePub, _haveSub;   // the sides created on the open node
        private bool _warnedClosed;
        internal bool Dirty;        // a side to drop, done from the frame outside a dispatch

        internal RantTopicBase(RantNodeUnity owner, string name, Qos qos)
        {
            _owner = owner; _name = name; _qos = qos;
        }

        public string Name => _name;
        /// <summary>Subscribers matched to this node's publishing side, 0 while it has none.</summary>
        public int Matches => _havePub ? MatchCount() : 0;
        /// <summary>Local handlers currently subscribed.</summary>
        public int SubscriberCount => _live;
        /// <summary>True when a publish would not wait on the match wait.</summary>
        public bool Ready => _havePub && PubReady();
        /// <summary>Consumer queue depth, bytes, capacity and drops since open.</summary>
        public (uint Messages, uint Bytes, uint Capacity, uint Dropped) QueueStats()
            => _haveSub ? SubQueueStats() : (0u, 0u, 0u, 0u);

        internal abstract void CreatePub(RantNode node, string name, Qos qos);
        internal abstract void CreateSub(RantNode node, string name, Qos qos, Action<RantMessage> deliver);
        internal abstract void DisposeSub();
        internal abstract void DropHandles();
        internal abstract int MatchCount();
        internal abstract bool PubReady();
        internal abstract (uint, uint, uint, uint) SubQueueStats();

        // The node advertises what is used locally: the publishing side is created on the first
        // Publish and the subscribing side on the first Subscribe, both over the one topic slot,
        // and the subscribing side is disposed after the last unsubscribe.
        internal void Apply()
        {
            Dirty = false;
            RantNode node = _owner != null ? _owner.NativeNode : null;
            if (node == null) return;               // deferred until the node opens
            if (_wantPub && !_havePub) { CreatePub(node, _name, _qos); _havePub = true; }
            if (_live > 0 && !_haveSub)
            {
                // Every Unity topic is queued, so the deliveries land on the frame.
                CreateSub(node, _name, _qos, Deliver);
                _haveSub = true;
            }
            if (_live == 0 && _haveSub) { DisposeSub(); _haveSub = false; }
        }

        internal void OnNodeOpened()
        {
            _warnedClosed = false;
            try { Apply(); }
            catch (Exception e) { Debug.LogError("[Rant] topic '" + _name + "' create failed: " + e.Message); }
        }

        internal void OnNodeClosed()
        {
            DropHandles();                          // the native handles died with the node
            _havePub = _haveSub = false;
        }

        // True when the publishing side exists, creating it on the first call.
        protected bool PubOpen()
        {
            _wantPub = true;
            Apply();
            if (!_havePub && !_warnedClosed)
            {
                _warnedClosed = true;
                Debug.LogWarning("[Rant] publish on '" + _name + "' dropped: no open RantNodeUnity "
                    + "(component disabled, Run In Edit Mode off, or open failed)");
            }
            return _havePub;
        }

        internal RantSubscription AddSub(Action<RantMessage> fn, Component owner, bool hasOwner)
        {
            var s = new Sub { Fn = fn, Owner = owner, HasOwner = hasOwner };
            _subs.Add(s); _live++;
            Apply();
            return new RantSubscription(() => RemoveSub(s));
        }

        // The native side is dropped on the next frame: a handler may unsubscribe from inside
        // a delivery, where the topic's queue is still being drained.
        internal void RemoveSub(Sub s)
        {
            if (s == null || s.Dead) return;
            s.Dead = true; _live--;
            if (_live == 0) Dirty = true;
        }

        // Main thread, from the node's per-frame dispatch. Handlers may subscribe, unsubscribe
        // and publish freely from inside a delivery.
        internal void Deliver(RantMessage m)
        {
            bool sawDead = false;
            int n = _subs.Count;   // additions during the loop wait for the next message
            for (int i = 0; i < n; i++)
            {
                Sub s = _subs[i];
                if (s.Dead) { sawDead = true; continue; }
                if (s.HasOwner && s.Owner == null)  // owner destroyed: auto-unsubscribe
                {
                    s.Dead = true; _live--; sawDead = true;
                    if (_live == 0) Dirty = true;
                    continue;
                }
                if (s.HasOwner && !OwnerActive(s.Owner)) continue;   // paused while disabled
                try { s.Fn(m); }
                catch (Exception e) { Debug.LogException(e); }
            }
            if (sawDead) _subs.RemoveAll(IsDead);
        }

        internal void PruneDeadOwners()
        {
            bool changed = false;
            for (int i = 0; i < _subs.Count; i++)
            {
                Sub s = _subs[i];
                if (!s.Dead && s.HasOwner && s.Owner == null) { s.Dead = true; _live--; changed = true; }
            }
            if (changed) _subs.RemoveAll(IsDead);
            if (changed && _live == 0) Dirty = true;
        }

        private static bool IsDead(Sub s) => s.Dead;

        private static bool OwnerActive(Component c)
        {
            var b = c as Behaviour;
            return b != null ? b.isActiveAndEnabled : c.gameObject.activeInHierarchy;
        }
    }

    /// <summary>A raw (schemaless) shared topic carrying bytes.</summary>
    public sealed class RantTopic : RantTopicBase
    {
        private Publisher<byte[]> _pub;
        private Subscriber<byte[]> _sub;

        internal RantTopic(RantNodeUnity owner, string name, Qos qos) : base(owner, name, qos) { }

        internal override void CreatePub(RantNode node, string name, Qos qos)
            => _pub = node.Publisher<byte[]>(name, qos);
        internal override void CreateSub(RantNode node, string name, Qos qos, Action<RantMessage> deliver)
        {
            _sub = node.Subscriber<byte[]>(name, qos);
            _sub.OnMessage += (byte[] b, RantMessage m) => deliver(m);
        }
        internal override void DisposeSub() { _sub.Dispose(); _sub = null; }
        internal override void DropHandles() { _pub = null; _sub = null; }
        internal override int MatchCount() => _pub.MatchCount;
        internal override bool PubReady() => _pub.Ready;
        internal override (uint, uint, uint, uint) SubQueueStats() => _sub.QueueStats();

        public SendStatus Publish(byte[] data)
            => PubOpen() ? _pub.Send(data) : SendStatus.NoTopic;

        public RantSubscription Subscribe(Action<RantMessage> handler)
        {
            if (handler == null) throw new ArgumentNullException(nameof(handler));
            return AddSub(handler, null, false);
        }

        /// <summary>Owner-bound: auto-unsubscribes when owner is destroyed, skipped
        /// while it is disabled.</summary>
        public RantSubscription Subscribe(Component owner, Action<RantMessage> handler)
        {
            if (owner == null) throw new ArgumentNullException(nameof(owner));
            if (handler == null) throw new ArgumentNullException(nameof(handler));
            return AddSub(handler, owner, true);
        }
    }

    /// <summary>A typed shared topic: T's public fields are the schema, as in the core
    /// wrapper's Publisher&lt;T&gt; and Subscriber&lt;T&gt;.</summary>
    public sealed class RantTopic<T> : RantTopicBase
    {
        private Publisher<T> _pub;
        private Subscriber<T> _sub;

        internal RantTopic(RantNodeUnity owner, string name, Qos qos) : base(owner, name, qos) { }

        internal override void CreatePub(RantNode node, string name, Qos qos)
            => _pub = node.Publisher<T>(name, qos);
        internal override void CreateSub(RantNode node, string name, Qos qos, Action<RantMessage> deliver)
        {
            _sub = node.Subscriber<T>(name, qos);
            _sub.OnMessage += (T v, RantMessage m) => deliver(m);
        }
        internal override void DisposeSub() { _sub.Dispose(); _sub = null; }
        internal override void DropHandles() { _pub = null; _sub = null; }
        internal override int MatchCount() => _pub.MatchCount;
        internal override bool PubReady() => _pub.Ready;
        internal override (uint, uint, uint, uint) SubQueueStats() => _sub.QueueStats();

        public SendStatus Publish(T message)
            => PubOpen() ? _pub.Send(message) : SendStatus.NoTopic;

        public RantSubscription Subscribe(Action<T> handler)
        {
            if (handler == null) throw new ArgumentNullException(nameof(handler));
            return AddSub(Typed(handler, null), null, false);
        }

        /// <summary>With the message beside the value: sender, both clocks, raw bytes.</summary>
        public RantSubscription Subscribe(Action<T, RantMessage> handler)
        {
            if (handler == null) throw new ArgumentNullException(nameof(handler));
            return AddSub(Typed(null, handler), null, false);
        }

        /// <summary>Owner-bound: auto-unsubscribes when owner is destroyed, skipped
        /// while it is disabled.</summary>
        public RantSubscription Subscribe(Component owner, Action<T> handler)
        {
            if (owner == null) throw new ArgumentNullException(nameof(owner));
            if (handler == null) throw new ArgumentNullException(nameof(handler));
            return AddSub(Typed(handler, null), owner, true);
        }

        /// <summary>Owner-bound, with the message beside the value.</summary>
        public RantSubscription Subscribe(Component owner, Action<T, RantMessage> handler)
        {
            if (owner == null) throw new ArgumentNullException(nameof(owner));
            if (handler == null) throw new ArgumentNullException(nameof(handler));
            return AddSub(Typed(null, handler), owner, true);
        }

        private static Action<RantMessage> Typed(Action<T> plain, Action<T, RantMessage> full)
            => m =>
            {
                T v;
                if (!Patterns.TryValue(m, out v)) return;
                if (plain != null) plain(v); else full(v, m);
            };
    }
}
#endif
