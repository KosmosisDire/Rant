/* Build the JS client distributables from ramble.ts into dist/: ramble.mjs by pure type
 * stripping, ramble.d.ts, and ramble.js as the classic script twin with one global. */
import { execFileSync } from "node:child_process";
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const dist = join(here, "..", "..", "dist");

/* tsc emits dist/ramble.js (module) + dist/ramble.d.ts */
execFileSync("npx", ["-y", "-p", "typescript@5.5", "tsc", "--project", join(here, "tsconfig.json")],
             { stdio: "inherit", shell: process.platform === "win32" });

const mod = readFileSync(join(dist, "ramble.js"), "utf8");
writeFileSync(join(dist, "ramble.mjs"), mod);

/* the classic twin: dumb text replacement of the single trailing export block */
const classic = mod.replace(/export\s*\{[^]*?\};?\s*$/, "globalThis.RambleNode = RambleNode;\n");
if (classic === mod) throw new Error("export block not found in emitted ramble.js");
writeFileSync(join(dist, "ramble.js"), classic);
console.log("built dist/ramble.mjs, dist/ramble.d.ts, dist/ramble.js");
