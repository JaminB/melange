// Tool group "fingerprint": sim_fingerprint, the open level's simulation fingerprint (lib/sim-fingerprint.ts), so a
// decoration pass (bench/caldera/detail.mjs) can prove it changed nothing the game's simulation reads.
import { diffFingerprints, simFingerprint, type Fingerprint } from "../lib/sim-fingerprint";
import { ToolError, type Tool } from "../registry";

export const tools: Tool[] = [
  {
    name: "sim_fingerprint",
    description: "The open level's simulation fingerprint: `sim`, a sha256 of everything that can change what the game simulates (voxel occupancy and the collision-shaping bits 8-23 of every frame, frames with their drape corners, trims, the spawn and object knots, cameras and look-at points, every detail not proven visual, the objects with their settings (the mine factory's too), water, spawn mode, the surround, the databank, the level script), with a hash per part (`parts`), and `visual`, the same for the channels proven visual only (scenery, light, emitter and sound details whose names carry no engine tag but VISIBLE; the primary material of solid voxels: `visualParts`). `against` (a previous answer, or its sim) answers `same` and the parts that differ; `detail` adds a hash per voxel frame (then `against` names the frames that changed) and the list of sim details with why each is sim. `strict` counts emitters, sounds and the primary paint as sim too. Read-only. docs/erg-mcp.md \"Detail layer\" has each channel's verdict and its evidence.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      against: { description: "A previous sim_fingerprint answer (or its `sim` string) to compare with.", anyOf: [{ type: "string" }, { type: "object" }] },
      detail: { type: "boolean", description: "Add per-frame voxel hashes and the sim details (default false)." },
      strict: { type: "boolean", description: "Treat emitters, sounds and primary paint as sim too (default false)." },
    } },
    async handler(s, a: { against?: string | Partial<Fingerprint>; detail?: boolean; strict?: boolean }) {
      const p = s.requireProject();
      // the script text from the server; one without level.script.get (an older server, the mock) gives the scene's
      // script metadata (present, sha256) instead, which changes whenever the text does
      let script: string, scriptFrom = "server";
      try {
        script = (await p.erg.script()) ?? "";
      } catch {
        const meta = p.store.scene.script;
        script = `meta:${meta?.present ? meta.sha256 ?? "present" : "none"}`;
        scriptFrom = "metadata";
      }
      const fp = simFingerprint(p.store, script, { detail: a.detail, strict: a.strict });
      const out: Record<string, unknown> = { ...fp, unsaved: p.store.dirty, scriptFrom };
      if (a.against !== undefined) {
        const prev: Partial<Fingerprint> = typeof a.against === "string" ? { sim: a.against } : a.against;
        if (!prev || typeof prev.sim !== "string") throw new ToolError("against: a sim_fingerprint answer or its `sim` (64 hex)");
        out.against = { same: prev.sim === fp.sim, ...diffFingerprints(prev, fp) };
      }
      return out;
    },
  },
];
