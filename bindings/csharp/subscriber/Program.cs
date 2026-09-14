// A 'tick' subscriber reporting the received rate, the pair of bindings/csharp/publisher or
// bindings/python/publisher.py, driven by a manual poll. Args: [seconds] [interface].

using System;
using System.Diagnostics;
using Rant;

struct Tick
{
    [RantField("seq")]   public ulong Seq;
    [RantField("when")] [RantTypeName("Timestamp")] public long When;   // Unix-epoch us, UTC
    [RantField("value")] public double Value;
    [RantField("at")]    public Pose At;                                // meters + a quaternion
}

static class Program
{
    static long _count = 0;
    static Tick _last;

    static int Main(string[] args)
    {
        double seconds = args.Length > 0 ? double.Parse(args[0]) : 0.0;  // 0 = run forever
        string iface = args.Length > 1 ? args[1] : null;

        var node = new RantNode("cs-subscriber", m => { _count++; _last = m.As<Tick>(); },
            e => Console.Error.WriteLine("event: " + e), multicastInterface: iface);
        new Topic<Tick>(node, "tick", Role.SubOnly);
        Console.WriteLine("subscribing to 'tick' (manual poll), reporting received Hz (Ctrl+C to stop)");

        bool stop = false;
        Console.CancelKeyPress += (o, e) => { e.Cancel = true; stop = true; };

        var sw = Stopwatch.StartNew();
        long lastCount = 0;
        double lastReport = 0;
        while (!stop)
        {
            node.Poll(1);                       // block up to 1 ms, wakes on receive and yields the CPU
            double now = sw.Elapsed.TotalSeconds;
            if (now - lastReport >= 1.0)
            {
                double rate = (_count - lastCount) / (now - lastReport);
                // `when` is the publisher's wall clock in the same units everywhere, so the
                // difference against ours is one-way latency plus clock skew.
                double ageMs = _last.When != 0 ? (Std.Now() - _last.When) / 1000.0 : 0.0;
                var p = _last.At.Position;
                Console.WriteLine($"received={_count}  rate={rate:F0} Hz  age={ageMs:F1} ms  " +
                                  $"at=({p.X:F2}, {p.Y:F2}, {p.Z:F2})");
                lastCount = _count; lastReport = now;
            }
            if (seconds > 0 && now >= seconds) break;
        }
        node.Close();
        return 0;
    }
}
