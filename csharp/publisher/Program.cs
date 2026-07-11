// DART C# publisher: streams a typed 'tick' message at 1000 Hz on the DEFAULT
// interface and domain (0), so any subscriber on the LAN (any language) can receive
// it. Uses the same schema/topic as python/publisher.py, so the two interoperate
// (the [DartField] overrides give lowercase wire names matching the Python fields).
//
//   dotnet run --project csharp/publisher [-- <seconds>]

using System;
using System.Diagnostics;
using System.Threading;
using Dart;

struct Tick
{
    [DartField("seq")]   public ulong Seq;
    [DartField("t_us")]  public ulong TUs;
    [DartField("value")] public double Value;
}

static class Program
{
    const int Hz = 1000;

    static int Main(string[] args)
    {
        double seconds = args.Length > 0 ? double.Parse(args[0]) : 0.0;  // 0 => run forever
        string iface = args.Length > 1 ? args[1] : null;                 // optional: pin the interface
        var opts = new NodeOptions();
        if (iface != null) opts.MulticastInterface = iface;
        var node = new Node("cs-publisher", null, e => Console.Error.WriteLine("event: " + e), opts);
        // keep_last deep enough that a small per-loop burst is not evicted before it flushes.
        var ch = new Channel<Tick>(node, "tick", Role.PubOnly, new Qos { KeepLast = 64 });
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
                ch.Send(new Tick { Seq = seq, TUs = (ulong)(now * 1e6), Value = Math.Sin(seq * 0.01) });
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
