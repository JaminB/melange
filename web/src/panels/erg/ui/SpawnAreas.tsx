// spawns.mode areas: the areas (name, group, knots), the deal pattern and the opposite pairs. A change the server would
// refuse is not applied; the field shows why.
import { useState } from "preact/hooks";
import { SPAWN_AREA_LIMITS, areaFirstKnot, spawnsProblems, type SpawnArea, type Spawns } from "../../../sdk/erg";

interface Props { spawns: Spawns; set: (v: Spawns) => void; }

const patternText = (s: Spawns) => (s.pattern ?? []).join(", ");
const oppositeText = (s: Spawns) => (s.opposite ?? []).map(([a, b]) => `${a}-${b}`).join(", ");

/** "a-b, c-d" -> pairs; null when a part is not two whole numbers. */
function parsePairs(t: string): [number, number][] | null {
  const parts = t.split(",").map((x) => x.trim()).filter(Boolean);
  const out: [number, number][] = [];
  for (const p of parts) {
    const m = /^(\d+)\s*-\s*(\d+)$/.exec(p);
    if (!m) return null;
    out.push([Number(m[1]), Number(m[2])]);
  }
  return out;
}

export function SpawnAreasEditor({ spawns, set }: Props) {
  const [problem, setProblem] = useState("");
  const areas = spawns.areas ?? [];
  const apply = (next: Spawns) => {
    const clean: Spawns = { mode: "areas", areas: next.areas };
    if (next.pattern?.length) clean.pattern = next.pattern;
    if (next.opposite?.length) clean.opposite = next.opposite;
    const errs = spawnsProblems(clean);
    setProblem(errs[0] ?? "");
    if (!errs.length) set(clean);
  };
  const setArea = (i: number, a: Partial<SpawnArea>) => apply({ ...spawns, areas: areas.map((x, k) => (k === i ? { ...x, ...a } : x)) });
  const remove = (i: number) => apply({
    ...spawns, areas: areas.filter((_, k) => k !== i),
    opposite: (spawns.opposite ?? []).filter(([a, b]) => a !== i && b !== i).map(([a, b]) => [a > i ? a - 1 : a, b > i ? b - 1 : b] as [number, number]),
  });
  const total = areas.reduce((n, a) => n + a.knots, 0);

  return (
    <div class="erg-field" data-field="spawn-areas">
      <span class="erg-label">Spawn areas ({total} knots of {SPAWN_AREA_LIMITS.knots})</span>
      {areas.map((a, i) => (
        <div class="row" key={`${i}:${a.name}`} data-area={i}>
          <span class="muted small">{i}</span>
          <input defaultValue={a.name} maxLength={24} title="Name" data-field={`area-name-${i}`}
                 onChange={(e) => setArea(i, { name: (e.target as HTMLInputElement).value })} />
          <input defaultValue={a.group} maxLength={16} title="Group" data-field={`area-group-${i}`}
                 onChange={(e) => setArea(i, { group: (e.target as HTMLInputElement).value.trim() })} />
          <input type="number" min={1} max={SPAWN_AREA_LIMITS.knotsPerArea} defaultValue={String(a.knots)} title="Knots" data-field={`area-knots-${i}`}
                 onChange={(e) => setArea(i, { knots: Number((e.target as HTMLInputElement).value) })} />
          <span class="muted small">WORM{areaFirstKnot(areas, i)}-WORM{areaFirstKnot(areas, i) + a.knots - 1}</span>
          <button type="button" disabled={areas.length < 2} onClick={() => remove(i)} title="Remove the area">x</button>
        </div>
      ))}
      <div class="row">
        <button type="button" disabled={areas.length >= SPAWN_AREA_LIMITS.areas} data-field="area-add"
                onClick={() => apply({ ...spawns, areas: [...areas, { name: `Area ${areas.length + 1}`, group: areas[areas.length - 1]?.group ?? "area", knots: 4 }] })}>
          Add area
        </button>
      </div>
      <label class="erg-field"><span class="erg-label">Deal pattern (groups in order; group:opposite)</span>
        <input key={patternText(spawns)} defaultValue={patternText(spawns)} data-field="area-pattern"
               onChange={(e) => apply({ ...spawns, pattern: (e.target as HTMLInputElement).value.split(",").map((x) => x.trim()).filter(Boolean) })} />
      </label>
      <label class="erg-field"><span class="erg-label">Opposite pairs (area numbers, e.g. 0-1, 2-3)</span>
        <input key={oppositeText(spawns)} defaultValue={oppositeText(spawns)} data-field="area-opposite"
               onChange={(e) => {
                 const pairs = parsePairs((e.target as HTMLInputElement).value);
                 if (pairs) apply({ ...spawns, opposite: pairs });
                 else setProblem("spawns.opposite: write pairs as a-b, c-d");
               }} />
      </label>
      {problem ? <span class="muted small" data-field="area-problem">{problem}</span> : null}
      <p class="hint">Each match deals every team one area from its seed (the same on every machine): the pattern's groups first, then the rest. A team's worms stand on its area's knots in order.</p>
    </div>
  );
}
