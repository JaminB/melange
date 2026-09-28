// Resolves web dependencies from the npm registry's metadata and writes web/web.lock.json. Maintainers only, under
// the portable Node (tools\node\node.exe). No npm client and no install script is involved.
//   node scripts/web/lock.mjs preact@^10.29.8 @tanstack/virtual-core@^3.17.11 --dev typescript@5.9.3 playwright-core@1.63.0
// Every package, transitive ones included, gets {name, version, tarball, integrity, license, dev}. Fails when a
// version cannot be resolved, two versions of one package would be needed, or a licence is not on the allowlist.
import { writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const REGISTRY = "https://registry.npmjs.org";
const ALLOW = ["MIT", "ISC", "BSD-2-Clause", "BSD-3-Clause", "Apache-2.0", "0BSD"];
const root = join(dirname(fileURLToPath(import.meta.url)), "..", "..");

export function licenseOk(expr) {
  if (typeof expr !== "string" || !expr.trim()) return false;
  let e = expr.trim();
  while (e.startsWith("(") && e.endsWith(")")) e = e.slice(1, -1).trim();
  if (e.includes("(")) return false;
  return e.split(/\s+OR\s+/).some((alt) => alt.split(/\s+AND\s+/).every((t) => ALLOW.includes(t.trim())));
}

function parseVersion(v) {
  const m = /^v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+.*)?$/.exec(v.trim());
  return m ? { major: +m[1], minor: +m[2], patch: +m[3], pre: m[4] ? m[4].split(".") : [] } : null;
}

export function compare(a, b) {
  const x = parseVersion(a), y = parseVersion(b);
  for (const k of ["major", "minor", "patch"]) if (x[k] !== y[k]) return x[k] - y[k];
  if (!x.pre.length || !y.pre.length) return y.pre.length - x.pre.length;
  for (let i = 0; i < Math.max(x.pre.length, y.pre.length); i++) {
    if (x.pre[i] === undefined) return -1;
    if (y.pre[i] === undefined) return 1;
    const na = /^\d+$/.test(x.pre[i]), nb = /^\d+$/.test(y.pre[i]);
    if (na && nb && +x.pre[i] !== +y.pre[i]) return +x.pre[i] - +y.pre[i];
    if (na !== nb) return na ? -1 : 1;
    if (x.pre[i] !== y.pre[i]) return x.pre[i] < y.pre[i] ? -1 : 1;
  }
  return 0;
}

// One comparator set ("^1.2.3", ">=1 <2", "1.x", "~1.2") as [op, version] pairs.
function comparators(set) {
  const out = [];
  const parts = set.trim().replace(/\s+-\s+/, " - ").split(/\s+/).filter(Boolean);
  for (let i = 0; i < parts.length; i++) {
    let p = parts[i];
    if (parts[i + 1] === "-") {
      out.push([">=", fill(p, 0)], ["<=", fill(parts[i + 2], 0)]);
      i += 2;
      continue;
    }
    const m = /^(\^|~|>=|<=|>|<|=)?\s*(.*)$/.exec(p);
    const op = m[1] || "", v = m[2];
    if (v === "*" || v === "" || v === "x" || v === "X") { out.push([">=", "0.0.0"]); continue; }
    const xs = v.split(".");
    const wild = xs.length < 3 || xs.some((s) => s === "x" || s === "X" || s === "*");
    const n = xs.map((s) => (/^\d+$/.test(s) ? +s : null));
    const base = fill(v, 0);
    if (op === "^") {
      const b = parseVersion(base);
      const upper = b.major > 0 ? `${b.major + 1}.0.0` : b.minor > 0 || n[1] === null ? `0.${b.minor + 1}.0` : `0.0.${b.patch + 1}`;
      out.push([">=", base], ["<", n[1] === null && b.major === 0 ? "1.0.0" : upper]);
    } else if (op === "~" || (op === "" && wild)) {
      const b = parseVersion(base);
      if (n[1] === null || n[1] === undefined) out.push([">=", base], ["<", `${b.major + 1}.0.0`]);
      else if (op === "~" || n[2] === null || n[2] === undefined) out.push([">=", base], ["<", `${b.major}.${b.minor + 1}.0`]);
      else out.push(["=", base]);
    } else {
      out.push([op || "=", base]);
    }
  }
  return out;
}

function fill(v, d) {
  const xs = v.split(".").map((s) => (/^\d+$/.test(s) ? s : String(d)));
  while (xs.length < 3) xs.push(String(d));
  return xs.slice(0, 3).join(".") + (/-/.test(v) && !v.includes("x") ? v.slice(v.indexOf("-")) : "");
}

export function satisfies(version, range) {
  if (!parseVersion(version)) return false;
  return range.split("||").some((set) => {
    const cs = comparators(set);
    if (parseVersion(version).pre.length && !cs.some(([, v]) => parseVersion(v).pre.length)) return false;
    return cs.every(([op, v]) => {
      const c = compare(version, v);
      return op === "=" ? c === 0 : op === ">" ? c > 0 : op === ">=" ? c >= 0 : op === "<" ? c < 0 : c <= 0;
    });
  });
}

async function get(url, accept) {
  const r = await fetch(url, { headers: { accept } });
  if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
  return r.json();
}

const pkgUrl = (name) => `${REGISTRY}/${name.replace("/", "%2f")}`;

async function resolve(name, range, cache) {
  if (parseVersion(range)) return range.replace(/^v/, "");
  if (!cache.has(name)) cache.set(name, await get(pkgUrl(name), "application/vnd.npm.install-v1+json"));
  const doc = cache.get(name);
  if (doc["dist-tags"]?.[range]) return doc["dist-tags"][range];
  const ok = Object.keys(doc.versions).filter((v) => satisfies(v, range)).sort(compare);
  if (!ok.length) throw new Error(`${name}@${range}: no matching version`);
  return ok[ok.length - 1];
}

function splitSpec(spec) {
  const at = spec.lastIndexOf("@");
  return at > 0 ? [spec.slice(0, at), spec.slice(at + 1)] : [spec, "latest"];
}

async function main(args) {
  const roots = [];
  let dev = false;
  for (const a of args) {
    if (a === "--dev") dev = true;
    else roots.push({ spec: a, dev });
  }
  if (!roots.length) throw new Error("usage: lock.mjs <name>@<range> ... [--dev <name>@<range> ...]");
  const cache = new Map();
  const chosen = new Map();
  const queue = roots.map((r) => {
    const [name, range] = splitSpec(r.spec);
    return { name, range, dev: r.dev, from: "(root)" };
  });
  while (queue.length) {
    const { name, range, dev: isDev, from } = queue.shift();
    const version = await resolve(name, range, cache);
    const have = chosen.get(name);
    if (have) {
      if (have.version !== version && !satisfies(have.version, range))
        throw new Error(`${name}: ${have.version} and ${version} are both needed (${from} wants ${range}); a flat node_modules cannot hold both`);
      if (!isDev) have.dev = false;
      continue;
    }
    const manifest = await get(`${pkgUrl(name)}/${version}`, "application/json");
    const license = typeof manifest.license === "string" ? manifest.license : manifest.license?.type;
    if (!licenseOk(license)) throw new Error(`${name}@${version}: licence '${license}' is not on the allowlist (${ALLOW.join(", ")})`);
    if (!manifest.dist?.integrity?.startsWith("sha512-")) throw new Error(`${name}@${version}: no sha512 integrity`);
    chosen.set(name, { name, version, tarball: manifest.dist.tarball, integrity: manifest.dist.integrity, license, dev: isDev,
      installScript: !!(manifest.scripts && (manifest.scripts.preinstall || manifest.scripts.install || manifest.scripts.postinstall)) });
    for (const [dep, r] of Object.entries(manifest.dependencies || {})) queue.push({ name: dep, range: r, dev: isDev, from: `${name}@${version}` });
  }
  const packages = [...chosen.values()].sort((a, b) => (a.name < b.name ? -1 : 1)).map((p) => {
    const o = { name: p.name, version: p.version, tarball: p.tarball, integrity: p.integrity, license: p.license };
    if (p.dev) o.dev = true;
    if (p.installScript) o.installScriptIgnored = true;
    return o;
  });
  const lock = { lockfileVersion: 1, registry: REGISTRY, roots: roots.map((r) => (r.dev ? `--dev ${r.spec}` : r.spec)), packages };
  const out = join(root, "web", "web.lock.json");
  writeFileSync(out, JSON.stringify(lock, null, 2) + "\n");
  for (const p of packages) console.log(`${p.name}@${p.version} ${p.license}${p.dev ? " (dev)" : ""}`);
  console.log(`lock: ${packages.length} packages -> ${out}`);
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  main(process.argv.slice(2)).catch((e) => {
    console.error(`lock: ${e.message}`);
    process.exit(1);
  });
}
