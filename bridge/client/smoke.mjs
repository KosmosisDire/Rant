/* End-to-end smoke test against a running bridge: two connections (= two nodes on the
 * mesh), a typed reliable channel between them, publish until discovery matches, then
 * verify every field kind round-trips. Exit 0 = pass.
 *   dart_bridge &         (defaults: ws://127.0.0.1:7480)
 *   node smoke.mjs [ws://host:port] */
import { DartClient } from "./dart.mjs";

const url = process.argv[2] ?? "ws://127.0.0.1:7480";
const SCHEMA = `
    Ping {
        seq:   u32,
        stamp: u64,
        score: i16,
        ok:    bool,
        pos:   f64[3],
        text:  u8[32],
        vel:   { dx: f32, dy: f32 }
    }`;

const fail = (msg) => { console.error(`FAIL: ${msg}`); process.exit(1); };
const eq = (a, b, what) => { if (a !== b) fail(`${what}: ${a} != ${b}`); };

setTimeout(() => fail("timeout: no delivery in 15s"), 15000).unref?.();

const pub = await DartClient.connect(url, { name: "smoke-pub" });
const sub = await DartClient.connect(url, { name: "smoke-sub" });
console.log(`connected: ${pub.name}, ${sub.name}`);

const out = await pub.channel("smoke", "pub", { schema: SCHEMA, reliable: true });
const inn = await sub.channel("smoke", "sub", { schema: SCHEMA, reliable: true });
eq(out.size, inn.size, "schema size");
eq(out.hash, inn.hash, "schema hash");
console.log(`channel 'smoke': ${out.size} B/msg, ${out.fields.size} fields, hash ${out.hash}`);

const got = new Promise((resolve) => { inn.onMessage = resolve; });

/* publish until the subscriber's node has matched us (discovery takes ~a second) */
let seq = 0;
const tick = setInterval(() => {
    out.send({ seq: ++seq, stamp: 12345678901234n, score: -321, ok: true,
               pos: [1.5, -2.5, 3.25], text: "hello bridge", "vel.dx": 0.5, "vel.dy": -0.25 });
}, 200);

const m = await got;

eq(m.senderName, "smoke-pub", "senderName");
if (Number(m.get("seq")) < 1) fail("seq");
eq(m.get("stamp"), 12345678901234n, "u64 stamp");
eq(m.get("score"), -321, "i16 score");
eq(m.get("ok"), true, "bool ok");
const pos = m.get("pos");
eq(pos[0], 1.5, "pos[0]"); eq(pos[1], -2.5, "pos[1]"); eq(pos[2], 3.25, "pos[2]");
eq(m.text("text"), "hello bridge", "text");
eq(m.get("vel.dx"), 0.5, "vel.dx"); eq(m.get("vel.dy"), -0.25, "vel.dy");

console.log(`delivered #${m.get("seq")} from '${m.senderName}'`);

/* a third connection discovers the topic off the peer table and adopts the schema the
 * publisher advertises (it has no schema text of its own) */
const spy = await DartClient.connect(url, { name: "smoke-spy" });
let advert;
while (!advert) {
    const peers = await spy.peersSnapshot();
    advert = peers.flatMap((p) => p.topics ?? []).find((t) => t.name === "smoke" && t.fields);
    if (!advert) await new Promise((r) => setTimeout(r, 200));
}
eq(advert.hash, out.hash, "advertised hash");
const adopted = await spy.channel("smoke", "sub", { adopt: true });
eq(adopted.hash, out.hash, "adopted hash");
const m2 = await new Promise((resolve) => { adopted.onMessage = resolve; });
clearInterval(tick);
eq(m2.text("text"), "hello bridge", "adopted decode");
console.log(`adopted 'smoke' (${adopted.fields.size} fields) and decoded #${m2.get("seq")}`);

const stats = await pub.stats();
console.log("pub stats:", stats);

await out.drain(2000);
pub.close(); sub.close(); spy.close();
console.log("PASS");
process.exit(0);
