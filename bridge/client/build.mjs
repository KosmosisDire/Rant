/* Build the JS client distributables from dart.ts (committed like the rest of dist/):
 *   dist/dart.mjs   ES module (pure type stripping by tsc)
 *   dist/dart.d.ts  type declarations
 *   dist/dart.js    classic-script twin: the export block replaced with ONE global
 *                   (globalThis.DartNode), for a plain <script src> tag
 * Run standalone:  node bridge/client/build.mjs      (needs node + npx; tsc pinned 5.5)
 * Or via CMake:    cmake -DDART_BUILD_JS_CLIENT=ON ... */
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
