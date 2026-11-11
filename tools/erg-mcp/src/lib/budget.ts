// The engine budget a level's details draw from (pure): the game's mesh-instance pool. XGraphicalResourceManager sizes it
// from the static property GRM.MaxMeshes = 550; every drawn detail, worm hat, thrown mine and effect mesh takes one, and a
// level that runs it out crashes at load (CreateResource E_FAIL, null instance: WormsMayhem+0x1d0fad in
// OilDrumGraphicEntity). Melange's [Fixes] MeshInstances (default 2000) raises it before the GRM starts. Round 7 of the
// Caldera detail layer pinned this (docs/erg-mcp.md).
export const VANILLA_MESH_POOL = 550;
export const MELANGE_MESH_POOL = 2000;
export const MAX_MESH_POOL = 4000;
/** What the level's own details leave for the match (worms' hats and weapons, thrown mines, effects): measured, round 7. */
export const DEFAULT_RESERVE = 250;

/** The pool a game folder's Melange.ini gives: [Fixes] Enabled and MeshInstances (missing key: Melange's default). */
export function meshPoolFromIni(ini: string | null): { pool: number; from: string } {
  if (ini == null) return { pool: VANILLA_MESH_POOL, from: "no Melange.ini: the vanilla pool" };
  let section = "", enabled = true, value: number | null = null;
  for (const raw of ini.split(/\r?\n/)) {
    const line = raw.replace(/[;#].*$/, "").trim();
    const m = /^\[(.+)\]$/.exec(line);
    if (m) { section = m[1].trim().toLowerCase(); continue; }
    if (section !== "fixes") continue;
    const kv = /^([^=]+)=(.*)$/.exec(line);
    if (!kv) continue;
    const k = kv[1].trim().toLowerCase(), v = kv[2].trim();
    if (k === "enabled") enabled = v !== "0";
    if (k === "meshinstances" && /^-?\d+$/.test(v)) value = +v;
  }
  if (!enabled) return { pool: VANILLA_MESH_POOL, from: "[Fixes] Enabled=0: the vanilla pool" };
  if (value === null) return { pool: MELANGE_MESH_POOL, from: "[Fixes] MeshInstances unset: Melange's default" };
  if (value <= VANILLA_MESH_POOL) return { pool: VANILLA_MESH_POOL, from: `[Fixes] MeshInstances=${value}: the vanilla pool` };
  return { pool: Math.min(value, MAX_MESH_POOL), from: `[Fixes] MeshInstances=${value}` };
}

/** The level's details against a pool: headroom after the reserve, and whether it loads with margin. */
export function budget(details: number, pool: number, reserve = DEFAULT_RESERVE) {
  const headroom = pool - reserve - details;
  return { details, pool, reserve, headroom, ok: headroom >= 0, vanillaHeadroom: VANILLA_MESH_POOL - reserve - details };
}
