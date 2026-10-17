// The C# side of an interop pair: `pub <seconds>` publishes the Tick schema every language's
// tick program shares, `sub <seconds> <publisher>` exits 0 once twenty ticks from that node
// decoded right and 1 on a wrong one or when the seconds run out. Loopback, on RANT_DOMAIN.

using System;
using System.Diagnostics;
using Rant;
using Rant.Types;

struct Tick
{
    public ulong Seq;
    [RantTypeName("Timestamp")] public long When;
    public double Value;
    public Transform At;
}

static class Program
{
    const int TickWant = 20;

    static bool Near(double a, double b) => Math.Abs(a - b) < 1e-9;

    static int Main(string[] args)
    {
        bool sub = args.Length > 0 && args[0] == "sub";
        double seconds = args.Length > 1 ? double.Parse(args[1], System.Globalization.CultureInfo.InvariantCulture) : 10.0;
        string from = args.Length > 2 ? args[2] : "";
        int good = 0, bad = 0;
        var node = new RantNode(sub ? "cs-tick-sub" : "cs-tick-pub",
                                new NodeOptions { MulticastInterface = "127.0.0.1", Threading = Threading.Manual });
        var qos = new Qos { Reliability = Reliability.Reliable, KeepLast = 64 };
        var clock = Stopwatch.StartNew();
        try
        {
            if (sub)
            {
                node.Subscriber<Tick>("tick", (t, m) =>
                {
                    double angle = t.Seq * 0.01;
                    var p = t.At.Pose.Position;
                    if (m.PublisherName == from && t.When > 0 && Near(t.Value, Math.Sin(angle)) && Near(p.X, Math.Cos(angle))
                        && Near(p.Y, Math.Sin(angle))) good++;
                    else if (bad++ == 0) Console.WriteLine($"FAIL: tick {t.Seq} decoded wrong");
                }, qos);
                while (clock.Elapsed.TotalSeconds < seconds && bad == 0 && good < TickWant) node.Poll(2);
                bool ok = good >= TickWant && bad == 0;
                Console.WriteLine($"{(ok ? "PASS" : "FAIL")}: {good} ticks decoded right, {bad} wrong");
                return ok ? 0 : 1;
            }
            var topic = node.Publisher<Tick>("tick", qos);
            for (ulong seq = 0; clock.Elapsed.TotalSeconds < seconds;)
            {
                if (topic.MatchCount > 0)
                {
                    double angle = seq * 0.01;
                    var at = new Transform();
                    at.Pose.Position.X = Math.Cos(angle);
                    at.Pose.Position.Y = Math.Sin(angle);
                    topic.Send(new Tick { Seq = seq, When = Timestamp.Now(), Value = Math.Sin(angle), At = at });
                    seq++;
                }
                node.Poll(2);
            }
            return 0;
        }
        finally
        {
            node.Close();
        }
    }
}
