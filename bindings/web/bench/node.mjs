// Runs the wasm bench in Node.js over real UDP, the control between the native build and a
// page behind the bridge:   node bindings/web/bench/node.mjs ping --size 64 --count 1000
// It prints the bench's JSON line and exits once the bench is done. --if 127.0.0.1 keeps it on
// loopback, as the tests do.

import { UdpNet } from "../dist/udp.js";
import createBench from "../../../bin/web/rant_web_bench.mjs";

const args = process.argv.slice(2);
const at = args.indexOf("--if");
const net = await UdpNet.create({ address: at >= 0 && args[at + 1] === "127.0.0.1" ? "127.0.0.1" : "0.0.0.0" });
let done;
const finished = new Promise((r) => { done = r; });
await createBench({
    arguments: args,
    rantNet: net,
    print: (line) => {
        console.log(line);
        if (line.startsWith("{\"mode\"")) done();
    },
    printErr: (line) => console.error(line),
});
if (args[0] === "echo") await new Promise(() => {});   /* an echo runs until it is stopped */
await finished;
net.shutdown();
process.exit(0);
