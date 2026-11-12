// Throwaway check of the dunes()/ridged() expressions and heightfield_drape's crestMaterial/troughMaterial: a small
// dune field draped on Multi.DinerMight, rendered from above and at an angle, then the project deleted.
//   tools\node\node.exe tools\erg-mcp\bench\dustcrown\dunes-probe.mjs <outDir>
// MELANGE_GAME_DIR (default testenv C) with the game closed; MELANGE_EXE names the Melange.exe to --serve.
import fs from "node:fs";
import path from "node:path";
import { startServer } from "../caldera/mcpclient.mjs";

const OUT = path.resolve(process.argv[2] || "out");
fs.mkdirSync(OUT, { recursive: true });
process.env.MELANGE_GAME_DIR ??= "C:/Users/Jamin/Desktop/WUMFix/testenv/C";
const ID = "dunesprobe";
const { call, stop } = await startServer();
const shot = async (name, tool, args) => {
  const r = await call(tool, args, { soft: true });
  if (r.images[0]) fs.writeFileSync(path.join(OUT, `${name}.png`), Buffer.from(r.images[0].data, "base64"));
  console.log(name, r.err ? r.text.slice(0, 300) : "ok");
};
try {
  await call("delete_project", { id: ID }, { soft: true });
  const np = await call("new_project", { base: "Multi.DinerMight", slug: ID, title: "Dunes probe" });
  if (np.json?.created?.id !== ID) throw new Error(`new_project gave ${np.json?.created?.id}`);
  await call("open_project", { id: ID });
  await call("set_theme", { theme: "PREHISTORIC", materialFile: "theme" });
  await call("set_time_of_day", { timeOfDay: "DAY" });
  await call("set_water", { level: -20 });
  await call("set_surround_mode", { mode: "none" });
  for (const search of ["Neon", "Grass", "BUILDING", "D05", "MusicNotes", "Applause", "JukeBox"]) await call("remove_scenery", { search }, { soft: true });
  await call("clear_base_terrain", { all: true, wipe: true });
  await call("remove_base_frames", { all: true }, { soft: true });
  // sand01 10 on the flats, dinoegg02 26 (pale) on the crests, rock02 6 in the hollows
  const field = {
    min: [-60, -60], max: [60, 60], floor: 0, units: "map", voxelSize: 1.5, material: 10,
    expr: "let d = dunes(x, z, 25, 34, 7, 0.7, 11); let rr = ridged(x/9, z/9, 3, 1.5, 5); 3 + d + rr * clamp(d / 3, 0, 1)",
    thickness: 2, maxBlocks: 20, core: false,
    crestMaterial: { material: 26, minCurvature: 0.02, feather: 0.02 },
    troughMaterial: { material: 6, minCurvature: 0.02, feather: 0.02 },
  };
  const plan = await call("heightfield_drape", { ...field, dryRun: true });
  console.log("plan", JSON.stringify({ fits: plan.json.fits, blocks: plan.json.budget, curve: plan.json.curveMaterials, peak: plan.json.region.peak }));
  const ap = await call("heightfield_drape", field);
  console.log("applied", JSON.stringify({ curve: ap.json.curveMaterials, limits: ap.json.limits }));
  await shot("topdown", "render_topdown", { step: 1, scale: 4 });
  await shot("view-se", "render_view", { pos: [90, 38, 90], target: [0, 4, 0], fov: 50, fog: 900 });
  await shot("view-low", "render_view", { pos: [-80, 14, 20], target: [0, 6, 0], fov: 55, fog: 900 });
  await call("close", { discard: true }, { soft: true });
} finally {
  await call("close", { discard: true }, { soft: true });
  const d = await call("delete_project", { id: ID }, { soft: true });
  console.log("delete", d.text.slice(0, 200));
  await stop();
}
