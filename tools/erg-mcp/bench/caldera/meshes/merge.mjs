// Merges mod mesh banks into ONE bank (docs/meshes.md "Many meshes in one bank"): every bank a mod lists under spice.json
// `meshes` takes one of the 44 mod sections all enabled mods share (476-519), so a map ships all its meshes in one bank.
// `xomtool convert --bundle` writes one mesh per bank; this joins such banks the way a vanilla BundlNN.xom is laid out:
// the root XGraphSet lists every XMeshDescriptor (all with one SectionId), each descriptor keeps its own name, graph,
// shader, material and texture. Pure JSON over `xomtool unpack` / `xomtool pack`; nothing else in the objects changes.
//
//   tools\node\node.exe tools\erg-mcp\bench\caldera\meshes\merge.mjs <out.xom> <in1.xom> <in2.xom> ... [--section N] [--xomtool <exe>]
// --section defaults to the first bank's. Melange relocates a bank whose section is taken (meshbank.h), so it is a hint.
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, "../../../../..");

// Every {ref: n} (n > 0: objects[n - 1]; 0 is null) in a value, rewritten by fn.
function mapRefs(v, fn) {
  if (Array.isArray(v)) return v.map((x) => mapRefs(x, fn));
  if (v && typeof v === "object") {
    const keys = Object.keys(v);
    if (keys.length === 1 && keys[0] === "ref" && typeof v.ref === "number") return { ref: v.ref ? fn(v.ref) : 0 };
    const o = {};
    for (const k of keys) o[k] = mapRefs(v[k], fn);
    return o;
  }
  return v;
}

/** Merges unpacked bank documents (melange-xom/1 JSON) into one. Throws on a shape it does not know. */
export function mergeDocs(docs, section) {
  if (!docs.length) throw new Error("no banks to merge");
  const typeNames = [];
  const typeByName = new Map();
  for (const d of docs) {
    if (d.format !== "melange-xom/1") throw new Error(`not a melange-xom/1 document: ${d.format}`);
    for (const t of d.types) {
      const have = typeByName.get(t.name);
      if (!have) { typeByName.set(t.name, { ...t }); typeNames.push(t.name); continue; }
      if (have.guid !== t.guid || have.version !== t.version) throw new Error(`type ${t.name} differs between banks`);
    }
  }
  // the objects of each type, bank by bank, in the type table's order (xom.cpp: objects are grouped by type)
  const roots = docs.map((d, b) => {
    const r = d.objects[d.root - 1];
    if (!r || r.type !== "XGraphSet") throw new Error(`bank ${b}: the root is not an XGraphSet`);
    return d.root - 1;
  });
  const order = [];   // [bank, oldIndex]
  for (const name of typeNames)
    docs.forEach((d, b) => d.objects.forEach((o, i) => {
      if (o.type !== name) return;
      if (b > 0 && i === roots[b]) return;   // the other banks' root graph sets fold into the first's
      order.push([b, i]);
    }));
  const newIndex = docs.map(() => new Map());
  order.forEach(([b, i], n) => newIndex[b].set(i, n));
  for (let b = 1; b < docs.length; b++) newIndex[b].set(roots[b], newIndex[0].get(roots[0]));
  const objects = order.map(([b, i]) => {
    const o = docs[b].objects[i];
    return { ...o, fields: mapRefs(o.fields, (r) => {
      const n = newIndex[b].get(r - 1);
      if (n === undefined) throw new Error(`bank ${b}: a ref to object ${r} that does not exist`);
      return n + 1;
    }) };
  });
  const root = objects[newIndex[0].get(roots[0])];
  const graphs = [];
  docs.forEach((d, b) => {
    for (const g of d.objects[roots[b]].fields.Graphs) graphs.push(mapRefs(g, (r) => newIndex[b].get(r - 1) + 1));
  });
  root.fields = { ...root.fields, Graphs: graphs };
  const names = new Set();
  let sec = section;
  for (const o of objects) {
    if (o.type !== "XMeshDescriptor") continue;
    if (names.has(o.fields.ResourceId)) throw new Error(`two banks name the mesh ${o.fields.ResourceId}`);
    names.add(o.fields.ResourceId);
    sec ??= o.fields.SectionId;
    o.fields = { ...o.fields, SectionId: sec };
  }
  const strings = [];
  for (const d of docs) for (const s of d.strings) if (!strings.includes(s)) strings.push(s);
  const counts = new Map();
  for (const o of objects) counts.set(o.type, (counts.get(o.type) ?? 0) + 1);
  const out = { ...docs[0], types: typeNames.map((n) => ({ ...typeByName.get(n), count: counts.get(n) ?? 0 })), strings, objects,
    root: newIndex[0].get(roots[0]) + 1 };
  for (const k of ["strs_raw", "tail_raw", "trailer"]) delete out[k];   // rebuilt by the writer
  return { doc: out, meshes: [...names], section: sec };
}

/** Merges bank files into `out` with xomtool; answers {meshes, section, bytes}. */
export function mergeBanks(inputs, out, { section, xomtool = path.join(ROOT, "dist/tools/xomtool.exe") } = {}) {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "bank-merge-"));
  try {
    const docs = inputs.map((f, i) => {
      const j = path.join(tmp, `${i}.json`);
      execFileSync(xomtool, ["unpack", f, "-o", j], { encoding: "utf8" });
      return JSON.parse(fs.readFileSync(j, "utf8"));
    });
    const m = mergeDocs(docs, section);
    const j = path.join(tmp, "merged.json");
    fs.writeFileSync(j, JSON.stringify(m.doc));
    execFileSync(xomtool, ["pack", j, out], { encoding: "utf8" });
    return { meshes: m.meshes, section: m.section, bytes: fs.statSync(out).size };
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const argv = process.argv.slice(2);
  const opt = (f) => { const i = argv.indexOf(f); if (i < 0) return undefined; const v = argv[i + 1]; argv.splice(i, 2); return v; };
  const section = opt("--section"), xomtool = opt("--xomtool");
  const [out, ...inputs] = argv;
  if (!out || !inputs.length) { console.error("usage: merge.mjs <out.xom> <in.xom>... [--section N] [--xomtool exe]"); process.exit(2); }
  const r = mergeBanks(inputs, out, { section: section ? Number(section) : undefined, ...(xomtool ? { xomtool } : {}) });
  console.log(JSON.stringify(r));
}
