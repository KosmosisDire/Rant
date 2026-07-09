// DART C# subscriber: subscribes to 'tick' and reports the received message rate.
// Pair with csharp/publisher or python/publisher.py. Manual single-threaded poll.
//
//   dotnet run --project csharp/subscriber [-- <seconds> <interface>]

using System;
using System.Diagnostics;
using Dart;

[DartSchema]
struct Tick
{
    [DartField("seq")]   public ulong Seq;
    [DartField("t_us")]  public ulong TUs;
    [DartField("value")] public double Value;
}

static class Program
{
    static long _count = 0;

    static int Main(string[] args)
    {
        double seconds = args.Length > 0 ? double.Parse(args[0]) : 0.0;  // 0 => run forever
        string iface = args.Length > 1 ? args[1] : null;
        var opts = new NodeOptions();
        if (iface != null) opts.MulticastInterface = iface;

        var node = Node.Open("cs-subscriber", _ => _count++,
            e => Console.Error.WriteLine("event: " + e), opts);
        node.CreateChannel("tick", Role.SubOnly, typeof(Tick));
        Console.WriteLine("subscribing to 'tick' (manual poll), reporting received Hz (Ctrl+C to stop)");

        bool stop = false;
        Console.CancelKeyPress += (o, e) => { e.Cancel = true; stop = true; };

        var sw = Stopwatch.StartNew();
        long lastCount = 0;
        double lastReport = 0;
        while (!stop)
        {
            node.Poll(1);                       // block up to 1ms; wakes on RX, yields the CPU
            double now = sw.Elapsed.TotalSeconds;
            if (now - lastReport >= 1.0)
            {
                double rate = (_count - lastCount) / (now - lastReport);
                Console.WriteLine($"received={_count}  rate={rate:F0} Hz");
                lastCount = _count; lastReport = now;
            }
            if (seconds > 0 && now >= seconds) break;
        }
        node.Close();
        return 0;
    }
}
