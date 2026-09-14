/* The example and end to end check: pub sub, a function, a task and a variable over the
 * bridge from Node. Start ramble_bridge --bind 127.0.0.1, then node example.mjs [ws url]. */
import { RambleNode, MetaSection } from "../../dist/ramble.mjs";

const url = process.argv[2] ?? "ws://127.0.0.1:7480";

/* Schemas are DSL text pasted identically on both ends. Timestamp, Double2 and Color are
 * standard types (docs/stdtypes.md), reported to the client as the field's named key. */
const TELEMETRY = `Telemetry { seq: u32, when: Timestamp, battery: f32, at: Double2 }`;
const ADD_REQ   = `AddReq { a: i32, b: i32 }`;
const ADD_RSP   = `AddRsp { sum: i32 }`;
const CONFIG    = `Config { rate_hz: u32, label: string<24> }`;

/* interface "127.0.0.1" pins discovery to loopback so the two same host nodes find each
 * other quickly. Drop it to run across a real network. */
const net = { interface: "127.0.0.1" };

const fail = setTimeout(() => { console.error("timeout: no progress in 30s (is the bridge running?)"); process.exit(1); }, 30000);

const robot = await RambleNode.connect(url, { name: "robot", ...net,
    onEvent: (e) => { if (e.event === "error") console.warn("robot:", e.text); } });
const dash  = await RambleNode.connect(url, { name: "dashboard", ...net, fetch_details: true,
    onEvent: (e) => { if (e.event === "error") console.warn("dash:", e.text); } });
console.log(`connected to ${url} (data over ${robot.transport})`);

/* ---- the robot hosts everything -------------------------------------------------- */
const telemetry = await robot.publisher("telemetry", TELEMETRY, { reliable: true });
let reqWrittenUs = 0;
const add = await robot.functionDefinition("add", ADD_REQ, ADD_RSP,
    (req, info) => { reqWrittenUs = info.writtenUs;                  /* the caller's source stamp */
                     return { sum: req.a + req.b }; });        /* return value = the reply */
const config = await robot.variableDefinition("config", CONFIG,
    { initial: { rate_hz: 50, label: "default" } });

/* ---- the dashboard uses them ------------------------------------------------------ */
let sawTelemetry, msgWrittenUs = 0;
const gotTelemetry = new Promise((res) => { sawTelemetry = res; });
await dash.subscriber("telemetry", TELEMETRY, (v, msg) => {
    msgWrittenUs = msg.writtenUs;   /* the publisher's wall clock when it sent (0 = opted out) */
    console.log(`telemetry #${v.seq}  battery=${v.battery.toFixed(1)}%  ` +
                `at=(${v.at.x}, ${v.at.y})  when=${v.when}  from peer ${msg.publisher}`);
    sawTelemetry(v);
});
const addRemote = await dash.remoteFunction("add", ADD_REQ, ADD_RSP);
const configRemote = await dash.remoteVariable("config", CONFIG);

/* let discovery + matching converge on both nodes before exercising anything */
await robot.settle(5000);
await dash.settle(5000);

/* pub/sub */
telemetry.send({ seq: 1, when: Date.now() * 1000, battery: 87.5, at: { x: 1.5, y: -0.5 } });
const t = await gotTelemetry;
if (t.seq !== 1 || t.at.x !== 1.5 || t.when === 0) throw new Error("telemetry mismatch");

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

/* ---- reflect: a handle typed by the mesh, no DSL on this end ---------------------- */
let sawMirror;
const gotMirror = new Promise((res) => { sawMirror = res; });
const mirror = await dash.subscriber("telemetry", null, (v) => { if (v.seq === 2) sawMirror(v); }, { reflect: true });
if (!mirror.topic.reflected) { await dash.settle(5000); await mirror.topic.refresh(); }
if (!mirror.topic.reflected) throw new Error("reflect found no telemetry schema on the mesh");
console.log(`reflected telemetry: ${[...mirror.topic.layout.fields.keys()].join(", ")} (hash ${mirror.topic.layout.hash})`);
if (mirror.topic.layout.hash !== telemetry.topic.layout.hash) throw new Error("reflected schema differs from the publisher's");
telemetry.send({ seq: 2, when: Date.now() * 1000, battery: 80, at: { x: 2, y: 3 } });
const mv = await gotMirror;
if (mv.at.y !== 3 || mv.battery !== 80) throw new Error("reflected subscriber decoded wrong");

/* ---- the mesh folded: every entity on the network with its provider's schema -------- */
const mesh = await dash.mesh();
const meshTelemetry = mesh.entities.find((e) => e.kind === "topic" && e.name === "telemetry");
console.log(`mesh: ${mesh.entities.length} entities (epoch ${mesh.epoch}); telemetry providers=${meshTelemetry?.providers} ` +
            `consumers=${meshTelemetry?.consumers} from=${meshTelemetry?.from}`);
if (!meshTelemetry?.providers) throw new Error("the mesh does not show the telemetry provider");
const foundConfig = await dash.meshFind("variable", "config");
if (!foundConfig?.schema?.fields?.some((f) => f.path === "rate_hz")) throw new Error("meshFind config: no schema");

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

/* A reflected peer schema carries the same rows a create reply does, named included, or a
 * client would rebuild at: Double2 as an anonymous struct every named reader refuses. */
const robotTelemetry = theirs.find((e) => e.kind === "topic" && e.name === "telemetry");
if (!robotTelemetry?.schema) throw new Error("expected the robot's telemetry schema");
const namedRows = robotTelemetry.schema.fields.filter((f) => f.named);
console.log(`  telemetry named types: ` +
            namedRows.map((f) => `${f.path}: ${f.named}`).join(", "));
if (!robotTelemetry.schema.fields.some((f) => f.path === "when" && f.named === "Timestamp"))
    throw new Error("peer reflection dropped the Timestamp name");
if (!robotTelemetry.schema.fields.some((f) => f.path === "at" && f.named === "Double2"))
    throw new Error("peer reflection dropped the Double2 name");

const snap = await dash.meta(robotPeer.id, MetaSection.Node | MetaSection.Topics);
console.log(`robot @ramble/meta: status=${snap.status} valid=${snap.valid}` +
            (snap.valid ? `  name=${snap.info.node?.name} topics=${snap.info.node?.topics}` : ""));
if (!snap.valid) throw new Error("meta query failed");

/* ---- on_write: every applied write, even a byte-identical re-set ------------------ */
const STATUS = `Status { state: u8 }`;
let writes = 0, changes = 0, writeWrittenUs = 0;
const status = await robot.variableDefinition("status", STATUS, { initial: { state: 1 }, onWrite: true });
status.onChange(() => { changes++; });   /* replays the initial once, then only on change */
status.onWrite((_v, info) => { writes++; writeWrittenUs = info.writtenUs; });
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
if (flagPub.topic.layout.hash !== "ee90234f61d2520b") throw new Error("bare `bool` hash is not canonical");
if (!(await gainRemote.wait(5000))) throw new Error("no bare-typed variable value");
if (gainRemote.get() !== 1.25) throw new Error("bare-typed variable value mismatch");
gainRemote.set(2.5);
for (let i = 0; i < 50 && gain.get() !== 2.5; i++) await new Promise((res) => setTimeout(res, 100));
if (gain.get() !== 2.5) throw new Error("bare-typed variable set did not replicate");
console.log(`bare variable: gain=${gain.get()} (set through the remote)`);

/* ---- tasks: a function with progress and cancellation ----------------------------- */
const XREQ = `Xfer { total: u32, delay_ms: u32 }`;
const XPRG = `Prog { done: u32 }`;
const XRSP = `Sum { bytes: u32 }`;
const sleep = (ms) => new Promise((res) => setTimeout(res, ms));
await robot.taskDefinition("transfer", XREQ, XPRG, XRSP, async (req, ctx) => {
    for (let i = 1; i <= req.total; i++) {
        ctx.signal.throwIfAborted();          /* honor a cancel: throw its AbortError */
        await sleep(req.delay_ms);
        ctx.progress({ done: i });
    }
    return { bytes: req.total };              /* return value = the ok response */
});
await robot.taskDefinition("fixed", XREQ, XPRG, XRSP,
    async (req) => { await sleep(50); return { bytes: req.total }; }, { no_cancel: true });
const xfer  = await dash.remoteTask("transfer", XREQ, XPRG, XRSP);
const fixed = await dash.remoteTask("fixed", XREQ, XPRG, XRSP);
await robot.settle(5000);
await dash.settle(5000);

/* a full run: the RUNNING ack (null) first, progress in order, one ok result */
const seen = [];
const run1 = xfer.call({ total: 3, delay_ms: 20 });
run1.onProgress((p) => seen.push(p === null ? "running" : p.done));
const out1 = await run1.result;
console.log(`transfer: progress=[${seen.join(", ")}] status=${out1.status} bytes=${out1.value?.bytes}`);
if (!out1.ok || out1.value.bytes !== 3) throw new Error("task result mismatch");
if (seen[0] !== "running") throw new Error("no RUNNING ack first");
if (seen.slice(1).join(",") !== "1,2,3") throw new Error("progress out of order");

/* cancel round trip: the handler observes ctx.signal, the caller sees "cancelled" */
const run2 = xfer.call({ total: 1000, delay_ms: 50 });
await new Promise((res) => run2.onProgress((p) => { if (p !== null) res(); }));   /* mid-run */
const verdict = await run2.cancel();
const out2 = await run2.result;
console.log(`cancel: verdict=${verdict} status=${out2.status} message="${out2.message}"`);
if (verdict !== "ok") throw new Error(`cancel not accepted: ${verdict}`);
if (out2.status !== "cancelled") throw new Error("cancel was not honored");

/* no_cancel: cancel is refused locally, the task still completes ok */
const run3 = fixed.call({ total: 1, delay_ms: 50 });
const refusal = await run3.cancel();
const out3 = await run3.result;
console.log(`no_cancel: verdict=${refusal} status=${out3.status}`);
if (refusal !== "no_cancel") throw new Error(`expected a no_cancel refusal, got ${refusal}`);
if (!out3.ok) throw new Error("no_cancel task did not complete ok");

/* ---- source timestamps: every delivery surface carries the sender's wall clock ----- */
const nowUs = Date.now() * 1000;
const stamps = { message: msgWrittenUs, call: r.writtenUs, request: reqWrittenUs,
                 variable: writeWrittenUs };
console.log("written_us per surface: " + Object.entries(stamps)
    .map(([k, v]) => `${k}=${v ? `${((nowUs - v) / 1000).toFixed(1)}ms ago` : "0"}`).join("  "));
for (const [what, us] of Object.entries(stamps)) {
    if (!us) throw new Error(`no source timestamp on the ${what} surface`);
    if (Math.abs(nowUs - us) > 60e6) throw new Error(`${what} written_us is not a wall clock: ${us}`);
}

console.log("done");
clearTimeout(fail);
robot.close();
dash.close();
process.exit(0);
