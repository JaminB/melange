// Rock mesh set -> ONE mod mesh bank (the generalised build.mjs + merge.mjs of bench/caldera/meshes, which stay as they are:
// Caldera's pack is shipped by ship.mjs). Every bank a pack lists under spice.json `meshes` takes one of the 44 mod mesh
// sections all enabled mods share, so a map ships one bank (docs/meshes.md "Many meshes in one bank"):
//   1. lib/rockmesh.ts makes each mesh's glTF and its baked-albedo PNG;
//   2. `xomtool convert --bundle` writes one bank per mesh, its material and texture stage copied from a vanilla rock
//      (HORROR5 of Bundl31.xom), then the copied XMaterial is made matte (Specular and Fresnel 0: the vanilla sheen read
//      as glossy crystal under Sunstone's Blinn-Phong);
//   3. mergeDocs joins the banks the way a vanilla BundlNN.xom is laid out: the root XGraphSet lists every
//      XMeshDescriptor, all with one SectionId, each keeping its name, graph, shader, material and texture.
import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { buildRocks, type BuiltRocks, type RockSpec, type Palette } from "./rockmesh";

// Every {ref: n} (n > 0: objects[n - 1]; 0 is null) in a value, rewritten by fn.
function mapRefs(v: any, fn: (n: number) => number): any {
  if (Array.isArray(v)) return v.map((x) => mapRefs(x, fn));
  if (v && typeof v === "object") {
    const keys = Object.keys(v);
    if (keys.length === 1 && keys[0] === "ref" && typeof v.ref === "number") return { ref: v.ref ? fn(v.ref) : 0 };
    const o: Record<string, unknown> = {};
    for (const k of keys) o[k] = mapRefs(v[k], fn);
    return o;
  }
  return v;
}

/** Merges unpacked bank documents (melange-xom/1 JSON, `xomtool unpack`) into one. Throws on a shape it does not know. */
export function mergeDocs(docs: any[], section?: number): { doc: any; meshes: string[]; section: number | undefined } {
  if (!docs.length) throw new Error("no banks to merge");
  const typeNames: string[] = [], typeByName = new Map<string, any>();
  for (const d of docs) {
    if (d.format !== "melange-xom/1") throw new Error(`not a melange-xom/1 document: ${d.format}`);
    for (const t of d.types) {
      const have = typeByName.get(t.name);
      if (!have) { typeByName.set(t.name, { ...t }); typeNames.push(t.name); continue; }
      if (have.guid !== t.guid || have.version !== t.version) throw new Error(`type ${t.name} differs between banks`);
    }
  }
  const roots = docs.map((d, b) => {
    const r = d.objects[d.root - 1];
    if (!r || r.type !== "XGraphSet") throw new Error(`bank ${b}: the root is not an XGraphSet`);
    return d.root - 1;
  });
  const order: [number, number][] = [];
  for (const name of typeNames)
    docs.forEach((d, b) => d.objects.forEach((o: any, i: number) => {
      if (o.type !== name) return;
      if (b > 0 && i === roots[b]) return;   // the other banks' root graph sets fold into the first's
      order.push([b, i]);
    }));
  const newIndex = docs.map(() => new Map<number, number>());
  order.forEach(([b, i], n) => newIndex[b].set(i, n));
  for (let b = 1; b < docs.length; b++) newIndex[b].set(roots[b], newIndex[0].get(roots[0])!);
  const objects = order.map(([b, i]) => {
    const o = docs[b].objects[i];
    return { ...o, fields: mapRefs(o.fields, (r) => {
      const n = newIndex[b].get(r - 1);
      if (n === undefined) throw new Error(`bank ${b}: a ref to object ${r} that does not exist`);
      return n + 1;
    }) };
  });
  const root = objects[newIndex[0].get(roots[0])!];
  const graphs: unknown[] = [];
  docs.forEach((d, b) => {
    for (const g of d.objects[roots[b]].fields.Graphs) graphs.push(mapRefs(g, (r) => newIndex[b].get(r - 1)! + 1));
  });
  root.fields = { ...root.fields, Graphs: graphs };
  const names = new Set<string>();
  let sec = section;
  for (const o of objects) {
    if (o.type !== "XMeshDescriptor") continue;
    if (names.has(o.fields.ResourceId)) throw new Error(`two banks name the mesh ${o.fields.ResourceId}`);
    names.add(o.fields.ResourceId);
    sec ??= o.fields.SectionId;
    o.fields = { ...o.fields, SectionId: sec };
  }
  const strings: string[] = [];
  for (const d of docs) for (const s of d.strings) if (!strings.includes(s)) strings.push(s);
  const counts = new Map<string, number>();
  for (const o of objects) counts.set(o.type, (counts.get(o.type) ?? 0) + 1);
  const out = { ...docs[0], types: typeNames.map((n) => ({ ...typeByName.get(n), count: counts.get(n) ?? 0 })), strings, objects,
    root: newIndex[0].get(roots[0])! + 1 };
  for (const k of ["strs_raw", "tail_raw", "trailer"]) delete (out as Record<string, unknown>)[k];   // rebuilt by the writer
  return { doc: out, meshes: [...names], section: sec };
}

/** Sets Specular and Fresnel of the bank's one XMaterial to black; answers how many it changed. */
export function matteDoc(doc: unknown): number {
  let mats = 0;
  const walk = (o: any) => {
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
  return mats;
}

export interface BankOptions {
  /** The mod id every mesh name starts with (`<id>.<Name>`). */
  id: string;
  rocks: RockSpec[];
  palette?: string | Palette;
  seed?: number;
  /** The bank file's name after the id: `<id>.<bank>.xom` (default Rocks). */
  bank?: string;
  /** The mod mesh section hint of the first mesh (default 486); Melange relocates a section that is taken. */
  section?: number;
  /** The game folder whose Data/Bundles/Bundl31.xom holds HORROR5, the material donor (default MELANGE_GAME_DIR). */
  gameDir?: string;
  xomtool?: string;
  materialFrom?: string;
  materialFile?: string;
  /** Keep the glTF and PNG sources in <out>/src (default false). */
  keepSource?: boolean;
  generator?: string;
}
export interface BankResult {
  file: string; bytes: number; section: number | undefined;
  meshes: { name: string; kind: string; seed: number; vertices: number; triangles: number; box: { min: number[]; max: number[] } }[];
  spice: { meshes: { file: string }[] };
}

/** The xomtool beside the repo's dist (tools/erg-mcp/dist/erg-mcp.mjs -> ../../../dist/tools/xomtool.exe), or $XOMTOOL. */
export function findXomtool(explicit?: string): string {
  const cands = [explicit, process.env.XOMTOOL, process.env.MELANGE_XOMTOOL].filter((x): x is string => !!x);
  for (const c of cands) if (existsSync(c)) return c;
  const here = (import.meta as unknown as { url: string }).url.replace(/^file:\/\/\//, "").replace(/%20/g, " ");
  const root = join(here, "..", "..", "..", "..");
  for (const rel of ["dist/tools/xomtool.exe", "tools/xomtool.exe"]) {
    const f = join(root, rel);
    if (existsSync(f)) return f;
  }
  throw new Error("xomtool not found: pass xomtool or set XOMTOOL (build.ps1 writes dist\\tools\\xomtool.exe)");
}

/** Writes the sources and the merged bank `<outDir>/<id>.<bank>.xom`. Needs xomtool and the donor bank. */
export function buildBank(outDir: string, o: BankOptions, built: BuiltRocks = buildRocks(o.rocks, { palette: o.palette, seed: o.seed, generator: o.generator })): BankResult {
  const xomtool = findXomtool(o.xomtool);
  const game = o.gameDir ?? process.env.MELANGE_GAME_DIR;
  const matFile = o.materialFile ?? (game ? join(game, "Data/Bundles/Bundl31.xom") : undefined);
  if (!matFile || !existsSync(matFile)) throw new Error("the material donor bundle is missing: pass gameDir (a game folder with Data/Bundles/Bundl31.xom) or set MELANGE_GAME_DIR");
  mkdirSync(outDir, { recursive: true });
  const tmp = mkdtempSync(join(tmpdir(), "rockbank-"));
  const x = (...a: string[]) => execFileSync(xomtool, a, { encoding: "utf8", windowsHide: true, timeout: 120000 });
  try {
    const src = o.keepSource ? join(outDir, "src") : join(tmp, "src");
    mkdirSync(src, { recursive: true });
    for (const [key, data] of built.textures) writeFileSync(join(src, `${key}.png`), data);
    const docs: unknown[] = [];
    let sec = o.section ?? 486;
    for (const r of built.rocks) {
      const id = `${o.id}.${r.name}`, g = join(src, `${r.name}.gltf`), bank = join(tmp, `${id}.xom`), json = join(tmp, `${id}.json`);
      writeFileSync(g, r.mesh.gltf); writeFileSync(join(src, `${r.name}.bin`), r.mesh.bin);
      x("convert", g, "--bundle", bank, "--as", id, "--section", String(sec++), "--material-from", o.materialFrom ?? "HORROR5",
        "--material-file", matFile, "--texture", join(src, `${r.textureKey}.png`));
      x("unpack", bank, "-o", json);
      const doc = JSON.parse(readFileSync(json, "utf8"));
      const mats = matteDoc(doc);
      if (mats !== 1) throw new Error(`${id}: expected one XMaterial in the bank, found ${mats} (xomtool unpack's JSON shape changed?)`);
      docs.push(doc);
    }
    const m = mergeDocs(docs, o.section ?? 486);
    const file = join(outDir, `${o.id}.${o.bank ?? "Rocks"}.xom`), j = join(tmp, "merged.json");
    writeFileSync(j, JSON.stringify(m.doc));
    x("pack", j, file);
    const want = built.rocks.map((r) => `${o.id}.${r.name}`).sort().join(), got = [...m.meshes].sort().join();
    if (want !== got) throw new Error(`the merged bank holds ${got}, not ${want}`);
    return { file, bytes: statSync(file).size, section: m.section,
      meshes: built.rocks.map((r) => ({ name: `${o.id}.${r.name}`, kind: r.kind, seed: r.seed, vertices: r.mesh.vertices, triangles: r.mesh.triangles, box: r.box })),
      spice: { meshes: [{ file: `assets/meshes/${o.id}.${o.bank ?? "Rocks"}.xom` }] } };
  } finally {
    rmSync(tmp, { recursive: true, force: true });
  }
}
