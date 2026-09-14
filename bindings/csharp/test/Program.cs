// The C# binding test: two nodes on one host over reliable typed pub sub, the consumer
// surface and the pattern legs. Exit 0 = pass (spec/testing.md).

using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using Ramble;

struct Twist { public float Dx; public float Dy; }

// A schema exercising the v4 variable-length kinds: a variable string, a variable
// scalar array, a variable string array, and a self-describing map.
[RambleSchema("Sensor")]
struct Sensor
{
    public uint Id;
    [RambleString(16)] public string Name;   // capped string (fixed)
    public string Note;                       // variable string
    public float[] Samples;                   // variable scalar array
    [RambleString(8)] public string[] Labels; // variable string array
    public Dictionary<string, object> Extras;  // self-describing map
}

struct Pose
{
    public ulong Stamp;
    public double X;
    public double Y;
    [RambleArray(4)] public byte[] Uuid;
    [RambleString(16)] public string Frame;
    [RambleArray(2), RambleString(8)] public string[] Tags;
    public Twist Vel;
}

// patterns leg types
struct AddReq { public int A; public int B; }
struct AddRsp { public int Sum; }
struct Level { public int Value; }

// tasks leg types
struct XferReq { public int Chunks; }
struct XferPrg { public int Done; }
struct XferRsp { public int Total; }

// IProgress that reports inline on the delivering thread (System.Progress posts to a
// SynchronizationContext, which a console app lacks, losing ordering).
sealed class InlineProgress<T> : IProgress<T>
{
    private readonly Action<T> _fn;
    public InlineProgress(Action<T> fn) { _fn = fn; }
    public void Report(T value) { _fn(value); }
}

static class Program
{
    static readonly ManualResetEventSlim Got = new ManualResetEventSlim(false);
    static RambleMessage Received;

    // Encode and decode round trip of the variable kinds, no networking.
    static bool RoundTrip()
    {
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }

        using var s = new Schema(typeof(Sensor));
        Console.WriteLine("Sensor DSL:\n" + s.Dsl);
        var src = new Sensor
        {
            Id = 42,
            Name = "lidar",
            Note = "a long unbounded note that exceeds sixteen bytes easily",
            Samples = new[] { 1.5f, -2.25f, 3.75f },
            Labels = new[] { "front", "left", "rearmost" },   // 'rearmost' is 8 bytes (cap 8)
            Extras = new Dictionary<string, object>
            {
                { "battery", 87 }, { "signed", -5 }, { "mid", 40000 }, { "neg32", -100000 },
                { "big", 5000000000L }, { "state", "docked" }, { "ok", true },
                { "temps", new object[] { 36.2, 34.9, -1.0 } },
                { "meta", new Dictionary<string, object> { { "fw", "1.2.3" }, { "rev", 7 } } },
            },
        };
        byte[] raw = s.Encode(src);
        Console.WriteLine($"encoded {raw.Length} bytes");
        var o = (Sensor)s.Decode(raw);

        Check("id", o.Id == 42);
        Check("name (capped)", o.Name == "lidar");
        Check("note (vstr)", o.Note == src.Note);
        Check("samples (varr f32)", o.Samples != null && o.Samples.Length == 3
            && Math.Abs(o.Samples[0] - 1.5) < 1e-6 && Math.Abs(o.Samples[1] + 2.25) < 1e-6
            && Math.Abs(o.Samples[2] - 3.75) < 1e-6);
        Check("labels (varr string)", o.Labels != null && o.Labels.Length == 3
            && o.Labels[0] == "front" && o.Labels[1] == "left" && o.Labels[2] == "rearmost");
        var e = o.Extras;
        Check("map battery", e != null && Convert.ToInt64(e["battery"]) == 87);
        Check("map signed", Convert.ToInt64(e["signed"]) == -5);
        Check("map mid u16", Convert.ToInt64(e["mid"]) == 40000);
        Check("map neg32 i32", Convert.ToInt64(e["neg32"]) == -100000);
        Check("map big u32+", Convert.ToInt64(e["big"]) == 5000000000L);
        Check("map state", (string)e["state"] == "docked");
        Check("map ok bool", (bool)e["ok"]);
        Check("map temps", e["temps"] is List<object> t && t.Count == 3
            && Math.Abs(Convert.ToDouble(t[0]) - 36.2) < 1e-9 && Math.Abs(Convert.ToDouble(t[2]) + 1.0) < 1e-9);
        Check("map nested", e["meta"] is Dictionary<string, object> m
            && (string)m["fw"] == "1.2.3" && Convert.ToInt64(m["rev"]) == 7);

        // empty variable fields must round-trip to empty (not null)
        var o2 = (Sensor)s.Decode(s.Encode(new Sensor { Id = 1 }));
        Check("empty note", o2.Note == "");
        Check("empty samples", o2.Samples != null && o2.Samples.Length == 0);
        Check("empty labels", o2.Labels != null && o2.Labels.Length == 0);
        Check("empty map", o2.Extras != null && o2.Extras.Count == 0);

        // decode to dict, the reflection path bridges and observers use, must surface the
        // variable kinds too: a string, a typed array and a dictionary
        var fd = s.DecodeFields(raw);
        Check("fields note", (string)fd["Note"] == src.Note);
        Check("fields samples", fd["Samples"] is float[] fs && fs.Length == 3);
        Check("fields labels", fd["Labels"] is string[] fl && fl.Length == 3 && fl[2] == "rearmost");
        Check("fields map", fd["Extras"] is Dictionary<string, object> fe && Convert.ToInt64(fe["battery"]) == 87);

        // encode straight from a Dictionary (no typed object), with List<> values
        var od = (Sensor)s.Decode(s.Encode(new Dictionary<string, object>
        {
            { "Id", 7 }, { "Name", "cam" }, { "Note", "from a dictionary source" },
            { "Samples", new List<float> { 0.5f, 1.5f } },     // List, not float[]
            { "Labels", new List<string> { "a", "bb" } },       // List<string>, not string[]
            { "Extras", new Dictionary<string, object> { { "k", 9 } } },
        }));
        Check("dict id", od.Id == 7);
        Check("dict name", od.Name == "cam");
        Check("dict note", od.Note == "from a dictionary source");
        Check("dict samples (List)", od.Samples != null && od.Samples.Length == 2
            && Math.Abs(od.Samples[0] - 0.5) < 1e-6 && Math.Abs(od.Samples[1] - 1.5) < 1e-6);
        Check("dict labels (List)", od.Labels != null && od.Labels.Length == 2 && od.Labels[1] == "bb");
        Check("dict map", od.Extras != null && Convert.ToInt64(od.Extras["k"]) == 9);

        // an over-cap element in a variable string array must throw
        try
        {
            s.Encode(new Sensor { Labels = new[] { "toolongforcap" } });
            Check("over-cap throws", false);
        }
        catch (SchemaException) { Check("over-cap throws", true); }

        Console.WriteLine(ok ? "variable-kinds round-trip: PASS\n" : "variable-kinds round-trip: FAIL\n");
        return ok;
    }

    // Functions and variables between two nodes on an isolated domain.
    static bool Patterns()
    {
        Console.WriteLine("patterns leg: two nodes, domain 43, loopback");
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }

        var srv = new RambleNode("srv", null, e => Console.WriteLine("event(srv): " + e),
                           domain: 43, multicastInterface: "127.0.0.1", maxTopics: 32);
        var cli = new RambleNode("cli", null, e => Console.WriteLine("event(cli): " + e),
                           domain: 43, multicastInterface: "127.0.0.1", maxTopics: 32);

        // definitions on srv: the simple form, a thrower giving AppError, and a full form that
        // defers off thread
        var add = new FunctionDefinition<AddReq, AddRsp>(srv, "add",
            q => new AddRsp { Sum = q.A + q.B });
        var boom = new FunctionDefinition<AddReq, AddRsp>(srv, "boom",
            (Func<AddReq, AddRsp>)(q => throw new Exception("kaboom")));
        var late = new FunctionDefinition<AddReq, AddRsp>(srv, "late", (q, req) =>
        {
            var d = req.Defer();
            ThreadPool.QueueUserWorkItem(_ =>
            {
                Thread.Sleep(50);
                d.Complete(new AddRsp { Sum = q.A + q.B });
            });
        });
        var lvlDef = new VariableDefinition<Level>(srv, "level", new Level { Value = 5 },
                                                   allowForce: true);

        srv.Start();   // the service thread owns srv's loop, handlers fire on it

        // remotes on cli (manual poll: blocking calls drive cli's loop themselves)
        var addR = new RemoteFunction<AddReq, AddRsp>(cli, "add");
        var boomR = new RemoteFunction<AddReq, AddRsp>(cli, "boom");
        var lateR = new RemoteFunction<AddReq, AddRsp>(cli, "late");
        var lvl = new RemoteVariable<Level>(cli, "level");

        var deadline = DateTime.UtcNow.AddSeconds(8);
        while (DateTime.UtcNow < deadline
               && !(addR.HasDefinition && boomR.HasDefinition && lateR.HasDefinition))
            cli.Poll(5);
        Check("definitions discovered", addR.HasDefinition && boomR.HasDefinition && lateR.HasDefinition);

        // blocking calls
        var r = addR.Call(new AddReq { A = 2, B = 3 }, 3000);
        Check("blocking call Ok", r.Ok && r.Status == CallStatus.Ok);
        Check("blocking call value", r.Ok && r.Value.Sum == 5);
        Check("provider set", r.Ok && r.Provider != 0);

        var rb = boomR.Call(new AddReq { A = 1, B = 1 }, 3000);
        Check("thrown handler -> AppError", rb.Status == CallStatus.AppError);
        bool threw = false;
        try { var _ = rb.Value; } catch (CallException) { threw = true; }
        Check("Value on !Ok throws CallException", threw);

        var rl = lateR.Call(new AddReq { A = 20, B = 22 }, 3000);
        Check("deferred completion", rl.Ok && rl.Value.Sum == 42);
        Check("caller count seen by definition", add.CallerCount == 1);

        // variable: catch_up hands the remote the initial value
        Check("variable wait", lvl.Wait(3000));
        Level lv;
        Check("initial value", lvl.TryGet(out lv) && lv.Value == 5);
        Check("HasDefinition", lvl.HasDefinition);
        Check("remote set accepted", lvl.Set(new Level { Value = 9 }) == SendStatus.Ok);
        deadline = DateTime.UtcNow.AddSeconds(5);
        while (DateTime.UtcNow < deadline && !(lvl.TryGet(out lv) && lv.Value == 9)) cli.Poll(5);
        Check("set round-trips to the remote", lvl.Value.Value == 9);
        Check("definition applied it", lvlDef.Value.Value == 9);

        // force overrides with a shadow source, unforce restores the latest set
        Check("force", lvlDef.Force(new Level { Value = 99 }) == SendStatus.Ok);
        deadline = DateTime.UtcNow.AddSeconds(5);
        while (DateTime.UtcNow < deadline && !(lvl.TryGet(out lv) && lv.Value == 99)) cli.Poll(5);
        Check("forced value visible remotely", lvl.Value.Value == 99);
        Check("remote sees Forced", lvl.Forced);
        Check("unforce", lvlDef.Unforce() == SendStatus.Ok);
        deadline = DateTime.UtcNow.AddSeconds(5);
        while (DateTime.UtcNow < deadline && !(lvl.TryGet(out lv) && lv.Value == 9)) cli.Poll(5);
        Check("unforce restores the latest set", lvl.Value.Value == 9 && !lvl.Forced);

        // variable events: OnChange dedups + replays at registration, OnWrite counts
        // every applied write
        var chg = new List<long>(); uint chgSource = 1234; int wr = 0;
        lvlDef.OnChange((Level v, VariableUpdate u) => { chg.Add(v.Value); chgSource = u.Source; });
        Check("OnChange replays current at registration", chg.Count == 1 && chg[0] == 9);
        lvlDef.OnWrite((Level v) => Interlocked.Increment(ref wr));
        Check("OnWrite does not replay", wr == 0);
        Check("identical re-set accepted", lvlDef.Set(new Level { Value = 9 }) == SendStatus.Ok);
        Check("identical re-set is a write, not a change", chg.Count == 1 && wr == 1);
        Check("new set accepted", lvlDef.Set(new Level { Value = 12 }) == SendStatus.Ok);
        Check("change fires inline with the new value",
              chg.Count == 2 && chg[1] == 12 && chgSource == 0 && wr == 2);
        var rchg = new List<long>();
        lvl.OnChange((Level v) => { lock (rchg) rchg.Add(v.Value); });
        lock (rchg) Check("remote OnChange replays the cache", rchg.Count == 1 && rchg[0] == 9);
        deadline = DateTime.UtcNow.AddSeconds(5);
        while (DateTime.UtcNow < deadline)
        {
            lock (rchg) if (rchg.Count > 0 && rchg[rchg.Count - 1] == 12) break;
            cli.Poll(5);
        }
        lock (rchg) Check("remote change arrives", rchg.Count > 0 && rchg[rchg.Count - 1] == 12);
        lvlDef.OnChange((Action<Level>)null);
        lvlDef.OnWrite((Action<Level>)null);
        lvl.OnChange((Action<Level>)null);

        // async form: start cli's service thread, await the Task (it never faults)
        cli.Start();
        var t = addR.CallAsync(new AddReq { A = 10, B = 5 });
        Check("await CallAsync", t.Wait(5000) && t.Result.Ok && t.Result.Value.Sum == 15);
        // a blocking call is refused while the service thread owns the loop, loudly
        var rr = addR.Call(new AddReq { A = 1, B = 2 }, 100);
        Check("blocking call refused under service thread",
              rr.Status == CallStatus.Timeout && rr.SendStatus == SendStatus.State);
        cli.Stop();

        // a call still pending at Close settles its Task with Cancelled, never hangs
        var never = new RemoteFunction(cli, "never-served", null, null, timeoutMs: 60000);
        var tc = never.CallAsync(null);
        srv.Close();
        cli.Close();
        Check("pending CallAsync settles Cancelled at Close",
              tc.Wait(2000) && tc.Result.Status == CallStatus.Cancelled);
        Console.WriteLine(ok ? "patterns: PASS\n" : "patterns: FAIL\n");
        return ok;
    }

    // Tasks between two nodes: async handlers, typed + untyped progress, RUNNING order,
    // CancellationToken cancel, noCancel refusal, and the async function-handler overload.
    static bool Tasks()
    {
        Console.WriteLine("tasks leg: two nodes, domain 47, loopback");
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }

        var srv = new RambleNode("tsrv", null, e => { if (e.IsError) Console.WriteLine("event(tsrv): " + e); },
                               domain: 47, multicastInterface: "127.0.0.1", maxTopics: 32);
        var cli = new RambleNode("tcli", null, e => { if (e.IsError) Console.WriteLine("event(tcli): " + e); },
                               domain: 47, multicastInterface: "127.0.0.1", maxTopics: 32);
        try
        {
            // a transfer that streams progress between awaits and honors its token
            var xfer = new TaskDefinition<XferReq, XferPrg, XferRsp>(srv, "xfer", async (q, ctx) =>
            {
                for (int i = 1; i <= q.Chunks; i++)
                {
                    await Task.Delay(20, ctx.CancellationToken).ConfigureAwait(false);
                    ctx.Progress(new XferPrg { Done = i });
                }
                return new XferRsp { Total = q.Chunks };
            });
            // runs until cancelled through its token
            var forever = new TaskDefinition<XferReq, XferPrg, XferRsp>(srv, "forever", async (q, ctx) =>
            {
                await Task.Delay(Timeout.Infinite, ctx.CancellationToken).ConfigureAwait(false);
                return new XferRsp();
            });
            // declares cancellation will not be honored
            var stubborn = new TaskDefinition<XferReq, XferPrg, XferRsp>(srv, "stubborn", async (q, ctx) =>
            {
                await Task.Delay(20).ConfigureAwait(false);
                return new XferRsp { Total = 1 };
            }, noCancel: true);
            // a second transfer for the untyped leg: a same-name second handle on one node
            // would be shadowed by the first (one name = one topic per node)
            var xfer2 = new TaskDefinition<XferReq, XferPrg, XferRsp>(srv, "xfer2", async (q, ctx) =>
            {
                for (int i = 1; i <= q.Chunks; i++)
                {
                    await Task.Delay(20, ctx.CancellationToken).ConfigureAwait(false);
                    ctx.Progress(new XferPrg { Done = i });
                }
                return new XferRsp { Total = q.Chunks };
            });
            // the async FUNCTION handler overload
            var amul = new FunctionDefinition<AddReq, AddRsp>(srv, "amul", async q =>
            {
                await Task.Delay(10).ConfigureAwait(false);
                return new AddRsp { Sum = q.A * q.B };
            });

            srv.Start();

            var xferR = new RemoteTask<XferReq, XferPrg, XferRsp>(cli, "xfer");
            var foreverR = new RemoteTask<XferReq, XferPrg, XferRsp>(cli, "forever");
            var stubbornR = new RemoteTask<XferReq, XferPrg, XferRsp>(cli, "stubborn");
            var reqS = new Schema(typeof(XferReq));
            var prgS = new Schema(typeof(XferPrg));
            var rspS = new Schema(typeof(XferRsp));
            var xferRaw = new RemoteTask(cli, "xfer2", reqS, prgS, rspS);
            var amulR = new RemoteFunction<AddReq, AddRsp>(cli, "amul");

            var deadline = DateTime.UtcNow.AddSeconds(8);
            while (DateTime.UtcNow < deadline
                   && !(xferR.HasDefinition && foreverR.HasDefinition && stubbornR.HasDefinition
                        && xferRaw.HasDefinition && amulR.HasDefinition))
                cli.Poll(5);
            Check("definitions discovered", xferR.HasDefinition && foreverR.HasDefinition
                  && stubbornR.HasDefinition && xferRaw.HasDefinition && amulR.HasDefinition);
            cli.Start();   // CallAsync outcomes and progress fire on cli's service thread

            // typed: progress values in order, terminal Ok with the decoded result
            var seen = new List<int>();
            var t1 = xferR.CallAsync(new XferReq { Chunks = 3 },
                new InlineProgress<XferPrg>(p => { lock (seen) seen.Add(p.Done); }));
            Check("typed task Ok", t1.Wait(10000) && t1.Result.Ok);
            Check("typed result decoded", t1.Result.Ok && t1.Result.Value.Total == 3);
            lock (seen)
                Check("typed progress in order (no RUNNING ack)",
                      seen.Count == 3 && seen[0] == 1 && seen[1] == 2 && seen[2] == 3);

            // untyped info form: the RUNNING ack (null Value) first, then the values
            var infos = new List<TaskProgress>();
            var t2 = xferRaw.CallAsync(reqS.Encode(new XferReq { Chunks = 2 }),
                new InlineProgress<TaskProgress>(p => { lock (infos) infos.Add(p); }));
            Check("untyped task Ok", t2.Wait(10000) && t2.Result.Status == CallStatus.Ok);
            lock (infos)
            {
                Check("RUNNING ack first (null Value)", infos.Count == 3 && infos[0].Value == null);
                Check("then the progress values", infos.Count == 3
                      && infos[1].Value != null && infos[2].Value != null
                      && Convert.ToInt64(prgS.DecodeFields(infos[2].Value)["Done"]) == 2);
                Check("info carries the provider", infos.Count == 3 && infos[1].Provider != 0);
            }

            // cancellation: the token ends the handler via ITS token, caller sees Cancelled
            var cts = new CancellationTokenSource();
            var t3 = foreverR.CallAsync(new XferReq(), null, cts.Token);
            Thread.Sleep(300);   // let RUNNING land (the deadline is dropped)
            cts.Cancel();
            Check("cancel honored end to end", t3.Wait(10000)
                  && t3.Result.Status == CallStatus.Cancelled);

            // noCancel: refused locally with BadRole, the call completes anyway
            uint sid;
            var t4 = stubbornR.CallAsync(new XferReq(), out sid);
            Check("call id delivered at commit", sid != 0);
            Check("noCancel refused locally", stubbornR.Cancel(sid) == SendStatus.BadRole);
            Check("noCancel call completes anyway", t4.Wait(10000) && t4.Result.Ok
                  && t4.Result.Value.Total == 1);

            // async function-handler overload round trip
            var t5 = amulR.CallAsync(new AddReq { A = 6, B = 7 });
            Check("async function handler", t5.Wait(10000) && t5.Result.Ok
                  && t5.Result.Value.Sum == 42);

            cli.Stop();
        }
        finally
        {
            srv.Close();
            cli.Close();
        }
        Console.WriteLine(ok ? "tasks: PASS\n" : "tasks: FAIL\n");
        return ok;
    }

    // The canonical wire of a bare type is its kind alone, so these hashes are the same in
    // every language binding (pinned in C by ramble_test's schema-root phase).
    const ulong HashBool = 0xee90234f61d2520bUL;
    const ulong HashF32Arr = 0x314844e3386a1fc4UL;

    enum Mode : byte { Idle = 0, Run = 1, Fault = 2 }

    // The Float3 schema is the shared cross-language golden vector: the same wire and the
    // same hash from C, C++, C# and Python (pinned in C by ramble_test's stdtypes phase).
    const ulong HashFloat3 = 0x04aa9469cd08b1ddUL;

    // The video family, same golden vectors (pinned in bindings/cpp/test.cpp too). A mirror whose
    // enum member names, values or order drifted would hash differently, so this catches it.
    const ulong HashImage = 0x489841f99f392b85UL;
    const ulong HashVideoFrame = 0xf677bd147b513fbcUL;
    const ulong HashExternalVideoStream = 0xaae502077016ac13UL;

    // Standard types as ordinary fields: an alias names a plain field's type, a composite is
    // a shipped mirror struct that names itself. Both narrow matching.
    struct Track
    {
        [RambleField("at")]   public Ramble.Transform At;
        [RambleField("when")] [RambleTypeName("Timestamp")] public long When;
        [RambleField("tag")]  public Ramble.Color Tag;
        [RambleField("id")]   [RambleTypeName("Uuid")] [RambleArray(16)] public byte[] Id;
        [RambleField("velocity")] public Ramble.Float3 Velocity;
    }

    static bool StdTypes()
    {
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }

        using (var f3 = new Schema("Float3"))
        using (var mirror = new Schema(typeof(Ramble.Float3)))
        {
            Check("Float3 compiles by name alone, golden hash", f3.Hash == HashFloat3);
            Check("Float3 is 12 message bytes", f3.Size == 12);
            Check("the mirror struct IS that type", mirror.Hash == HashFloat3);
        }
        // a name narrows: an anonymous field of the same shape reads a Transform field, never
        // the reverse, and Transform/Twist are distinct names never mistaken for each other
        using (var named = new Schema("W { at: Transform }"))
        using (var bare = new Schema("W { at: { translation: { x: f64, y: f64, z: f64 }," +
                                     "         rotation: { x: f64, y: f64, z: f64, w: f64 }," +
                                     "         parent: string<30> } }"))
        using (var xform = new Schema("Transform"))
        using (var twist = new Schema("Twist"))
        {
            Check("an anonymous field of the same shape reads a Transform field",
                  bare.CanRead(named) && !named.CanRead(bare));
            Check("Transform and Twist never cross-wire", !twist.CanRead(xform));
        }
        using (var sch = new Schema(typeof(Track)))
        {
            string text = sch.Dsl;
            Check("the reflected schema spells the names, not the shapes",
                  text.Contains("at: Transform") && text.Contains("when: Timestamp")
                  && text.Contains("id: Uuid") && text.Contains("velocity: Float3"));
            Check("its message is the sum of the wire shapes (88+8+4+16+12)", sch.Size == 128);

            var t = new Track {
                At = new Ramble.Transform { Translation = new Ramble.Double3 { X = 4.5, Y = -1.25, Z = 9.0 },
                                Rotation = Std.IdentityRotation() },
                When = Std.Now(),
                Tag = Std.ColorFromHex(0x112233FFu),
                Id = new byte[16],
                Velocity = new Ramble.Float3 { X = 1.0f, Y = 2.0f, Z = 3.0f } };
            for (int i = 0; i < 16; i++) t.Id[i] = (byte)i;
            var back = (Track)sch.Decode(sch.Encode(t));
            Check("a Track round-trips whole",
                  back.At.Translation.X == 4.5 && back.At.Rotation.W == 1.0
                  && back.When == t.When && back.Tag.R == 0x11 && back.Tag.A == 0xFF
                  && back.Id != null && back.Id[15] == 15 && back.Velocity.Z == 3.0f);
            Check("Std.Now is Unix-epoch microseconds", Std.Now() > 1600000000000000L);
        }
        // the video family: the shipped mirror must compile to the canonical bytes, and the
        // bare name must resolve to the same ones, so both forms are checked hash-exact
        (string, Type, ulong)[] video =
        {
            ("Image", typeof(Ramble.Image), HashImage),
            ("VideoFrame", typeof(Ramble.VideoFrame), HashVideoFrame),
            ("ExternalVideoStream", typeof(Ramble.ExternalVideoStream), HashExternalVideoStream),
        };
        foreach (var (name, clr, gold) in video)
        {
            using var text = new Schema(name);
            using var mirror = new Schema(clr);
            Console.WriteLine($"       {name}: text 0x{text.Hash:x16} mirror 0x{mirror.Hash:x16}");
            Check(name + " compiles by name alone, golden hash", text.Hash == gold);
            Check("the " + name + " mirror IS that type", mirror.Hash == gold);
        }
        return ok;
    }

    // The video family live: an Image (a variable payload beside fixed fields) crosses two
    // nodes, and ExternalVideoStream is the latched variable it exists for.
    static bool VideoLive()
    {
        Console.WriteLine("video live leg: two nodes, domain 46, loopback");
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }

        var a = new RambleNode("MA", null, e => { if (e.IsError) Console.WriteLine("event(MA): " + e); },
                             domain: 46, multicastInterface: "127.0.0.1");
        var b = new RambleNode("MB", null, e => { if (e.IsError) Console.WriteLine("event(MB): " + e); },
                             domain: 46, multicastInterface: "127.0.0.1");
        try
        {
            var pub = new Topic<Ramble.Image>(a, "frame", Role.PubOnly, new Qos { Reliability = Reliability.Reliable, KeepLast = 4 });
            var sub = new Topic<Ramble.Image>(b, "frame", Role.SubOnly, new Qos { Reliability = Reliability.Reliable, KeepLast = 4 });
            sub.TryTake(out Ramble.Image _);   // switch to queued delivery
            var initial = new Ramble.ExternalVideoStream
            {
                Kind = Ramble.VideoStreamKind.Rtsp,
                Codec = Ramble.VideoCodec.H264,
                Width = 1920,
                Height = 1080,
                Url = "rtsp://cam.local/main",
                Name = "front door",
            };
            var vd = new VariableDefinition<Ramble.ExternalVideoStream>(a, "stream", initial);
            var rv = new RemoteVariable<Ramble.ExternalVideoStream>(b, "stream");

            var deadline = DateTime.UtcNow.AddSeconds(8);
            while (DateTime.UtcNow < deadline
                   && (pub.MatchCount() == 0 || !rv.TryGet(out Ramble.ExternalVideoStream _)))
            {
                a.Poll(1);
                b.Poll(1);
            }
            Check("image topic matched", pub.MatchCount() == 1);

            var img = new Ramble.Image
            {
                Width = 64, Height = 4, Stride = 64,
                Format = Ramble.ImageFormat.Mono8,
                Data = new byte[256],
            };
            for (int i = 0; i < img.Data.Length; i++) img.Data[i] = (byte)(i * 7);
            Check("image send", pub.Send(img) == SendStatus.Ok);

            bool gotImage = false;
            deadline = DateTime.UtcNow.AddSeconds(5);
            while (DateTime.UtcNow < deadline && !gotImage)
            {
                a.Poll(1);
                b.Poll(1);
                if (sub.TryTake(out Ramble.Image got, 1))
                    gotImage = got.Width == 64 && got.Height == 4 && got.Stride == 64
                               && got.Format == Ramble.ImageFormat.Mono8
                               && got.Data != null && got.Data.Length == img.Data.Length
                               && got.Data[0] == img.Data[0] && got.Data[255] == img.Data[255];
            }
            Check("an Image crosses whole (fixed fields + the variable payload)", gotImage);

            Check("stream variable replicated the initial",
                  rv.TryGet(out Ramble.ExternalVideoStream s0)
                  && s0.Kind == Ramble.VideoStreamKind.Rtsp
                  && s0.Codec == Ramble.VideoCodec.H264
                  && s0.Width == 1920 && s0.Height == 1080
                  && s0.Url == "rtsp://cam.local/main" && s0.Name == "front door");

            Check("stream variable set accepted", rv.Set(new Ramble.ExternalVideoStream
            {
                Kind = Ramble.VideoStreamKind.WebrtcWhep,
                Url = "https://gw.local/whep/cam1",
                Name = "front door",
            }) == SendStatus.Ok);
            deadline = DateTime.UtcNow.AddSeconds(5);
            while (DateTime.UtcNow < deadline
                   && !(vd.TryGet(out Ramble.ExternalVideoStream v)
                        && v.Kind == Ramble.VideoStreamKind.WebrtcWhep))
            {
                a.Poll(1);
                b.Poll(1);
            }
            Check("stream variable set converged at the owner",
                  vd.TryGet(out Ramble.ExternalVideoStream s1)
                  && s1.Kind == Ramble.VideoStreamKind.WebrtcWhep
                  && s1.Url == "https://gw.local/whep/cam1");
        }
        finally
        {
            a.Close();
            b.Close();
        }
        Console.WriteLine(ok ? "video live leg: PASS\n" : "video live leg: FAIL\n");
        return ok;
    }

    // Bare types as whole schemas: no struct wrapper, plain values through Send/TryTake.
    static bool ValueRoots()
    {
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }

        using (var sb = new Schema(typeof(bool)))
        using (var sa = new Schema(typeof(float[])))
        {
            Check("bool canonical hash", sb.Hash == HashBool);
            Check("float[] canonical hash", sa.Hash == HashF32Arr);
            Check("bool dsl", sb.Dsl == "bool\n");
            Check("float[] dsl", sa.Dsl == "f32[]\n");
            Check("bare root is one anonymous field",
                  sb.FieldCount == 1 && sb.Name == "" && sb.IsValueRoot);
            using (var text = new Schema("bool"))
                Check("text `bool` == typeof(bool)", text.Hash == sb.Hash);
        }
        // every bare kind round-trips as a plain value
        (Type, object)[] cases =
        {
            (typeof(bool), true), (typeof(byte), (byte)200), (typeof(int), -7),
            (typeof(long), 5L), (typeof(float), 1.5f), (typeof(double), -2.25),
            (typeof(string), "unbounded"), (typeof(Mode), Mode.Fault),
        };
        foreach (var (t, v) in cases)
        {
            using var s = new Schema(t);
            object back = s.Decode(s.Encode(v));
            Check($"round-trip {s.Dsl.Trim()} -> {back}", Equals(back, v));
        }
        using (var s = new Schema(typeof(float[])))
        {
            var back = (float[])s.Decode(s.Encode(new[] { 1.5f, -2.5f }));
            Check("round-trip f32[]", back.Length == 2 && back[0] == 1.5f && back[1] == -2.5f);
        }
        using (var s = new Schema(typeof(Dictionary<string, object>)))
        {
            var back = (Dictionary<string, object>)s.Decode(
                s.Encode(new Dictionary<string, object> { { "battery", 87 } }));
            Check("round-trip map", back.Count == 1 && Convert.ToInt64(back["battery"]) == 87);
        }
        Console.WriteLine(ok ? "bare-type roots: PASS\n" : "bare-type roots: FAIL\n");
        return ok;
    }

    // Two nodes: bare-typed topics and a bare-typed variable over loopback.
    static bool ValueRootsLive()
    {
        Console.WriteLine("bare-root live leg: two nodes, domain 44, loopback");
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }

        var a = new RambleNode("VA", null, e => { if (e.IsError) Console.WriteLine("event(VA): " + e); },
                             domain: 44, multicastInterface: "127.0.0.1");
        var b = new RambleNode("VB", null, e => { if (e.IsError) Console.WriteLine("event(VB): " + e); },
                             domain: 44, multicastInterface: "127.0.0.1");
        try
        {
            var pubFlag = new Topic<bool>(a, "flag", Role.PubOnly, new Qos { Reliability = Reliability.Reliable, KeepLast = 4 });
            var subFlag = new Topic<bool>(b, "flag", Role.SubOnly, new Qos { Reliability = Reliability.Reliable, KeepLast = 4 });
            var pubNote = new Topic<string>(a, "note", Role.PubOnly, new Qos { Reliability = Reliability.Reliable, KeepLast = 4 });
            var subNote = new Topic<string>(b, "note", Role.SubOnly, new Qos { Reliability = Reliability.Reliable, KeepLast = 4 });
            subFlag.TryTake(out bool _);      // switch both to queued delivery
            subNote.TryTake(out string _);
            var vd = new VariableDefinition<double>(a, "gain", 1.25);
            var rv = new RemoteVariable<double>(b, "gain");
            var deadline = DateTime.UtcNow.AddSeconds(8);
            while (DateTime.UtcNow < deadline && (pubFlag.MatchCount() == 0
                   || pubNote.MatchCount() == 0 || !rv.TryGet(out double _)))
            {
                a.Poll(1);
                b.Poll(1);
            }
            Check("bare topics matched", pubFlag.MatchCount() == 1 && pubNote.MatchCount() == 1);
            Check("bare variable replicated the initial",
                  rv.TryGet(out double gain0) && gain0 == 1.25);
            Check("bool send", pubFlag.Send(true) == SendStatus.Ok);
            Check("string send", pubNote.Send("a bare unbounded string") == SendStatus.Ok);
            bool gotFlag = false, gotNote = false;
            deadline = DateTime.UtcNow.AddSeconds(5);
            while (DateTime.UtcNow < deadline && !(gotFlag && gotNote))
            {
                a.Poll(1);
                b.Poll(1);
                if (subFlag.TryTake(out bool f, 1) && f) gotFlag = true;
                if (subNote.TryTake(out string s, 1) && s == "a bare unbounded string") gotNote = true;
            }
            Check("bool taken as a plain value", gotFlag);
            Check("string taken as a plain value", gotNote);
            Check("bare variable set accepted", rv.Set(2.5) == SendStatus.Ok);
            deadline = DateTime.UtcNow.AddSeconds(5);
            while (DateTime.UtcNow < deadline && !(vd.TryGet(out double g) && g == 2.5))
            {
                a.Poll(1);
                b.Poll(1);
            }
            Check("bare variable set converged", vd.TryGet(out double gain1) && gain1 == 2.5);
        }
        finally
        {
            a.Close();
            b.Close();
        }
        Console.WriteLine(ok ? "bare-root live leg: PASS\n" : "bare-root live leg: FAIL\n");
        return ok;
    }

    // The callback dispatcher: with one set, every event, handler, observer and awaited
    // result must run on the thread that drains it, never on a service thread.
    static bool DispatcherLeg()
    {
        Console.WriteLine("dispatcher leg: two nodes, domain 48, loopback");
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }

        var work = new System.Collections.Concurrent.ConcurrentQueue<Action>();
        int drainId = Thread.CurrentThread.ManagedThreadId;
        void Drain() { Action a; while (work.TryDequeue(out a)) a(); }

        int evtId = 0, fnId = 0, changeId = 0, doneId = 0, sum = 0, level = 0;

        var srv = new RambleNode("dsrv", null,
            e => { if (evtId == 0) evtId = Thread.CurrentThread.ManagedThreadId; },
            domain: 48, multicastInterface: "127.0.0.1", maxTopics: 32);
        var cli = new RambleNode("dcli", null, e => { },
            domain: 48, multicastInterface: "127.0.0.1", maxTopics: 32);
        srv.CallbackDispatcher = a => work.Enqueue(a);
        cli.CallbackDispatcher = a => work.Enqueue(a);

        var add = new FunctionDefinition<AddReq, AddRsp>(srv, "dadd", q =>
        {
            fnId = Thread.CurrentThread.ManagedThreadId;
            return new AddRsp { Sum = q.A + q.B };
        });
        var lvlDef = new VariableDefinition<Level>(srv, "dlevel", new Level { Value = 1 });

        srv.Start();          // both wires owned by service threads: nothing polls on this one
        cli.Start();

        var call = new RemoteFunction<AddReq, AddRsp>(cli, "dadd");
        var rvar = new RemoteVariable<Level>(cli, "dlevel");
        rvar.OnChange((v, u) =>
        {
            changeId = Thread.CurrentThread.ManagedThreadId;
            level = v.Value;
        });

        var deadline = DateTime.UtcNow.AddSeconds(8);
        while (DateTime.UtcNow < deadline && !(call.HasDefinition && rvar.RemoteCount > 0))
        {
            Drain();
            Thread.Sleep(5);
        }
        Check("definitions matched", call.HasDefinition && rvar.RemoteCount > 0);

        // the handler runs a frame later, through the parked reply, and still answers
        var t = call.CallAsync(new AddReq { A = 2, B = 3 });
        t.ContinueWith(x =>
        {
            doneId = Thread.CurrentThread.ManagedThreadId;
            sum = x.Result.Ok ? x.Result.Value.Sum : -1;
        }, TaskContinuationOptions.ExecuteSynchronously);
        deadline = DateTime.UtcNow.AddSeconds(8);
        while (DateTime.UtcNow < deadline && doneId == 0) { Drain(); Thread.Sleep(5); }
        Check("deferred handler answered the call", sum == 5);
        Check("handler ran on the drain thread", fnId == drainId);
        Check("awaited result resumed on the drain thread", doneId == drainId);

        lvlDef.Set(new Level { Value = 9 });
        deadline = DateTime.UtcNow.AddSeconds(8);
        while (DateTime.UtcNow < deadline && level != 9) { Drain(); Thread.Sleep(5); }
        Check("variable change arrived", level == 9);
        Check("observer ran on the drain thread", changeId == drainId);

        // several scripts share one variable: every observer fires, a late one is replayed
        // the current value, and dropping one leaves the rest alone
        int a2 = 0, b2 = 0;
        var subA = rvar.OnChange(v => a2 = v.Value);
        var subB = rvar.OnChange(v => b2 = v.Value);
        Check("late observers replayed the current value", a2 == 9 && b2 == 9);

        lvlDef.Set(new Level { Value = 11 });
        deadline = DateTime.UtcNow.AddSeconds(8);
        while (DateTime.UtcNow < deadline && !(a2 == 11 && b2 == 11 && level == 11))
        {
            Drain();
            Thread.Sleep(5);
        }
        Check("every observer fired", a2 == 11 && b2 == 11 && level == 11);

        subA.Dispose();
        lvlDef.Set(new Level { Value = 12 });
        deadline = DateTime.UtcNow.AddSeconds(8);
        while (DateTime.UtcNow < deadline && b2 != 12) { Drain(); Thread.Sleep(5); }
        Check("a disposed observer stops, the others go on", a2 == 11 && b2 == 12 && level == 12);

        deadline = DateTime.UtcNow.AddSeconds(8);
        while (DateTime.UtcNow < deadline && evtId == 0) { Drain(); Thread.Sleep(5); }
        Check("events ran on the drain thread", evtId == drainId);

        // nothing may have slipped onto a service thread
        Check("no callback ran off the drain thread",
              evtId == drainId && fnId == drainId && changeId == drainId && doneId == drainId);

        // a handle that outlives its node must refuse, never read the freed arena
        var staleTopic = new Topic<Level>(cli, "dstale", Role.PubOnly);
        bool matchedBefore = call.HasDefinition;
        srv.Close();
        cli.Close();
        Level got;
        Check("stale variable set refuses", rvar.Set(new Level { Value = 1 }) == SendStatus.NoTopic);
        Check("stale variable read is empty", !rvar.TryGet(out got));
        Check("stale topic send refuses", staleTopic.Send(new Level { Value = 1 }) == SendStatus.NoTopic);
        Check("stale remote function is unmatched", matchedBefore && !call.HasDefinition);

        Console.WriteLine(ok ? "dispatcher: PASS\n" : "dispatcher: FAIL\n");
        return ok;
    }

    // reflect_from_mesh: a handle carrying no type of its own takes the provider's from the
    // mesh, which is what an observer tool or a generic HMI needs.
    static bool ReflectLeg()
    {
        Console.WriteLine("reflect leg: two nodes, domain 49, loopback");
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }

        var a = new RambleNode("rsrv", null, e => { },
            domain: 49, multicastInterface: "127.0.0.1", maxTopics: 32);
        var b = new RambleNode("rcli", null, e => { },
            domain: 49, multicastInterface: "127.0.0.1", maxTopics: 32, fetchDetails: true);

        var pub = new Topic<Level>(a, "reflected", Role.PubOnly,
                                   new Qos { Reliability = Reliability.Reliable, KeepLast = 4 });
        a.Start();
        b.Start();
        pub.Send(new Level { Value = 3 });

        // nobody provides this one, so there is nothing to copy and it stays untyped
        var plain = new Topic(b, "unprovided", Role.SubOnly, new Qos { ReflectFromMesh = true });
        Check("an unprovided reflect topic stays untyped",
              Native.ramble_topic_schema(plain._handle) == IntPtr.Zero);

        var reflect = new Topic(b, "reflected", Role.SubOnly, new Qos { ReflectFromMesh = true });
        var deadline = DateTime.UtcNow.AddSeconds(10);
        while (DateTime.UtcNow < deadline && Native.ramble_topic_schema(reflect._handle) == IntPtr.Zero)
        {
            reflect.Refresh();          // never automatic: the app picks the moment
            Thread.Sleep(20);
        }
        Check("a reflect topic took the provider's schema",
              Native.ramble_topic_schema(reflect._handle) != IntPtr.Zero);
        Check("refresh on a settled handle reports no change", !reflect.Refresh());

        a.Close();
        b.Close();
        Console.WriteLine(ok ? "reflect: PASS\n" : "reflect: FAIL\n");
        return ok;
    }

    static int Main()
    {
        if (!RoundTrip()) return 1;
        if (!ValueRoots()) return 1;
        if (!StdTypes()) return 1;
        Console.WriteLine("opening nodes...");

        var sub = new RambleNode("sub",
            onMessage: m =>
            {
                Received = m;
                Console.WriteLine($"recv: [{m.TopicName}] from {m.PublisherName} -> {m.As<Pose>()}");
                Got.Set();
            },
            onEvent: e => Console.WriteLine("event(sub): " + e),
            domain: 42, multicastInterface: "127.0.0.1");

        var pub = new RambleNode("pub", null, e => Console.WriteLine("event(pub): " + e),
            domain: 42, multicastInterface: "127.0.0.1");

        new Topic<Pose>(sub, "pose", Role.SubOnly, new Qos { Reliability = Reliability.Reliable, KeepLast = 8 });
        var pubch = new Topic<Pose>(pub, "pose", Role.PubOnly, new Qos { Reliability = Reliability.Reliable, KeepLast = 8 });

        var sent = new Pose
        {
            Stamp = 7,
            X = 1.5,
            Y = -2.5,
            Uuid = new byte[] { 1, 2, 3, 4 },
            Frame = "map",
            Tags = new[] { "fast", "ok" },
            Vel = new Twist { Dx = 0.5f, Dy = 0.25f },
        };

        // Single-threaded: drive both nodes by polling them in the loop (no start()).
        long captured = Std.Now() - 5000;   // "true" 5 ms before the send, to read back
        var deadline = DateTime.UtcNow.AddSeconds(8);
        while (DateTime.UtcNow < deadline && !Got.IsSet)
        {
            pubch.Send(sent, captured);
            pub.Poll(1);
            sub.Poll(1);
        }

        bool ok = Got.IsSet;
        if (ok)
        {
            var r = Received.As<Pose>();
            ok = r.Stamp == 7 && Math.Abs(r.X - 1.5) < 1e-9 && Math.Abs(r.Y + 2.5) < 1e-9
                 && r.Uuid != null && r.Uuid.Length == 4 && r.Uuid[0] == 1 && r.Uuid[3] == 4
                 && r.Frame == "map"
                 && r.Tags != null && r.Tags.Length == 2 && r.Tags[0] == "fast" && r.Tags[1] == "ok"
                 && Math.Abs(r.Vel.Dx - 0.5) < 1e-6 && Math.Abs(r.Vel.Dy - 0.25) < 1e-6;
            if (!ok) Console.WriteLine("FAIL: decoded value mismatch");
            // The three stamps: the sender's commit, its stated capture time, our receipt.
            if (ok && (Received.CaptureUs != (ulong)captured
                       || Received.WrittenUs < Received.CaptureUs || Received.RecvUs == 0))
            {
                ok = false;
                Console.WriteLine($"FAIL: stamps capture={Received.CaptureUs} (want {captured})"
                                  + $" written={Received.WrittenUs} recv={Received.RecvUs}");
            }
            if (ok) Console.WriteLine("PASS");
            using (var s = new Schema(typeof(Pose)))
                Console.WriteLine("Pose DSL (for C interop):\n" + s.Dsl);
        }
        else
        {
            Console.WriteLine("FAIL: no message delivered within timeout");
        }

        // An over-cap string must throw, never silently truncate.
        if (ok)
        {
            try
            {
                pubch.Send(new Pose { Frame = "way-too-long-for-sixteen-bytes" });
                Console.WriteLine("FAIL: over-cap string did not throw");
                ok = false;
            }
            catch (SchemaException) { Console.WriteLine("PASS: over-cap string refused"); }
        }

        // threaded: both nodes on their service threads, sent from this thread, delivered with
        // no Poll() anywhere
        if (ok)
        {
            if (!pub.Start() || !sub.Start()) { Console.WriteLine("FAIL: Start"); ok = false; }
            else if (pub.Poll(0) != (int)SendStatus.State) { Console.WriteLine("FAIL: Poll not refused while started"); ok = false; }
            else
            {
                Got.Reset();
                sent.Stamp = 8;
                pubch.Send(sent);
                ok = Got.Wait(3000) && Received.As<Pose>().Stamp == 8;
                Console.WriteLine(ok ? $"PASS: threaded delivery via Start() (evictedUnsent={pub.EvictedUnsent})"
                                     : "FAIL: threaded delivery");
                pub.Stop();
                sub.Stop();
            }
        }

        pub.Close();
        sub.Close();

        if (ok) ok = ValueRootsLive();
        if (ok) ok = VideoLive();
        if (ok) ok = Patterns();
        if (ok) ok = Tasks();
        if (ok) ok = DispatcherLeg();
        if (ok) ok = ReflectLeg();
        Console.WriteLine(ok ? "ALL PASS" : "FAIL");
        return ok ? 0 : 1;
    }
}
