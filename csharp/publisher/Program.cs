// A typed 'tick' publisher at 1000 Hz on the default interface and domain, sharing its
// schema with python/publisher.py. The [RambleField] overrides give the lowercase wire names.

using System;
using System.Diagnostics;
using System.Threading;
using Ramble;

struct Tick
{
    [RambleField("seq")]   public ulong Seq;
    [RambleField("when")] [RambleTypeName("Timestamp")] public long When;   // Unix-epoch us, UTC
    [RambleField("value")] public double Value;
    [RambleField("at")]    public Pose At;                                // meters + a quaternion
}

static class Program
{
    const int Hz = 1000;

    static int Main(string[] args)
    {
        double seconds = args.Length > 0 ? double.Parse(args[0]) : 0.0;  // 0 = run forever
        string iface = args.Length > 1 ? args[1] : null;                 // optional: pin the interface
        var node = new RambleNode("cs-publisher", null, e => Console.Error.WriteLine("event: " + e),
                            multicastInterface: iface);
        // keep_last deep enough that a small per-loop burst is not evicted before it flushes.
        var ch = new Topic<Tick>(node, "tick", Role.PubOnly, new Qos { KeepLast = 64 });
        Console.WriteLine($"publishing 'tick' at {Hz} Hz on the default interface, domain 0 (Ctrl+C to stop)");
        using (var s = new Schema(typeof(Tick)))
            Console.WriteLine("schema: " + string.Join(" ", s.Dsl.Split((char[])null, StringSplitOptions.RemoveEmptyEntries)));

        bool stop = false;
        Console.CancelKeyPress += (o, e) => { e.Cancel = true; stop = true; };

        double period = 1.0 / Hz;
        var sw = Stopwatch.StartNew();
        ulong seq = 0, lastSeq = 0;
        double lastReport = 0;
        while (!stop)
        {
            double now = sw.Elapsed.TotalSeconds;
            long due = (long)(now / period);                   // ticks that should exist by now
            while ((long)seq < due)
            {
                double angle = seq * 0.01;
                ch.Send(new Tick {
                    Seq = seq, When = Std.Now(), Value = Math.Sin(angle),
                    At = new Pose { Position = new Double3 { X = Math.Cos(angle), Y = Math.Sin(angle) },
                                    Orientation = Std.IdentityRotation() } });
                seq++;
            }
            node.Poll(0);                                      // non-blocking: flush the burst + service RX
            if (now - lastReport >= 1.0)
            {
                double rate = (seq - lastSeq) / (now - lastReport);
                Console.WriteLine($"seq={seq}  rate={rate:F0} Hz  subscribers={ch.MatchCount()}");
                lastReport = now; lastSeq = seq;
            }
            if (seconds > 0 && now >= seconds) break;
            Thread.Sleep(1);
        }
        node.Close();
        return 0;
    }
}
