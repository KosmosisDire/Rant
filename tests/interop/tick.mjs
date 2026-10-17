// The TypeScript side of an interop pair: `tick.mjs pub <seconds>` publishes the Tick schema every
// language's tick program shares, `tick.mjs sub <seconds> <publisher>` exits 0 once twenty ticks
// from that node decoded right and 1 on a wrong one or when the seconds run out. Loopback, on
// RANT_DOMAIN.

import { openNode, struct, u64, f64, types } from "../../bindings/web/dist/rant.js";

const TICK_WANT = 20;
const [mode, secs, sender] = process.argv.slice(2);
const sub = mode === "sub";
const Tick = struct("Tick", { seq: u64, when: types.Timestamp, value: f64, at: types.Transform });
const near = (a, b) => Math.abs(a - b) < 1e-9;

const node = await openNode(sub ? "ts-tick-sub" : "ts-tick-pub", { multicastInterface: "127.0.0.1" });
const deadline = Date.now() + Number(secs ?? 10) * 1000;

if (sub) {
    let good = 0, bad = 0;
    node.subscriber("tick", Tick, (t, m) => {
        const angle = Number(t.seq) * 0.01, p = t.at.pose.position;
        if (m.publisherName === sender && t.when > 0n && near(t.value, Math.sin(angle)) &&
            near(p.x, Math.cos(angle)) && near(p.y, Math.sin(angle))) good++;
        else if (!bad++) console.log(`FAIL: tick ${t.seq} decoded wrong`);
    }, { reliable: true, keepLast: 64 });
    while (Date.now() < deadline && !bad && good < TICK_WANT) await new Promise((r) => setTimeout(r, 5));
    const ok = good >= TICK_WANT && !bad;
    console.log(`${ok ? "PASS" : "FAIL"}: ${good} ticks decoded right, ${bad} wrong`);
    node.close();
    process.exit(ok ? 0 : 1);
}

const pub = node.publisher("tick", Tick, { reliable: true, keepLast: 64 });
let seq = 0;
while (Date.now() < deadline) {
    if (pub.matchCount > 0) {
        const angle = seq * 0.01;
        pub.send({ seq: BigInt(seq), when: BigInt(Date.now()) * 1000n, value: Math.sin(angle),
                   at: { pose: { position: { x: Math.cos(angle), y: Math.sin(angle), z: 0 }, orientation: { x: 0, y: 0, z: 0, w: 1 } }, parent: "" } });
        seq++;
    }
    await new Promise((r) => setTimeout(r, 2));
}
node.close();
process.exit(0);
