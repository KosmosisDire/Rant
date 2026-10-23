// The C# binding test: two nodes on one host over reliable typed pub sub, the consumer
// surface and the pattern legs. No arguments runs every leg, a name runs one, --list lists
// them. Exit 0 = pass (spec/testing.md).

using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using Rant;

// A nested type of the test's own. Twist would clash with the standard one and be refused.
struct Velocity { public float Dx; public float Dy; }

// A schema exercising the v4 variable-length kinds: a variable string, a variable
// scalar array, a variable string array, and a self-describing map.
[RantSchema("Sensor")]
struct Sensor
{
    public uint Id;
    [RantString(16)] public string Name;     // capped string (fixed)
    public string Note;                       // variable string
    public float[] Samples;                   // variable scalar array
    [RantString(8)] public string[] Labels; // variable string array
    public Dictionary<string, object> Extras;  // self-describing map
}

struct Reading
{
    public ulong Stamp;
    public double X;
    public double Y;
    [RantArray(4)] public byte[] Uuid;
    [RantString(16)] public string Frame;
    [RantArray(2), RantString(8)] public string[] Tags;
    public Velocity Vel;
}

// struct arrays: a nested type defined once and used by name, fixed, variable and standard
struct Corner { public float X; public float Y; }
struct Shape
{
    public Corner[] Corners;
    [RantArray(2)] public Corner[] Bounds;
    public Rant.Types.Float3[] Normals;
    public Corner Origin;
}
// a fixed struct array inside a struct array's element
struct Outline { [RantArray(4)] public Rant.Types.Float2[] Corners; public byte N; }

// properties and records reflect like fields, in the order written
class PoseClass
{
    public double X { get; set; }
    public double Y { get; init; }
    [RantString(16)] public string Frame { get; set; }
    public double Length => Math.Sqrt(X * X + Y * Y);   // computed, not sent
}
record Rate(double Vx, double Vy, [property: RantField("w")] double Omega);
record struct Point2(float X, float Y);
struct Sample { public uint Id; public float Value { get; set; } public ulong Stamp; }
class GetOnly { public int Id { get; } }

// member names in C# style, snake_case on the wire
struct Waypoint
{
    [RantString(16)] public string FrameId;
    public float MaxSpeedMps;
    public bool GpsFix;
}

// patterns leg types
struct AddReq { public int A; public int B; }
struct AddRsp { public int Sum; }
struct Level { public int Value; }

// rebind leg types: one wire name in three layouts, each writer puts the reader's fields elsewhere
[RantSchema("Probe")] record struct ProbeRead(uint A, uint B);
[RantSchema("Probe")] struct ProbeFirst { public uint B; public uint A; }
[RantSchema("Probe")] struct ProbeSecond { public uint A; public uint X; public uint B; }

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

    // ctest hands each program a block of 64 domains through RANT_TEST_DOMAIN, a run by hand
    // takes one from the clock, so parallel runs never meet
    static readonly int DomainBase =
        int.TryParse(Environment.GetEnvironmentVariable("RANT_TEST_DOMAIN"), out int d) && d > 0
            ? d : 1000 + (int)(Environment.TickCount64 % 900) * 64;

    // A node on the loopback interface of the domain at offset in this run's block, on its
    // service thread unless the leg polls it by hand. Loopback only: the firewall never asks.
    static NodeOptions Local(int offset, Threading threading = Threading.ServiceThread,
                             bool fetchDetails = false)
        => new NodeOptions
        {
            Domain = (ushort)(DomainBase + offset), MulticastInterface = "127.0.0.1", MaxTopics = 32,
            Threading = threading, FetchDetails = fetchDetails,
        };
    static RantMessage Received;
    static Reading ReceivedPose;

    // Encode and decode round trip of the variable kinds, no networking.
    static bool RoundTrip()
    {
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }

        using var node = new RantNode("rt", Local(9, Threading.Manual));
        var s = node.Schema(typeof(Sensor));
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

        // a text schema of the same shape decodes to a dictionary, the form bridges and
        // observers use, and it must surface the variable kinds too
        var text = node.Schema(s.Dsl);
        var fd = (Dictionary<string, object>)text.Decode(raw);
        Check("fields note", (string)fd["note"] == src.Note);
        Check("fields samples", fd["samples"] is float[] fs && fs.Length == 3);
        Check("fields labels", fd["labels"] is string[] fl && fl.Length == 3 && fl[2] == "rearmost");
        Check("fields map", fd["extras"] is Dictionary<string, object> fe && Convert.ToInt64(fe["battery"]) == 87);

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

        // nested types are definitions: written once, used by name, and as array elements
        {
            var sh = node.Schema(typeof(Shape));
            string dsl = sh.Dsl;
            Console.WriteLine("Shape DSL:\n" + dsl);
            Check("a nested type is defined once and used by name",
                  dsl.IndexOf("Corner {") >= 0 && dsl.IndexOf("Corner {") == dsl.LastIndexOf("Corner {")
                  && dsl.Contains("corners: Corner[]") && dsl.Contains("bounds: Corner[2]")
                  && dsl.Contains("normals: Float3[]") && dsl.Contains("origin: Corner"));
            var same = node.Schema("Corner { X: f32, Y: f32 }\n"
                                 + "Shape { Corners: Corner[], Bounds: Corner[2], Normals: Float3[], Origin: Corner }");
            Check("the reflected text is the spelled out text", same.Handle == sh.Handle);
            var shape = new Shape
            {
                Corners = new[] { new Corner { X = 1, Y = 2 }, new Corner { X = 3, Y = 4 }, new Corner { X = 5, Y = 6 } },
                Bounds = new[] { new Corner { X = -1, Y = -2 }, new Corner { X = 7, Y = 8 } },
                Normals = new[] { new Rant.Types.Float3 { X = 0, Y = 0, Z = 1 } },
                Origin = new Corner { X = 9, Y = 10 },
            };
            var back = (Shape)sh.Decode(sh.Encode(shape));
            Check("a variable struct array round trips", back.Corners != null && back.Corners.Length == 3
                  && back.Corners[0].X == 1 && back.Corners[2].Y == 6);
            Check("a fixed struct array round trips", back.Bounds != null && back.Bounds.Length == 2
                  && back.Bounds[0].Y == -2 && back.Bounds[1].X == 7);
            Check("a standard type array round trips", back.Normals != null && back.Normals.Length == 1
                  && back.Normals[0].Z == 1);
            Check("the nested member beside them", back.Origin.X == 9 && back.Origin.Y == 10);
            var asDict = (Dictionary<string, object>)same.Decode(sh.Encode(shape));
            Check("a struct array decodes as a list of dictionaries",
                  asDict["corners"] is List<Dictionary<string, object>> cl && cl.Count == 3
                  && Convert.ToDouble(cl[1]["x"]) == 3 && asDict["origin"] is Dictionary<string, object>);
            var fromDict = (Shape)sh.Decode(sh.Encode(new Dictionary<string, object>
            {
                { "Corners", new List<object> { new Dictionary<string, object> { { "X", 11 }, { "Y", 12 } } } },
                { "Bounds", new List<object> { new Dictionary<string, object> { { "X", 13 } } } },
            }));
            Check("a struct array encodes from a list of dictionaries",
                  fromDict.Corners.Length == 1 && fromDict.Corners[0].Y == 12 && fromDict.Bounds.Length == 2
                  && fromDict.Bounds[0].X == 13 && fromDict.Bounds[1].X == 0);
            var empty = (Shape)sh.Decode(sh.Encode(new Shape()));
            Check("an unset struct array is empty", empty.Corners != null && empty.Corners.Length == 0
                  && empty.Bounds != null && empty.Bounds.Length == 2);
        }

        // a fixed struct array nests inside a struct array's element, one index per level
        {
            var os = node.Schema(typeof(Outline[]));
            var outlines = new[]
            {
                new Outline { Corners = new[] { new Rant.Types.Float2 { X = 1, Y = 2 } }, N = 1 },
                new Outline { Corners = new[] { new Rant.Types.Float2 { X = 3, Y = 4 },
                                                new Rant.Types.Float2 { X = 5, Y = 6 } }, N = 2 },
            };
            var ob = (Outline[])os.Decode(os.Encode(outlines));
            Check("a nested struct array round trips typed", ob.Length == 2 && ob[0].Corners.Length == 4
                  && ob[0].Corners[0].Y == 2 && ob[1].Corners[1].X == 5 && ob[1].Corners[3].X == 0
                  && ob[1].N == 2);
            var inner = Array.Find(os.Fields, f => f.Name == "x");
            Check("the inner member takes two indices", inner != null && inner.ArrayDepth == 2);
            var ol = node.Schema("Outline[]").Decode(os.Encode(outlines)) as List<Dictionary<string, object>>;
            Check("a nested struct array decodes as nested lists of dictionaries",
                  ol != null && ol[1]["corners"] is List<Dictionary<string, object>> cl2
                  && Convert.ToDouble(cl2[1]["y"]) == 6);
            var fd2 = (Outline[])os.Decode(os.Encode(new List<object>
            {
                new Dictionary<string, object> { { "Corners", new List<object>
                    { new Dictionary<string, object> { { "X", 7 }, { "Y", 8 } } } }, { "N", 3 } },
            }));
            Check("nested lists of dictionaries encode a nested struct array",
                  fd2.Length == 1 && fd2[0].Corners[0].Y == 8 && fd2[0].N == 3);
        }

        // properties and records reflect like fields
        {
            var ps = node.Schema(typeof(PoseClass));
            Check("properties reflect, a computed one does not",
                  ps.Dsl.Contains("x: f64") && ps.Dsl.Contains("frame: string<16>") && !ps.Dsl.Contains("length"));
            var pb = (PoseClass)ps.Decode(ps.Encode(new PoseClass { X = 3, Y = 4, Frame = "map" }));
            Check("a class with set and init round trips", pb.X == 3 && pb.Y == 4 && pb.Frame == "map" && pb.Length == 5);
            var rs = node.Schema(typeof(Rate));
            Check("a record spells its parameters", rs.Dsl.Contains("vx: f64") && rs.Dsl.Contains("w: f64"));
            var rb = (Rate)rs.Decode(rs.Encode(new Rate(1, 2, 3)));
            Check("a positional record decodes through its constructor", rb == new Rate(1, 2, 3));
            var ss = node.Schema(typeof(Sample));
            Check("fields and properties keep the written order",
                  ss.Dsl.IndexOf("id:") < ss.Dsl.IndexOf("value:") && ss.Dsl.IndexOf("value:") < ss.Dsl.IndexOf("stamp:"));
            var sb = (Sample)ss.Decode(ss.Encode(new Sample { Id = 1, Value = 2.5f, Stamp = 3 }));
            Check("a mixed struct round trips", sb.Id == 1 && sb.Value == 2.5f && sb.Stamp == 3);
            var cs = node.Schema(typeof(Point2));
            var cb = (Point2)cs.Decode(cs.Encode(new Point2(1, 2)));
            Check("a record struct round trips", cb.X == 1 && cb.Y == 2);
            try { node.Schema(typeof(GetOnly)); Check("a getter only member nobody fills is refused", false); }
            catch (SchemaException ex) { Check("a getter only member nobody fills is refused: " + ex.Message, true); }
            var anon = ps.Encode(new { X = 7.0, Y = 8.0, Frame = "odom" });
            Check("an anonymous object encodes by name", ((PoseClass)ps.Decode(anon)).Frame == "odom");
        }

        // members spell snake_case on the wire, whatever the C# style, and meet any text
        {
            var ws = node.Schema(typeof(Waypoint));
            Check("PascalCase members spell snake_case on the wire",
                  ws.Dsl.Contains("frame_id: string<16>") && ws.Dsl.Contains("max_speed_mps: f32") && ws.Dsl.Contains("gps_fix: bool"));
            Check("a text in another spelling is the same handle",
                  node.Schema("Waypoint { frameId: string<16>, MaxSpeedMps: f32, GPSFix: bool }").Handle == ws.Handle);
            var wb = (Waypoint)ws.Decode(ws.Encode(new Waypoint { FrameId = "map", MaxSpeedMps = 1.5f, GpsFix = true }));
            Check("and decode back into the members", wb.FrameId == "map" && wb.MaxSpeedMps == 1.5f && wb.GpsFix);
            var wd = (Dictionary<string, object>)node.Schema(ws.Dsl).Decode(ws.Encode(new Waypoint { FrameId = "odom" }));
            Check("a dictionary carries the wire names", (string)wd["frame_id"] == "odom");
            var fromAny = (Waypoint)ws.Decode(ws.Encode(new Dictionary<string, object> { { "frameId", "x" }, { "MaxSpeedMps", 2.0f } }));
            Check("a dictionary source may use any spelling", fromAny.FrameId == "x" && fromAny.MaxSpeedMps == 2.0f);
        }

        // a tuple has no name of its own: only a handle can name it
        try { node.Schema(typeof((double, double))); Check("nameless tuple refused", false); }
        catch (SchemaException) { Check("nameless tuple refused", true); }

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
        Console.WriteLine("patterns leg: two nodes, loopback");
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }

        // srv runs on its service thread, so handlers fire there. cli is polled by hand, so
        // its blocking calls drive its loop themselves.
        var srv = new RantNode("srv", Local(1));
        srv.OnEvent += e => Console.WriteLine("event(srv): " + e);
        var cli = new RantNode("cli", Local(1, Threading.Manual));
        cli.OnEvent += e => Console.WriteLine("event(cli): " + e);

        // definitions on srv: the simple form, a thrower giving AppError, and a full form that
        // defers off thread
        var add = srv.FunctionDefinition<AddReq, AddRsp>("add", q => new AddRsp { Sum = q.A + q.B });
        // a tuple type is named after the handle, so both ends of a C# pair match
        var add2 = srv.FunctionDefinition<(double, double), double>("add2", v => v.Item1 + v.Item2);
        var boom = srv.FunctionDefinition<AddReq, AddRsp>("boom",
            (Func<AddReq, AddRsp>)(q => throw new Exception("kaboom")));
        var late = srv.FunctionDefinition<AddReq, AddRsp>("late", (q, req) =>
        {
            var d = req.Defer();
            ThreadPool.QueueUserWorkItem(_ =>
            {
                Thread.Sleep(50);
                d.Complete(new AddRsp { Sum = q.A + q.B });
            });
        });
        var lvlDef = srv.VariableDefinition<Level>("level", new Level { Value = 5 },
                                                   new VariableOptions { AllowForce = true });

        var addR = cli.RemoteFunction<AddReq, AddRsp>("add");
        var add2R = cli.RemoteFunction<(double, double), double>("add2");
        var boomR = cli.RemoteFunction<AddReq, AddRsp>("boom");
        var lateR = cli.RemoteFunction<AddReq, AddRsp>("late");
        var lvl = cli.RemoteVariable<Level>("level");

        var deadline = DateTime.UtcNow.AddSeconds(8);
        while (DateTime.UtcNow < deadline
               && !(addR.MatchCount > 0 && boomR.MatchCount > 0 && lateR.MatchCount > 0 && add2R.MatchCount > 0))
            cli.Poll(5);
        Check("definitions discovered", addR.MatchCount > 0 && boomR.MatchCount > 0 && lateR.MatchCount > 0
              && add2R.MatchCount > 0);
        Check("tuple request and response", add2R.Call((2.5, 4.0), 3000) == 6.5);
        Check("a handle names a tuple in PascalCase", Codec.WireName("motor/speed_2d") == "MotorSpeed2d"
              && Codec.WireName("add2_req") == "Add2Req");

        // blocking calls
        Check("blocking call returns the value", addR.Call(new AddReq { A = 2, B = 3 }, 3000).Sum == 5);
        var r = addR.TryCall(new AddReq { A = 2, B = 3 }, 3000);
        Check("TryCall Ok", r.Ok && r.Status == CallStatus.Ok && r.Value.Sum == 5);
        Check("provider set", r.Ok && r.Provider != 0);

        // a failed call is loud: Call throws with the handler's text, TryCall answers a value
        CallException boomEx = null;
        try { boomR.Call(new AddReq { A = 1, B = 1 }, 3000); } catch (CallException e) { boomEx = e; }
        Check("thrown handler -> Call throws AppError", boomEx != null && boomEx.Status == CallStatus.AppError
              && boomEx.Provider != 0);
        Check("the exception names the call and carries the handler's text",
              boomEx != null && boomEx.Message.Contains("'boom'") && boomEx.Message.Contains("kaboom"));
        Check("CallException is a RantException", boomEx is RantException);
        RantException dup = null;
        try { cli.RemoteFunction<AddReq, AddRsp>("boom"); } catch (RantException e) { dup = e; }
        Check("a failed create throws RantException with the C reason",
              dup != null && dup.Error != null && dup.Error.Error == ErrorKind.NameCollision);
        Check("and LastError reads the same reason",
              cli.LastError != null && cli.LastError.Error == ErrorKind.NameCollision);
        // a code near the end of the enum: any drift from the C enum shows as another name
        RantException badName = null;
        try { cli.Publisher<Level>("bad@name"); } catch (RantException e) { badName = e; }
        Check("a late error code keeps its name",
              badName != null && badName.Error != null && badName.Error.Error == ErrorKind.BadName);
        var rb = boomR.TryCall(new AddReq { A = 1, B = 1 }, 3000);
        Check("TryCall answers AppError as a value", rb.Status == CallStatus.AppError);
        bool threw = false;
        try { var _ = rb.Value; } catch (CallException) { threw = true; }
        Check("Value on !Ok throws CallException", threw);
        var boomT = boomR.CallAsync(new AddReq { A = 1, B = 1 });
        deadline = DateTime.UtcNow.AddSeconds(3);
        while (DateTime.UtcNow < deadline && !boomT.IsCompleted) cli.Poll(5);
        Check("CallAsync faults with CallException", boomT.IsFaulted
              && boomT.Exception.InnerException is CallException);

        Check("deferred completion", lateR.Call(new AddReq { A = 20, B = 22 }, 3000).Sum == 42);
        Check("caller count seen by definition", add.MatchCount == 1);

        // variable: catch_up hands the remote the initial value
        Check("variable wait", lvl.Wait(3000));
        Level lv;
        Check("initial value", lvl.TryGet(out lv) && lv.Value == 5);
        Check("owner matched", lvl.MatchCount > 0);
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
        lvlDef.OnChange += (v, u) => { chg.Add(v.Value); chgSource = u.Source; };
        Check("OnChange replays current at registration", chg.Count == 1 && chg[0] == 9);
        lvlDef.OnWrite += (v, u) => Interlocked.Increment(ref wr);
        Check("OnWrite does not replay", wr == 0);
        Check("identical re-set accepted", lvlDef.Set(new Level { Value = 9 }) == SendStatus.Ok);
        Check("identical re-set is a write, not a change", chg.Count == 1 && wr == 1);
        Check("new set accepted", lvlDef.Set(new Level { Value = 12 }) == SendStatus.Ok);
        Check("change fires inline with the new value",
              chg.Count == 2 && chg[1] == 12 && chgSource == 0 && wr == 2);
        var rchg = new List<long>();
        lvl.OnChange += (v, u) => { lock (rchg) rchg.Add(v.Value); };
        lock (rchg) Check("remote OnChange replays the cache", rchg.Count == 1 && rchg[0] == 9);
        deadline = DateTime.UtcNow.AddSeconds(5);
        while (DateTime.UtcNow < deadline)
        {
            lock (rchg) if (rchg.Count > 0 && rchg[rchg.Count - 1] == 12) break;
            cli.Poll(5);
        }
        lock (rchg) Check("remote change arrives", rchg.Count > 0 && rchg[rchg.Count - 1] == 12);
        // a call still pending at Close settles its Task with Cancelled, never hangs. A byte[]
        // typed remote with no schema is the raw form.
        var never = cli.RemoteFunction<byte[], byte[]>("never-served",
                                                       new FunctionOptions { TimeoutUs = 60000000 });
        var tc = never.TryCallAsync(null);
        srv.Close();
        cli.Close();
        Check("pending TryCallAsync settles Cancelled at Close",
              tc.Wait(2000) && tc.Result.Status == CallStatus.Cancelled);
        Console.WriteLine(ok ? "patterns: PASS\n" : "patterns: FAIL\n");
        return ok;
    }

    // Tasks between two nodes: async handlers, typed + raw progress, CancellationToken cancel,
    // noCancel refusal, and the async function-handler overload.
    static bool Tasks()
    {
        Console.WriteLine("tasks leg: two nodes, loopback");
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }

        var srv = new RantNode("tsrv", Local(5));
        srv.OnEvent += e => { if (e.IsError) Console.WriteLine("event(tsrv): " + e); };
        var cli = new RantNode("tcli", Local(5));
        cli.OnEvent += e => { if (e.IsError) Console.WriteLine("event(tcli): " + e); };
        try
        {
            // a transfer that streams progress between awaits and honors its token
            var xfer = srv.TaskDefinition<XferReq, XferPrg, XferRsp>("xfer", async (q, ctx) =>
            {
                for (int i = 1; i <= q.Chunks; i++)
                {
                    await Task.Delay(20, ctx.CancellationToken).ConfigureAwait(false);
                    ctx.Progress(new XferPrg { Done = i });
                }
                return new XferRsp { Total = q.Chunks };
            });
            // runs until cancelled through its token
            var forever = srv.TaskDefinition<XferReq, XferPrg, XferRsp>("forever", async (q, ctx) =>
            {
                await Task.Delay(Timeout.Infinite, ctx.CancellationToken).ConfigureAwait(false);
                return new XferRsp();
            });
            // honors a cancel with what it got done so far
            var partial = srv.TaskDefinition<XferReq, XferPrg, XferRsp>("partial", async (q, ctx) =>
            {
                try { await Task.Delay(Timeout.Infinite, ctx.CancellationToken).ConfigureAwait(false); }
                catch (OperationCanceledException) { throw new CancelledException<XferRsp>(new XferRsp { Total = 7 }, "stopped"); }
                return new XferRsp();
            });
            // declares cancellation will not be honored
            var stubborn = srv.TaskDefinition<XferReq, XferPrg, XferRsp>("stubborn", async (q, ctx) =>
            {
                await Task.Delay(20).ConfigureAwait(false);
                return new XferRsp { Total = 1 };
            }, new TaskOptions { NoCancel = true });
            // a second transfer for the raw leg: a live same name second handle on one node is
            // refused (one name = one topic per node)
            var xfer2 = srv.TaskDefinition<XferReq, XferPrg, XferRsp>("xfer2", async (q, ctx) =>
            {
                for (int i = 1; i <= q.Chunks; i++)
                {
                    await Task.Delay(20, ctx.CancellationToken).ConfigureAwait(false);
                    ctx.Progress(new XferPrg { Done = i });
                }
                return new XferRsp { Total = q.Chunks };
            });
            // the async FUNCTION handler overload
            var amul = srv.FunctionDefinition<AddReq, AddRsp>("amul", async q =>
            {
                await Task.Delay(10).ConfigureAwait(false);
                return new AddRsp { Sum = q.A * q.B };
            });

            var xferR = cli.RemoteTask<XferReq, XferPrg, XferRsp>("xfer");
            var foreverR = cli.RemoteTask<XferReq, XferPrg, XferRsp>("forever");
            var stubbornR = cli.RemoteTask<XferReq, XferPrg, XferRsp>("stubborn");
            var partialR = cli.RemoteTask<XferReq, XferPrg, XferRsp>("partial");
            var reqS = cli.Schema(typeof(XferReq));
            var prgS = cli.Schema(typeof(XferPrg));
            var rspS = cli.Schema(typeof(XferRsp));
            // the raw form: byte[] types carry the encoded messages as is under explicit schemas
            var xferRaw = cli.RemoteTask<byte[], byte[], byte[]>("xfer2",
                requestSchema: reqS, progressSchema: prgS, responseSchema: rspS);
            var amulR = cli.RemoteFunction<AddReq, AddRsp>("amul");

            var deadline = DateTime.UtcNow.AddSeconds(8);
            while (DateTime.UtcNow < deadline
                   && !(xferR.MatchCount > 0 && foreverR.MatchCount > 0 && stubbornR.MatchCount > 0
                        && xferRaw.MatchCount > 0 && amulR.MatchCount > 0))
                Thread.Sleep(5);
            Check("definitions discovered", xferR.MatchCount > 0 && foreverR.MatchCount > 0
                  && stubbornR.MatchCount > 0 && xferRaw.MatchCount > 0 && amulR.MatchCount > 0
                  && partialR.MatchCount > 0);
            // CallAsync outcomes and progress fire on cli's service thread

            // typed: progress values in order, terminal Ok with the decoded result
            var seen = new List<int>();
            var t1 = xferR.CallAsync(new XferReq { Chunks = 3 },
                new InlineProgress<XferPrg>(p => { lock (seen) seen.Add(p.Done); }));
            Check("typed task returns the result", t1.Wait(10000) && t1.Result.Total == 3);
            lock (seen)
                Check("typed progress in order (no RUNNING ack)",
                      seen.Count == 3 && seen[0] == 1 && seen[1] == 2 && seen[2] == 3);

            // raw form: the progress bytes decode with the schema, the result bytes likewise
            var infos = new List<byte[]>();
            var t2 = xferRaw.TryCallAsync(reqS.Encode(new XferReq { Chunks = 2 }),
                new InlineProgress<byte[]>(p => { lock (infos) infos.Add(p); }));
            Check("raw task Ok", t2.Wait(10000) && t2.Result.Status == CallStatus.Ok);
            lock (infos)
            {
                Check("raw progress values", infos.Count == 2
                      && ((XferPrg)prgS.Decode(infos[1])).Done == 2);
            }
            Check("raw result decodes", t2.Result.Ok && ((XferRsp)rspS.Decode(t2.Result.Value)).Total == 2);

            // cancellation: the token ends the handler via ITS token, caller sees Cancelled
            var cts = new CancellationTokenSource();
            var t3 = foreverR.TryCallAsync(new XferReq(), null, cts.Token);
            var t3loud = foreverR.CallAsync(new XferReq(), null, cts.Token);
            Thread.Sleep(300);   // let RUNNING land (the deadline is dropped)
            cts.Cancel();
            Check("cancel honored end to end", t3.Wait(10000)
                  && t3.Result.Status == CallStatus.Cancelled);
            try { t3loud.Wait(10000); } catch (AggregateException) { }
            Check("a cancelled CallAsync ends as a cancelled Task", t3loud.IsCanceled);

            // noCancel: the cancel request is refused by the provider's declaration and the
            // call completes anyway
            var cts4 = new CancellationTokenSource();
            var t4 = stubbornR.CallAsync(new XferReq(), null, cts4.Token);
            Thread.Sleep(5);
            cts4.Cancel();
            Check("noCancel call completes anyway", t4.Wait(10000) && t4.Result.Total == 1);

            // async function-handler overload round trip
            var t5 = amulR.CallAsync(new AddReq { A = 6, B = 7 });
            Check("async function handler", t5.Wait(10000) && t5.Result.Sum == 42);
            // a blocking call under the service thread sleeps on its progress and answers
            Check("blocking call answers under service thread",
                  amulR.Call(new AddReq { A = 2, B = 3 }, 2000).Sum == 6);

            // the blocking task forms: progress where callbacks run, the result returned
            var bseen = new List<int>();
            XferRsp br = xferR.Call(new XferReq { Chunks = 2 },
                new InlineProgress<XferPrg>(p => { lock (bseen) bseen.Add(p.Done); }), 10000);
            Check("blocking task returns the result", br.Total == 2);
            lock (bseen)
                Check("blocking task progress in order", bseen.Count == 2 && bseen[0] == 1 && bseen[1] == 2);
            var bcts = new CancellationTokenSource(300);
            var tr = foreverR.TryCall(new XferReq(), null, 10000, bcts.Token);
            Check("a blocking task call cancels by token", tr.Status == CallStatus.Cancelled);

            // a cancel honored with a partial result: Value reads it though the call was not Ok
            var pcts = new CancellationTokenSource(300);
            var pr = partialR.TryCall(new XferReq(), null, 10000, pcts.Token);
            Check("a partial result comes back Cancelled", pr.Status == CallStatus.Cancelled && pr.HasValue);
            Check("and Value reads it", pr.HasValue && pr.Value.Total == 7);
        }
        finally
        {
            srv.Close();
            cli.Close();
        }
        Console.WriteLine(ok ? "tasks: PASS\n" : "tasks: FAIL\n");
        return ok;
    }

    // tests/golden/schemas.txt, copied next to the test: the hash every binding pins for each
    // schema text
    static readonly Dictionary<string, ulong> Golden = ReadGolden();
    static Dictionary<string, ulong> ReadGolden()
    {
        var table = new Dictionary<string, ulong>();
        foreach (var line in File.ReadAllLines(Path.Combine(AppContext.BaseDirectory, "schemas.txt")))
        {
            var parts = line.Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length >= 2 && !parts[0].StartsWith("#"))
                table[parts[0]] = Convert.ToUInt64(parts[1], 16);
        }
        return table;
    }

    // The canonical wire of a bare type is its kind alone, so these hashes are the same in
    // every language binding.
    static readonly ulong HashBool = Golden["bool"];
    static readonly ulong HashF32Arr = Golden["f32[]"];

    enum Mode : byte { Idle = 0, Run = 1, Fault = 2 }

    // The Float3 schema is the shared cross language golden vector: the same wire and the
    // same hash from C, C++, C# and Python.
    static readonly ulong HashFloat3 = Golden["Float3"];

    // The video family, the same golden vectors. A mirror whose enum member names, values or
    // order drifted would hash differently, so this catches it.
    static readonly ulong HashImage = Golden["Image"];
    static readonly ulong HashVideoFrame = Golden["VideoFrame"];
    static readonly ulong HashExternalVideoStream = Golden["ExternalVideoStream"];

    // Standard types as ordinary fields: an alias names a plain field's type, a composite is
    // a shipped mirror struct that names itself. Both narrow matching.
    struct Track
    {
        [RantField("at")]   public Rant.Types.Transform At;
        [RantField("when")] [RantTypeName("Timestamp")] public long When;
        [RantField("tag")]  public Rant.Types.Color Tag;
        [RantField("id")]   [RantTypeName("Uuid")] [RantArray(16)] public byte[] Id;
        [RantField("velocity")] public Rant.Types.Float3 Velocity;
    }

    // The fixed message size of a schema: its default message with nothing set.
    static int SizeOf(Schema s) => s.Encode(new Dictionary<string, object>()).Length;

    static bool StdTypes()
    {
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }

        using var a = new RantNode("sa", Local(10, Threading.Manual));
        using var b = new RantNode("sb", Local(10, Threading.Manual));
        {
            var f3 = a.Schema("Float3");
            var mirror = a.Schema(typeof(Rant.Types.Float3));
            Check("Float3 compiles by name alone, golden hash", f3.Hash == HashFloat3);
            Check("Float3 is 12 message bytes", SizeOf(f3) == 12);
            Check("the mirror struct IS that type", mirror.Hash == HashFloat3);
            Check("the same text is the same handle", a.Schema("Float3").Handle == f3.Handle);
        }
        // a name narrows: an anonymous field of the same shape reads a Transform field, never
        // the reverse, and Transform/Twist are distinct names never mistaken for each other.
        // Two shapes of W, so each goes on its own node
        {
            var named = a.Schema("W { at: Transform }");
            var bare = b.Schema("W { at: { pose: { position: { x: f64, y: f64, z: f64 }," +
                                "                  orientation: { x: f64, y: f64, z: f64, w: f64 } }," +
                                "         parent: string<30> } }");
            var xform = a.Schema("Transform");
            var twist = a.Schema("Twist");
            Check("an anonymous field of the same shape reads a Transform field",
                  bare.CanRead(named) && !named.CanRead(bare));
            Check("Transform and Twist never cross-wire", !twist.CanRead(xform));
            try { a.Schema("W { at: Twist }"); Check("a second shape of W on one node is refused", false); }
            catch (SchemaException e) { Check("a second shape of W on one node is refused: " + e.Message, true); }
        }
        {
            var sch = a.Schema(typeof(Track));
            string text = sch.Dsl;
            Check("the reflected schema spells the names, not the shapes",
                  text.Contains("at: Transform") && text.Contains("when: Timestamp")
                  && text.Contains("id: Uuid") && text.Contains("velocity: Float3"));
            Check("its message is the sum of the wire shapes (88+8+4+16+12)", SizeOf(sch) == 128);

            var t = new Track {
                At = new Rant.Types.Transform { Pose = new Rant.Types.Pose {
                    Position = new Rant.Types.Double3 { X = 4.5, Y = -1.25, Z = 9.0 },
                    Orientation = new Rant.Types.Quaternion { W = 1.0 } } },
                When = Rant.Types.Timestamp.Now(),
                Tag = new Rant.Types.Color { R = 0x11, G = 0x22, B = 0x33, A = 0xFF },
                Id = new byte[16],
                Velocity = new Rant.Types.Float3 { X = 1.0f, Y = 2.0f, Z = 3.0f } };
            for (int i = 0; i < 16; i++) t.Id[i] = (byte)i;
            var back = (Track)sch.Decode(sch.Encode(t));
            Check("a Track round-trips whole",
                  back.At.Pose.Position.X == 4.5 && back.At.Pose.Orientation.W == 1.0
                  && back.When == t.When && back.Tag.R == 0x11 && back.Tag.A == 0xFF
                  && back.Id != null && back.Id[15] == 15 && back.Velocity.Z == 3.0f);
            Check("Timestamp.Now is Unix-epoch microseconds", Rant.Types.Timestamp.Now() > 1600000000000000L);
        }
        // the shapes: every mirror compiles to its canonical name and size, Capsule and
        // Cylinder never cross wire, and a variable Polygon2D and an Empty round-trip
        {
            (Type, int)[] shapes =
            {
                (typeof(Rant.Types.Pose), 56), (typeof(Rant.Types.Pose2D), 24),
                (typeof(Rant.Types.Wrench), 48), (typeof(Rant.Types.AlignedBox), 48),
                (typeof(Rant.Types.AlignedBox2D), 32), (typeof(Rant.Types.OrientedBox), 80),
                (typeof(Rant.Types.OrientedBox2D), 40), (typeof(Rant.Types.Plane), 48),
                (typeof(Rant.Types.Segment), 48), (typeof(Rant.Types.Sphere), 32),
                (typeof(Rant.Types.Capsule), 56), (typeof(Rant.Types.Cylinder), 56),
                (typeof(Rant.Types.Cone), 56), (typeof(Rant.Types.Circle), 24),
                (typeof(Rant.Types.Empty), 0),
                (typeof(Rant.Types.Transform2D), 56), (typeof(Rant.Types.Twist2D), 24),
                (typeof(Rant.Types.Wrench2D), 24), (typeof(Rant.Types.Plane2D), 32),
                (typeof(Rant.Types.Segment2D), 32), (typeof(Rant.Types.Capsule2D), 40),
            };
            foreach (var (type, size) in shapes)
            {
                var mirror = a.Schema(type);
                Check($"{type.Name} mirror is the canonical type, {size} bytes",
                      mirror.Hash == a.Schema(type.Name).Hash && SizeOf(mirror) == size);
            }
            Check("Capsule and Cylinder never cross wire",
                  !a.Schema("Capsule").CanRead(a.Schema("Cylinder")));
            var poly = a.Schema(typeof(Rant.Types.Polygon2D));
            var p = new Rant.Types.Polygon2D { Points = new[] {
                new Rant.Types.Double2 { X = 0, Y = 0 }, new Rant.Types.Double2 { X = 1, Y = 0 },
                new Rant.Types.Double2 { X = 1, Y = 2 } } };
            var pb = (Rant.Types.Polygon2D)poly.Decode(poly.Encode(p));
            Check("a Polygon2D round-trips whole", pb.Points != null && pb.Points.Length == 3 && pb.Points[2].Y == 2);
            var empty = a.Schema(typeof(Rant.Types.Empty));
            Check("an Empty encodes to zero bytes and decodes back",
                  empty.Encode(new Rant.Types.Empty()).Length == 0
                  && empty.Decode(Array.Empty<byte>()) is Rant.Types.Empty);
        }
        // the video family: the shipped mirror must compile to the canonical bytes, and the
        // bare name must resolve to the same ones, so both forms are checked hash-exact
        (string, Type, ulong)[] video =
        {
            ("Image", typeof(Rant.Types.Image), HashImage),
            ("VideoFrame", typeof(Rant.Types.VideoFrame), HashVideoFrame),
            ("ExternalVideoStream", typeof(Rant.Types.ExternalVideoStream), HashExternalVideoStream),
        };
        foreach (var (name, clr, gold) in video)
        {
            var text = a.Schema(name);
            var mirror = a.Schema(clr);
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
        Console.WriteLine("video live leg: two nodes, loopback");
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }

        var a = new RantNode("MA", Local(4, Threading.Manual));
        a.OnEvent += e => { if (e.IsError) Console.WriteLine("event(MA): " + e); };
        var b = new RantNode("MB", Local(4, Threading.Manual));
        b.OnEvent += e => { if (e.IsError) Console.WriteLine("event(MB): " + e); };
        try
        {
            var qos = new Qos { Reliability = Reliability.Reliable, KeepLast = 4 };
            var pub = a.Publisher<Rant.Types.Image>("frame", qos);
            bool gotImage = false;
            var img = new Rant.Types.Image
            {
                Width = 64, Height = 4, Stride = 64,
                Format = Rant.Types.ImageFormat.Mono8,
                Data = new byte[256],
            };
            for (int i = 0; i < img.Data.Length; i++) img.Data[i] = (byte)(i * 7);
            var sub = b.Subscriber<Rant.Types.Image>("frame", (got, m) =>
                gotImage = got.Width == 64 && got.Height == 4 && got.Stride == 64
                           && got.Format == Rant.Types.ImageFormat.Mono8
                           && got.Data != null && got.Data.Length == img.Data.Length
                           && got.Data[0] == img.Data[0] && got.Data[255] == img.Data[255], qos);
            var initial = new Rant.Types.ExternalVideoStream
            {
                Kind = Rant.Types.VideoStreamKind.Rtsp,
                Codec = Rant.Types.VideoCodec.H264,
                Width = 1920,
                Height = 1080,
                Url = "rtsp://cam.local/main",
                Name = "front door",
            };
            var vd = a.VariableDefinition<Rant.Types.ExternalVideoStream>("stream", initial);
            var rv = b.RemoteVariable<Rant.Types.ExternalVideoStream>("stream");

            var deadline = DateTime.UtcNow.AddSeconds(8);
            while (DateTime.UtcNow < deadline
                   && (pub.MatchCount == 0 || !rv.TryGet(out Rant.Types.ExternalVideoStream _)))
            {
                a.Poll(1);
                b.Poll(1);
            }
            Check("image topic matched", pub.MatchCount == 1);

            Check("image send", pub.Send(img) == SendStatus.Ok);

            deadline = DateTime.UtcNow.AddSeconds(5);
            while (DateTime.UtcNow < deadline && !gotImage)
            {
                a.Poll(1);
                b.Poll(1);
            }
            Check("an Image crosses whole (fixed fields + the variable payload)", gotImage);

            Check("stream variable replicated the initial",
                  rv.TryGet(out Rant.Types.ExternalVideoStream s0)
                  && s0.Kind == Rant.Types.VideoStreamKind.Rtsp
                  && s0.Codec == Rant.Types.VideoCodec.H264
                  && s0.Width == 1920 && s0.Height == 1080
                  && s0.Url == "rtsp://cam.local/main" && s0.Name == "front door");

            Check("stream variable set accepted", rv.Set(new Rant.Types.ExternalVideoStream
            {
                Kind = Rant.Types.VideoStreamKind.WebrtcWhep,
                Url = "https://gw.local/whep/cam1",
                Name = "front door",
            }) == SendStatus.Ok);
            deadline = DateTime.UtcNow.AddSeconds(5);
            while (DateTime.UtcNow < deadline
                   && !(vd.TryGet(out Rant.Types.ExternalVideoStream v)
                        && v.Kind == Rant.Types.VideoStreamKind.WebrtcWhep))
            {
                a.Poll(1);
                b.Poll(1);
            }
            Check("stream variable set converged at the owner",
                  vd.TryGet(out Rant.Types.ExternalVideoStream s1)
                  && s1.Kind == Rant.Types.VideoStreamKind.WebrtcWhep
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

    // A scalar array of another element type converts to the field's kind, never reinterprets.
    static bool ArrayConvert()
    {
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }

        using var node = new RantNode("ac", Local(12, Threading.Manual));
        var s = node.Schema("Conv { f: f32[], i: i32[], u: u8[], w: u32[], d: f64[], g: f32[2], h: i16[] }");
        var back = (Dictionary<string, object>)s.Decode(s.Encode(new Dictionary<string, object>
        {
            { "f", new int[] { 1, -2, 3 } }, { "i", new float[] { 1f, -2f, 3f } },
            { "u", new int[] { 1, 2, 250 } }, { "w", new int[] { 1, 2, 3 } },
            { "d", new float[] { 0.5f, 1.5f } }, { "g", new int[] { 7, 8 } },
            { "h", new ushort[] { 1, 2 } },
        }));
        Check("int[] for f32[]", back["f"] is float[] f && f.Length == 3 && f[0] == 1f && f[1] == -2f && f[2] == 3f);
        Check("float[] for i32[]", back["i"] is int[] i && i.Length == 3 && i[0] == 1 && i[1] == -2 && i[2] == 3);
        Check("int[] for u8[]", back["u"] is byte[] u && u.Length == 3 && u[2] == 250);
        Check("int[] for u32[]", back["w"] is uint[] w && w.Length == 3 && w[1] == 2);
        Check("float[] for f64[]", back["d"] is double[] d && d.Length == 2 && d[0] == 0.5 && d[1] == 1.5);
        Check("int[] for f32[2]", back["g"] is float[] g && g.Length == 2 && g[0] == 7f && g[1] == 8f);
        Check("ushort[] for i16[]", back["h"] is short[] h && h.Length == 2 && h[1] == 2);
        return ok;
    }

    // Bare types as whole schemas: no struct wrapper, plain values through Send and the handler.
    static bool ValueRoots()
    {
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }

        using var node = new RantNode("vr", Local(11, Threading.Manual));
        {
            var sb = node.Schema(typeof(bool));
            var sa = node.Schema(typeof(float[]));
            Check("bool canonical hash", sb.Hash == HashBool);
            Check("float[] canonical hash", sa.Hash == HashF32Arr);
            Check("bool dsl", sb.Dsl == "bool\n");
            Check("float[] dsl", sa.Dsl == "f32[]\n");
            var f = sb.Fields;
            Check("bare root is one anonymous field",
                  f.Length == 1 && sb.Name == "" && f[0].Name == "" && f[0].Kind == FieldType.Bool);
            var text = node.Schema("bool");
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
            var s = node.Schema(t);
            object back = s.Decode(s.Encode(v));
            Check($"round-trip {s.Dsl.Trim()} -> {back}", Equals(back, v));
        }
        {
            var s = node.Schema(typeof(float[]));
            var back = (float[])s.Decode(s.Encode(new[] { 1.5f, -2.5f }));
            Check("round-trip f32[]", back.Length == 2 && back[0] == 1.5f && back[1] == -2.5f);
        }
        {
            var s = node.Schema(typeof(Dictionary<string, object>));
            var back = (Dictionary<string, object>)s.Decode(
                s.Encode(new Dictionary<string, object> { { "battery", 87 } }));
            Check("round-trip map", back.Count == 1 && Convert.ToInt64(back["battery"]) == 87);
        }
        // a standard alias root is named after its type and is still a bare value
        (string, object)[] aliases =
        {
            ("Timestamp", 1234567L), ("Duration", -5L),
            ("Uuid", new byte[] { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 }),
            ("Matrix3x3", new float[] { 0, 1, 2, 3, 4, 5, 6, 7, 8 }),
            ("Matrix4x4", new float[] { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 }),
            ("Uri", "rtsp://host/stream"),
        };
        foreach (var (name, v) in aliases)
        {
            var s = node.Schema(name);
            object back = s.Decode(s.Encode(v));
            Check($"round-trip {name} root -> {back}", System.Collections.StructuralComparisons
                  .StructuralEqualityComparer.Equals(back, v));
        }
        // a struct array root defines its element above it and is spelled by name
        {
            var s = node.Schema(typeof(Corner[]));
            Check("a struct array root defines its element once",
                  s.Dsl.Contains("Corner {") && s.Dsl.TrimEnd().EndsWith("Corner[]") && s.Name == "");
            var text = node.Schema("Corner { x: f32, y: f32 }\nCorner[]");
            Check("the reflected root is the spelled out root", text.Handle == s.Handle);
            var corners = new[] { new Corner { X = 1, Y = 2 }, new Corner { X = 3, Y = 4 } };
            var back = (Corner[])s.Decode(s.Encode(corners));
            Check("a struct array root round trips", back.Length == 2 && back[0].Y == 2 && back[1].X == 3);
            var untyped = node.Schema("Corner[]");
            var list = untyped.Decode(s.Encode(corners)) as List<Dictionary<string, object>>;
            Check("with no type it decodes as a list of dictionaries",
                  list != null && list.Count == 2 && Convert.ToDouble(list[1]["x"]) == 3);
            var fromDicts = (Corner[])s.Decode(s.Encode(new List<object>
                { new Dictionary<string, object> { { "X", 5 }, { "y", 6 } } }));
            Check("a list of dictionaries encodes a struct array root",
                  fromDicts.Length == 1 && fromDicts[0].X == 5 && fromDicts[0].Y == 6);
            var fixed4 = node.Schema("Corner[4]");
            var fl = fixed4.Decode(fixed4.Encode(corners)) as List<Dictionary<string, object>>;
            Check("an array fills a fixed struct array root and zero pads it",
                  fl != null && fl.Count == 4 && Convert.ToDouble(fl[1]["x"]) == 3 && Convert.ToDouble(fl[3]["x"]) == 0);
            var empty = (Corner[])s.Decode(s.Encode(new Corner[0]));
            Check("an empty struct array root is empty", empty.Length == 0);
        }
        Console.WriteLine(ok ? "bare-type roots: PASS\n" : "bare-type roots: FAIL\n");
        return ok;
    }

    // Two nodes: bare-typed topics and a bare-typed variable over loopback.
    static bool ValueRootsLive()
    {
        Console.WriteLine("bare-root live leg: two nodes, loopback");
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }

        var a = new RantNode("VA", Local(2, Threading.Manual));
        a.OnEvent += e => { if (e.IsError) Console.WriteLine("event(VA): " + e); };
        var b = new RantNode("VB", Local(2, Threading.Manual));
        b.OnEvent += e => { if (e.IsError) Console.WriteLine("event(VB): " + e); };
        try
        {
            var qos = new Qos { Reliability = Reliability.Reliable, KeepLast = 4 };
            var pubFlag = a.Publisher<bool>("flag", qos);
            bool gotFlag = false, gotNote = false, gotPts = false;
            var subFlag = b.Subscriber<bool>("flag", f => { if (f) gotFlag = true; }, qos);
            var pubNote = a.Publisher<string>("note", qos);
            var subNote = b.Subscriber<string>("note", str => { if (str == "a bare unbounded string") gotNote = true; }, qos);
            var pubPts = a.Publisher<Corner[]>("pts", qos);
            var subPts = b.Subscriber<Corner[]>("pts", pts => { if (pts.Length == 2 && pts[1].Y == 8) gotPts = true; }, qos);
            var vd = a.VariableDefinition<double>("gain", 1.25);
            var rv = b.RemoteVariable<double>("gain");
            var deadline = DateTime.UtcNow.AddSeconds(8);
            while (DateTime.UtcNow < deadline && (pubFlag.MatchCount == 0
                   || pubNote.MatchCount == 0 || pubPts.MatchCount == 0 || !rv.TryGet(out double _)))
            {
                a.Poll(1);
                b.Poll(1);
            }
            Check("bare topics matched", pubFlag.MatchCount == 1 && pubNote.MatchCount == 1
                  && pubPts.MatchCount == 1);
            Check("bare variable replicated the initial",
                  rv.TryGet(out double gain0) && gain0 == 1.25);
            Check("bool send", pubFlag.Send(true) == SendStatus.Ok);
            Check("string send", pubNote.Send("a bare unbounded string") == SendStatus.Ok);
            Check("struct array send", pubPts.Send(new[] { new Corner { X = 5, Y = 6 },
                                                           new Corner { X = 7, Y = 8 } }) == SendStatus.Ok);
            deadline = DateTime.UtcNow.AddSeconds(5);
            while (DateTime.UtcNow < deadline && !(gotFlag && gotNote && gotPts))
            {
                a.Poll(1);
                b.Poll(1);
            }
            Check("bool received as a plain value", gotFlag);
            Check("string received as a plain value", gotNote);
            Check("struct array received as a typed array", gotPts);
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

    // Threading.Dispatch: every event, handler, observer and awaited result must run on the
    // thread that calls Dispatch(), never on a service thread.
    static bool DispatchLeg()
    {
        Console.WriteLine("dispatch leg: two nodes, loopback");
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }

        int drainId = Thread.CurrentThread.ManagedThreadId;

        int evtId = 0, fnId = 0, changeId = 0, doneId = 0, sum = 0, level = 0;

        // both wires owned by service threads: nothing polls on this one
        var srv = new RantNode("dsrv", Local(6, Threading.Dispatch));
        srv.OnEvent += e => { if (evtId == 0) evtId = Thread.CurrentThread.ManagedThreadId; };
        var cli = new RantNode("dcli", Local(6, Threading.Dispatch));
        void Drain() { srv.Dispatch(); cli.Dispatch(); }

        var add = srv.FunctionDefinition<AddReq, AddRsp>("dadd", q =>
        {
            fnId = Thread.CurrentThread.ManagedThreadId;
            return new AddRsp { Sum = q.A + q.B };
        });
        var lvlDef = srv.VariableDefinition<Level>("dlevel", new Level { Value = 1 });

        var call = cli.RemoteFunction<AddReq, AddRsp>("dadd");
        var rvar = cli.RemoteVariable<Level>("dlevel");
        rvar.OnChange += (v, u) =>
        {
            changeId = Thread.CurrentThread.ManagedThreadId;
            level = v.Value;
        };

        var deadline = DateTime.UtcNow.AddSeconds(8);
        while (DateTime.UtcNow < deadline && !(call.MatchCount > 0 && rvar.MatchCount > 0))
        {
            Drain();
            Thread.Sleep(5);
        }
        Check("definitions matched", call.MatchCount > 0 && rvar.MatchCount > 0);

        // the handler runs at the next Dispatch and answers from there
        var t = call.CallAsync(new AddReq { A = 2, B = 3 });
        t.ContinueWith(x =>
        {
            doneId = Thread.CurrentThread.ManagedThreadId;
            sum = x.IsFaulted ? -1 : x.Result.Sum;
        }, TaskContinuationOptions.ExecuteSynchronously);
        deadline = DateTime.UtcNow.AddSeconds(8);
        while (DateTime.UtcNow < deadline && doneId == 0) { Drain(); Thread.Sleep(5); }
        Check("dispatched handler answered the call", sum == 5);
        Check("handler ran on the drain thread", fnId == drainId);
        Check("awaited result resumed on the drain thread", doneId == drainId);

        lvlDef.Set(new Level { Value = 9 });
        deadline = DateTime.UtcNow.AddSeconds(8);
        while (DateTime.UtcNow < deadline && level != 9) { Drain(); Thread.Sleep(5); }
        Check("variable change arrived", level == 9);
        Check("observer ran on the drain thread", changeId == drainId);

        // several scripts share one variable: every handler fires, a late one is replayed
        // the current value, and removing one leaves the rest alone
        int a2 = 0, b2 = 0;
        Action<Level, VariableUpdate> subA = (v, u) => a2 = v.Value;
        rvar.OnChange += subA;
        rvar.OnChange += (v, u) => b2 = v.Value;
        Check("late observers replayed the current value", a2 == 9 && b2 == 9);

        lvlDef.Set(new Level { Value = 11 });
        deadline = DateTime.UtcNow.AddSeconds(8);
        while (DateTime.UtcNow < deadline && !(a2 == 11 && b2 == 11 && level == 11))
        {
            Drain();
            Thread.Sleep(5);
        }
        Check("every observer fired", a2 == 11 && b2 == 11 && level == 11);

        rvar.OnChange -= subA;
        lvlDef.Set(new Level { Value = 12 });
        deadline = DateTime.UtcNow.AddSeconds(8);
        while (DateTime.UtcNow < deadline && b2 != 12) { Drain(); Thread.Sleep(5); }
        Check("a disposed observer stops, the others go on", a2 == 11 && b2 == 12 && level == 12);

        deadline = DateTime.UtcNow.AddSeconds(8);
        while (DateTime.UtcNow < deadline && evtId == 0) { Drain(); Thread.Sleep(5); }
        Check("events ran on the drain thread", evtId == drainId);

        int logId = 0; string logText = null;
        cli.OnLog += l => { logId = Thread.CurrentThread.ManagedThreadId; logText = l.Text; };
        srv.Log(Rant.Level.Info, "dispatched line");
        deadline = DateTime.UtcNow.AddSeconds(8);
        while (DateTime.UtcNow < deadline && logText != "dispatched line") { Drain(); Thread.Sleep(5); }
        Check("log line arrived", logText == "dispatched line");

        // nothing may have slipped onto a service thread
        Check("no callback ran off the drain thread",
              evtId == drainId && fnId == drainId && changeId == drainId && doneId == drainId
              && logId == drainId);

        // a handle that outlives its node must refuse, never read the freed arena
        var stalePub = cli.Publisher<Level>("dstale");
        bool matchedBefore = call.MatchCount > 0;
        srv.Close();
        cli.Close();
        Level got;
        Check("stale variable set refuses", rvar.Set(new Level { Value = 1 }) == SendStatus.NoTopic);
        Check("stale variable read is empty", !rvar.TryGet(out got));
        Check("stale publisher send refuses", stalePub.Send(new Level { Value = 1 }) == SendStatus.NoTopic);
        Check("stale remote function is unmatched", matchedBefore && call.MatchCount == 0);

        Console.WriteLine(ok ? "dispatch: PASS\n" : "dispatch: FAIL\n");
        return ok;
    }

    // OnLog on service threads: a line logged before the subscribe replays, a live one follows
    static bool LogLeg()
    {
        Console.WriteLine("log leg: two nodes, loopback");
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }

        using var a = new RantNode("lsrc", Local(8));
        using var b = new RantNode("lsink", Local(8));
        var lines = new System.Collections.Concurrent.ConcurrentQueue<RantLogLine>();
        a.Log(Rant.Level.Warn, "before");
        a.Log(Rant.Level.Debug, "hidden");
        b.OnLog += l => lines.Enqueue(l);
        bool Has(string t, Rant.Level lv)
        {
            foreach (var l in lines)
                if (l.Text == t && l.Level == lv && l.Origin == Origin.App && l.Node == "lsrc" && l.WallUs != 0) return true;
            return false;
        }
        var deadline = DateTime.UtcNow.AddSeconds(8);
        while (DateTime.UtcNow < deadline && !Has("before", Rant.Level.Warn)) Thread.Sleep(5);
        Check("a line logged before the subscribe replays", Has("before", Rant.Level.Warn));
        a.Log(Rant.Level.Error, "live");
        deadline = DateTime.UtcNow.AddSeconds(8);
        while (DateTime.UtcNow < deadline && !Has("live", Rant.Level.Error)) Thread.Sleep(5);
        Check("a live line arrives with its level, origin and node", Has("live", Rant.Level.Error));
        Check("a line below the level never arrives", !Has("hidden", Rant.Level.Debug));

        var o = Local(8); o.DisableLogs = true;
        using var c = new RantNode("lnone", o);
        bool threw = false;
        try { c.OnLog += l => { }; } catch (RantException) { threw = true; }
        Check("OnLog on a node without logs throws", threw);

        var bad = Local(8); bad.LogLevel = (Rant.Level)9;
        threw = false;
        try { using var d = new RantNode("lbad", bad); } catch (RantException) { threw = true; }
        Check("a bad LogLevel refuses the open", threw);

        Console.WriteLine(ok ? "log: PASS\n" : "log: FAIL\n");
        return ok;
    }

    // OnStatus: A raises before B listens, B binds late and sees the whole set, B clears the
    // clearable one and A's event names it, and A closing empties B's view
    static bool StatusLeg()
    {
        Console.WriteLine("status leg: two nodes, loopback");
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }
        bool Until(int ms, Func<bool> f)
        {
            var end = DateTime.UtcNow.AddMilliseconds(ms);
            while (DateTime.UtcNow < end) { if (f()) return true; Thread.Sleep(5); }
            return f();
        }

        var a = new RantNode("ssrc", Local(16));
        using var b = new RantNode("ssink", Local(16));
        object mu = new object();
        RantStatusUpdate last = null; int updates = 0;
        string clearedKey = null;
        a.OnEvent += e => { if (e.Kind == EventKind.StatusCleared) lock (mu) clearedKey = e.Detail; };
        Check("raised before anyone listens",
              a.SetStatus("estop", Rant.Level.Error, "pressed") == SendStatus.Ok
              && a.SetStatus("hot", Rant.Level.Warn, "motor hot", clearable: true) == SendStatus.Ok);
        Check("a long key is refused", a.SetStatus(new string('k', 49), Rant.Level.Info, "x") == SendStatus.TooBig);
        b.OnStatus += u => { if (u.Node == "ssrc") lock (mu) { last = u; updates++; } };
        int Count() { lock (mu) return last == null ? -1 : last.Conditions.Count; }
        Check("the late binder gets the whole set", Until(8000, () => Count() == 2));
        RantCondition hot = null;
        lock (mu)
        {
            var c = last.Conditions;
            Check("the set decodes", c.Count == 2 && c[0].Key == "estop" && c[0].Level == Rant.Level.Error
                  && !c[0].Clearable && c[1].Key == "hot" && c[1].Text == "motor hot" && c[1].Clearable
                  && c[1].SinceUs != 0 && c[1].Origin == Origin.App);
            if (c.Count == 2) hot = c[1];
        }
        uint from; lock (mu) from = last.NodeId;
        Check("a peer clear is sent", hot != null && b.ClearPeerStatus(from, "hot", hot.SinceUs) == SendStatus.Ok);
        Check("A's app hears which was cleared", Until(8000, () => { lock (mu) return clearedKey == "hot"; }));
        Check("B sees the smaller set", Until(8000, () => Count() == 1));
        Check("an own clear", a.ClearStatus("estop") == SendStatus.Ok);
        Check("B sees it empty", Until(8000, () => Count() == 0));
        a.SetStatus("estop", Rant.Level.Error, "pressed again");
        Check("raised again", Until(8000, () => Count() == 1));
        int before; lock (mu) before = updates;
        a.Dispose();
        Check("a node that went away reads as empty",
              Until(8000, () => { lock (mu) return updates > before && last.Conditions.Count == 0; }));

        var o = Local(16); o.DisableStatus = true;
        using var c2 = new RantNode("snone", o);
        bool threw = false;
        try { c2.OnStatus += u => { }; } catch (RantException) { threw = true; }
        Check("OnStatus on a node without status throws", threw);
        Check("SetStatus on a node without status is NoSys", c2.SetStatus("k", Rant.Level.Info, "x") == SendStatus.NoSys);

        Console.WriteLine(ok ? "status: PASS\n" : "status: FAIL\n");
        return ok;
    }

    // A subscriber made without a handler is pulled: TryTake and TryTakeLatest read it on the
    // calling thread, and it never parks on the node's Dispatch queue
    static bool PullLeg()
    {
        Console.WriteLine("pull leg: two nodes, loopback");
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }

        using var a = new RantNode("psrc", Local(12));
        using var b = new RantNode("psink", Local(12, Threading.Dispatch));
        var qos = new Qos { Reliability = Reliability.Reliable, KeepLast = 8 };
        var pub = a.Publisher<Level>("pulled", qos);
        var sub = b.Subscriber<Level>("pulled", qos);
        var deadline = DateTime.UtcNow.AddSeconds(8);
        while (DateTime.UtcNow < deadline && pub.MatchCount == 0) Thread.Sleep(5);
        Check("matched", pub.MatchCount == 1);
        for (int i = 1; i <= 3; i++) pub.Send(new Level { Value = i });
        Level v;
        Check("TryTake waits for the oldest", sub.TryTake(out v, 2000) && v.Value == 1);
        // the taken record stays counted until the next take, so 3 means 2 and 3 have arrived
        deadline = DateTime.UtcNow.AddSeconds(8);
        while (DateTime.UtcNow < deadline && sub.QueueStats().Messages < 3) Thread.Sleep(5);
        Check("the node's Dispatch leaves it waiting", b.Dispatch() >= 0 && sub.QueueStats().Messages >= 3);
        Check("TryTakeLatest returns the newest", sub.TryTakeLatest(out v) && v.Value == 3);
        Check("and dropped the older ones", !sub.TryTake(out v));

        var pushed = b.Subscriber<Level>("pushed", x => { }, qos);
        bool threw = false;
        try { pushed.TryTake(out v); } catch (InvalidOperationException) { threw = true; }
        Check("TryTake on a handler subscriber throws", threw);
        threw = false;
        try { b.Subscriber<Level>("pq", new Qos { Queue = b.CreateQueue() }); } catch (ArgumentException) { threw = true; }
        Check("a pulled subscriber refuses a Queue", threw);

        Console.WriteLine(ok ? "pull: PASS\n" : "pull: FAIL\n");
        return ok;
    }

    // reflect_from_mesh: a handle carrying no type of its own takes the provider's from the
    // mesh, which is what an observer tool or a generic HMI needs.
    static bool ReflectLeg()
    {
        Console.WriteLine("reflect leg: two nodes, loopback");
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }

        var a = new RantNode("rsrv", Local(7));
        var b = new RantNode("rcli", Local(7, fetchDetails: true));

        var pub = a.Publisher<Level>("reflected", new Qos { Reliability = Reliability.Reliable, KeepLast = 4 });
        pub.Send(new Level { Value = 3 });

        // a byte[] subscriber with no schema is raw. Nobody provides this one, so there is
        // nothing to copy and it stays untyped
        var plain = b.Subscriber<byte[]>("unprovided", qos: new Qos { ReflectFromMesh = true });
        Check("an unprovided reflect topic stays untyped",
              Native.rant_topic_schema(plain.Topic._handle) == IntPtr.Zero);

        var reflect = b.Subscriber<byte[]>("reflected", qos: new Qos { ReflectFromMesh = true });
        var deadline = DateTime.UtcNow.AddSeconds(10);
        while (DateTime.UtcNow < deadline && Native.rant_topic_schema(reflect.Topic._handle) == IntPtr.Zero)
        {
            reflect.Refresh();          // never automatic: the app picks the moment
            Thread.Sleep(20);
        }
        Check("a reflect topic took the provider's schema",
              Native.rant_topic_schema(reflect.Topic._handle) != IntPtr.Zero);
        Check("refresh on a settled handle reports no change", !reflect.Refresh());
        Check("a handle reports its name and schema", pub.Name == "reflected" && pub.Schema.Name == "Level");
        Check("a reflect topic reports the schema it adopted",
              reflect.Schema != null && reflect.Schema.Hash == pub.Schema.Hash);

        // a reflect remote function reports the schemas it adopted
        var radd = a.FunctionDefinition<AddReq, AddRsp>("radd", q => new AddRsp { Sum = q.A + q.B });
        var raddR = b.RemoteFunction<byte[], byte[]>("radd", new FunctionOptions { ReflectFromMesh = true });
        Check("a definition reports its schemas", radd.Name == "radd"
              && radd.RequestSchema.Name == "AddReq" && radd.ResponseSchema.Name == "AddRsp");
        deadline = DateTime.UtcNow.AddSeconds(10);
        // the two channels' details arrive apart, so a refresh may adopt one before the other
        while (DateTime.UtcNow < deadline && (raddR.RequestSchema == null || raddR.ResponseSchema == null))
        {
            raddR.Refresh();
            Thread.Sleep(20);
        }
        Check("a reflect remote reports the schemas it adopted",
              raddR.RequestSchema != null && raddR.RequestSchema.Hash == radd.RequestSchema.Hash
              && raddR.ResponseSchema.Hash == radd.ResponseSchema.Hash);
        var level = a.VariableDefinition<Level>("rlevel", new Level { Value = 1 });
        Check("a variable reports its name and schema", level.Name == "rlevel" && level.Schema.Name == "Level");
        Check("drain returns once every reader acked", pub.Drain(2000));

        // the blocking meta form answers on this thread
        uint aId = 0;
        foreach (var p in b.Reflection.Peers()) if (p.Active && p.Name == "rsrv") aId = p.Id;
        Check("the peer is known", aId != 0);
        var snap = b.Reflection.Meta(aId, MetaSection.All, 2000);
        Check("blocking meta names the peer", snap.Name == "rsrv");
        Check("meta counts the matched peers", snap.MatchedPeers >= 1 && snap.MatchedPeers <= snap.MaxPeers);

        // a payload that does not decode is an event naming the topic, never a silent drop
        RantEvent decodeEvt = null;
        b.OnEvent += e => { if (e.IsError && e.TopicName == "t/undecodable") decodeEvt = e; };
        b.ReportDecode("t/undecodable", "a message did not decode");
        Check("a decode failure is a SchemaMismatch event naming the topic",
              decodeEvt != null && decodeEvt.Error == ErrorKind.SchemaMismatch);

        a.Close();
        b.Close();
        Console.WriteLine(ok ? "reflect: PASS\n" : "reflect: FAIL\n");
        return ok;
    }

    // Same name handles on one node share the native topic: disposing one leaves its siblings
    // receiving, disposing the last retires the slot so the name can carry another schema.
    static bool DisposeLeg()
    {
        Console.WriteLine("dispose leg: two nodes, loopback");
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }

        var a = new RantNode("da", Local(8, Threading.Manual));
        var b = new RantNode("db", Local(8, Threading.Manual));
        try
        {
            Check("LastError is null on a clean node", a.LastError == null);
            var qos = new Qos { Reliability = Reliability.Reliable, KeepLast = 4 };
            int n1 = 0, n2 = 0;
            var pub = a.Publisher<Level>("shared", qos);
            var s1 = b.Subscriber<Level>("shared", v => n1 = v.Value, qos);
            var s2 = b.Subscriber<Level>("shared", v => n2 = v.Value, qos);
            var deadline = DateTime.UtcNow.AddSeconds(8);
            while (DateTime.UtcNow < deadline && !(n1 == 1 && n2 == 1))
            {
                pub.Send(new Level { Value = 1 });
                a.Poll(1);
                b.Poll(1);
            }
            Check("two subscribers share one slot", n1 == 1 && n2 == 1 && pub.MatchCount == 1);

            s1.Dispose();
            deadline = DateTime.UtcNow.AddSeconds(5);
            while (DateTime.UtcNow < deadline && n2 != 2)
            {
                pub.Send(new Level { Value = 2 });
                a.Poll(1);
                b.Poll(1);
            }
            Check("the sibling still receives after one dispose", n2 == 2 && n1 == 1);
            Check("the slot is still matched", pub.MatchCount == 1);

            s2.Dispose();
            bool retyped = false;
            try { b.Subscriber<AddReq>("shared", qos: qos); retyped = true; }
            catch (RantException) { }
            Check("the last dispose retires the slot, the name retypes", retyped);
            pub.Dispose();
            Check("a disposed publisher refuses", pub.Send(new Level { Value = 3 }) == SendStatus.NoTopic);
            Check("dispose is idempotent", DisposeTwice(s2) && DisposeTwice(pub));
        }
        finally
        {
            a.Close();
            b.Close();
        }
        Console.WriteLine(ok ? "dispose: PASS\n" : "dispose: FAIL\n");
        return ok;
    }

    // A rebase, then another after the reader's retired slot goes to another name: each
    // writer's layout decodes with its own offsets, never those cached for an earlier one.
    static bool RebindLeg()
    {
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }
        bool Until(int ms, Func<bool> done)
        {
            var deadline = DateTime.UtcNow.AddMilliseconds(ms);
            while (DateTime.UtcNow < deadline && !done()) Thread.Sleep(2);
            return done();
        }

        using var r = new RantNode("rr", Local(13));
        using var w1 = new RantNode("rw1", Local(13));
        using var w2 = new RantNode("rw2", Local(13));
        var qos = new Qos { Reliability = Reliability.Reliable };
        int got = 0;
        uint a = 0, b = 0;
        Action<ProbeRead> on = p => { a = p.A; b = p.B; Interlocked.Increment(ref got); };

        using (var pub = w1.Publisher<ProbeFirst>("rb/first", qos))
        using (var sub = r.Subscriber<ProbeRead>("rb/first", on, qos))
        {
            Check("first pair matched", Until(4000, () => pub.MatchCount > 0 && pub.Ready));
            pub.Send(new ProbeFirst { B = 2, A = 1 });
            Check("the first layout decodes", Until(3000, () => Volatile.Read(ref got) == 1) && a == 1 && b == 2);
        }
        using var pub2 = w2.Publisher<ProbeSecond>("rb/second", qos);
        using var sub2 = r.Subscriber<ProbeRead>("rb/second", on, qos);
        Check("second pair matched", Until(4000, () => pub2.MatchCount > 0 && pub2.Ready));
        pub2.Send(new ProbeSecond { A = 1, X = 99, B = 2 });
        Check("the second layout decodes with its own offsets",
              Until(3000, () => Volatile.Read(ref got) == 2) && a == 1 && b == 2);
        if (!ok) Console.WriteLine($"  decoded a={a} b={b}");
        return ok;
    }

    // Calls on other threads while the node closes answer NoTopic or their usual result and
    // never touch the freed node, and a close inside the node's own callback runs once it
    // returns. Only a sanitizer build of the native library sees every miss.
    static bool CloseRaceLeg()
    {
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }
        bool Until(int ms, Func<bool> done)
        {
            var deadline = DateTime.UtcNow.AddMilliseconds(ms);
            while (DateTime.UtcNow < deadline && !done()) Thread.Sleep(2);
            return done();
        }

        bool refusedAfter = true;
        for (int round = 0; round < 40; round++)
        {
            NodeOptions o = Local(14);
            o.MatchWaitMs = -1;   // a send with nobody matched drops at once, so the loops spin
            var n = new RantNode("cr", o);
            n.OnEvent += e => { };
            var pub = n.Publisher<Level>("cr/level");
            var v = n.VariableDefinition<Level>("cr/var", new Level());
            int stop = 0;
            long calls = 0;
            var threads = new[]
            {
                new Thread(() =>
                {
                    for (int i = 0; Volatile.Read(ref stop) == 0; i++) { pub.Send(new Level { Value = i }); Interlocked.Increment(ref calls); }
                }),
                new Thread(() =>
                {
                    for (int i = 0; Volatile.Read(ref stop) == 0; i++) { v.Set(new Level { Value = i }); Interlocked.Increment(ref calls); }
                }),
                new Thread(() =>
                {
                    while (Volatile.Read(ref stop) == 0)
                    {
                        try { using (var p = n.Publisher<Level>("cr/churn")) p.Send(new Level { Value = 1 }); }
                        catch (RantException) { }
                        Interlocked.Increment(ref calls);
                    }
                }),
            };
            foreach (var t in threads) t.Start();
            while (Interlocked.Read(ref calls) < 300) Thread.Yield();
            n.Close();
            refusedAfter &= pub.Send(new Level()) == SendStatus.NoTopic && v.Set(new Level()) == SendStatus.NoTopic;
            Volatile.Write(ref stop, 1);
            foreach (var t in threads) t.Join();
        }
        Check("after close every call answers NoTopic", refusedAfter);

        var src = new RantNode("cs", Local(14));
        var self = new RantNode("ce", Local(14, Threading.Manual));
        try
        {
            var qos = new Qos { Reliability = Reliability.Reliable };
            var srcPub = src.Publisher<Level>("cr/close", qos);
            bool accepted = false;
            self.Subscriber<Level>("cr/close", _ => accepted = self.Close(), qos);
            var probe = self.Publisher<Level>("cr/probe");
            Check("self closing node matched",
                  Until(4000, () => { self.Poll(2); return srcPub.MatchCount > 0 && srcPub.Ready; }));
            srcPub.Send(new Level { Value = 5 });
            Check("a close inside its own callback is accepted", Until(3000, () => { self.Poll(2); return accepted; }));
            self.Close();   // waits for the close the callback started
            Check("the node closed once the callback returned", probe.Send(new Level()) == SendStatus.NoTopic);
        }
        finally
        {
            self.Close();
            src.Close();
        }
        return ok;
    }

    static bool DisposeTwice(IDisposable d)
    {
        try { d.Dispose(); d.Dispose(); return true; }
        catch (Exception) { return false; }
    }

    static bool VersionLeg()
    {
        bool ok = System.Text.RegularExpressions.Regex.IsMatch(RantNode.Version ?? "", @"^\d+\.\d+\.\d+$");
        Console.WriteLine("version: " + RantNode.Version + (ok ? " PASS\n" : " FAIL\n"));
        return ok;
    }

    // Every interop mirror against the C layout the build writes: a stale mirror crashes every
    // node open, so a size or offset that drifted fails here first.
    static bool LayoutLeg()
    {
        string path = Environment.GetEnvironmentVariable("RANT_LAYOUT");
        if (string.IsNullOrEmpty(path) || !File.Exists(path))
        {
            Console.WriteLine($"FAIL: no C layout at '{path}', run through ctest");
            return false;
        }
        var layout = new Dictionary<string, int>();
        foreach (var line in File.ReadAllLines(path))
        {
            var parts = line.Split(' ');
            if (parts.Length == 2) layout[parts[0]] = int.Parse(parts[1]);
        }
        bool ok = true;
        foreach (var t in typeof(RantNode).Assembly.GetTypes())
        {
            if (t.Namespace != "Rant" || !t.IsValueType || t.IsEnum || t.IsPublic || t.IsNested
                || !t.Name.StartsWith("Rant") || t.Name == "RantValueUnion") continue;
            string c = t.Name == "RantStringView" ? "RantString"
                     : t.Name.EndsWith("Native") ? t.Name.Substring(0, t.Name.Length - 6) : t.Name;
            if (!layout.TryGetValue(c, out int size))
            {
                Console.WriteLine($"FAIL: {t.Name} has no C layout, add it to tests/support/layout.c");
                ok = false;
                continue;
            }
            if (Marshal.SizeOf(t) != size)
            {
                Console.WriteLine($"FAIL: {t.Name} is {Marshal.SizeOf(t)} bytes, C says {size}");
                ok = false;
            }
            foreach (var f in t.GetFields(System.Reflection.BindingFlags.Instance
                                          | System.Reflection.BindingFlags.Public
                                          | System.Reflection.BindingFlags.NonPublic))
            {
                int at = (int)Marshal.OffsetOf(t, f.Name);
                if (!layout.TryGetValue(c + "." + f.Name, out int want) || want != at)
                {
                    Console.WriteLine($"FAIL: {t.Name}.{f.Name} at {at}, C says {(layout.ContainsKey(c + "." + f.Name) ? want.ToString() : "nothing")}");
                    ok = false;
                }
            }
        }
        Console.WriteLine(ok ? "layout: PASS\n" : "layout: FAIL\n");
        return ok;
    }

    // typed pub sub polled by hand, the over cap refusal, then the same over service threads
    static bool PoseLeg()
    {
        Console.WriteLine("opening nodes...");

        // Single-threaded: both nodes are driven by polling them in the loop below.
        var sub = new RantNode("sub", Local(0, Threading.Manual));
        sub.OnEvent += e => Console.WriteLine("event(sub): " + e);
        var pub = new RantNode("pub", Local(0, Threading.Manual));
        pub.OnEvent += e => Console.WriteLine("event(pub): " + e);

        var qos = new Qos { Reliability = Reliability.Reliable, KeepLast = 8 };
        // the handler gets the value and the envelope: the stamps are checked below
        sub.Subscriber<Reading>("pose", (p, m) =>
        {
            Received = m;
            ReceivedPose = p;
            Console.WriteLine($"recv: [{m.TopicName}] from {m.PublisherName} -> {p}");
            Got.Set();
        }, qos);
        var pubch = pub.Publisher<Reading>("pose", qos);

        var sent = new Reading
        {
            Stamp = 7,
            X = 1.5,
            Y = -2.5,
            Uuid = new byte[] { 1, 2, 3, 4 },
            Frame = "map",
            Tags = new[] { "fast", "ok" },
            Vel = new Velocity { Dx = 0.5f, Dy = 0.25f },
        };

        long captured = Rant.Types.Timestamp.Now() - 5000;   // "true" 5 ms before the send, to read back
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
            var r = ReceivedPose;
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
            var s = pub.Schema(typeof(Reading));
            Console.WriteLine("Reading DSL (for C interop):\n" + s.Dsl);
            if (!s.Dsl.Contains("vel: Velocity")) { ok = false; Console.WriteLine("FAIL: the nested type is not named"); }
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
                pubch.Send(new Reading { Frame = "way-too-long-for-sixteen-bytes" });
                Console.WriteLine("FAIL: over-cap string did not throw");
                ok = false;
            }
            catch (SchemaException) { Console.WriteLine("PASS: over-cap string refused"); }
        }

        pub.Close();
        sub.Close();

        // threaded: both nodes on their service threads, sent from this thread, delivered with
        // no Poll() anywhere
        if (ok)
        {
            var subT = new RantNode("sub", Local(0));
            var pubT = new RantNode("pub", Local(0));
            subT.Subscriber<Reading>("pose", p => { ReceivedPose = p; Got.Set(); }, qos);
            var pubchT = pubT.Publisher<Reading>("pose", qos);
            if (pubT.Poll(0) != (int)SendStatus.State) { Console.WriteLine("FAIL: Poll not refused under the service thread"); ok = false; }
            else
            {
                Got.Reset();
                sent.Stamp = 8;
                deadline = DateTime.UtcNow.AddSeconds(8);
                while (DateTime.UtcNow < deadline && !Got.IsSet)
                {
                    pubchT.Send(sent);
                    Got.Wait(50);
                }
                ok = Got.IsSet && ReceivedPose.Stamp == 8;
                Console.WriteLine(ok ? $"PASS: threaded delivery on the service thread (evictedUnsent={pubT.Stats.EvictedUnsent})"
                                     : "FAIL: threaded delivery");
            }
            pubT.Close();
            subT.Close();
        }

        return ok;
    }

    // Reports on the thread that delivers the update, where Progress<T> would post it away.
    sealed class InlineProgress<T> : IProgress<T>
    {
        private readonly Action<T> _report;
        public InlineProgress(Action<T> report) { _report = report; }
        public void Report(T value) => _report(value);
    }

    // The handle gate: a remote's Dispose waits for a blocking call on another thread, a Dispose
    // inside the call's own callback is refused at once, and a deferred answer after its
    // definition closed is refused.
    static bool HandleGateLeg()
    {
        bool ok = true;
        void Check(string n, bool c) { Console.WriteLine((c ? "  ok  " : " FAIL ") + n); ok &= c; }
        bool WaitFor(Func<bool> cond)
        {
            var end = DateTime.UtcNow.AddSeconds(8);
            while (DateTime.UtcNow < end && !cond()) Thread.Sleep(5);
            return cond();
        }

        var a = new RantNode("ga", Local(15));
        var b = new RantNode("gb", Local(15));
        try
        {
            var slow = a.FunctionDefinition<AddReq, AddRsp>("g/slow", q =>
            {
                Thread.Sleep(300);
                return new AddRsp { Sum = q.A + q.B };
            });
            var rem = b.RemoteFunction<AddReq, AddRsp>("g/slow");
            Check("matched", WaitFor(() => rem.MatchCount > 0));
            var callDone = new ManualResetEventSlim(false);
            CallStatus status = CallStatus.Timeout;
            var caller = new Thread(() =>
            {
                status = rem.TryCall(new AddReq { A = 1, B = 2 }, 3000).Status;
                callDone.Set();
            });
            caller.Start();
            Thread.Sleep(100);
            rem.Dispose();
            bool doneFirst = callDone.IsSet;
            caller.Join();
            Check("a Dispose waits for the blocking call on another thread", doneFirst);
            Check("which completed", status == CallStatus.Ok);
            Check("then the remote answers NoTopic", rem.TryCall(new AddReq { A = 1, B = 2 }, 100).SendStatus == SendStatus.NoTopic);

            var job = a.TaskDefinition<XferReq, XferPrg, XferRsp>("g/job", async (q, ctx) =>
            {
                ctx.Progress(new XferPrg { Done = 1 });
                await Task.Delay(200);
                return new XferRsp { Total = q.Chunks };
            });
            var jr = b.RemoteTask<XferReq, XferPrg, XferRsp>("g/job");
            Check("task matched", WaitFor(() => jr.MatchCount > 0));
            SendStatus inside = SendStatus.Ok;
            bool tried = false;
            var r2 = jr.TryCall(new XferReq { Chunks = 3 }, new InlineProgress<XferPrg>(_ =>
            {
                tried = true;
                try { jr.Dispose(); }
                catch (RantException e) { inside = e.SendStatus; }
            }), 3000);
            Check("a Dispose inside the call's own callback is refused at once", tried && inside == SendStatus.State);
            Check("and the call still completes", r2.Status == CallStatus.Ok);
            jr.Dispose();
            Check("a Dispose after it retires the remote", jr.TryCall(new XferReq { Chunks = 1 }, null, 100).SendStatus == SendStatus.NoTopic);

            Deferred<AddRsp> parked = null;
            var park = a.FunctionDefinition<AddReq, AddRsp>("g/park", (q, req) => { Volatile.Write(ref parked, req.Defer()); });
            var pr = b.RemoteFunction<AddReq, AddRsp>("g/park");
            Check("park matched", WaitFor(() => pr.MatchCount > 0));
            _ = pr.TryCallAsync(new AddReq { A = 1, B = 1 });
            Check("the answer parked", WaitFor(() => Volatile.Read(ref parked) != null));
            park.Dispose();
            Check("a deferred answer after its definition closed is refused", !parked.Complete(new AddRsp { Sum = 2 }));
        }
        finally
        {
            a.Close();
            b.Close();
        }
        Console.WriteLine(ok ? "handle_gate: PASS\n" : "handle_gate: FAIL\n");
        return ok;
    }

    static readonly (string Name, Func<bool> Run)[] Legs =
    {
        ("version", VersionLeg),
        ("layout", LayoutLeg),
        ("round_trip", RoundTrip),
        ("value_roots", ValueRoots),
        ("array_convert", ArrayConvert),
        ("std_types", StdTypes),
        ("pose", PoseLeg),
        ("value_roots_live", ValueRootsLive),
        ("video_live", VideoLive),
        ("patterns", Patterns),
        ("tasks", Tasks),
        ("dispatch", DispatchLeg),
        ("log", LogLeg),
        ("status", StatusLeg),
        ("pull", PullLeg),
        ("reflect", ReflectLeg),
        ("dispose", DisposeLeg),
        ("rebind", RebindLeg),
        ("close_race", CloseRaceLeg),
        ("handle_gate", HandleGateLeg),
    };

    // Every leg runs, so one failure never hides the rest. A throw fails that leg.
    static int Main(string[] args)
    {
        if (args.Length == 1 && args[0] == "--list")
        {
            foreach (var leg in Legs) Console.WriteLine(leg.Name);
            return 0;
        }
        foreach (var a in args)
            if (Array.FindIndex(Legs, l => l.Name == a) < 0)
            {
                Console.Error.WriteLine($"no leg named {a}, --list lists them");
                return 2;
            }
        var failed = new List<string>();
        foreach (var leg in Legs)
        {
            if (args.Length > 0 && Array.IndexOf(args, leg.Name) < 0) continue;
            Console.WriteLine("-- " + leg.Name);
            bool ok;
            try { ok = leg.Run(); }
            catch (Exception e) { Console.WriteLine($"FAIL: {leg.Name} threw {e}"); ok = false; }
            if (!ok) failed.Add(leg.Name);
        }
        Console.WriteLine(failed.Count == 0 ? "RESULT: PASS" : "RESULT: FAIL in " + string.Join(" ", failed));
        return failed.Count == 0 ? 0 : 1;
    }
}
