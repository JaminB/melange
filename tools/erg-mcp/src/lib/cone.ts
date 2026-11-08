// sculpt_cone's surface as a heightfield expression (heightfield-expr's language, never run as code): a radial profile
// over the distance from the axis as a fraction t of a radius that wanders by angle, plus roughness, gullies and a
// crater. The expression is in the field's units with x, z, r and theta about the cone's axis (the field's centre).
import { ToolError } from "../registry";

export type Profile = "concave" | "linear" | "convex" | "gaussian" | [number, number][];

export interface ConeArgs {
  at: [number, number];
  floor: number;
  radius: number;
  height: number;
  profile?: Profile;
  angleNoise?: { amount: number; lobes?: number; seed?: number };
  surfaceNoise?: { amount: number; scale?: number; seed?: number };
  gullies?: { count: number; depth: number; width?: number; angle?: number };
  crater?: { radius: number; depth: number; rim?: number };
}

const n = (v: number) => (Number.isInteger(v) ? String(v) : v.toFixed(6).replace(/0+$/, "").replace(/\.$/, ""));

/** The flank's height (fraction of `height`) as an expression of `t` (0 at the axis, 1 at the foot). */
function profileExpr(p: Profile): string {
  if (p === "concave") return "(1 - t)^1.6";
  if (p === "linear") return "(1 - t)";
  if (p === "convex") return "(1 - t*t)";
  if (p === "gaussian") return `((exp(-3*t*t) - ${n(Math.exp(-3))}) / ${n(1 - Math.exp(-3))})`;
  if (!Array.isArray(p) || p.length < 2) throw new ToolError("profile: concave, linear, convex, gaussian, or at least two [t, h] points");
  for (const [i, q] of p.entries()) {
    if (!Array.isArray(q) || q.length !== 2 || !q.every(Number.isFinite)) throw new ToolError(`profile[${i}]: [t, h], two numbers`);
    if (q[0] < 0 || q[0] > 1 || (i && q[0] <= p[i - 1][0])) throw new ToolError(`profile[${i}]: t runs from 0 to 1, rising`);
  }
  // piecewise linear: each segment's lerp where t is in it, chosen by select from the last segment back
  let e = "0";
  for (let i = p.length - 2; i >= 0; i--) {
    const [t0, h0] = p[i], [t1, h1] = p[i + 1];
    const seg = `lerp(${n(h0)}, ${n(h1)}, clamp((t - ${n(t0)}) / ${n(t1 - t0)}, 0, 1))`;
    e = i === p.length - 2 ? `select(${n(t1)} - t + 0.000001, ${seg}, 0)` : `select(${n(t1)} - t, ${seg}, ${e})`;
  }
  return p[0][0] > 0 ? `select(t - ${n(p[0][0])}, ${e}, ${n(p[0][1])})` : e;
}

/** The expression and the half side of the square it needs (the widest the outline can reach, and a margin). */
export function coneExpr(a: ConeArgs): { expr: string; half: number } {
  if (!(a.radius > 0) || !(a.height > 0)) throw new ToolError("radius and height must be above 0");
  const an = a.angleNoise, sn = a.surfaceNoise, gu = a.gullies, cr = a.crater;
  const parts: string[] = [];
  const lobes = an?.lobes ?? 5;
  // the outline: fbm sampled on a circle of circumference ~ lobes, so it closes on itself round the cone
  parts.push(an && an.amount > 0
    ? `let Rt = ${n(a.radius)} * (1 + ${n(an.amount)} * fbm(${n(lobes / (2 * Math.PI))} * cos(theta) + 17.3, ${n(lobes / (2 * Math.PI))} * sin(theta) + 5.1, ${an.seed ?? 1}, 3));`
    : `let Rt = ${n(a.radius)};`);
  parts.push("let t = clamp(r / Rt, 0, 1);");
  parts.push(`let base = ${n(a.height)} * ${profileExpr(a.profile ?? "concave")};`);
  let body = "base";
  if (sn && sn.amount) {
    const sc = sn.scale ?? a.radius / 6;
    // roughness fades out at the summit and the foot (4 t (1 - t)), so the outline and the top stay where they are
    body += ` + ${n(sn.amount)} * 4 * t * (1 - t) * fbm(x / ${n(sc)}, z / ${n(sc)}, ${sn.seed ?? 2}, 3)`;
  }
  if (gu && gu.count > 0 && gu.depth) {
    const w = gu.width ?? 0.25, a0 = ((gu.angle ?? 0) * Math.PI) / 180, k = gu.count;
    // the angle to the nearest gully, wrapped to [-pi/k, pi/k]
    parts.push(`let dg = mod(theta - ${n(a0)} + ${n(Math.PI / k)}, ${n((2 * Math.PI) / k)}) - ${n(Math.PI / k)};`);
    body += ` - ${n(gu.depth)} * exp(-(dg / ${n(w)})^2) * 4 * t * (1 - t)`;
  }
  if (cr) {
    if (!(cr.radius > 0) || cr.radius >= a.radius) throw new ToolError("crater.radius is above 0 and under the cone's radius");
    // a bowl under the surface inside the crater's radius, and a rim ridge on its edge
    parts.push(`let cd = r / ${n(cr.radius)};`);
    body += ` - select(1 - cd, ${n(cr.depth)} * (1 - cd*cd), 0)`;
    if (cr.rim) body += ` + ${n(cr.rim)} * exp(-((cd - 1) / 0.3)^2)`;
  }
  const expr = `${parts.join(" ")} select(1 - t, max(0, ${body}), 0)`;
  const half = a.radius * (1 + (an?.amount ?? 0) * 1.2) + 2;
  return { expr, half };
}
