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
import { DartNode, MetaSection } from "../../dist/dart.mjs";

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
const dash  = await DartNode.connect(url, { name: "dashboard", ...net, fetch_details: true,
    onEvent: (e) => { if (e.event === "error") console.warn("dash:", e.text); } });
console.log(`connected to ${url}`);

/* ---- the robot hosts everything -------------------------------------------------- */
const telemetry = await robot.publisher("telemetry", TELEMETRY, { reliable: true });
let reqSentUs = 0;
const add = await robot.functionDefinition("add", ADD_REQ, ADD_RSP,
    (req, info) => { reqSentUs = info.sentUs;                  /* the caller's source stamp */
                     return { sum: req.a + req.b }; });        /* return value = the reply */
const config = await robot.variableDefinition("config", CONFIG,
    { initial: { rate_hz: 50, label: "default" } });
const alerts = await robot.signal("alert", ALERT);            /* emit side */

/* ---- the dashboard uses them ------------------------------------------------------ */
let sawTelemetry, msgSentUs = 0;
const gotTelemetry = new Promise((res) => { sawTelemetry = res; });
await dash.subscriber("telemetry", TELEMETRY, (v, msg) => {
    msgSentUs = msg.sentUs;   /* the publisher's wall clock when it sent (0 = opted out) */
    console.log(`telemetry #${v.seq}  battery=${v.battery.toFixed(1)}%  ` +
                `pos=(${v.pos.x}, ${v.pos.y})  from peer ${msg.publisher}`);
    sawTelemetry(v);
});
const addRemote = await dash.remoteFunction("add", ADD_REQ, ADD_RSP);
const configRemote = await dash.remoteVariable("config", CONFIG);
let sawAlert, alertSentUs = 0;
const gotAlert = new Promise((res) => { sawAlert = res; });
await dash.signal("alert", ALERT, (v, info) => {
    alertSentUs = info.sentUs;
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

/* ---- introspection: query the mesh through the bridge (pull-only) ----------------- */
const peers = await dash.peers();
console.log(`dashboard sees ${peers.length} peer(s): ` +
            peers.map((p) => `${p.name}@${p.address}${p.active ? "" : " (dormant)"}`).join(", "));
const robotPeer = peers.find((p) => p.name === "robot");
if (!robotPeer) throw new Error("robot not in the peer table");

const mine = await dash.entities();
console.log(`dashboard hosts ${mine.length} entit(y|ies): ` +
            mine.map((e) => `${e.kind}:${e.name}`).join(", "));

const theirs = await dash.peerEntities(robotPeer.id);
console.log(`robot advertises: ` + theirs.map((e) =>
    `${e.kind}:${e.name}${e.provides ? " (provides)" : ""}`).join(", "));
const robotConfig = theirs.find((e) => e.kind === "variable" && e.name === "config");
if (!robotConfig) throw new Error("expected the robot's config variable in its entities");

/* schema field reflection: render a discovered topic's types with no shared DSL */
if (!robotConfig.schema) throw new Error("expected config's schema field table (fetch_details?)");
console.log(`  config schema (${robotConfig.schema.fields.length} fields):`);
for (const f of robotConfig.schema.fields)
    console.log(`    ${f.path}: ${f.kind}${f.cap ? `<${f.cap}>` : ""} @${f.offset}`);
if (!robotConfig.schema.fields.some((f) => f.path === "rate_hz" && f.kind === "u32"))
    throw new Error("config schema fields did not reflect");

const snap = await dash.meta(robotPeer.id, MetaSection.Node | MetaSection.Topics);
console.log(`robot @dart/meta: status=${snap.status} valid=${snap.valid}` +
            (snap.valid ? `  name=${snap.info.node?.name} topics=${snap.info.node?.topics}` : ""));
if (!snap.valid) throw new Error("meta query failed");

/* ---- on_write: every applied write, even a byte-identical re-set ------------------ */
const STATUS = `Status { state: u8 }`;
let writes = 0, changes = 0, writeSentUs = 0;
const status = await robot.variableDefinition("status", STATUS, { initial: { state: 1 }, onWrite: true });
status.onChange(() => { changes++; });   /* replays the initial once, then only on change */
status.onWrite((_v, info) => { writes++; writeSentUs = info.sentUs; });
status.set({ state: 1 });                /* byte-identical: a write, not a change */
status.set({ state: 2 });                /* a real change: both fire */
await new Promise((res) => setTimeout(res, 200));
console.log(`status writes=${writes} changes=${changes} (writes > changes: re-sets fire on_write only)`);
if (writes < 2) throw new Error("on_write did not fire for every write");

/* ---- bare-type schemas: the whole schema is one type, the message IS that value ---- */
let sawFlag, sawTemp;
const gotFlag = new Promise((res) => { sawFlag = res; });
const gotTemp = new Promise((res) => { sawTemp = res; });
const flagPub = await robot.publisher("estop", "bool", { reliable: true });
await dash.subscriber("estop", "bool", (v) => sawFlag(v));
const tempPub = await robot.publisher("temps", "f32[]", { reliable: true });
await dash.subscriber("temps", "f32[]", (v) => sawTemp(v));
const gain = await robot.variableDefinition("gain", "f64", { initial: 1.25 });
const gainRemote = await dash.remoteVariable("gain", "f64");
await robot.settle(5000);
await dash.settle(5000);
flagPub.send(true);                       /* not { "": true }: the bare value */
tempPub.send([36.5, 34.25]);
const flag = await gotFlag, temps = await gotTemp;
console.log(`bare roots: estop=${flag}  temps=[${temps.join(", ")}]  ` +
            `estop schema hash=${flagPub.topic.layout.hash}`);
if (flag !== true) throw new Error("bare bool did not arrive as true");
if (temps.length !== 2 || Math.abs(temps[0] - 36.5) > 1e-3) throw new Error("bare f32[] mismatch");
if (flagPub.topic.layout.hash !== "b1edca4f3f7a622a") throw new Error("bare `bool` hash is not canonical");
if (!(await gainRemote.wait(5000))) throw new Error("no bare-typed variable value");
if (gainRemote.get() !== 1.25) throw new Error("bare-typed variable value mismatch");
gainRemote.set(2.5);
for (let i = 0; i < 50 && gain.get() !== 2.5; i++) await new Promise((res) => setTimeout(res, 100));
if (gain.get() !== 2.5) throw new Error("bare-typed variable set did not replicate");
console.log(`bare variable: gain=${gain.get()} (set through the remote)`);

/* ---- source timestamps: every delivery surface carries the sender's wall clock ----- */
const nowUs = Date.now() * 1000;
const stamps = { message: msgSentUs, call: r.sentUs, request: reqSentUs,
                 signal: alertSentUs, variable: writeSentUs };
console.log("sent_us per surface: " + Object.entries(stamps)
    .map(([k, v]) => `${k}=${v ? `${((nowUs - v) / 1000).toFixed(1)}ms ago` : "0"}`).join("  "));
for (const [what, us] of Object.entries(stamps)) {
    if (!us) throw new Error(`no source timestamp on the ${what} surface`);
    if (Math.abs(nowUs - us) > 60e6) throw new Error(`${what} sent_us is not a wall clock: ${us}`);
}

console.log("done");
clearTimeout(fail);
robot.close();
dash.close();
process.exit(0);
