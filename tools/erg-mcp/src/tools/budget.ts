// Tool group "budget": detail_budget, how many more details the open level can take before the game's mesh-instance pool
// runs out at load (lib/budget.ts). The round-6 judge's tool gap: "no tool reports the remaining engine budget before saving".
import fs from "node:fs";
import path from "node:path";
import { budget, DEFAULT_RESERVE, meshPoolFromIni } from "../lib/budget";
import { simFingerprint } from "../lib/sim-fingerprint";
import { shipRange } from "../lib/ship-gate";
import { ToolError, type Tool } from "../registry";

export const tools: Tool[] = [
  {
    name: "detail_budget",
    description: "The open level's details (sim and visual: sim_fingerprint's counts) against the game's mesh-instance pool, which every drawn detail, worm hat, thrown mine and effect takes one of: vanilla 550 (a level with about 540 details crashes at load: WormsMayhem+0x1d0fad), Melange's [Fixes] MeshInstances (default 2000) raises it. `gameDir` reads that folder's Melange.ini for the pool (or give `pool`); `reserve` is what the match itself needs (default 250). Answers details, pool, headroom (pool - reserve - details), ok, and the headroom a player without the fix would have. Read-only.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      gameDir: { type: "string", description: "The game folder whose Melange.ini sets the pool." },
      pool: { type: "integer", minimum: 1, maximum: 65535, description: "The pool size, instead of reading gameDir." },
      reserve: { type: "integer", minimum: 0, maximum: 4000, description: `Instances the match needs besides the level's details (default ${DEFAULT_RESERVE}).` },
    } },
    async handler(s, a: { gameDir?: string; pool?: number; reserve?: number }) {
      const p = s.requireProject();
      const c = simFingerprint(p.store, "").counts;
      const details = (c.simDetails ?? 0) + (c.scenery ?? 0) + (c.light ?? 0) + (c.emitter ?? 0) + (c.sound ?? 0);
      let pool = a.pool, from = "given";
      if (pool === undefined) {
        if (!a.gameDir) throw new ToolError("detail_budget: give gameDir (its Melange.ini sets the pool) or pool");
        const f = path.join(a.gameDir, "Melange.ini");
        const r = meshPoolFromIni(fs.existsSync(f) ? fs.readFileSync(f, "utf8") : null);
        pool = r.pool; from = r.from;
      }
      return { ...budget(details, pool, a.reserve ?? DEFAULT_RESERVE), poolFrom: from, counts: c, ship: shipRange(details) };
    },
  },
];
