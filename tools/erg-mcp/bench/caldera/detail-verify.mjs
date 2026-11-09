// The detail layer's readability check on the SAVED project: every _dtl scenery copy must be outside the keep-out of
// detail-keepout.mjs (trail + kerb, castle courts, spawns, launcher pads, the mine factory), and its top must show above the
// water (round 2: sea stacks sit on the sea bed, so their origins are deep; a prop is sunk when origin + 0.75 of its height,
// Horror5 2.05 map units tall at scale 1, is under the water at 1).
//   tools\node\node.exe tools\erg-mcp\bench\caldera\detail-verify.mjs --url <server url>|(default testenv D) [--project caldera]
import path from "node:path";
import { startServer, ROOT } from "./mcpclient.mjs";
import { keepOut, keepOutFoot } from "./detail-keepout.mjs";
const argv = process.argv.slice(2), opt = (f, d) => { const i = argv.indexOf(f); return i >= 0 ? argv[i + 1] : d; };
process.env.MELANGE_EXE ??= path.join(ROOT, "build/x86-release/Melange.exe");
const { call, stop } = await startServer();
try {
  await call("connect", opt("--url") ? { url: opt("--url") } : { gameDir: process.env.MELANGE_GAME_DIR || "C:/Users/Jamin/Desktop/WUMFix/testenv/D" });
  await call("open_project", { id: opt("--project", "caldera") });
  const spawns = ((await call("describe_level", { limit: 1 })).json.spawns ?? []).map((s) => [s.map[0], s.map[2]]);
  const d = (await call("list_scenery", { search: "_dtl", units: "map", limit: 2000 })).json.details;
  // round 3: each piece's footprint (Horror5's 3.11 x 3.21 box at its x and z scale, 0.85 of the half side) must be clear too,
  // and a piece under the water is a fault only when no _dtl piece within 4 of it shows above the water (the lower blocks
  // of a kitbashed sea stack are sunk on purpose under the ones that show)
  const top = (x) => x.at[1] + 0.75 * 2.05 * Math.abs(x.scale?.[1] ?? 1);
  const shows = d.filter((x) => top(x) >= 1);
  const foot = (x) => 0.85 * Math.max(3.11 * Math.abs(x.scale?.[0] ?? 1), 3.21 * Math.abs(x.scale?.[2] ?? 1)) / 2;
  const why = (x) => keepOut(x.at[0], x.at[2], 0, spawns) || (foot(x) > 0.3 && keepOutFoot(x.at[0], x.at[2], foot(x) * 0.6, foot(x) * 0.6, 0, spawns, 0) && "footprint")
    || (top(x) < 1 && !shows.some((y) => Math.hypot(y.at[0] - x.at[0], y.at[2] - x.at[2]) < 4) && "sunk");
  const bad = d.filter((x) => why(x));
  console.log(JSON.stringify({ props: d.length, violations: bad.length, first: bad.slice(0, 5).map((x) => [x.name, x.at, why(x)]) }));
  await call("close", { discard: true });
  if (bad.length) process.exitCode = 1;
} finally { await stop(); }
