/* Example + E2E check: pub/sub, a function, a variable and a signal over the DART
 * WebSocket bridge from Node.
 *
 * Start the bridge, then run this:
 *   dart_bridge --bind 127.0.0.1 &
 *   node example.mjs                 (or: node example.mjs ws://host:7480)
 *
 * It opens two nodes on the mesh through the bridge (a "robot" hosting the
 * definitions and a "dashboard" using them), exercises every pattern, prints the
 * results, and exits 0 on success. One WebSocket connection = one DART node, so
 * this is exactly what two separate machines would do. */
import { DartNode } from "../../dist/dart.mjs";

const url = process.argv[2] ?? "ws://127.0.0.1:7480";

/* Schemas are DSL text, pasted identically on both ends: the wire carries no field
 * names, so a value's meaning is its position. */
const TELEMETRY = `Telemetry { seq: u32, battery: f32, pos: { x: f64, y: f64 } }`;
const ADD_REQ   = `AddReq { a: i32, b: i32 }`;
const ADD_RSP   = `AddRsp { sum: i32 }`;
const CONFIG    = `Config { rate_hz: u32, label: string<24> }`;
const ALERT     = `Alert { level: u8, what: string<48> }`;

/* interface "127.0.0.1" pins discovery to loopback so the two same-host nodes find
 * each other quickly; drop it to run across a real network. */
const net = { interface: "127.0.0.1" };

const fail = setTimeout(() => { console.error("timeout: no progress in 20s (is the bridge running?)"); process.exit(1); }, 20000);

const robot = await DartNode.connect(url, { name: "robot", ...net,
    onEvent: (e) => { if (e.event === "error") console.warn("robot:", e.text); } });
const dash  = await DartNode.connect(url, { name: "dashboard", ...net,
    onEvent: (e) => { if (e.event === "error") console.warn("dash:", e.text); } });
console.log(`connected to ${url}`);

/* ---- the robot hosts everything -------------------------------------------------- */
const telemetry = await robot.publisher("telemetry", TELEMETRY, { reliable: true });
const add = await robot.functionDefinition("add", ADD_REQ, ADD_RSP,
    (req) => ({ sum: req.a + req.b }));                       /* return value = the reply */
const config = await robot.variableDefinition("config", CONFIG,
    { initial: { rate_hz: 50, label: "default" } });
const alerts = await robot.signal("alert", ALERT);            /* emit side */

/* ---- the dashboard uses them ------------------------------------------------------ */
let sawTelemetry;
const gotTelemetry = new Promise((res) => { sawTelemetry = res; });
await dash.subscriber("telemetry", TELEMETRY, (v, msg) => {
    console.log(`telemetry #${v.seq}  battery=${v.battery.toFixed(1)}%  ` +
                `pos=(${v.pos.x}, ${v.pos.y})  from peer ${msg.publisher}`);
    sawTelemetry(v);
});
const addRemote = await dash.remoteFunction("add", ADD_REQ, ADD_RSP);
const configRemote = await dash.remoteVariable("config", CONFIG);
let sawAlert;
const gotAlert = new Promise((res) => { sawAlert = res; });
await dash.signal("alert", ALERT, (v) => {
    console.log(`alert level ${v.level}: "${v.what}"`);
    sawAlert(v);
});

/* let discovery + matching converge on both nodes before exercising anything */
await robot.settle(5000);
await dash.settle(5000);

/* pub/sub */
telemetry.send({ seq: 1, battery: 87.5, pos: { x: 1.5, y: -0.5 } });
const t = await gotTelemetry;
if (t.seq !== 1 || t.pos.x !== 1.5) throw new Error("telemetry mismatch");

/* function: request/response with exactly one reply */
const r = await addRemote.call({ a: 2, b: 3 });
console.log(`add(2, 3) -> status=${r.status} sum=${r.value?.sum} (provider ${r.provider})`);
if (!r.ok || r.value.sum !== 5) throw new Error("call failed");

/* variable: the dashboard waits for the replicated value, then writes it back */
if (!(await configRemote.wait(5000))) throw new Error("no variable value");
console.log(`config = rate_hz=${configRemote.get().rate_hz} label="${configRemote.get().label}"`);
configRemote.set({ rate_hz: 100, label: "from-dash" });
for (let i = 0; i < 50 && config.get()?.rate_hz !== 100; i++)   /* owner-side echo */
    await new Promise((res) => setTimeout(res, 100));
if (config.get()?.rate_hz !== 100) throw new Error("variable set did not replicate");
console.log(`owner sees rate_hz=${config.get().rate_hz} label="${config.get().label}"`);

/* signal: fire-and-forget event, robot -> dashboard */
alerts.emit({ level: 2, what: "low battery" });
await gotAlert;

console.log("done");
clearTimeout(fail);
robot.close();
dash.close();
process.exit(0);
