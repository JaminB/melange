// The selected detail's fields. Positions are shown in world units (x20) and rotations in degrees; the patch stores
// .xan units and radians.
import { JsonTree } from "../../../sdk/ui";
import {
  WORLD_PER_XAN, printable, type CrateKind, type Detail, type DetailFields, type ObjectSpec, type Vec3,
} from "../../../sdk/erg";
import { round } from "../model/geometry";
import { objectOf, type ObjectCatalog } from "../model/placing";
import type { EditorStore } from "../model/store";

interface Props {
  store: EditorStore; set(id: number, f: DetailFields): void;
  setObject?(knot: string, o: ObjectSpec): void; catalog?: ObjectCatalog;
}

function NumInput({ label, name, value, lo, hi, int = true, onSet }: {
  label: string; name: string; value: number; lo: number; hi: number; int?: boolean; onSet(v: number): void;
}) {
  return (
    <label class="erg-field">
      <span class="erg-label">{label}</span>
      <input key={value} type="number" min={lo} max={hi} step={int ? 1 : "any"} defaultValue={String(value)} data-field={name}
             onChange={(e) => {
               const el = e.target as HTMLInputElement, n = Number(el.value);
               if (Number.isFinite(n) && n >= lo && n <= hi && (!int || Number.isInteger(n))) onSet(n);
               else el.value = String(value);
             }} />
    </label>
  );
}

function Check({ label, name, value, onSet }: { label: string; name: string; value: boolean; onSet(v: boolean): void }) {
  return (
    <label class="erg-field">
      <span class="erg-label">{label}</span>
      <input type="checkbox" checked={value} data-field={name} onChange={(e) => onSet((e.target as HTMLInputElement).checked)} />
    </label>
  );
}

const TEAMS = ["any team", ...Array.from({ length: 8 }, (_, i) => `team ${i + 1}`)];

/** A level object's settings. Crate contents come from the install's list; a name it does not know stays shown. */
export function ObjectProps({ o, set, catalog }: { o: ObjectSpec; set(o: ObjectSpec): void; catalog?: ObjectCatalog }) {
  if (o.type === "telepad")
    return <div class="erg-field" data-erg-object="telepad"><span class="erg-label">Telepad</span><span>group {o.group}: a worm on one pad of the group comes out of another</span></div>;
  if (o.type === "minefactory")
    return <p class="hint" data-erg-object="minefactory">Mine factory. It replaces the scheme's own factory, so a match never has two.</p>;
  if (o.type === "trigger") {
    const t = o.trigger;
    const put = (f: Partial<typeof t>) => set({ ...o, trigger: { ...t, ...f } });
    return (
      <div data-erg-object="trigger">
        <NumInput label="Trigger index" name="trigger.index" value={t.index} lo={0} hi={255} onSet={(v) => put({ index: v })} />
        <NumInput label="Radius (world)" name="trigger.radius" value={t.radius ?? 60} lo={1} hi={1000} int={false} onSet={(v) => put({ radius: v })} />
        {(["teamCollect", "teamDestroy"] as const).map((k) => (
          <label class="erg-field" key={k}>
            <span class="erg-label">{k === "teamCollect" ? "Collected by" : "Destroyed by"}</span>
            <select value={String(t[k] ?? (k === "teamCollect" ? 0 : 4))} data-field={`trigger.${k}`}
                    onChange={(e) => put({ [k]: Number((e.target as HTMLSelectElement).value) })}>
              {TEAMS.map((n, i) => <option key={i} value={String(i)}>{n}</option>)}
            </select>
          </label>
        ))}
        <NumInput label="Hit points" name="trigger.hitpoints" value={t.hitpoints ?? 1} lo={0} hi={1000} onSet={(v) => put({ hitpoints: v })} />
        <Check label="Worms collect it" name="trigger.wormCollect" value={!!t.wormCollect} onSet={(v) => put({ wormCollect: v })} />
      </div>
    );
  }
  const c = o.crate;
  const common = { hitpoints: c.hitpoints ?? 25, parachute: !!c.parachute };
  const kindOf = (kind: CrateKind): ObjectSpec => {
    if (kind === "health") return { ...o, crate: { kind, amount: 25, ...common } };
    const list = (kind === "weapon" ? catalog?.weapons : catalog?.utilities) ?? [];
    return { ...o, crate: { kind, contents: list[0] ?? "", count: 1, ...common } };
  };
  const names = c.kind === "health" ? [] : (c.kind === "weapon" ? catalog?.weapons : catalog?.utilities) ?? [];
  return (
    <div data-erg-object="crate">
      <label class="erg-field">
        <span class="erg-label">Crate</span>
        <select value={c.kind} data-field="crate.kind" onChange={(e) => set(kindOf((e.target as HTMLSelectElement).value as CrateKind))}>
          <option value="weapon">Weapon</option><option value="health">Health</option><option value="utility">Utility</option>
        </select>
      </label>
      {c.kind === "health" ? (
        <NumInput label="Health" name="crate.amount" value={c.amount} lo={1} hi={500} onSet={(v) => set({ ...o, crate: { ...c, amount: v } })} />
      ) : (
        <>
          <label class="erg-field">
            <span class="erg-label">Contents</span>
            <select value={c.contents} data-field="crate.contents" onChange={(e) => set({ ...o, crate: { ...c, contents: (e.target as HTMLSelectElement).value } })}>
              {names.includes(c.contents) ? null : <option value={c.contents}>{c.contents || "(choose)"}</option>}
              {names.map((n) => <option key={n} value={n}>{n.replace(/^k(Weapon|Utility)/, "")}</option>)}
            </select>
          </label>
          {!names.length ? <p class="hint warn">{catalog?.error ?? "The install's weapon list is not available."}</p> : null}
          <NumInput label="Count" name="crate.count" value={c.count ?? 1} lo={1} hi={99} onSet={(v) => set({ ...o, crate: { ...c, count: v } })} />
        </>
      )}
      <NumInput label="Hit points" name="crate.hitpoints" value={common.hitpoints} lo={1} hi={1000} onSet={(v) => set({ ...o, crate: { ...c, hitpoints: v } })} />
      <Check label="Parachute" name="crate.parachute" value={common.parachute} onSet={(v) => set({ ...o, crate: { ...c, parachute: v } })} />
    </div>
  );
}

const DEG = 180 / Math.PI;

function VecInput({ label, value, scale, digits, onSet, name }: {
  label: string; value: Vec3; scale: number; digits: number; name: string; onSet(v: Vec3): void;
}) {
  return (
    <div class="erg-field">
      <span class="erg-label">{label}</span>
      <div class="erg-vec">
        {[0, 1, 2].map((i) => (
          <input key={`${i}:${value[i]}`} type="number" step="any" data-field={`${name}.${i}`} aria-label={`${label} ${"xyz"[i]}`}
                 defaultValue={String(round(value[i] * scale, digits))}
                 onChange={(e) => {
                   const n = Number((e.target as HTMLInputElement).value);
                   if (!Number.isFinite(n)) return;
                   const v: Vec3 = [value[0], value[1], value[2]];
                   v[i] = round(n / scale, 6);
                   onSet(v);
                 }} />
        ))}
      </div>
    </div>
  );
}

function TextInput({ label, value, name, onSet }: { label: string; value: string; name: string; onSet(v: string): void }) {
  return (
    <label class="erg-field">
      <span class="erg-label">{label}</span>
      <input key={value} defaultValue={value} maxLength={63} data-field={name}
             onChange={(e) => {
               const v = (e.target as HTMLInputElement).value;
               if (printable(v, 1, 63)) onSet(v);
               else (e.target as HTMLInputElement).value = value;
             }} />
    </label>
  );
}

export function Properties({ store, set, setObject, catalog }: Props) {
  const sel = store.selected();
  if (!sel.length) return <p class="muted pad-x">Select a detail in the view or the outliner. Shift-drag in the view selects a box.</p>;
  if (sel.length > 1) return <p class="pad-x" data-erg-multi>{sel.length} details selected: move them together, duplicate, drop or delete.</p>;
  const d: Detail = sel[0];
  const frame = store.frames.byId.get(d.frame);
  const world = store.frames.detailWorld(d);
  const animated = store.translationOnly.has(d.frame);
  const obj = objectOf(store.scene, d);
  return (
    <div class="erg-props" data-erg-props={d.id}>
      {obj ? (
        <>
          <div class="erg-field"><span class="erg-label">Knot</span><code>{d.name}</code></div>
          <ObjectProps o={obj} catalog={catalog} set={(o) => setObject?.(obj.knot, o)} />
        </>
      ) : (
        <>
          <TextInput label="Name" name="name" value={d.name} onSet={(v) => set(d.id, { name: v })} />
          <TextInput label="Resource" name="resource" value={d.resource} onSet={(v) => set(d.id, { resource: v })} />
        </>
      )}
      <div class="erg-field"><span class="erg-label">Role</span><span>{d.role}</span></div>
      <div class="erg-field"><span class="erg-label">Frame</span><span>{frame?.name || "(unnamed)"} #{d.frame}{frame?.folder ? " (folder)" : ""}</span></div>
      <VecInput label="Position (world, in frame)" name="pos" value={d.pos} scale={WORLD_PER_XAN} digits={3} onSet={(v) => set(d.id, { pos: v })} />
      {animated ? <p class="hint warn">This frame swings in the game: only moving is offered, and the place shown is approximate.</p> : (
        <>
          <VecInput label="Rotation (degrees)" name="rot" value={d.rot} scale={DEG} digits={3} onSet={(v) => set(d.id, { rot: v })} />
          <VecInput label="Scale" name="scale" value={d.scale} scale={1} digits={4} onSet={(v) => set(d.id, { scale: v })} />
        </>
      )}
      <div class="erg-field"><span class="erg-label">In the level</span>
        <code>{world.map((v) => round(v * WORLD_PER_XAN, 2)).join(", ")}</code></div>
      <div class="erg-field"><span class="erg-label">Source</span><span>{d.src === null ? "added in this project" : `object #${d.src} of the base`}</span></div>
      <details class="erg-raw"><summary>Raw</summary><JsonTree value={d} open={1} /></details>
    </div>
  );
}
