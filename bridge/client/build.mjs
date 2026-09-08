/* Build the JS client distributables from dart.ts into dist/: dart.mjs by pure type
 * stripping, dart.d.ts, and dart.js as the classic script twin with one global. */
import { execFileSync } from "node:child_process";
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const dist = join(here, "..", "..", "dist");

/* tsc emits dist/dart.js (module) + dist/dart.d.ts */
execFileSync("npx", ["-y", "-p", "typescript@5.5", "tsc", "--project", join(here, "tsconfig.json")],
             { stdio: "inherit", shell: process.platform === "win32" });

const mod = readFileSync(join(dist, "dart.js"), "utf8");
writeFileSync(join(dist, "dart.mjs"), mod);

/* the classic twin: dumb text replacement of the single trailing export block */
const classic = mod.replace(/export\s*\{[^]*?\};?\s*$/, "globalThis.DartNode = DartNode;\n");
if (classic === mod) throw new Error("export block not found in emitted dart.js");
writeFileSync(join(dist, "dart.js"), classic);
console.log("built dist/dart.mjs, dist/dart.d.ts, dist/dart.js");
