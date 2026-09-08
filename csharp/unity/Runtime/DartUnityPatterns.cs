// The patterns for Unity: variables, functions and tasks shared by name through the scene's
// DartNodeUnity component. Every handler is marshaled to the main thread (unity/README.md).
#if UNITY_5_3_OR_NEWER
using System;
using System.Collections.Generic;
using System.Threading.Tasks;
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

        // Create the native core object if the node is open and it does not exist yet, and drop
        // the dead reference when the node closes.
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
            int n = _subs.Count;   // additions during the loop wait for the next message
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

    /// <summary>The shared base of a scene variable handle. A definition owns the value, a
    /// remote references one owned elsewhere. Observers run on the main thread.</summary>
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

        // Subclasses build the core definition or remote, the base wires the observers onto it.
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

        // The core replays the current value synchronously at registration on this, the main
        // thread, so the first observer sees it through the fan out. Later changes post to it.
        private void RegisterChange()
            => _coreVar.OnChange((v, u) => Owner.RunOnMain(() => DeliverChange(new Update(v, u))));
        private void RegisterWrite()
            => _coreVar.OnWrite((v, u) => Owner.RunOnMain(() => _write.Deliver(new Update(v, u))));

        private void DeliverChange(Update e) { _cache = e; _hasCache = true; _change.Deliver(e); }

        /// <summary>Read the current value copied out, the store or the cached latest. False when
        /// no value exists yet or the node is closed.</summary>
        public bool TryGet(out T value)
        {
            if (_coreVar != null) return _coreVar.TryGet(out value);
            value = default(T);
            return false;
        }

        /// <summary>The current value, throws if none exists yet. Set publishes or sends over the
        /// set channel, and a refused set logs a warning. Set() returns the status.</summary>
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

        /// <summary>Observe state changes: the first value, different bytes or a forced flip.
        /// Replays the current value once at registration.</summary>
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

        /// <summary>Peers matched: remotes for a definition, owners for a remote.</summary>
        public int RemoteCount => _coreVar != null ? _coreVar.RemoteCount : 0;

        private DartSubscription AddChange(Action<Update> fn, Component owner, bool hasOwner)
        {
            if (fn == null) throw new ArgumentNullException(nameof(fn));
            var s = _change.Add(fn, owner, hasOwner);
            if (!_wantChange)
            {
                _wantChange = true;
                if (_coreVar != null) RegisterChange();   // the replay fans out to this one
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
        private readonly int _catchUp, _keepLast;

        internal DartVariableDefinition(DartNodeUnity owner, string name,
                                        bool readOnly, bool allowForce, int catchUp, int keepLast)
            : base(owner, name)
        {
            _readOnly = readOnly; _allowForce = allowForce; _catchUp = catchUp; _keepLast = keepLast;
        }

        private protected override VariableDefinition<T> CreateCore(DartNode node)
            => new VariableDefinition<T>(node, Name, _readOnly, _allowForce, _catchUp, _keepLast);
    }

    /// <summary>A reference to a variable owned by another node: reads see the cached
    /// latest, writes go over the set channel (dumb writes, no response).</summary>
    public sealed class DartRemoteVariable<T> : DartVariableBase<T>
    {
        private readonly int _catchUp, _keepLast;

        internal DartRemoteVariable(DartNodeUnity owner, string name, int catchUp, int keepLast)
            : base(owner, name)
        {
            _catchUp = catchUp; _keepLast = keepLast;
        }

        private protected override VariableDefinition<T> CreateCore(DartNode node)
            => new RemoteVariable<T>(node, Name, _catchUp, _keepLast);

        /// <summary>Owners currently matched (0 = no owner present).</summary>
        public int MatchCount => RemoteCount;
        public bool HasDefinition => RemoteCount > 0;
    }

    // ---- function ---------------------------------------------------------------

    /// <summary>The implementation side of a function, one definition per name. The handler
    /// runs on the main thread and a thrown handler fails the call with AppError.</summary>
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

    /// <summary>A reference to a function defined elsewhere. Call is non blocking and the
    /// result callback fires on the main thread.</summary>
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

        /// <summary>Call the remote function. onResult fires once on the main thread with the
        /// outcome: inspect Status, and reading Value when not Ok throws.</summary>
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

    // ---- task -------------------------------------------------------------------

    /// <summary>One in-flight task call (from DartRemoteTask.Call). Cancel requests
    /// cooperative cancellation: the outcome says whether it was honored.</summary>
    public sealed class DartTaskRun
    {
        private readonly Func<SendStatus> _cancel;
        /// <summary>The call id (0 = the request never committed).</summary>
        public readonly uint CallId;

        internal DartTaskRun(uint callId, Func<SendStatus> cancel) { CallId = callId; _cancel = cancel; }

        public SendStatus Cancel() => _cancel != null ? _cancel() : SendStatus.NoTopic;
    }

    /// <summary>The implementation side of a task, one definition per name. The async handler
    /// runs on the main thread and its completion answers the call (docs/csharp.md).</summary>
    public sealed class DartTaskDefinition<TReq, TPrg, TRsp> : DartPatternEntity
    {
        private TaskDefinition<TReq, TPrg, TRsp> _core;
        private readonly Func<TReq, TaskContext<TPrg>, Task<TRsp>> _handler;
        private readonly bool _noCancel, _exclusive;

        internal DartTaskDefinition(DartNodeUnity owner, string name,
                                    Func<TReq, TaskContext<TPrg>, Task<TRsp>> handler,
                                    bool noCancel, bool exclusive)
            : base(owner, name)
        {
            _handler = handler ?? throw new ArgumentNullException(nameof(handler));
            _noCancel = noCancel; _exclusive = exclusive;
        }

        internal override void EnsureNative()
        {
            if (_core != null) return;
            DartNode node = Owner.NativeNode;
            if (node == null) return;
            _core = new TaskDefinition<TReq, TPrg, TRsp>(node, Name, (req, ctx) =>
            {
                // Hop to the frame. The returned Task's completion answers the call.
                var tcs = new TaskCompletionSource<TRsp>(TaskCreationOptions.RunContinuationsAsynchronously);
                Owner.RunOnMain(async () =>
                {
                    try { tcs.TrySetResult(await _handler(req, ctx)); }
                    catch (OperationCanceledException) { tcs.TrySetCanceled(); }
                    catch (Exception e) { tcs.TrySetException(e); Debug.LogException(e); }
                });
                return tcs.Task;
            }, noCancel: _noCancel, exclusive: _exclusive);
        }

        internal override void DropNative() { _core = null; }

        /// <summary>Callers on other nodes matched to this definition.</summary>
        public int CallerCount => _core != null ? _core.CallerCount : 0;
    }

    /// <summary>A reference to a task defined elsewhere. Call is non blocking, onProgress and
    /// onResult fire on the main thread, and the returned run handle cancels.</summary>
    public sealed class DartRemoteTask<TReq, TPrg, TRsp> : DartPatternEntity
    {
        private RemoteTask<TReq, TPrg, TRsp> _core;

        internal DartRemoteTask(DartNodeUnity owner, string name) : base(owner, name) { }

        internal override void EnsureNative()
        {
            if (_core != null) return;
            DartNode node = Owner.NativeNode;
            if (node == null) return;
            _core = new RemoteTask<TReq, TPrg, TRsp>(node, Name);
        }

        internal override void DropNative() { _core = null; }

        private sealed class MainThreadProgress : IProgress<TPrg>
        {
            internal DartNodeUnity Owner;
            internal Action<TPrg> Fn;
            public void Report(TPrg value)
                => Owner.RunOnMain(() =>
                {
                    try { Fn(value); }
                    catch (Exception e) { Debug.LogException(e); }
                });
        }

        /// <summary>Start the task. onResult fires once on the main thread, onProgress per typed
        /// update. Inspect Status, it never throws.</summary>
        public DartTaskRun Call(TReq request, Action<DartResponse<TRsp>> onResult,
                                Action<TPrg> onProgress = null)
        {
            if (onResult == null) throw new ArgumentNullException(nameof(onResult));
            EnsureNative();
            RemoteTask<TReq, TPrg, TRsp> core = _core;
            if (core == null)
            {
                onResult(new DartResponse<TRsp> { Core = new DartResponse { SendStatus = SendStatus.NoTopic } });
                return new DartTaskRun(0, null);
            }
            IProgress<TPrg> prg = onProgress != null
                ? new MainThreadProgress { Owner = Owner, Fn = onProgress } : null;
            uint id;
            core.CallAsync(request, out id, prg).ContinueWith(t =>
            {
                DartResponse<TRsp> res = t.Result;   // CallAsync never faults
                Owner.RunOnMain(() =>
                {
                    try { onResult(res); }
                    catch (Exception e) { Debug.LogException(e); }
                });
            });
            uint cid = id;
            return new DartTaskRun(id, () => core.Cancel(cid));
        }

        /// <summary>Providers currently matched (the definition side present).</summary>
        public int MatchCount => _core != null ? _core.MatchCount : 0;
        public bool HasDefinition => _core != null && _core.HasDefinition;
    }
}
#endif
