// How each detail role is drawn: a marker glyph and colour (scenery is a box instead).
import type { Detail, Role } from "../../../sdk/erg";

export const ROLE_STYLE: Record<Role, { glyph: string; color: string; label: string }> = {
  spawn: { glyph: "W", color: "#2f7de1", label: "Spawn knots" },
  object: { glyph: "O", color: "#d9534f", label: "Objects" },
  camera: { glyph: "C", color: "#7a6ff0", label: "Cameras" },
  light: { glyph: "L", color: "#f0c419", label: "Lights" },
  emitter: { glyph: "E", color: "#e67e22", label: "Emitters" },
  sound: { glyph: "S", color: "#16a085", label: "Sounds" },
  collision: { glyph: "X", color: "#95a5a6", label: "Collision" },
  marker: { glyph: "+", color: "#8e8e8e", label: "Markers" },
  scenery: { glyph: "#", color: "#b0875a", label: "Scenery" },
  other: { glyph: "?", color: "#6c7a89", label: "Other" },
};

/** The marker text: a knot's number, a drum or mine letter, else the role glyph. */
export function glyphOf(d: Pick<Detail, "name" | "role">): string {
  const k = /^WORM(\d)$/.exec(d.name);
  if (k) return k[1];
  const n = d.name.toLowerCase();
  if (n === "oildrum") return "D";
  if (n === "mine") return "M";
  return ROLE_STYLE[d.role].glyph;
}
