// The detail layer's readability check on the SAVED project: every _dtl scenery copy must be outside the keep-out of
// detail-keepout.mjs (trail + kerb, castle courts, spawns, launcher pads, the mine factory), and its top must show above the
// water (round 2: sea stacks sit on the sea bed, so their origins are deep; a prop is sunk when origin + 0.75 of its height,
// Horror5 2.05 map units tall at scale 1, is under the water at 1).
//   tools\node\node.exe tools\erg-mcp\bench\caldera\detail-verify.mjs --url <server url>|(default testenv D) [--project caldera]
import path from "node:path";
import { startServer, ROOT } from "./mcpclient.mjs";
import { keepOut, keepOutFoot, keepOutLow, keepOutFlora } from "./detail-keepout.mjs";
import { FLORA } from "./detail-flora.mjs";
import * as T from "./terrain.mjs";
const argv = process.argv.slice(2), opt = (f, d) => { const i = argv.indexOf(f); return i >= 0 ? argv[i + 1] : d; };
process.env.MELANGE_EXE ??= path.join(ROOT, "build/x86-release/Melange.exe");
const { call, stop } = await startServer();
try {
  await call("connect", opt("--url") ? { url: opt("--url") } : { gameDir: process.env.MELANGE_GAME_DIR || "C:/Users/Jamin/Desktop/WUMFix/testenv/D" });
  await call("open_project", { id: opt("--project", "caldera") });
  const spawns = ((await call("describe_level", { limit: 1 })).json.spawns ?? []).map((s) => [s.map[0], s.map[2]]);
  // round 6: the rock (_dtl), the strata (_ldg) and the flora (_flo)
  const d = [];
  for (const q of ["_dtl", "_ldg", "_flo", "_cpl", "_slb"]) d.push(...(await call("list_scenery", { search: q, units: "map", limit: 2000 })).json.details);
  // round 3: each piece's footprint (Horror5's 3.11 x 3.21 box at its x and z scale, 0.85 of the half side) must be clear too,
  // and a piece under the water is a fault only when no _dtl piece within 4 of it shows above the water (the lower blocks
  // of a kitbashed sea stack are sunk on purpose under the ones that show)
  const top = (x) => x.at[1] + 0.75 * 2.05 * Math.abs(x.scale?.[1] ?? 1);
  const shows = d.filter((x) => top(x) >= 1);
  const foot = (x) => 0.85 * Math.max(3.11 * Math.abs(x.scale?.[0] ?? 1), 3.21 * Math.abs(x.scale?.[2] ?? 1)) / 2;
  // round 4: a low piece (drawn under 0.9 tall: the low scree, showing under 0.4) may stand in the kerb band but not on the
  // walked bed and its shoulder; and nothing under 1.7 tall may lie on the crater's open floor (it read as a mine or pickup)
  const height = (x) => 2.05 * Math.abs(x.scale?.[1] ?? 1);
  const low = (x) => height(x) <= 0.9;
  const why = (x) => (low(x) ? keepOutLow(x.at[0], x.at[2], foot(x) * 0.6, foot(x) * 0.6, 0, spawns, 0.5) && "low piece on the walked bed"
      : keepOut(x.at[0], x.at[2], 0, spawns) || (foot(x) > 0.3 && keepOutFoot(x.at[0], x.at[2], foot(x) * 0.6, foot(x) * 0.6, 0, spawns, 0) && "footprint"))
    || (T.rhoAt(x.at[0], x.at[2]) < T.FLAT_R - 0.5 && height(x) < 1.7 && !d.some((y) => y !== x && height(y) >= 3 && Math.hypot(y.at[0] - x.at[0], y.at[2] - x.at[2]) < 3.5) && "small on the crater floor")
    || (top(x) < 1 && !shows.some((y) => Math.hypot(y.at[0] - x.at[0], y.at[2] - x.at[2]) < 4) && "sunk");
  // round 7: plants are checked with their own footprint (the catalog mesh at the copy's scale) and the flora rule (they may
  // grow against a castle's outer walls, never on the trail band, spawns, pads or the factory); a plant over 1.3 tall (a
  // bush) must stand at a wall, a cliff foot, a rock base or on a slope: reported, not a violation (the pass decides that)
  const mesh = {};
  for (const p of Object.values(FLORA).flat()) {
    const row = (await call("scenery_catalog", { bases: [p.base], roles: ["scenery"], search: p.resource, limit: 10 })).json.list.find((r) => r.resource === p.resource);
    // round 8: two bases can share a resource name (CAMELOT3 is a flower in one and a bush in another): keep the larger
    if (row) mesh[p.resource] = mesh[p.resource] ? mesh[p.resource].map((v, i) => Math.max(v, row.meshSize[i])) : row.meshSize;
  }
  const isFlo = (x) => /_flo/.test(x.name);
  const floR = (x) => { const m = mesh[x.resource] ?? [1, 1, 1], s = x.scale ?? [1, 1, 1]; return 0.5 * Math.max(m[0] * Math.abs(s[0]), m[2] * Math.abs(s[2])); };
  const whyFlo = (x) => keepOutFlora(x.at[0], x.at[2], floR(x) * 0.8, floR(x) * 0.8, 0, spawns, 0.3);
  const plants = d.filter(isFlo);
  const rockish = d.filter((x) => !isFlo(x));
  const badFlo = plants.filter((x) => whyFlo(x));
  const bad = rockish.filter((x) => why(x)).concat(badFlo);
  const whyAny = (x) => (isFlo(x) ? whyFlo(x) : why(x));
  const tinted = d.filter((x) => /_tint[0-9a-f]{6}/.test(x.name)).length;
  const byTag = Object.fromEntries(["_dtl", "_ldg", "_cpl", "_flo", "_slb"].map((t) => [t, d.filter((x) => x.name.includes(t)).length]));
  console.log(JSON.stringify({ props: d.length, byTag, low: rockish.filter(low).length, plants: plants.length, tinted, violations: bad.length, floraViolations: badFlo.length, first: bad.slice(0, 5).map((x) => [x.name, x.at, whyAny(x)]) }));
  await call("close", { discard: true });
  if (bad.length) process.exitCode = 1;
} finally { await stop(); }
