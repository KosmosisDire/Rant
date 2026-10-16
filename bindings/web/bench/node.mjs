// Runs the wasm bench in Node.js over real UDP, the control between the native build and a
// page behind the bridge:   node bindings/web/bench/node.mjs ping --size 64 --count 1000
// It prints the bench's JSON line and exits once the bench is done.

import { UdpNet } from "../udp.mjs";
import createBench from "../../../bin/web/rant_web_bench.mjs";

const args = process.argv.slice(2);
const net = await UdpNet.create();
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
