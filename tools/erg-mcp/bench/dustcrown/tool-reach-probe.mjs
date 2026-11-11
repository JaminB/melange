// Throwaway check of check_tool_reach: a small sand floor with a stepped plinth and tower in the middle and a gap-separated
// pad, read from two starts with the tools' calibrated limits, rendered, then the project deleted.
//   tools\node\node.exe tools\erg-mcp\bench\dustcrown\tool-reach-probe.mjs <outDir> [--limits <json>]
// MELANGE_GAME_DIR (default testenv C) with the game closed; MELANGE_EXE names the Melange.exe to --serve.
import fs from "node:fs";
import path from "node:path";
import { startServer } from "../caldera/mcpclient.mjs";

const argv = process.argv.slice(2);
const OUT = path.resolve(argv[0] || "out");
const limits = argv.includes("--limits") ? JSON.parse(argv[argv.indexOf("--limits") + 1]) : undefined;
fs.mkdirSync(OUT, { recursive: true });
process.env.MELANGE_GAME_DIR ??= "C:/Users/Jamin/Desktop/WUMFix/testenv/C";
const ID = "toolreachprobe";
const { call, stop } = await startServer();
try {
  await call("delete_project", { id: ID }, { soft: true });
  const np = await call("new_project", { base: "Multi.DinerMight", slug: ID, title: "Tool reach probe" });
  if (np.json?.created?.id !== ID) throw new Error(`new_project gave ${np.json?.created?.id}`);
  await call("open_project", { id: ID });
  await call("set_theme", { theme: "ARABIAN", materialFile: "theme" }, { soft: true });
  await call("set_time_of_day", { timeOfDay: "DAY" });
  await call("set_water", { level: -20 });
  await call("set_surround_mode", { mode: "none" });
  for (const search of ["Neon", "Grass", "BUILDING", "D05", "MusicNotes", "Applause", "JukeBox"]) await call("remove_scenery", { search }, { soft: true });
  await call("clear_base_terrain", { all: true, wipe: true });
  await call("remove_base_frames", { all: true }, { soft: true });
  await call("trim_base_frames", { clear: true }, { soft: true });   // the frames the wipe left a few voxels in
  const box = (at, w, d, h, scale = 1, material = 10) => call("build_compound", { at, material, scale, shapes: [{ kind: "box", size: [w, d], height: h }] });
  await box([0, 0, 0], 120, 90, 3, 3);                                   // the sand floor, top y 3
  await call("build_compound", { at: [0, 3, 0], material: 6, scale: 1, shapes: [{ kind: "tiers", size: [40, 40], steps: 3, riser: 6, ledge: 5 }, { kind: "box", size: [6, 6], height: 20, at: [0, 18, 0] }] });
  await box([48, 3, -20], 8, 8, 5, 1, 6);                                // a pad
  await box([48, 3, -6], 8, 8, 5, 1, 6);                                  // another, 6 across the gap
  const targets = [
    { name: "ledge1", x: 17.5, z: 0, y: 9, radius: 2 },
    { name: "ledge2", x: 12.5, z: 0, y: 15, radius: 2 },
    { name: "ledge3", x: 6.5, z: 6.5, y: 21, radius: 2 },
    { name: "tower", x: 0, z: 0, radius: 2 },
  ];
  const res = await call("check_tool_reach", { x0: -60, z0: -45, x1: 60, z1: 45, water: -20, from: [[-50, 3, 0], [50, 3, 36]], targets, render: true, ...(limits ? { limits } : {}) });
  if (res.images[0]) fs.writeFileSync(path.join(OUT, "check-tool-reach.png"), Buffer.from(res.images[0].data, "base64"));
  fs.writeFileSync(path.join(OUT, "check-tool-reach.json"), res.text);
  console.log(res.err ? res.text.slice(0, 1500) : JSON.stringify(res.json.summary, null, 1));
  const pad = await call("check_tool_reach", { x0: -60, z0: -45, x1: 60, z1: 45, water: -20, from: [[48, 8, -20]], targets: [{ name: "pad2", x: 48, z: -6, radius: 2 }], ...(limits ? { limits } : {}) });
  console.log(pad.err ? pad.text.slice(0, 600) : JSON.stringify(pad.json.summary));
  await call("close", { discard: true }, { soft: true });
} finally {
  await call("close", { discard: true }, { soft: true });
  const d = await call("delete_project", { id: ID }, { soft: true });
  console.log("delete", d.text.slice(0, 200));
  await stop();
}
