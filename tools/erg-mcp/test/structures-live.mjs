// Live check of the structures tools against the real Oasis server (Melange.exe --serve from a free game folder).
// Not part of test.ps1 (it needs a game folder). Usage:
//   node test/structures-live.mjs <game dir> [Melange.exe] [base key]
// Builds a project (ids mcpstructures*), places every structure, saves through level.save, reopens, and deletes the project folder it made.
import { spawn } from "node:child_process";
import { rmSync } from "node:fs";
import { join } from "node:path";
import { homedir } from "node:os";
import { spawnMcp } from "./harness.mjs";

const gameDir = process.argv[2];
const exe = process.argv[3] || join(gameDir, "Melange.exe");
const baseKey = process.argv[4] || "Multi.DinerMight";
const id = "mcpstructures1";

const serve = spawn(exe, ["--serve", "--game", gameDir], { stdio: ["ignore", "pipe", "pipe"] });
const url = await new Promise((resolve, reject) => {
  let buf = "";
  const t = setTimeout(() => reject(new Error("no listening line: " + buf)), 30000);
  const on = (d) => { buf += d; const m = /listening, (\S+)/.exec(buf); if (m) { clearTimeout(t); resolve(m[1]); } };
  serve.stdout.on("data", on); serve.stderr.on("data", on);
});

const m = spawnMcp({ ERG_MCP_URL: url, USERPROFILE: process.env.USERPROFILE, OneDrive: process.env.OneDrive ?? "" });
const out = [];
const say = (...a) => { console.log(...a); out.push(a.join(" ")); };
let failed = 0;
const check = (ok, what) => { say(ok ? "ok  " : "FAIL", what); if (!ok) failed++; };
const T = async (name, args) => {
  const r = await m.tool(name, args);
  if (r.isError) say("  [error]", name, r.text.slice(0, 400));
  return r;
};

try {
  await m.initialize();
  await T("connect", {});
  const n = await T("new_project", { base: baseKey, slug: "mcpstructures1", title: "MCP structures" });
  check(!n.isError, "new_project " + (n.json?.created?.id ?? n.text));
  const pid = n.json?.created?.id ?? id;
  await T("open_project", { id: pid });
  const d = await T("describe_level", { limit: 3, minVoxels: 200 });
  const ext = d.json.extent.map;
  say("extent", JSON.stringify(ext), "frames", d.json.voxelFrames.total, "solid", d.json.voxelFrames.solid);
  const big = d.json.frames[0];
  say("largest frame", JSON.stringify({ id: big.id, size: big.size, bounds: big.bounds.map }));
  const cx = (ext.min[0] + ext.max[0]) / 2, cz = (ext.min[2] + ext.max[2]) / 2;
  const bc = [0, 1, 2].map((a) => (big.bounds.map.min[a] + big.bounds.map.max[a]) / 2);
  const floorY = big.bounds.map.max[1];
  const outX = ext.max[0] + 40;                       // empty space beside the level
  const calls = [
    ["build_castle", { at: [outX + 20, floorY, cz], size: [30, 24], foundation: 2 }],
    ["build_tower", { at: [outX + 20, floorY, cz - 40], shape: "square", radius: 4, height: 12, hollow: true }],
    ["build_keep", { at: [outX + 20, floorY, cz + 40], size: [12, 10] }],
    ["build_wall", { points: [[outX + 60, floorY, cz - 20], [outX + 80, floorY + 4, cz - 20], [outX + 80, floorY + 4, cz + 10]], height: 4, thickness: 3 }],
    ["build_ramp", { from: [outX + 60, floorY, cz + 30], to: [outX + 80, floorY + 20, cz + 30], width: 4, switchback: true }],
    ["build_stairs", { from: [outX + 100, floorY, cz], to: [outX + 108, floorY + 6, cz], width: 3 }],
    ["build_arch", { at: [outX + 100, floorY, cz + 20], span: 6, rise: 3 }],
    ["build_bridge", { from: [outX + 100, floorY + 8, cz - 30], to: [outX + 130, floorY + 8, cz - 30], pierSpacing: 10, downTo: floorY }],
    // into the level's own frames: the middle of the biggest solid frame
    ["dig_crater", { at: [bc[0], bc[1] + 1, bc[2]], radius: 4, depth: 2, place: "existing" }],
    ["dig_tunnel", { points: [[big.bounds.map.min[0] - 2, bc[1], bc[2]], [big.bounds.map.max[0] + 2, bc[1], bc[2]]], radius: 1.2 }],
    ["build_tower", { at: [bc[0], big.bounds.map.min[1], bc[2] + 6], radius: 2.5, height: 6, place: "existing" }],
  ];
  for (const [name, args] of calls) {
    const r = await T(name, args);
    check(!r.isError, `${name} -> ${r.json ? `changed ${r.json.voxels.changed}, frames ${r.json.frames.length}, new blocks ${r.json.newBlocks.length}, usage ${r.json.usage.newBlocks}` : r.text.slice(0, 200)}`);
  }
  const mid = await T("describe_level", { limit: 0 });
  say("usage after", JSON.stringify(mid.json.limits.newFrames), JSON.stringify(mid.json.limits.patchBytes), "problems", JSON.stringify(mid.json.problems));
  check(mid.json.problems.length === 0, "no patch problems before save");
  const sv = await T("save", {});
  check(!sv.isError && sv.json.saved === true, "save: " + sv.text.replace(/\s+/g, " ").slice(0, 300));
  check(!sv.isError && sv.json.warnings.length === 0, "save has no warnings");
  const solidBefore = mid.json.voxelFrames.solid;
  await T("close", {});
  await T("open_project", { id: pid });
  const re = await T("describe_level", { limit: 0 });
  check(re.json.voxelFrames.total === mid.json.voxelFrames.total, `reopened: ${re.json.voxelFrames.total} voxel frames (was ${mid.json.voxelFrames.total})`);
  check(re.json.voxelFrames.solid === solidBefore, `reopened: ${re.json.voxelFrames.solid} solid voxels (was ${solidBefore})`);
  check(re.json.limits.newFrames.used === mid.json.limits.newFrames.used, "reopened: same number of added blocks");
  const u = await T("undo", {});
  check(u.isError, "nothing to undo after reopen: " + u.text.slice(0, 80));
  await T("close", { discard: true });
} catch (e) {
  say("EXCEPTION", e.stack || e);
  failed++;
} finally {
  await m.close().catch(() => {});
  serve.kill();
  // only the project this run created
  try { rmSync(join(homedir(), "Documents", "Melange", "erg", "projects", "mcpstructures1"), { recursive: true, force: true }); } catch {}
}
say(failed ? `FAILED: ${failed}` : "ALL OK");
process.exit(failed ? 1 : 0);
