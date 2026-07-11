// DART C# test: two nodes, reliable typed pub/sub, on one host.
// Exercises compile-on-first-use, discovery/match, and schema reflection (encode a
// struct on one node, decode it back on the other), including capped strings and
// string arrays. Exit 0 = crossed and matched.
//
//   dotnet run --project csharp/test

using System;
using System.Threading;
using Dart;

struct Twist { public float Dx; public float Dy; }

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

    static int Main()
    {
        Console.WriteLine("opening nodes (first run compiles the embedded C, please wait)...");
        var netOpts = new NodeOptions { Domain = 42, MulticastInterface = "127.0.0.1" };

        var sub = new Node("sub",
            onMessage: m =>
            {
                Received = m;
                Console.WriteLine($"recv: [{m.ChannelName}] from {m.SenderName} -> {m.As<Pose>()}");
                Got.Set();
            },
            onEvent: e => Console.WriteLine("event(sub): " + e),
            netOpts);

        var pub = new Node("pub", null, e => Console.WriteLine("event(pub): " + e),
            new NodeOptions { Domain = 42, MulticastInterface = "127.0.0.1" });

        var qos = new Qos { Reliability = Reliability.Reliable, KeepLast = 8 };
        new Channel<Pose>(sub, "pose", Role.SubOnly, qos);
        var pubch = new Channel<Pose>(pub, "pose", Role.PubOnly, qos);

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
