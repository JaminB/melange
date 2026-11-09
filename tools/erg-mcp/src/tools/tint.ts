// Tool group "tint": per-copy colour for scenery copies (lib/tint.ts). A copy whose name carries "_tint<rrggbb>" is drawn
// recoloured toward that colour by Sunstone's model shaders (Melange's mg_tint); with Sunstone off it draws in its own
// texture. Names are a visual channel (sim_fingerprint's scenery part), so tinting never changes the sim.
import { setMany, type Detail } from "../core";
import { commit, pointOut, unitsSchema, type Units } from "../lib/place-geom";
import { classifyDetail } from "../lib/sim-fingerprint";
import { tintHex, tintOf, withTint } from "../lib/tint";
import { ToolError, type Tool } from "../registry";

export const tools: Tool[] = [
  {
    name: "tint_scenery",
    description: "Recolour scenery copies: sets (or with color \"none\" removes) the tint tag \"_tint<rrggbb>\" in their names. In the game, Sunstone's model shaders recolour a tinted piece toward the colour and keep its texture's light and dark detail (a texel of luminance 0.5 becomes the colour), so a pale base mesh can read as dark basalt; with Sunstone off the piece draws in its own texture. Only visual scenery copies (not the base's own details, not lights, emitters or sounds, whose names pick their effect). Select by `ids`, or `search` (name or resource contains it) and/or `near` {x, z, radius}. `ramp` sets a colour per copy from its height instead: [[y, \"#rrggbb\"], ...] (the call's units), interpolated. Names are a visual channel: sim_fingerprint is unchanged. One undo step; dryRun lists the changes.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      ids: { type: "array", items: { type: "integer" }, maxItems: 4000 },
      search: { type: "string" },
      near: { type: "object", additionalProperties: false, required: ["x", "z", "radius"], properties: { x: { type: "number" }, z: { type: "number" }, radius: { type: "number", minimum: 0 } } },
      color: { type: "string", description: "\"#rrggbb\", or \"none\" to remove the tint." },
      ramp: { type: "array", minItems: 1, maxItems: 16, items: { type: "array", minItems: 2, maxItems: 2 }, description: "[[y, \"#rrggbb\"], ...] sorted or not: each copy takes the colour at its height." },
      dryRun: { type: "boolean" },
      units: unitsSchema,
    } },
    handler(s, a: { ids?: number[]; search?: string; near?: { x: number; z: number; radius: number }; color?: string; ramp?: [number, string][]; dryRun?: boolean; units?: Units }) {
      const { store } = s.requireProject();
      const u = a.units ?? "world", k = u === "map" ? 1 : 20;
      if (a.color === undefined && !a.ramp) throw new ToolError("give color (\"#rrggbb\" or \"none\") or ramp");
      if (!a.ids && !a.search && !a.near) throw new ToolError("name the copies: ids, search or near");
      let color: string | null = null;
      try { if (a.color !== undefined && a.color !== "none") color = tintHex(a.color); } catch (e) { throw new ToolError((e as Error).message); }
      const ramp = (a.ramp ?? []).map(([y, c], i) => {
        if (typeof y !== "number") throw new ToolError(`ramp[${i}]: [y, "#rrggbb"]`);
        try { return [y / k, tintHex(c, `ramp[${i}]`)] as [number, string]; } catch (e) { throw new ToolError((e as Error).message); }
      }).sort((p, q) => p[0] - q[0]);
      const pos = (d: Detail) => store.frames.detailWorld(d);
      const q = a.search?.toLowerCase();
      let list: Detail[] = a.ids ? a.ids.map((id) => { const d = store.detail(id); if (!d) throw new ToolError(`no detail ${id}`); return d; }) : store.scene.details;
      list = list.filter((d) => {
        if (q && !`${d.name} ${d.resource}`.toLowerCase().includes(q)) return false;
        if (a.near) { const p = pos(d); if (Math.hypot(p[0] - a.near.x / k, p[2] - a.near.z / k) > a.near.radius / k) return false; }
        return true;
      });
      const skipped: object[] = [];
      list = list.filter((d) => {
        const cl = classifyDetail(d);
        const ok = d.src === null && cl.cls === "visual" && cl.role === "scenery";
        if (!ok && a.ids) skipped.push({ id: d.id, name: d.name, why: d.src !== null ? "a base detail" : cl.cls === "sim" ? `sim (${cl.why})` : `role ${cl.role}` });
        return ok;
      });
      if (!list.length) throw new ToolError(`no scenery copy matches${skipped.length ? `: ${JSON.stringify(skipped.slice(0, 5))}` : ""}`);
      const at = (y: number) => {
        if (y <= ramp[0][0]) return ramp[0][1];
        for (let i = 1; i < ramp.length; i++) if (y <= ramp[i][0]) {
          const [y0, c0] = ramp[i - 1], [y1, c1] = ramp[i], t = (y - y0) / (y1 - y0 || 1);
          let out = "";
          for (let j = 0; j < 3; j++) { const v0 = parseInt(c0.slice(2 * j, 2 * j + 2), 16), v1 = parseInt(c1.slice(2 * j, 2 * j + 2), 16); out += Math.round(v0 + (v1 - v0) * t).toString(16).padStart(2, "0"); }
          return out;
        }
        return ramp[ramp.length - 1][1];
      };
      const changes: [number, { name: string }][] = [], out: object[] = [];
      for (const d of list) {
        const want = ramp.length ? at(pos(d)[1]) : color;
        const name = withTint(d.name, want);
        if (name.length > 120) throw new ToolError(`detail ${d.id}: the name would be ${name.length} characters`);
        if (name === d.name) continue;
        changes.push([d.id, { name }]);
        out.push({ id: d.id, from: d.name, name, tint: tintOf(name), at: pointOut(pos(d), u) });
      }
      if (a.dryRun || !changes.length) return { dryRun: !!a.dryRun, changed: changes.length, matched: list.length, details: out.slice(0, 200), ...(skipped.length ? { skipped } : {}) };
      commit(store, setMany(changes, `Tint ${changes.length} scenery copies`));
      return { changed: changes.length, matched: list.length, details: out.slice(0, 200), ...(skipped.length ? { skipped } : {}),
        note: "drawn recoloured by Sunstone's model shaders (Melange MirageTints); sim_fingerprint is unchanged", undo: "undo puts the names back" };
    },
  },
];
