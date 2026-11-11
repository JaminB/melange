// The water look block of a level script (set_water_look): one wum.level.water{...} call between two marker lines at the
// top of the script, which set_water_look writes, rewrites or removes and leaves the rest of the script as it is.
// wum.level.water (Melange 0.13+, levels/water_look.cpp) sets fields of the theme's WaterPlaneTweaks for the match.

export const WATER_LOOK_BEGIN = "-- erg-mcp set_water_look: begin (generated: set_water_look rewrites this block)";
export const WATER_LOOK_END = "-- erg-mcp set_water_look: end";

/** WaterPlaneTweaks' fields wum.level.water takes, and what each holds (CG\water.cg reads the numbers through
 * combinedWaterParams; the Light colours light the land too: in a Test LightAmbient and LightDiffuse red turned the
 * island red, so they are the landscape shader's globalAmbient and globalDiffuse). */
export const WATER_FIELDS: Record<string, { kind: "number" | "rgb" | "rgba" | "vec3"; min?: number; max?: number; what: string }> = {
  NearOpacity: { kind: "number", min: 0, max: 1, what: "the water's alpha (0 clear, 1 opaque)" },
  ReflectionStrength: { kind: "number", min: 0, max: 8, what: "how much of the environment map it reflects" },
  ReflectionContrast: { kind: "number", min: 0, max: 16, what: "the power the reflection is raised to" },
  SubtractColourScale: { kind: "number", min: -4, max: 4, what: "how much the diffuse texture's darker layer is taken off" },
  SpecularContrast: { kind: "number", min: 0, max: 16, what: "the highlight's strength" },
  SpecularPower: { kind: "number", min: 0, max: 256, what: "the highlight's sharpness" },
  SpecularFadeScale: { kind: "number", min: 0, max: 16, what: "the highlight's fade" },
  TextureScale: { kind: "number", min: 0.01, max: 64, what: "the ripple texture's scale" },
  LandSpecularPower: { kind: "number", min: 0, max: 256, what: "the land's highlight under the water" },
  LandFresnelPower: { kind: "number", min: 0, max: 16, what: "the land's rim light under the water" },
  SkyBlendColor: { kind: "rgba", what: "{r, g, b[, a]} 0-255" },
  LightAmbient: { kind: "rgb", what: "{r, g, b} 0-255: the land's ambient light (the landscape shader's globalAmbient)" },
  LightDiffuse: { kind: "rgb", what: "{r, g, b} 0-255: the land's sun light (globalDiffuse)" },
  LightSpecular: { kind: "rgb", what: "{r, g, b} 0-255" },
  LightFresnel: { kind: "rgb", what: "{r, g, b} 0-255" },
  LandFresnelColour: { kind: "rgb", what: "{r, g, b} 0-255" },
  NormalIntensity: { kind: "vec3", what: "{x, y, z}: the highlight's ripple strength" },
  NormalIntensity1: { kind: "vec3", what: "{x, y, z}: the reflection's ripple strength" },
  LightDirection: { kind: "vec3", what: "{x, y, z}" },
};

export type WaterLook = Record<string, number | number[]>;

/** Checks a look: known fields with values of their kind. The problems in words (empty: fine). */
export function waterLookProblems(look: WaterLook): string[] {
  const out: string[] = [];
  for (const [k, v] of Object.entries(look)) {
    const f = WATER_FIELDS[k];
    if (!f) { out.push(`${k}: not a water field (${Object.keys(WATER_FIELDS).join(", ")})`); continue; }
    if (f.kind === "number") {
      if (typeof v !== "number" || !Number.isFinite(v) || v < (f.min ?? -1000) || v > (f.max ?? 1000)) out.push(`${k}: a number from ${f.min} to ${f.max}`);
    } else {
      const n = f.kind === "rgba" ? [3, 4] : [3];
      if (!Array.isArray(v) || !n.includes(v.length) || v.some((x) => typeof x !== "number" || !Number.isFinite(x))) out.push(`${k}: ${f.what}`);
      else if (f.kind !== "vec3" && v.some((x) => x < 0 || x > 255)) out.push(`${k}: colour parts 0-255`);
      else if (f.kind === "vec3" && v.some((x) => Math.abs(x) > 1000)) out.push(`${k}: parts from -1000 to 1000`);
    }
  }
  return out;
}

const num = (v: number) => String(Math.round(v * 10000) / 10000);

/** The block for a look (sorted fields, so the same look is the same text). */
export function waterLookBlock(look: WaterLook): string {
  const fields = Object.keys(look).sort().map((k) => {
    const v = look[k];
    return `${k} = ${Array.isArray(v) ? `{${v.map(num).join(", ")}}` : num(v)}`;
  });
  return [WATER_LOOK_BEGIN,
    "if wum.level and wum.level.water then",
    `  local ok, why = wum.level.water{${fields.join(", ")}}`,
    "  if not ok then wum.log.warn(\"water look: \" .. tostring(why)) end",
    "end",
    WATER_LOOK_END].join("\n");
}

/** The script's block, as [start, end) character offsets including its trailing newline, or null. */
function blockAt(script: string): [number, number] | null {
  const a = script.indexOf(WATER_LOOK_BEGIN);
  if (a < 0) return null;
  const e = script.indexOf(WATER_LOOK_END, a);
  if (e < 0) return null;
  let end = e + WATER_LOOK_END.length;
  if (script[end] === "\r") end++;
  if (script[end] === "\n") end++;
  return [a, end];
}

/** The script with the block for `look` at its top (an earlier block replaced where it was), or without one (null). */
export function withWaterLook(script: string, look: WaterLook | null): string {
  const at = blockAt(script);
  const rest = at ? script.slice(0, at[0]) + script.slice(at[1]) : script;
  if (!look || !Object.keys(look).length) return rest;
  const block = waterLookBlock(look) + "\n";
  return at ? script.slice(0, at[0]) + block + script.slice(at[1]) : block + rest;
}

/** The look a script's block sets, or null without one. */
export function readWaterLook(script: string): WaterLook | null {
  const at = blockAt(script);
  if (!at) return null;
  const m = /wum\.level\.water\{(.*)\}/.exec(script.slice(at[0], at[1]));
  if (!m) return null;
  const out: WaterLook = {};
  for (const part of m[1].matchAll(/([A-Za-z0-9]+) = (\{[^}]*\}|[-0-9.e]+)/g)) {
    const v = part[2];
    out[part[1]] = v.startsWith("{") ? v.slice(1, -1).split(",").map((x) => Number(x.trim())) : Number(v);
  }
  return out;
}
