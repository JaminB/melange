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

/** The sea material (set_water_look `sea`): what the water plane should look like as a surface of its own, which the
 * game's CG\water.cg cannot draw (its colour is the theme's blue texture) and a water shader mod can. The level script
 * sets it with wum.level.water{Sea = {r, g, b, opacity}, SeaKeep = {reflection, foam}} (Melange 0.15: the colour 0-255,
 * the rest 0-1), and Melange tells client mods with the event SEA_EVENT; a mod that replaces the water shader (Sunstone
 * 2.4+) listens for it (wum.events.on) and draws the sea as that matte colour. docs/erg.md "Sea material" is the contract. */
export interface SeaLook { albedo: number[]; opacity: number; reflection: number; foam: number; }
export const SEA_EVENT = "melange.level.sea";

/** A sea look checked and its defaults filled in (opaque, no reflection, no foam); the problems in words. */
export function seaLook(v: { albedo?: number[]; opacity?: number; reflection?: number; foam?: number }): { sea?: SeaLook; problems: string[] } {
  const problems: string[] = [];
  if (!Array.isArray(v.albedo) || v.albedo.length !== 3 || v.albedo.some((x) => typeof x !== "number" || !(x >= 0 && x <= 255))) problems.push("sea.albedo: [r, g, b] 0-255");
  for (const k of ["opacity", "reflection", "foam"] as const)
    if (v[k] !== undefined && !(typeof v[k] === "number" && v[k]! >= 0 && v[k]! <= 1)) problems.push(`sea.${k}: 0-1`);
  if (problems.length) return { problems };
  return { sea: { albedo: v.albedo!.map((x) => Math.round(x)), opacity: v.opacity ?? 1, reflection: v.reflection ?? 0, foam: v.foam ?? 0 }, problems };
}

/** The fields of the script's call for a sea look. */
export const seaFields = (s: SeaLook) => { const n = (x: number) => String(Math.round(x * 1000) / 1000); return `Sea = {${[...s.albedo, s.opacity].map(n).join(", ")}}, SeaKeep = {${n(s.reflection)}, ${n(s.foam)}}`; };

/** The call's fields back to a sea look (null: not one). */
export function parseSea(fields: string): SeaLook | null {
  const m = /Sea = \{([^}]*)\}, SeaKeep = \{([^}]*)\}/.exec(fields);
  if (!m) return null;
  const p = m[1].split(",").map((x) => Number(x.trim())), k = m[2].split(",").map((x) => Number(x.trim()));
  if (p.length !== 4 || k.length !== 2 || [...p, ...k].some((x) => !Number.isFinite(x))) return null;
  return { albedo: p.slice(0, 3), opacity: p[3], reflection: k[0], foam: k[1] };
}

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

/** The block for a look (sorted fields, so the same look is the same text) and, with `sea`, the sea material's message.
 * Without a sea the block is what it always was. */
export function waterLookBlock(look: WaterLook, sea?: SeaLook | null): string {
  const fields = Object.keys(look).sort().map((k) => {
    const v = look[k];
    return `${k} = ${Array.isArray(v) ? `{${v.map(num).join(", ")}}` : num(v)}`;
  });
  return [WATER_LOOK_BEGIN,
    ...(fields.length ? [
      "if wum.level and wum.level.water then",
      `  local ok, why = wum.level.water{${fields.join(", ")}}`,
      "  if not ok then wum.log.warn(\"water look: \" .. tostring(why)) end",
      "end"] : []),
    // a call of its own: an older Melange refuses the unknown field, and the look above is set all the same
    ...(sea ? [
      "-- the sea material (docs/erg.md, Melange 0.15): a water shader mod draws the sea as this matte colour",
      "if wum.level and wum.level.water then",
      `  local ok, why = wum.level.water{${seaFields(sea)}}`,
      "  if not ok then wum.log.info(\"sea material: \" .. tostring(why)) end",
      "end"] : []),
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
export function withWaterLook(script: string, look: WaterLook | null, sea?: SeaLook | null): string {
  const at = blockAt(script);
  const rest = at ? script.slice(0, at[0]) + script.slice(at[1]) : script;
  if ((!look || !Object.keys(look).length) && !sea) return rest;
  const block = waterLookBlock(look ?? {}, sea) + "\n";
  return at ? script.slice(0, at[0]) + block + script.slice(at[1]) : block + rest;
}

/** The sea material a script's block sends, or null. */
export function readSea(script: string): SeaLook | null {
  const at = blockAt(script);
  if (!at) return null;
  return parseSea(script.slice(at[0], at[1]));
}

/** A mod that replaces the water plane's shader (shaders\Water.*.glsl), and whether its files name SEA_EVENT. */
export interface WaterShaderMod { mod: string; file: string; honoursSea: boolean; enabled: boolean | null; }

/** What the water looks of a level meet in a game folder: the mods whose water shader draws the plane instead of the
 * game's, as warning lines. A shader mod ignores the WaterPlaneTweaks fields it does not read, and draws a sea material
 * only when it listens for SEA_EVENT. */
export function shaderOverrideWarnings(mods: WaterShaderMod[], look: WaterLook | null, sea: SeaLook | null): string[] {
  const out: string[] = [];
  for (const m of mods.filter((x) => x.enabled !== false)) {
    const on = m.enabled === null ? "installed (its enabled state is not known)" : "enabled";
    if (sea && !m.honoursSea) out.push(`the sea material is not drawn while ${m.mod} is ${on}: its ${m.file} replaces the water shader and does not listen for ${SEA_EVENT} (a ${m.mod} that honours the sea material, or the mod off, shows it)`);
    else if (!sea && look && Object.keys(look).some((k) => !k.startsWith("Light")))
      out.push(`${m.mod} is ${on} and its ${m.file} replaces the water shader: it draws its own water and may ignore these fields (NearOpacity, ReflectionStrength, SubtractColourScale: the game's CG\\water.cg reads them)` +
        (m.honoursSea ? `; it honours the sea material (set_water_look sea: {albedo}), which says the colour outright` : ""));
  }
  return out;
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

/** set_water_look presets: a named look as WaterPlaneTweaks fields, with the theme/time-of-day pairs it was seen to read
 * right on in a Test. A preset on another pair still applies, and the answer says it is unverified there.
 * What the fields can do (CG\water.cg, tried in a Test on ARABIAN DAY): the water's colour is the theme's blue ripple
 * texture minus SubtractColourScale times its ripple layer (so a positive scale goes deeper blue then black, a negative one
 * pale then white), plus ReflectionStrength times the sky map (blue by day) plus a grey highlight. water.cg reads
 * neither SkyBlendColor nor the Light* colours, so the plane itself cannot be made warm: a sandy sea is pale and opaque,
 * and a warm tint goes to the land's light (LightAmbient and LightDiffuse, which do colour the terrain). */
export interface WaterPreset {
  what: string;
  fields: WaterLook;
  verified: { theme: string; timeOfDay: string }[];
}
export const WATER_PRESETS: Record<string, WaterPreset> = {
  sand: { what: "a pale opaque sea that hides what is under it: no sky reflection, a bleached shade, no highlight",
    fields: { NearOpacity: 1, ReflectionStrength: 0, SubtractColourScale: -1.5, SpecularContrast: 0 },
    verified: [{ theme: "ARABIAN", timeOfDay: "DAY" }] },
  lava: { what: "a dark opaque sea, no reflection and no highlight (the glow is the tint on the land's light)",
    fields: { NearOpacity: 1, ReflectionStrength: 0, SubtractColourScale: 2.5, SpecularContrast: 0 },
    verified: [] },
  murk: { what: "a dull near-opaque sea: a faint reflection, a darker shade, a weak highlight",
    fields: { NearOpacity: 0.95, ReflectionStrength: 0.25, SubtractColourScale: 0.6, SpecularContrast: 0.3 },
    verified: [] },
};

/** The land's light for a tint [r, g, b] 0-255: the sun is the tint, the ambient half of it. */
export function tintLight(tint: number[]): WaterLook {
  return { LightDiffuse: tint.slice(0, 3), LightAmbient: tint.slice(0, 3).map((x) => Math.round(x / 2)) };
}

/** The fields a preset expands to for a theme and time of day: the preset's, the tint's land light, then the explicit
 * fields over both. verified says whether the preset was tried on this pair (the tint is the caller's own colour and is
 * not part of it). */
export function expandWaterPreset(name: string, tint: number[] | undefined, explicit: WaterLook, theme: string, timeOfDay: string):
  { look: WaterLook; verified: boolean; overridden: string[]; note: string } {
  const p = WATER_PRESETS[name];
  if (!p) throw new Error(`preset '${name}' is not one of ${Object.keys(WATER_PRESETS).join(", ")}`);
  const look: WaterLook = { ...p.fields, ...(tint ? tintLight(tint) : {}) };
  const overridden = Object.keys(explicit).filter((k) => k in look);
  Object.assign(look, explicit);
  const verified = p.verified.some((v) => v.theme === theme && v.timeOfDay === timeOfDay);
  const where = p.verified.map((v) => `${v.theme} ${v.timeOfDay}`).join(", ");
  const note = verified ? `preset ${name} was tried in a Test on ${theme} ${timeOfDay}`
    : `preset ${name} is UNVERIFIED on ${theme} ${timeOfDay}${where ? ` (tried on ${where})` : " (not tried in a Test anywhere yet)"}: its numbers are a starting point, Test and adjust`;
  return { look, verified, overridden, note };
}
