// wum-coverage: every wum.* name the C++ sandbox registers must be documented in docs/lua-api.md.
//
// Finds the names by reading the C++ source (no build needed):
//   - `luaL_Reg kX[] = {{"name", Fn}, ...}` tables in src/lua/**/*.cpp,*.h;
//   - which `wum.<ns>` each table becomes, from how it is registered:
//       `Ns{"log", kLog}` pairs (wum_core.cpp's loop),
//       `RegisterFunctions(L, -1, kX); lua_setfield(L, wum, "ns");` in the same function,
//       a file with one table left over and one `lua_setfield(L, wum, "ns")` that had nothing pending
//       (wum_unsafe.cpp: the table is built in Shared, installed per mod in PerEnv),
//       HAND_MAP below for anything else;
//   - fields set with `lua_setfield(L, -2, "name")` in a function that ends in `lua_setfield(L, wum, "ns")`
//     (wum.mod.id, wum.mod.readFile, ...);
//   - `melange::lua::AddLibrary("ns", &Open)` anywhere in src/** (wum.wormsign): the fields and tables `Open` sets.
//
// A name counts as documented when `wum.<ns>.<name>` appears in a heading or in inline code of docs/lua-api.md
// (fenced code blocks do not count). Fails on undocumented names unless scripts/wum-coverage.allow.json lists them
// with a reason. Warns for documented names nothing registers (unless allowlisted the same way).
import fs from 'node:fs/promises';
import path from 'node:path';
import { REPO_DIR, SITE_DIR, rel, runIfMain } from './lib/cli.mjs';

const LUA_DIR = path.join(REPO_DIR, 'src', 'lua');
const SRC_DIR = path.join(REPO_DIR, 'src');
const DOC = path.join(REPO_DIR, 'docs', 'lua-api.md');
const ALLOW = path.join(SITE_DIR, 'scripts', 'wum-coverage.allow.json');

// Hand-maintained map for luaL_Reg tables whose namespace cannot be derived from the code above.
// Key: "<repo-relative file>:<table>", value: the wum namespace, or null for a table that is not part of wum.*.
// Keep it small; an unmapped table that is not listed here is a hard error, so a new registration pattern is noticed.
const HAND_MAP = {
  // Replacements for the global rawset/setmetatable (RegisterFunctions(L, g, kRepl)), not wum.* functions.
  'src/lua/sandbox.cpp:kRepl': null,
};

async function walk(dir, exts) {
  const out = [];
  for (const ent of await fs.readdir(dir, { withFileTypes: true })) {
    const p = path.join(dir, ent.name);
    if (ent.isDirectory()) out.push(...(await walk(p, exts)));
    else if (exts.includes(path.extname(ent.name))) out.push(p);
  }
  return out.sort();
}

const RE_TABLE = /luaL_Reg\s+(\w+)\s*\[\s*\]\s*=\s*\{([\s\S]*?)\}\s*;/g;
const RE_ENTRY = /\{\s*"(\w+)"\s*,\s*&?[\w:]+\s*\}/g;
const RE_NS_PAIR = /Ns\s*\{\s*"(\w+)"\s*,\s*(\w+)\s*\}/g;
const RE_ADDLIB = /AddLibrary\(\s*"(\w+)"\s*,\s*&?([\w:]+)/g;
// One regex for the statements the function scan cares about, so several on one line keep their order.
const RE_EVENT =
  /RegisterFunctions\(\s*L\s*,\s*([^,]+?)\s*,\s*([\w.]+)\s*\)|lua_setfield\(\s*L\s*,\s*-2\s*,\s*"(\w+)"\s*\)|lua_setfield\(\s*L\s*,\s*wum\s*,\s*"(\w+)"\s*\)|lua_push(cfunction|cclosure)\b|lua_(push\w+|newtable|createtable|rawgeti)\b/g;

function stripComments(text) {
  return text.replace(/\/\*[\s\S]*?\*\//g, (m) => m.replace(/[^\n]/g, ' ')).replace(/\/\/[^\n]*/g, '');
}

// Scans one file; adds names to `api` (Map name -> {kind, file}) and returns its unmapped tables.
function scanFile(file, text, api, openFns) {
  const relFile = rel(file);
  const tables = new Map(); // table name -> [entry names]
  for (const m of text.matchAll(RE_TABLE)) {
    tables.set(m[1], [...m[2].matchAll(RE_ENTRY)].map((e) => e[1]));
  }
  const mapped = new Set();
  const add = (ns, name, kind) => {
    const full = `wum.${ns}.${name}`;
    if (!api.has(full)) api.set(full, { kind, file: relFile });
  };
  const addTable = (ns, t) => {
    mapped.add(t);
    if (ns === null) return;
    for (const n of tables.get(t) ?? []) add(ns, n, 'function');
  };

  for (const m of text.matchAll(RE_NS_PAIR)) if (tables.has(m[2])) addTable(m[1], m[2]);

  const emptyAssigns = [];
  let pendingTables = [];
  let pendingFields = [];
  let lastPush = 'value';
  let openNs = null;
  const flush = (ns) => {
    for (const t of pendingTables) addTable(ns, t);
    for (const f of pendingFields) add(ns, f.name, f.kind);
    pendingTables = [];
    pendingFields = [];
  };
  for (const line of text.split('\n')) {
    if (/^\S.*\{\s*$/.test(line) && !/^(namespace|struct|class|enum|extern)\b/.test(line)) {
      // Column-0 line opening a block: a function definition (or a file-scope table, harmless).
      pendingTables = [];
      pendingFields = [];
      openNs = null;
      for (const [fn, ns] of openFns) if (new RegExp(`\\b${fn}\\s*\\(`).test(line)) openNs = ns;
      continue;
    }
    if (/^\}/.test(line)) {
      if (openNs) flush(openNs);
      pendingTables = [];
      pendingFields = [];
      openNs = null;
      continue;
    }
    for (const m of line.matchAll(RE_EVENT)) {
      if (m[2] !== undefined) {
        if (tables.has(m[2])) pendingTables.push(m[2]);
      } else if (m[3] !== undefined) {
        pendingFields.push({ name: m[3], kind: lastPush });
      } else if (m[4] !== undefined) {
        if (!pendingTables.length && !pendingFields.length) emptyAssigns.push(m[4]);
        flush(m[4]);
      } else if (m[5] !== undefined) {
        lastPush = 'function';
      } else if (m[6] !== undefined) {
        lastPush = 'value';
      }
    }
  }

  let unmapped = [...tables.keys()].filter((t) => !mapped.has(t));
  for (const t of [...unmapped]) {
    const key = `${relFile}:${t}`;
    if (Object.prototype.hasOwnProperty.call(HAND_MAP, key)) {
      addTable(HAND_MAP[key], t);
      unmapped = unmapped.filter((u) => u !== t);
    }
  }
  if (unmapped.length === 1 && emptyAssigns.length === 1) {
    addTable(emptyAssigns[0], unmapped[0]);
    unmapped = [];
  }
  return unmapped.map((t) => `${relFile}:${t}`);
}

function documentedNames(md) {
  const names = new Set();
  const grab = (s) => {
    for (const m of s.matchAll(/\bwum\.(\w+)\.(\w+)/g)) names.add(`wum.${m[1]}.${m[2]}`);
  };
  // Drop fenced code blocks; examples do not count as documentation.
  const prose = md.replace(/^(```|~~~)[^\n]*\n[\s\S]*?^\1[^\n]*$/gm, '');
  for (const line of prose.split('\n')) {
    if (/^#{1,6}\s/.test(line)) grab(line);
  }
  for (const m of prose.matchAll(/(`+)([\s\S]*?[^`])\1(?!`)/g)) grab(m[2]);
  return names;
}

async function readAllow() {
  let raw;
  try {
    raw = JSON.parse(await fs.readFile(ALLOW, 'utf8'));
  } catch (err) {
    if (err.code === 'ENOENT') return { undocumented: new Map(), unregistered: new Map() };
    throw new Error(`wum-coverage: cannot read ${rel(ALLOW)}: ${err.message}`);
  }
  const load = (key) => {
    const map = new Map();
    for (const e of raw[key] ?? []) {
      if (!e || typeof e.name !== 'string' || typeof e.reason !== 'string' || !e.reason.trim())
        throw new Error(`wum-coverage: ${rel(ALLOW)} ${key}: every entry needs a "name" and a "reason"`);
      map.set(e.name, e.reason);
    }
    return map;
  };
  return { undocumented: load('undocumented'), unregistered: load('unregistered') };
}

export default async function wumCoverage() {
  const api = new Map();
  const files = await walk(LUA_DIR, ['.cpp', '.h']);

  // AddLibrary("ns", &Open) anywhere in src/: Open's body defines wum.ns.
  const openFnsByFile = new Map();
  for (const f of await walk(SRC_DIR, ['.cpp'])) {
    const text = stripComments(await fs.readFile(f, 'utf8'));
    for (const m of text.matchAll(RE_ADDLIB)) {
      if (!openFnsByFile.has(f)) openFnsByFile.set(f, new Map());
      openFnsByFile.get(f).set(m[2].split('::').pop(), m[1]);
    }
  }
  const scanList = [...new Set([...files, ...openFnsByFile.keys()])];

  const unmapped = [];
  for (const f of scanList) {
    const text = stripComments(await fs.readFile(f, 'utf8'));
    unmapped.push(...scanFile(f, text, api, openFnsByFile.get(f) ?? new Map()));
  }
  if (unmapped.length) {
    throw new Error(
      `wum-coverage: cannot tell which wum namespace these luaL_Reg tables belong to; add them to HAND_MAP in scripts/wum-coverage.mjs:\n  ${unmapped.join('\n  ')}`,
    );
  }
  if (api.size < 60) throw new Error(`wum-coverage: found only ${api.size} wum.* names in the C++ source; the scan is broken`);

  const documented = documentedNames(await fs.readFile(DOC, 'utf8'));
  const allow = await readAllow();
  const warnings = [];

  const missing = [...api.keys()].filter((n) => !documented.has(n) && !allow.undocumented.has(n)).sort();
  const unregistered = [...documented].filter((n) => !api.has(n) && !allow.unregistered.has(n)).sort();
  for (const n of unregistered) warnings.push(`documented in docs/lua-api.md but not registered: ${n}`);
  for (const n of allow.undocumented.keys()) {
    if (!api.has(n)) warnings.push(`allowlist entry ${n} (undocumented) is no longer registered; remove it`);
    else if (documented.has(n)) warnings.push(`allowlist entry ${n} (undocumented) is documented now; remove it`);
  }
  for (const n of allow.unregistered.keys()) {
    if (!documented.has(n)) warnings.push(`allowlist entry ${n} (unregistered) is no longer documented; remove it`);
    else if (api.has(n)) warnings.push(`allowlist entry ${n} (unregistered) is registered now; remove it`);
  }
  for (const w of warnings) console.warn(`WARN wum-coverage: ${w}`);

  if (missing.length) {
    const lines = missing.map((n) => `  ${n}  (${api.get(n).file})`);
    throw new Error(
      `wum-coverage: FAIL, ${missing.length} wum.* name(s) registered in C++ but not documented in docs/lua-api.md ` +
        `(as \`wum.ns.name\` in a heading or inline code):\n${lines.join('\n')}`,
    );
  }
  const fns = [...api.values()].filter((v) => v.kind === 'function').length;
  const ns = new Set([...api.keys()].map((n) => n.split('.')[1])).size;
  console.log(
    `wum-coverage: ok, ${api.size} names (${fns} functions) in ${ns} namespaces documented` +
      `${allow.undocumented.size ? `, ${allow.undocumented.size} allowlisted` : ''}${warnings.length ? `, ${warnings.length} warning(s)` : ''}`,
  );
  return { names: [...api.keys()].sort(), missing, warnings };
}

runIfMain(import.meta.url, wumCoverage);
