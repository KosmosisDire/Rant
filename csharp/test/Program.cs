// DART C# test: two nodes, reliable typed pub/sub, on one host.
// Exercises compile-on-first-use, discovery/match, and schema reflection (encode a
// struct on one node, decode it back on the other), including capped strings and
// string arrays. Exit 0 = crossed and matched.
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

static class Program
{
    static readonly ManualResetEventSlim Got = new ManualResetEventSlim(false);
    static Message Received;

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

    static int Main()
    {
        if (!RoundTrip()) return 1;
        Console.WriteLine("opening nodes (first run compiles the embedded C, please wait)...");
        var netOpts = new NodeOptions { Domain = 42, MulticastInterface = "127.0.0.1" };

        var sub = new Node("sub",
            onMessage: m =>
            {
                Received = m;
                Console.WriteLine($"recv: [{m.TopicName}] from {m.PublisherName} -> {m.As<Pose>()}");
                Got.Set();
            },
            onEvent: e => Console.WriteLine("event(sub): " + e),
            netOpts);

        var pub = new Node("pub", null, e => Console.WriteLine("event(pub): " + e),
            new NodeOptions { Domain = 42, MulticastInterface = "127.0.0.1" });

        var qos = new Qos { Reliability = Reliability.Reliable, KeepLast = 8 };
        new Topic<Pose>(sub, "pose", Role.SubOnly, qos);
        var pubch = new Topic<Pose>(pub, "pose", Role.PubOnly, qos);

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
        return ok ? 0 : 1;
    }
}
