// Types of the game-state channels and methods, and the pure helpers the panel uses (unit-tested).

export interface Vec3 { x: number | null; y: number | null; z: number | null; }
export interface Worm {
  slot: number; team: number; posInTeam: number; name: string; active: boolean; alive: boolean;
  health: number; physicsState: number; weapon: number; pos: Vec3; vel: Vec3;
}
export interface Team {
  slot: number; name: string; active: boolean; ai: boolean; local: boolean;
  colour: number; alliance: number; roundsWon: number; score: number;
}
export interface Match {
  inMatch: boolean; online: boolean; currentTeam: number; activeWorm: number;
  turnMs: number; turnMsLeft: number; roundMs: number; roundMsLeft: number;
  windSpeed: number | null; windDir: number | null; waterLevel: number | null;
  turnsStarted: number; suddenDeath: boolean; theme: string;
}
export interface Snapshot { available: boolean; frame: number; matchSerial: number; match: Match; teams: Team[]; worms: Worm[]; }

export type Kind = "Worm" | "Projectile" | "Crate" | "Barrel" | "Other";
export const KINDS: Kind[] = ["Worm", "Projectile", "Crate", "Barrel", "Other"];
export interface Entity {
  handle: number; object: number; vtable: number; kind: Kind; type: string; label: string;
  pos: Vec3 | null; vel: Vec3 | null;
}

export type VarType = "Int" | "Uint" | "Float" | "Vector" | "String" | "Container" | "StringTable" | "Color" | "Undefined";
export interface Var { name: string; type: VarType; value: unknown; }

export interface Inspection {
  handle?: number; addr: number; len: number; vtable: number; type: string; kind?: Kind;
  fields: Record<string, unknown>; hex: string | null;
}

const PHYSICS = ["Ambulatory", "DetectJump", "Ballistic", "Sliding", "Vaulting", "Override", "Passive", "DeathThroes", "DrownFloat", "Undefined"];
const WEAPONS: Record<number, string> = {
  1: "Bazooka", 2: "Grenade", 3: "Cluster Grenade", 4: "Airstrike", 5: "Dynamite", 6: "Holy Hand Grenade",
  7: "Banana Bomb", 8: "Landmine", 9: "Shotgun", 10: "Baseball Bat", 11: "Prod", 12: "Fire Punch",
  13: "Homing Missile", 14: "Flood", 15: "Sheep", 16: "Gas Canister", 17: "Old Woman", 18: "Concrete Donkey",
  19: "Super Sheep", 20: "Starburst", 21: "Factory Weapon", 22: "Alien Abduction", 23: "Fatkins", 24: "Scouser",
  25: "No More Nails", 26: "Poison Arrow", 27: "Sentry Gun", 28: "Sniper Rifle", 29: "Super Airstrike",
  30: "Cluster Bomb", 31: "Bananette", 34: "Girder", 35: "Ninja Rope", 36: "Parachute", 37: "Jetpack",
  38: "Skip Go", 39: "Surrender", 40: "Change Worm", 41: "Redbull", 42: "Bubble Trouble", 43: "Binoculars",
  44: "Double Damage", 45: "Crate Shower", 46: "Crate Spy", 47: "Armour",
};

export function physicsName(n: number): string {
  return PHYSICS[n] ?? `state ${n}`;
}

export function weaponName(id: number): string {
  if (id < 0) return "—";
  if (id >= 50 && id <= 64) return `Mystery ${id - 49}`;
  return WEAPONS[id] ?? `#${id}`;
}

export function hex32(n: number): string {
  return `0x${(n >>> 0).toString(16).padStart(8, "0")}`;
}

// "0x95b4a8", "95B4A8h" or a decimal: the address, or undefined.
export function parseAddress(s: string): number | undefined {
  const t = s.trim().toLowerCase().replace(/_/g, "");
  let n: number;
  if (/^0x[0-9a-f]{1,8}$/.test(t)) n = parseInt(t.slice(2), 16);
  else if (/^[0-9a-f]{1,8}h$/.test(t)) n = parseInt(t.slice(0, -1), 16);
  else if (/^[0-9]{1,10}$/.test(t)) n = Number(t);
  else return undefined;
  return n > 0 && n <= 0xffffffff ? n : undefined;
}

export function fmtNum(v: number | null | undefined, digits = 1): string {
  return typeof v === "number" && Number.isFinite(v) ? v.toFixed(digits) : "—";
}

export function fmtVec(v: Vec3 | null | undefined, digits = 1): string {
  return v ? `${fmtNum(v.x, digits)}, ${fmtNum(v.y, digits)}, ${fmtNum(v.z, digits)}` : "—";
}

export function speed(v: Vec3 | null | undefined): number {
  if (!v) return 0;
  const x = v.x ?? 0, y = v.y ?? 0, z = v.z ?? 0;
  return Math.sqrt(x * x + y * y + z * z);
}

export function fmtClock(ms: number): string {
  if (!Number.isFinite(ms) || ms <= 0) return "0:00";
  const s = Math.ceil(ms / 1000);
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;
}

// One row of a hex dump: `cells` are byte values or null for unreadable ("??") bytes.
export interface HexRow { offset: number; addr: number; cells: (number | null)[]; ascii: string; }

export function hexRows(hex: string, base: number, width = 16): HexRow[] {
  const cells: (number | null)[] = [];
  for (let i = 0; i + 1 < hex.length; i += 2) {
    const pair = hex.slice(i, i + 2);
    cells.push(pair === "??" ? null : parseInt(pair, 16));
  }
  const rows: HexRow[] = [];
  for (let off = 0; off < cells.length; off += width) {
    const part = cells.slice(off, off + width);
    const ascii = part.map((b) => (b === null ? " " : b >= 0x20 && b < 0x7f ? String.fromCharCode(b) : ".")).join("");
    rows.push({ offset: off, addr: (base + off) >>> 0, cells: part, ascii });
  }
  return rows;
}

// Little-endian values at `offset` of a dump, for the inspector's value strip; undefined when unreadable.
export function readLE(hex: string, offset: number, size: 1 | 2 | 4): number | undefined {
  let v = 0;
  for (let i = size - 1; i >= 0; i--) {
    const pair = hex.slice((offset + i) * 2, (offset + i) * 2 + 2);
    if (pair.length < 2 || pair === "??") return undefined;
    v = v * 256 + parseInt(pair, 16);
  }
  return v;
}

export function readFloat(hex: string, offset: number): number | undefined {
  const u = readLE(hex, offset, 4);
  if (u === undefined) return undefined;
  const dv = new DataView(new ArrayBuffer(4));
  dv.setUint32(0, u, true);
  return dv.getFloat32(0, true);
}

export function filterEntities(list: readonly Entity[], kinds: ReadonlySet<Kind>, text: string): Entity[] {
  const q = text.trim().toLowerCase();
  return list.filter((e) => kinds.has(e.kind) &&
    (!q || e.type.toLowerCase().includes(q) || e.label.toLowerCase().includes(q) || hex32(e.handle).includes(q)));
}

export function kindCounts(list: readonly Entity[]): Record<Kind, number> {
  const c: Record<Kind, number> = { Worm: 0, Projectile: 0, Crate: 0, Barrel: 0, Other: 0 };
  for (const e of list) c[e.kind] = (c[e.kind] ?? 0) + 1;
  return c;
}

export function filterVars(list: readonly Var[], text: string, types?: ReadonlySet<VarType>): Var[] {
  const q = text.trim().toLowerCase();
  return list
    .filter((v) => (!types || types.has(v.type)) && (!q || v.name.toLowerCase().includes(q) || valueText(v).toLowerCase().includes(q)))
    .sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
}

export function valueText(v: Var): string {
  const x = v.value;
  if (x === null || x === undefined) return "—";
  if (typeof x === "string") return v.type === "String" ? JSON.stringify(x) : x;
  if (typeof x === "number") return String(x);
  if (Array.isArray(x)) return x.map((n) => (typeof n === "number" ? String(n) : "—")).join(", ");
  if (typeof x === "object" && "addr" in x) {
    const c = x as { addr: number; class?: string };
    return `${c.class || "container"} @ ${hex32(c.addr)}`;
  }
  return JSON.stringify(x);
}

export function containerAddr(v: Var): number | undefined {
  const x = v.value as { addr?: unknown } | null;
  return v.type === "Container" && x && typeof x === "object" && typeof x.addr === "number" ? x.addr : undefined;
}

export type View = "top" | "side";
export interface Plotted<T> { item: T; x: number; y: number; }
export interface Plot<T> { points: Plotted<T>[]; waterY?: number; bounds: { minA: number; maxA: number; minB: number; maxB: number }; }

// World to SVG: "top" looks down +Y (X across, Z down the page), "side" looks along Z (X across, +Y up).
export function project<T>(items: readonly T[], pos: (t: T) => Vec3 | null | undefined, view: View,
                           w: number, h: number, pad = 16, water?: number | null): Plot<T> {
  const pts: { item: T; a: number; b: number }[] = [];
  for (const it of items) {
    const p = pos(it);
    if (!p || p.x === null || p.y === null || p.z === null) continue;
    pts.push({ item: it, a: p.x, b: view === "top" ? p.z : p.y });
  }
  let minA = Infinity, maxA = -Infinity, minB = Infinity, maxB = -Infinity;
  for (const p of pts) {
    minA = Math.min(minA, p.a); maxA = Math.max(maxA, p.a);
    minB = Math.min(minB, p.b); maxB = Math.max(maxB, p.b);
  }
  const useWater = view === "side" && typeof water === "number" && Number.isFinite(water);
  if (useWater && pts.length) { minB = Math.min(minB, water!); maxB = Math.max(maxB, water!); }
  if (!pts.length) { minA = minB = -1; maxA = maxB = 1; }
  // Same scale on both axes so the layout keeps its shape; a small margin so nothing sits on the edge.
  const spanA = Math.max(maxA - minA, 1), spanB = Math.max(maxB - minB, 1);
  const scale = Math.min((w - 2 * pad) / spanA, (h - 2 * pad) / spanB);
  const offA = (w - spanA * scale) / 2, offB = (h - spanB * scale) / 2;
  const toX = (a: number) => offA + (a - minA) * scale;
  const toY = (b: number) => (view === "top" ? offB + (b - minB) * scale : h - offB - (b - minB) * scale);
  return {
    points: pts.map((p) => ({ item: p.item, x: toX(p.a), y: toY(p.b) })),
    waterY: useWater ? toY(water!) : undefined,
    bounds: { minA, maxA, minB, maxB },
  };
}

const TEAM_COLOURS = ["#d1495b", "#2e86ab", "#3aa35b", "#e0a526", "#8e5bd1", "#e07a26", "#2bb3a8", "#9a9a9a"];
export function teamColour(team: number): string {
  return TEAM_COLOURS[((team % TEAM_COLOURS.length) + TEAM_COLOURS.length) % TEAM_COLOURS.length];
}

// Wind as an arrow angle in degrees for CSS (0 = pointing right, clockwise positive on screen).
export function windDegrees(dirRadians: number | null): number {
  return typeof dirRadians === "number" && Number.isFinite(dirRadians) ? (-dirRadians * 180) / Math.PI : 0;
}
