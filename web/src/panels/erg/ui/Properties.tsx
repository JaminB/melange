// The selected detail's fields. Positions are shown in world units (x20) and rotations in degrees; the patch stores
// .xan units and radians.
import { JsonTree } from "../../../sdk/ui";
import { WORLD_PER_XAN, printable, type Detail, type DetailFields, type Vec3 } from "../../../sdk/erg";
import { round } from "../model/geometry";
import type { EditorStore } from "../model/store";

interface Props { store: EditorStore; set(id: number, f: DetailFields): void; }

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

export function Properties({ store, set }: Props) {
  const sel = store.selected();
  if (!sel.length) return <p class="muted pad-x">Select a detail in the view or the outliner. Shift-drag in the view selects a box.</p>;
  if (sel.length > 1) return <p class="pad-x" data-erg-multi>{sel.length} details selected: move them together, duplicate, drop or delete.</p>;
  const d: Detail = sel[0];
  const frame = store.frames.byId.get(d.frame);
  const world = store.frames.detailWorld(d);
  const animated = store.translationOnly.has(d.frame);
  return (
    <div class="erg-props" data-erg-props={d.id}>
      <TextInput label="Name" name="name" value={d.name} onSet={(v) => set(d.id, { name: v })} />
      <TextInput label="Resource" name="resource" value={d.resource} onSet={(v) => set(d.id, { resource: v })} />
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
