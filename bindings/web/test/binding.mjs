// The TypeScript binding's test: two nodes in one Node.js process over UdpNet on loopback, through
// every pattern and the reflection.   node bindings/web/test/binding.mjs

import assert from "node:assert/strict";
import {
    openNode, struct, f64, f32, bool, u64, i32, string, array, enumOf, map, types, CancelledError, AppError,
} from "../dist/rant.js";

const domain = 190 + (process.pid % 10);
const opts = { domain, multicastInterface: "127.0.0.1", announceIntervalMs: 500 };
const events = [];
const a = await openNode("a", { ...opts, onEvent: (e) => events.push(e) });
const b = await openNode("b", { ...opts, onEvent: (e) => events.push(e) });
let failed = 0;

async function check(name, f) {
    try {
        await f();
        console.log(`ok   ${name}`);
    } catch (e) {
        failed++;
        console.log(`FAIL ${name}: ${e?.stack ?? e}`);
    }
}

const until = (f, ms = 5000) => new Promise((res, rej) => {
    const end = Date.now() + ms;
    const tick = () => {
        let v;
        try { v = f(); } catch (e) { rej(e); return; }
        if (v) res(v); else if (Date.now() > end) rej(new Error("timed out")); else setTimeout(tick, 10);
    };
    tick();
});

const Point = struct("Point", { x: f32, y: f32 });
const Conveyor = struct("Conveyor", {
    speed: f64, running: bool, partNumber: string(16), count: u64, mode: enumOf({ Idle: 0, Run: 1, Fault: 7 }),
    gains: array(f32, 3), labels: array(string(8)), note: string(), origin: Point, path: array(Point), extra: map,
});
const sample = {
    speed: 0.5, running: true, partNumber: "bracket-12", count: 1n << 40n, mode: "Fault",
    gains: new Float32Array([1, 2, 3]), labels: ["a", "bb"], note: "a note of any length",
    origin: { x: 1, y: -2 }, path: [{ x: 1, y: 2 }, { x: 3, y: 4 }], extra: { n: 3, ok: true, s: "x", list: [1, "two"], sub: { f: 1.5 } },
};

await check("a typed message crosses whole", async () => {
    const got = [];
    b.subscriber("Line1/Conveyor", Conveyor, (v, m) => got.push([v, m]), { reliable: true });
    const pub = a.publisher("Line1/Conveyor", Conveyor, { reliable: true });
    await until(() => pub.matchCount === 1);
    assert.equal(pub.send(sample), "ok");
    await until(() => got.length);
    const [v, m] = got[0];
    assert.equal(m.publisherName, "a");
    assert.ok(m.writtenUs > 1.7e15);
    assert.deepEqual({ ...v, gains: [...v.gains] }, { ...sample, gains: [1, 2, 3] });
    assert.equal(pub.send({ ...sample, partNumber: "far too long a part number" }), "schema");
});

await check("a pulled subscriber takes in order", async () => {
    const sub = b.subscriber("Counter", i32);
    const pub = a.publisher("Counter", i32, { reliable: true, keepLast: 10 });
    await until(() => pub.matchCount === 1);
    for (const x of [1, 2, 3]) pub.send(x);
    await until(() => sub.queueStats.messages === 3);
    assert.equal(sub.take().value, 1);
    assert.equal(sub.latest().value, 3);
    assert.equal(sub.take(), null);
});

await check("standard types and raw bytes", async () => {
    const poses = [], raws = [];
    b.subscriber("Pose", types.Pose, (v) => poses.push(v));
    b.subscriber("Raw", null, (v) => raws.push(v));
    const pose = a.publisher("Pose", types.Pose);
    const raw = a.publisher("Raw", null);
    await until(() => pose.matchCount && raw.matchCount);
    pose.send({ position: { x: 1, y: 2, z: 3 }, orientation: { x: 0, y: 0, z: 0, w: 1 } });
    raw.send(new Uint8Array([9, 8, 7]));
    await until(() => poses.length && raws.length);
    assert.deepEqual(poses[0], { position: { x: 1, y: 2, z: 3 }, orientation: { x: 0, y: 0, z: 0, w: 1 } });
    assert.deepEqual([...raws[0]], [9, 8, 7]);
    assert.equal(raw.send("text"), "schema");
});

const AddReq = struct("AddReq", { a: i32, b: i32 });
const AddRsp = struct("AddRsp", { sum: i32 });

await check("functions answer, defer and fail", async () => {
    a.functionDefinition("Add", (v, req) => { assert.equal(req.callerName, "b"); return { sum: v.a + v.b }; }, AddReq, AddRsp);
    a.functionDefinition("SlowAdd", async (v) => { await new Promise((r) => setTimeout(r, 20)); return { sum: v.a + v.b }; }, AddReq, AddRsp);
    a.functionDefinition("Fails", () => { throw new AppError("no luck", { sum: -1 }); }, AddReq, AddRsp);
    const add = b.remoteFunction("Add", AddReq, AddRsp);
    const slow = b.remoteFunction("SlowAdd", AddReq, AddRsp);
    const fails = b.remoteFunction("Fails", AddReq, AddRsp);
    await until(() => add.matchCount && slow.matchCount && fails.matchCount);
    const r = await add.call({ a: 2, b: 3 });
    assert.equal(r.status, "ok");
    assert.deepEqual(r.value, { sum: 5 });
    assert.deepEqual((await slow.call({ a: 4, b: 5 })).value, { sum: 9 });
    const f = await fails.call({ a: 1, b: 1 });
    assert.equal(f.status, "app_error");
    assert.equal(f.message, "no luck");
    assert.deepEqual(f.value, { sum: -1 });
    const none = b.remoteFunction("Nobody", AddReq, AddRsp, { timeoutMs: 300 });
    assert.equal((await none.call({ a: 1, b: 1 })).status, "no_provider");
});

await check("tasks stream progress and cancel", async () => {
    const Steps = struct("Steps", { n: i32 });
    const Done = struct("Done", { total: i32 });
    a.taskDefinition("Count", async (v, ctx) => {
        for (let i = 1; i <= v.n; i++) {
            if (ctx.cancelled) throw new CancelledError("stopped", { total: i - 1 });
            ctx.progress({ n: i });
            await new Promise((r) => setTimeout(r, 10));
        }
        return { total: v.n };
    }, Steps, Steps, Done);
    const count = b.remoteTask("Count", Steps, Steps, Done);
    await until(() => count.matchCount);
    const seen = [];
    const run = count.call({ n: 3 }, { onProgress: (v) => seen.push(v === null ? null : v.n) });
    const r = await run.result;
    assert.equal(r.status, "ok");
    assert.deepEqual(r.value, { total: 3 });
    assert.deepEqual(seen, [null, 1, 2, 3]);
    const long = count.call({ n: 1000 });
    await new Promise((res) => setTimeout(res, 60));
    assert.equal(long.cancel(), "ok");
    const c = await long.result;
    assert.equal(c.status, "cancelled");
    assert.ok(c.value.total > 0);
});

await check("variables replicate and force", async () => {
    const owned = a.variableDefinition("Speed", f64, { initial: 1.5, allowForce: true });
    const remote = b.remoteVariable("Speed", f64);
    const changes = [];
    remote.onChange((v, u) => changes.push([v, u.forced]));
    assert.equal(await remote.wait(5000), true);
    assert.equal(remote.get(), 1.5);
    assert.equal(remote.set(2.5), "ok");
    await until(() => owned.get() === 2.5);
    assert.equal(owned.force(9), "ok");
    await until(() => remote.get() === 9 && remote.forced);
    owned.unforce();
    await until(() => remote.get() === 2.5);
    assert.deepEqual(changes.map((c) => c[0]), [1.5, 2.5, 9, 2.5]);
});

await check("reflection sees the other node", async () => {
    const peer = (await until(() => b.reflection.peers().find((p) => p.name === "a" && p.active)));
    const topic = b.reflection.find("topic", "Line1/Conveyor");
    assert.ok(topic && topic.provides);
    const own = a.reflection.entities(0).find((e) => e.name === "Add");
    assert.equal(own.kind, "function");
    const meta = await b.reflection.meta(peer.id);
    assert.equal(meta.valid, true);
    assert.equal(meta.info.node.name, "a");
    const s = b.schema(Conveyor);
    assert.equal(s.name, "Conveyor");
    assert.ok(s.dsl.includes("part_number"));
    assert.deepEqual(s.decode(s.encode({ speed: 1 })).speed, 1);
});

await check("log lines arrive", async () => {
    const lines = [];
    b.onLog((l) => lines.push(l));
    await new Promise((r) => setTimeout(r, 300));
    a.log("warn", "careful");
    await until(() => lines.find((l) => l.text === "careful"));
    assert.equal(lines.find((l) => l.text === "careful").level, "warn");
});

await check("a handler may close its own handle", async () => {
    const got = [];
    const sub = b.subscriber("Once", i32, (v) => { got.push(v); sub.close(); });
    const pub = a.publisher("Once", i32, { reliable: true });
    await until(() => pub.matchCount === 1);
    pub.send(1);
    await until(() => sub.closed);
    pub.send(2);
    await new Promise((r) => setTimeout(r, 100));
    assert.deepEqual(got, [1]);
});

const errors = events.filter((e) => e.kind === "error");
await check("no errors were reported", async () => assert.deepEqual(errors.map((e) => e.text), []));

assert.equal(a.close(), true);
assert.equal(b.close(), true);
console.log(failed ? `${failed} failed` : "all passed");
process.exitCode = failed ? 1 : 0;
