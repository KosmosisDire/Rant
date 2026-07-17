// DART C# test: two nodes, reliable typed pub/sub, on one host, plus a patterns
// leg (functions / variables / signals). Exercises discovery/match, schema
// reflection (capped strings, string arrays, maps), the consumer surface, and the
// typed pattern handles. Exit 0 = all legs passed.
//
//   dotnet run --project csharp/test

using System;
using System.Collections.Generic;
using System.Threading;
using Dart;

struct Twist { public float Dx; public float Dy; }

// A schema exercising the v4 variable-length kinds: a variable string, a variable
// scalar array, a variable string array, and a self-describing map.
[DartSchema("Sensor")]
struct Sensor
{
    public uint Id;
    [DartString(16)] public string Name;   // capped string (fixed)
    public string Note;                     // variable string
    public float[] Samples;                 // variable scalar array
    [DartString(8)] public string[] Labels; // variable string array
    public Dictionary<string, object> Extras;  // self-describing map
}

struct Pose
{
    public ulong Stamp;
    public double X;
    public double Y;
    [DartArray(4)] public byte[] Uuid;
    [DartString(16)] public string Frame;
    [DartArray(2), DartString(8)] public string[] Tags;
    public Twist Vel;
}

// patterns leg types
struct AddReq { public int A; public int B; }
struct AddRsp { public int Sum; }
struct Level { public int Value; }

static class Program
{
    static readonly ManualResetEventSlim Got = new ManualResetEventSlim(false);
    static DartMessage Received;

    // Encode -> decode round-trip of the v4 variable kinds (no networking).
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

        // decode-to-dict (the reflection path bridges/observers use) must surface the
        // new kinds too: variable string -> string, variable array -> typed array, map -> dict
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

    // Functions / variables / signals between two nodes on an isolated domain.
    static bool Patterns()
    {
        Console.WriteLine("patterns leg: two nodes, domain 43, loopback");
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }

        var srv = new DartNode("srv", null, e => Console.WriteLine("event(srv): " + e),
                           domain: 43, multicastInterface: "127.0.0.1", maxTopics: 32);
        var cli = new DartNode("cli", null, e => Console.WriteLine("event(cli): " + e),
                           domain: 43, multicastInterface: "127.0.0.1", maxTopics: 32);

        // definitions on srv: simple form (return = reply), a thrower (-> AppError),
        // and a full form that defers off-thread
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
        int sigCount = 0;
        var sigIn = new Signal(srv, "estop", null, m => Interlocked.Increment(ref sigCount));
        var lvlDef = new VariableDefinition<Level>(srv, "level", new Level { Value = 5 },
                                                   allowForce: true);

        srv.Start();   // service thread owns srv's loop; handlers fire on it

        // remotes on cli (manual poll: blocking calls drive cli's loop themselves)
        var addR = new RemoteFunction<AddReq, AddRsp>(cli, "add");
        var boomR = new RemoteFunction<AddReq, AddRsp>(cli, "boom");
        var lateR = new RemoteFunction<AddReq, AddRsp>(cli, "late");
        var sigOut = new Signal(cli, "estop");
        var lvl = new RemoteVariable<Level>(cli, "level");

        var deadline = DateTime.UtcNow.AddSeconds(8);
        while (DateTime.UtcNow < deadline
               && !(addR.HasDefinition && boomR.HasDefinition && lateR.HasDefinition
                    && sigOut.ListenerCount > 0))
            cli.Poll(5);
        Check("definitions discovered", addR.HasDefinition && boomR.HasDefinition && lateR.HasDefinition);
        Check("signal listener matched", sigOut.ListenerCount == 1);

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

        // force overrides with a shadow source; unforce restores the latest set
        Check("force", lvlDef.Force(new Level { Value = 99 }) == SendStatus.Ok);
        deadline = DateTime.UtcNow.AddSeconds(5);
        while (DateTime.UtcNow < deadline && !(lvl.TryGet(out lv) && lv.Value == 99)) cli.Poll(5);
        Check("forced value visible remotely", lvl.Value.Value == 99);
        Check("remote sees Forced", lvl.Forced);
        Check("unforce", lvlDef.Unforce() == SendStatus.Ok);
        deadline = DateTime.UtcNow.AddSeconds(5);
        while (DateTime.UtcNow < deadline && !(lvl.TryGet(out lv) && lv.Value == 9)) cli.Poll(5);
        Check("unforce restores the latest set", lvl.Value.Value == 9 && !lvl.Forced);

        // signal: three payload-less emits (untyped form), delivered on srv's thread
        Check("emit accepted", sigOut.Emit() == SendStatus.Ok);
        sigOut.Emit();
        sigOut.Emit();
        deadline = DateTime.UtcNow.AddSeconds(5);
        while (DateTime.UtcNow < deadline && Volatile.Read(ref sigCount) < 3) cli.Poll(5);
        Check("three signals delivered", Volatile.Read(ref sigCount) == 3);

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

    static int Main()
    {
        if (!RoundTrip()) return 1;
        Console.WriteLine("opening nodes...");

        var sub = new DartNode("sub",
            onMessage: m =>
            {
                Received = m;
                Console.WriteLine($"recv: [{m.TopicName}] from {m.PublisherName} -> {m.As<Pose>()}");
                Got.Set();
            },
            onEvent: e => Console.WriteLine("event(sub): " + e),
            domain: 42, multicastInterface: "127.0.0.1");

        var pub = new DartNode("pub", null, e => Console.WriteLine("event(pub): " + e),
            domain: 42, multicastInterface: "127.0.0.1");

        new Topic<Pose>(sub, "pose", Role.SubOnly, reliable: true, keepLast: 8);
        var pubch = new Topic<Pose>(pub, "pose", Role.PubOnly, reliable: true, keepLast: 8);

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
        var deadline = DateTime.UtcNow.AddSeconds(8);
        while (DateTime.UtcNow < deadline && !Got.IsSet)
        {
            pubch.Send(sent);
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
            Console.WriteLine(ok ? "PASS" : "FAIL: decoded value mismatch");
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

        // Threaded: both nodes on their C-level service threads; send from this thread,
        // delivery arrives with no Poll() anywhere.
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

        if (ok) ok = Patterns();
        Console.WriteLine(ok ? "ALL PASS" : "FAIL");
        return ok ? 0 : 1;
    }
}
