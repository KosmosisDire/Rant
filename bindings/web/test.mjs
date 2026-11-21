// The web build's test: an echo and a ping, each Rant in wasm in its own Node.js over UdpNet on
// loopback, so the platform layer, the driver and the net object run as they do in a page.
// Small messages, then fragmented ones, both reliable:   node bindings/web/test.mjs

import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const bench = fileURLToPath(new URL("./bench/node.mjs", import.meta.url));
const domain = String(170 + (process.pid % 20));
const common = ["--domain", domain, "--reliable", "--if", "127.0.0.1"];

const echo = spawn(process.execPath, [bench, "echo", ...common], { stdio: ["ignore", "ignore", "inherit"] });
let failed = 0;
try {
    for (const [size, count] of [[64, 500], [20000, 200]]) {
        const line = await run([bench, "ping", ...common, "--size", String(size), "--count", String(count), "--window", "16"]);
        const r = JSON.parse(line);
        const ok = !r.error && r.got === count && r.lost.length === 0;
        console.log(`${ok ? "ok  " : "FAIL"} ${size} B x ${count}: got ${r.got}, p50 ${r.p50_us} us${r.error ? ", " + r.error : ""}`);
        if (!ok) failed++;
    }
} finally {
    echo.kill();
}
process.exit(failed ? 1 : 0);

function run(args) {
    return new Promise((resolve, reject) => {
        const p = spawn(process.execPath, args, { stdio: ["ignore", "pipe", "inherit"] });
        let out = "";
        p.stdout.on("data", (d) => { out += d; });
        p.on("exit", (code) => {
            const line = out.split("\n").find((l) => l.startsWith("{\"mode\""));
            if (line) resolve(line); else reject(new Error(`the ping exited ${code} with no result`));
        });
    });
}
