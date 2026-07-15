/* Example: typed pub/sub over the DART WebSocket bridge from Node.
 *
 * Start the bridge, then run this:
 *   dart_bridge --bind 127.0.0.1 &
 *   node example.mjs                 (or: node example.mjs ws://host:7480)
 *
 * It opens two nodes on the mesh through the bridge -- a publisher and a
 * subscriber -- declares the same typed "telemetry" topic on each, publishes a
 * few messages, and prints them as they arrive. One WebSocket connection = one
 * DART node, so this is exactly what two separate machines would do. */
import { DartClient } from "./dart.mjs";

const url = process.argv[2] ?? "ws://127.0.0.1:7480";

/* One schema, pasted identically on both ends: the wire carries no field names, so
 * a value's meaning is its position. Both nodes must use the same text. */
const SCHEMA = `
    Telemetry {
        seq:     u32,
        battery: f32,
        label:   string<24>,
        pos:     { x: f64, y: f64 }
    }`;

/* interface "127.0.0.1" pins discovery to loopback so the two same-host nodes find
 * each other quickly; drop it to run across a real network. */
const net = { interface: "127.0.0.1" };

const publisher  = await DartClient.connect(url, { name: "publisher",  ...net });
const subscriber = await DartClient.connect(url, { name: "subscriber", ...net });
console.log(`connected to ${url}`);

const out = await publisher.topic("telemetry", "pub", { schema: SCHEMA, reliable: true });
const inn = await subscriber.topic("telemetry", "sub", { schema: SCHEMA, reliable: true });

/* Typed reads by name (dotted path for nested fields); no per-message parsing. */
let received = 0;
inn.onMessage = (m) => {
    console.log(`recv #${m.get("seq")}  battery=${m.get("battery").toFixed(1)}%  ` +
                `label="${m.get("label")}"  pos=(${m.get("pos.x")}, ${m.get("pos.y")})`);
    if (++received >= 5) shutdown();
};

/* Publish on a timer until the subscriber has seen a few (discovery takes ~a second,
 * so the earliest sends may land before the match forms; reliable + KEEP_LAST covers it). */
let seq = 0;
const timer = setInterval(() => {
    out.send({ seq: ++seq, battery: 100 - seq, label: `sample-${seq}`,
               "pos.x": seq * 1.5, "pos.y": seq * -0.5 });
}, 300);

setTimeout(() => { console.error("timeout: no delivery in 15s (is the bridge running?)"); process.exit(1); }, 15000);

async function shutdown() {
    clearInterval(timer);
    await out.drain(1000);          /* let the last messages reach the subscriber */
    publisher.close();
    subscriber.close();
    console.log("done");
    process.exit(0);
}
