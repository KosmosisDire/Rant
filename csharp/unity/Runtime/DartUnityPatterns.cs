// DART patterns for Unity: variables and functions, shared by name through
// the scene's DartNodeUnity component exactly like DartNodeUnity.Topic<T>. Every handler
// is marshaled to the MAIN thread (the core patterns fire their callbacks inline on the
// service thread, which is illegal for the Unity API), observers are Component-bound (they
// die with the component and are skipped while it is disabled), and each handle survives
// the native node closing and reopening (edit-mode toggles, inspector changes).
//
//   // variable: ONE owner (the definition), remotes reference it
//   var speed = DartNodeUnity.VariableDefinition<float>("motor/speed");
//   speed.Set(1.5f);
//   DartNodeUnity.RemoteVariable<float>("motor/speed").OnChange(this, v => label.text = v.ToString());
//
//   // function: request/response, ONE definition
//   DartNodeUnity.FunctionDefinition<int, int>("square", x => x * x);
//   DartNodeUnity.RemoteFunction<int, int>("square").Call(7, r => Debug.Log(r.Value));  // 49, on the main thread
#if UNITY_5_3_OR_NEWER
using System;
using System.Collections.Generic;
using UnityEngine;

namespace Dart
{
    // ---- shared base + fan-out helper -------------------------------------------

    /// <summary>Base for a scene-shared pattern handle (variable/function). Holds
    /// its native core object across node close/reopen, recreating it lazily.</summary>
    public abstract class DartPatternEntity
    {
        protected readonly DartNodeUnity Owner;
        protected readonly string Name;

        internal DartPatternEntity(DartNodeUnity owner, string name) { Owner = owner; Name = name; }

        /// <summary>The pattern's name (its cross-node identity).</summary>
        public string EntityName => Name;

        internal void OnNodeOpened()
        {
            try { EnsureNative(); }
            catch (Exception e) { Debug.LogError("[DART] pattern '" + Name + "' create failed: " + e.Message); }
        }
        internal void OnNodeClosed() { DropNative(); }
        internal virtual void PruneDeadOwners() { }

        // Create the native core object if the node is open and it does not exist yet;
        // drop the (now dead) reference when the node closes.
        internal abstract void EnsureNative();
        internal abstract void DropNative();
    }

    // Owner-bound, main-thread fan-out to N observers. Mirrors DartTopicBase's sub list:
    // an observer dies with a destroyed owner and is skipped while the owner is disabled.
    internal sealed class DartFanout<TArg>
    {
        internal sealed class Sub
        {
            internal Action<TArg> Fn;
            internal Component Owner;
            internal bool HasOwner;
            internal bool Dead;
        }

        private readonly List<Sub> _subs = new List<Sub>();
        private int _live;

        internal int LiveCount => _live;

        internal Sub Add(Action<TArg> fn, Component owner, bool hasOwner)
        {
            var s = new Sub { Fn = fn, Owner = owner, HasOwner = hasOwner };
            _subs.Add(s); _live++;
            return s;
        }

        internal void Remove(Sub s)
        {
            if (s == null || s.Dead) return;
            s.Dead = true; _live--;
        }

        // Deliver to one observer (a late-joiner replay), honoring owner state.
        internal void DeliverOne(Sub s, TArg arg)
        {
            if (s.Dead) return;
            if (s.HasOwner && (s.Owner == null || !OwnerActive(s.Owner))) return;
            try { s.Fn(arg); }
            catch (Exception e) { Debug.LogException(e); }
        }

        internal void Deliver(TArg arg)
        {
            bool sawDead = false;
            int n = _subs.Count;                    // additions during the loop wait for the next event
            for (int i = 0; i < n; i++)
            {
                Sub s = _subs[i];
                if (s.Dead) { sawDead = true; continue; }
                if (s.HasOwner && s.Owner == null) { s.Dead = true; _live--; sawDead = true; continue; }
                if (s.HasOwner && !OwnerActive(s.Owner)) continue;
                try { s.Fn(arg); }
                catch (Exception e) { Debug.LogException(e); }
            }
            if (sawDead) _subs.RemoveAll(IsDead);
        }

        internal void Prune()
        {
            bool changed = false;
            for (int i = 0; i < _subs.Count; i++)
            {
                Sub s = _subs[i];
                if (!s.Dead && s.HasOwner && s.Owner == null) { s.Dead = true; _live--; changed = true; }
            }
            if (changed) _subs.RemoveAll(IsDead);
        }

        private static bool IsDead(Sub s) => s.Dead;

        private static bool OwnerActive(Component c)
        {
            var b = c as Behaviour;
            return b != null ? b.isActiveAndEnabled : c.gameObject.activeInHierarchy;
        }
    }

    // ---- variable ---------------------------------------------------------------

    /// <summary>Shared base for a scene variable handle. A DEFINITION owns the value; a
    /// REMOTE references one owned elsewhere. OnChange/OnWrite observers are owner-bound
    /// and fire on the main thread.</summary>
    public abstract class DartVariableBase<T> : DartPatternEntity
    {
        /// <summary>What an OnChange/OnWrite observer receives: the decoded value plus the
        /// write metadata (forced flag, source peer, sequence, arrival time).</summary>
        public readonly struct Update
        {
            public readonly T Value;
            public readonly VariableUpdate Meta;
            internal Update(T value, VariableUpdate meta) { Value = value; Meta = meta; }
        }

        private protected VariableDefinition<T> _coreVar;   // RemoteVariable<T> derives from this
        private readonly DartFanout<Update> _change = new DartFanout<Update>();
        private readonly DartFanout<Update> _write = new DartFanout<Update>();
        private bool _wantChange, _wantWrite;
        private bool _hasCache; private Update _cache;      // latest change, for late-joiner replay

        internal DartVariableBase(DartNodeUnity owner, string name) : base(owner, name) { }

        // Subclasses build the core definition/remote; base wires the observers onto it.
        private protected abstract VariableDefinition<T> CreateCore(DartNode node);

        internal override void EnsureNative()
        {
            if (_coreVar != null) return;
            DartNode node = Owner.NativeNode;
            if (node == null) return;
            _coreVar = CreateCore(node);
            _hasCache = false;
            if (_wantChange) RegisterChange();
            if (_wantWrite) RegisterWrite();
        }

        internal override void DropNative() { _coreVar = null; _hasCache = false; }
        internal override void PruneDeadOwners() { _change.Prune(); _write.Prune(); }

        // The core replays the current value synchronously at registration (on this, the
        // main thread), so the first observer sees it via the fan-out below; ongoing
        // changes fire on the service thread and are posted to the frame.
        private void RegisterChange()
            => _coreVar.OnChange((v, u) => Owner.RunOnMain(() => DeliverChange(new Update(v, u))));
        private void RegisterWrite()
            => _coreVar.OnWrite((v, u) => Owner.RunOnMain(() => _write.Deliver(new Update(v, u))));

        private void DeliverChange(Update e) { _cache = e; _hasCache = true; _change.Deliver(e); }

        /// <summary>Read the current value, fully copied out (definition: the store;
        /// remote: the cached latest). False when no value exists yet or the node is closed.</summary>
        public bool TryGet(out T value)
        {
            if (_coreVar != null) return _coreVar.TryGet(out value);
            value = default(T);
            return false;
        }

        /// <summary>The current value; throws if none exists yet. Set publishes (definition)
        /// or sends over the set channel (remote); a refused set logs a warning (use Set for
        /// the status-returning form).</summary>
        public T Value
        {
            get
            {
                T v;
                if (!TryGet(out v))
                    throw new InvalidOperationException("variable '" + Name + "' has no value yet");
                return v;
            }
            set
            {
                SendStatus st = Set(value);
                if (st != SendStatus.Ok)
                    Debug.LogWarning("[DART] variable '" + Name + "' set returned " + st);
            }
        }

        public SendStatus Set(T value)
        {
            EnsureNative();
            if (_coreVar == null) return SendStatus.NoTopic;
            return _coreVar.Set(value);
        }

        /// <summary>Force the value with a shadow source until Unforce (definition must have
        /// been created with allowForce).</summary>
        public SendStatus Force(T value)
        {
            EnsureNative();
            if (_coreVar == null) return SendStatus.NoTopic;
            return _coreVar.Force(value);
        }
        public SendStatus Unforce() => _coreVar != null ? _coreVar.Unforce() : SendStatus.NoTopic;
        public bool Forced => _coreVar != null && _coreVar.Forced;

        /// <summary>Observe state CHANGES (first value, different bytes, or a forced flip; a
        /// byte-identical re-set stays silent). Replays the current value once at
        /// registration so it can never be missed.</summary>
        public DartSubscription OnChange(Action<T> handler)
            => AddChange(e => handler(e.Value), null, false);
        public DartSubscription OnChange(Action<T, Update> handler)
            => AddChange(e => handler(e.Value, e), null, false);
        public DartSubscription OnChange(Component owner, Action<T> handler)
            => AddChange(e => handler(e.Value), owner, true);
        public DartSubscription OnChange(Component owner, Action<T, Update> handler)
            => AddChange(e => handler(e.Value, e), owner, true);

        /// <summary>Observe every applied WRITE, byte-identical or not (no replay at
        /// registration: writes are events, not state).</summary>
        public DartSubscription OnWrite(Action<T> handler)
            => AddWrite(e => handler(e.Value), null, false);
        public DartSubscription OnWrite(Action<T, Update> handler)
            => AddWrite(e => handler(e.Value, e), null, false);
        public DartSubscription OnWrite(Component owner, Action<T> handler)
            => AddWrite(e => handler(e.Value), owner, true);
        public DartSubscription OnWrite(Component owner, Action<T, Update> handler)
            => AddWrite(e => handler(e.Value, e), owner, true);

        /// <summary>Peers matched (definition: remotes; remote: owners, 0 = none present).</summary>
        public int RemoteCount => _coreVar != null ? _coreVar.RemoteCount : 0;

        private DartSubscription AddChange(Action<Update> fn, Component owner, bool hasOwner)
        {
            if (fn == null) throw new ArgumentNullException(nameof(fn));
            var s = _change.Add(fn, owner, hasOwner);
            if (!_wantChange)
            {
                _wantChange = true;
                if (_coreVar != null) RegisterChange();   // synchronous replay fans out to this observer
            }
            else if (_hasCache)
            {
                _change.DeliverOne(s, _cache);            // late joiner: replay the cached latest
            }
            return new DartSubscription(() => _change.Remove(s));
        }

        private DartSubscription AddWrite(Action<Update> fn, Component owner, bool hasOwner)
        {
            if (fn == null) throw new ArgumentNullException(nameof(fn));
            var s = _write.Add(fn, owner, hasOwner);
            if (!_wantWrite)
            {
                _wantWrite = true;
                if (_coreVar != null) RegisterWrite();
            }
            return new DartSubscription(() => _write.Remove(s));
        }
    }

    /// <summary>The authoritative variable: this node owns the value. Remotes on other
    /// nodes cache the latest published value.</summary>
    public sealed class DartVariableDefinition<T> : DartVariableBase<T>
    {
        private readonly bool _readOnly, _allowForce;
        private readonly int _catchUp;

        internal DartVariableDefinition(DartNodeUnity owner, string name,
                                        bool readOnly, bool allowForce, int catchUp)
            : base(owner, name)
        {
            _readOnly = readOnly; _allowForce = allowForce; _catchUp = catchUp;
        }

        private protected override VariableDefinition<T> CreateCore(DartNode node)
            => new VariableDefinition<T>(node, Name, _readOnly, _allowForce, _catchUp);
    }

    /// <summary>A reference to a variable owned by another node: reads see the cached
    /// latest, writes go over the set channel (dumb writes, no response).</summary>
    public sealed class DartRemoteVariable<T> : DartVariableBase<T>
    {
        private readonly int _catchUp;

        internal DartRemoteVariable(DartNodeUnity owner, string name, int catchUp)
            : base(owner, name)
        {
            _catchUp = catchUp;
        }

        private protected override VariableDefinition<T> CreateCore(DartNode node)
            => new RemoteVariable<T>(node, Name, _catchUp);

        /// <summary>Owners currently matched (0 = no owner present).</summary>
        public int MatchCount => RemoteCount;
        public bool HasDefinition => RemoteCount > 0;
    }

    // ---- function ---------------------------------------------------------------

    /// <summary>The implementation side of a request/response function: ONE definition per
    /// name on the network. The handler runs on the MAIN thread (the reply is parked on
    /// the service thread and completed from the frame), and a THROWN handler fails the
    /// call with CallStatus.AppError.</summary>
    public sealed class DartFunctionDefinition<TReq, TRsp> : DartPatternEntity
    {
        private FunctionDefinition<TReq, TRsp> _core;
        private readonly Func<TReq, TRsp> _handler;

        internal DartFunctionDefinition(DartNodeUnity owner, string name, Func<TReq, TRsp> handler)
            : base(owner, name)
        {
            _handler = handler ?? throw new ArgumentNullException(nameof(handler));
        }

        internal override void EnsureNative()
        {
            if (_core != null) return;
            DartNode node = Owner.NativeNode;
            if (node == null) return;
            _core = new FunctionDefinition<TReq, TRsp>(node, Name, (req, r) =>
            {
                // Defer on the service thread, run the user handler on the frame, complete there.
                Deferred<TRsp> reply = r.Defer();
                Owner.RunOnMain(() =>
                {
                    try { reply.Complete(_handler(req)); }
                    catch (Exception e) { reply.Fail(); Debug.LogException(e); }
                });
            });
        }

        internal override void DropNative() { _core = null; }

        /// <summary>Callers on other nodes matched to this definition.</summary>
        public int CallerCount => _core != null ? _core.CallerCount : 0;
    }

    /// <summary>A reference to a function defined on another node. Call is non-blocking:
    /// the result callback fires on the MAIN thread (the blocking core Call is illegal
    /// under Unity's service thread).</summary>
    public sealed class DartRemoteFunction<TReq, TRsp> : DartPatternEntity
    {
        private RemoteFunction<TReq, TRsp> _core;

        internal DartRemoteFunction(DartNodeUnity owner, string name) : base(owner, name) { }

        internal override void EnsureNative()
        {
            if (_core != null) return;
            DartNode node = Owner.NativeNode;
            if (node == null) return;
            _core = new RemoteFunction<TReq, TRsp>(node, Name);
        }

        internal override void DropNative() { _core = null; }

        /// <summary>Call the remote function; onResult fires once on the main thread with the
        /// outcome (inspect Status, never throws; reading Value when !Ok throws).</summary>
        public void Call(TReq request, Action<DartResponse<TRsp>> onResult)
        {
            if (onResult == null) throw new ArgumentNullException(nameof(onResult));
            EnsureNative();
            RemoteFunction<TReq, TRsp> rf = _core;
            if (rf == null)
            {
                onResult(new DartResponse<TRsp> { Core = new DartResponse { SendStatus = SendStatus.NoTopic } });
                return;
            }
            rf.CallAsync(request).ContinueWith(t =>
            {
                DartResponse<TRsp> res = t.Result;   // CallAsync never faults
                Owner.RunOnMain(() =>
                {
                    try { onResult(res); }
                    catch (Exception e) { Debug.LogException(e); }
                });
            });
        }

        /// <summary>Providers currently matched (the definition side present).</summary>
        public int MatchCount => _core != null ? _core.MatchCount : 0;
        public bool HasDefinition => _core != null && _core.HasDefinition;
    }
}
#endif
