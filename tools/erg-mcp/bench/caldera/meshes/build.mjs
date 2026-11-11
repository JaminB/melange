// Builds Caldera's mesh banks (round 10): gen.mjs's models -> xomtool convert --bundle (one bank per mesh, its own section),
// the material and its one texture stage copied from the game's HORROR5 rock (Bundl31.xom) with our basalt texture, then
// the copied XMaterial made MATTE: Specular and Fresnel to 0 (HORROR5 has 0.3 / 0.275 / 0.175 at power 8, the sheen that
// made the stacks read as glossy crystal under Sunstone's Blinn-Phong on the material's own specular).
//
//   tools\node\node.exe tools\erg-mcp\bench\caldera\meshes\build.mjs <outDir> [--game <game dir>] [--xomtool <exe>]
// Writes <outDir>/caldera.<Name>.xom and meshes.json (each bank's box in map units for place_mod_mesh). A level that
// names these meshes needs them loaded: a content mod whose spice.json lists them under `meshes` (docs/spice.md), the
// mod id "caldera" (every name in a bank starts with the mod's id).
import fs from "node:fs";
import path from "node:path";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { generate, MESHES } from "./gen.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, "../../../../..");
const argv = process.argv.slice(2);
const opt = (f, d) => { const i = argv.indexOf(f); return i >= 0 ? argv[i + 1] : d; };
const OUT = path.resolve(argv[0] && !argv[0].startsWith("--") ? argv[0] : path.join(HERE, "..", "out", "meshes"));
const GAME = opt("--game", process.env.MELANGE_GAME_DIR || "C:/Users/Jamin/Desktop/WUMFix/testenv/D");
const XOM = opt("--xomtool", path.join(ROOT, "dist/tools/xomtool.exe"));
const MAT_FROM = "HORROR5", MAT_FILE = path.join(GAME, "Data/Bundles/Bundl31.xom");

const src = path.join(OUT, "src");
const info = generate(src);
const x = (...a) => execFileSync(XOM, a, { encoding: "utf8" });
for (const name of Object.keys(MESHES)) {
  const id = `caldera.${name}`, bank = path.join(OUT, `${id}.xom`), json = path.join(OUT, `${id}.json`);
  x("convert", path.join(src, `${name}.gltf`), "--bundle", bank, "--as", id, "--section", String(MESHES[name].section),
    "--material-from", MAT_FROM, "--material-file", MAT_FILE, "--texture", path.join(src, `${MESHES[name].texture}.png`));
  // matte: the copied material's specular and fresnel to zero
  x("unpack", bank, "-o", json);
  const doc = JSON.parse(fs.readFileSync(json, "utf8"));
  let mats = 0;
  const walk = (o) => {
    if (!o || typeof o !== "object") return;
    if (Array.isArray(o)) { o.forEach(walk); return; }
    if ((o.type === "XMaterial" || o.class === "XMaterial" || o._type === "XMaterial") && (o.fields ?? o).Specular) {
      const f = o.fields ?? o;
      f.Specular = [0, 0, 0, 1]; f.Fresnel = [0, 0, 0, 1]; mats++;
      return;
    }
    for (const v of Object.values(o)) walk(v);
  };
  walk(doc);
  if (mats !== 1) throw new Error(`${id}: expected one XMaterial in the bank, found ${mats} (xomtool unpack's JSON shape changed?)`);
  fs.writeFileSync(json, JSON.stringify(doc, null, 1));
  x("pack", json, bank);
  fs.rmSync(json);
  info[id].bank = path.basename(bank);
  info[id].bytes = fs.statSync(bank).size;
  console.log(id, info[id].vertices, "vertices", info[id].triangles, "triangles", info[id].bytes, "bytes", "matte");
}
fs.writeFileSync(path.join(OUT, "meshes.json"), JSON.stringify(info, null, 1));
