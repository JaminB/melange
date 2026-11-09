// The detail layer's readability check on the SAVED project: every _dtl scenery copy must be outside the keep-out of
// detail-keepout.mjs (trail + kerb, castle courts, spawns, launcher pads, the mine factory) and not far under the water (a seated prop's origin sits below the surface).
//   tools\node\node.exe tools\erg-mcp\bench\caldera\detail-verify.mjs --url <server url>|(default testenv D) [--project caldera]
import path from "node:path";
import { startServer, ROOT } from "./mcpclient.mjs";
import { keepOut } from "./detail-keepout.mjs";
const argv = process.argv.slice(2), opt = (f, d) => { const i = argv.indexOf(f); return i >= 0 ? argv[i + 1] : d; };
process.env.MELANGE_EXE ??= path.join(ROOT, "build/x86-release/Melange.exe");
const { call, stop } = await startServer();
try {
  await call("connect", opt("--url") ? { url: opt("--url") } : { gameDir: process.env.MELANGE_GAME_DIR || "C:/Users/Jamin/Desktop/WUMFix/testenv/D" });
  await call("open_project", { id: opt("--project", "caldera") });
  const spawns = ((await call("describe_level", { limit: 1 })).json.spawns ?? []).map((s) => [s.map[0], s.map[2]]);
  const d = (await call("list_scenery", { search: "_dtl", units: "map" })).json.details;
  const bad = d.filter((x) => keepOut(x.at[0], x.at[2], 0, spawns) || x.at[1] < -2);
  console.log(JSON.stringify({ props: d.length, violations: bad.length, first: bad.slice(0, 5).map((x) => [x.name, x.at, keepOut(x.at[0], x.at[2], 0, spawns)]) }));
  await call("close", { discard: true });
  if (bad.length) process.exitCode = 1;
} finally { await stop(); }
