// Level settings: title, theme, time of day, material file, heightmap textures, water, spawn mode and surround.
import { SetLevel, THEMES, TIMES_OF_DAY, printable, type Databank, type HmpMode, type LevelSettings as Settings, type SpawnMode } from "../../../sdk/erg";
import type { EditorStore } from "../model/store";

export interface ThemeInfo { themes: string[]; times: string[]; materialFiles: string[]; }

const names = (v: unknown): string[] =>
  Array.isArray(v) ? v.map((x) => (typeof x === "string" ? x : typeof x?.name === "string" ? x.name : "")).filter(Boolean) : [];

/** level.themes, read defensively; the fixed lists stand in for anything missing. */
export function themesOf(r: unknown): ThemeInfo {
  const o = (r && typeof r === "object" ? r : {}) as Record<string, unknown>;
  const themes = names(o.themes).filter((t) => (THEMES as readonly string[]).includes(t));
  const times = names(o.timesOfDay ?? o.times).filter((t) => (TIMES_OF_DAY as readonly string[]).includes(t));
  return {
    themes: themes.length ? themes : [...THEMES],
    times: times.length ? times : [...TIMES_OF_DAY],
    materialFiles: names(o.materialFiles ?? o.materials),
  };
}

interface Props { store: EditorStore; themes: ThemeInfo; }

export function LevelSettings({ store, themes }: Props) {
  const s = store.scene;
  const base = store.base;
  const set = (v: Settings, label: string) => store.exec(new SetLevel(v, label));
  const db = (k: keyof Databank, v: string) => set({ databank: { [k]: v } }, `Change ${k}`);
  const changed = (k: keyof Databank) => (s.databank[k] !== base.databank[k] ? " (changed)" : "");
  const water = s.water.level;
  const materials = themes.materialFiles.includes(s.databank.materialFile) || !s.databank.materialFile
    ? themes.materialFiles : [s.databank.materialFile, ...themes.materialFiles];

  return (
    <div class="erg-level" data-erg-level>
      <label class="erg-field"><span class="erg-label">Title</span>
        <input key={s.title} defaultValue={s.title} maxLength={40} data-field="title"
               onChange={(e) => {
                 const v = (e.target as HTMLInputElement).value;
                 if (printable(v, 1, 40)) set({ title: v }, "Change title");
                 else (e.target as HTMLInputElement).value = s.title;
               }} />
      </label>
      <label class="erg-field"><span class="erg-label">Theme{changed("theme")}</span>
        <select value={s.databank.theme} onChange={(e) => db("theme", (e.target as HTMLSelectElement).value)} data-field="theme">
          {themes.themes.map((t) => <option key={t} value={t}>{t}</option>)}
        </select>
      </label>
      <label class="erg-field"><span class="erg-label">Time of day{changed("timeOfDay")}</span>
        <select value={s.databank.timeOfDay} onChange={(e) => db("timeOfDay", (e.target as HTMLSelectElement).value)} data-field="timeOfDay">
          {themes.times.map((t) => <option key={t} value={t}>{t}</option>)}
        </select>
      </label>
      <p class="hint">Quick Game and network games load DAY whatever is set here.</p>
      <label class="erg-field"><span class="erg-label">Material file{changed("materialFile")}</span>
        {materials.length ? (
          <select value={s.databank.materialFile} onChange={(e) => db("materialFile", (e.target as HTMLSelectElement).value)} data-field="materialFile">
            {!s.databank.materialFile ? <option value="">(none)</option> : null}
            {materials.map((m) => <option key={m} value={m}>{m}</option>)}
          </select>
        ) : (
          <input key={s.databank.materialFile} defaultValue={s.databank.materialFile} data-field="materialFile"
                 onChange={(e) => { const v = (e.target as HTMLInputElement).value.trim(); if (v) db("materialFile", v); }} />
        )}
      </label>
      {(["heightmapBase", "heightmapSecond"] as const).map((k) => (
        <label key={k} class="erg-field"><span class="erg-label">{k === "heightmapBase" ? "Heightmap texture" : "Second heightmap texture"}{changed(k)}</span>
          <input key={s.databank[k]} defaultValue={s.databank[k]} maxLength={64} data-field={k}
                 onChange={(e) => { const v = (e.target as HTMLInputElement).value.trim(); if (!v || printable(v, 1, 64)) db(k, v); }} />
        </label>
      ))}
      <div class="erg-field"><span class="erg-label">Water level (world units, sea level 0)</span>
        <div class="row">
          <label class="erg-inline"><input type="checkbox" checked={water !== null} data-field="water-set"
                 onChange={(e) => set({ water: (e.target as HTMLInputElement).checked ? (base.water.level ?? 0) : null }, "Change water")} /> Set</label>
          <input type="number" step="any" min={-1000} max={1000} disabled={water === null} key={String(water)} data-field="water"
                 defaultValue={water === null ? "" : String(water)}
                 onChange={(e) => {
                   const n = Number((e.target as HTMLInputElement).value);
                   if (Number.isFinite(n) && Math.abs(n) <= 1000) set({ water: n }, "Change water");
                 }} />
        </div>
        {water === null ? <span class="muted small">The level's own water (the default of its scripts).</span> : null}
      </div>
      <label class="erg-field"><span class="erg-label">Spawns</span>
        <select value={s.spawns.mode} onChange={(e) => set({ spawns: (e.target as HTMLSelectElement).value as SpawnMode }, "Change spawns")} data-field="spawns">
          <option value="random">Random (the game picks)</option>
          <option value="knots">Knots WORM0-WORM7</option>
        </select>
      </label>
      <label class="erg-field"><span class="erg-label">Surround</span>
        <select value={s.hmp.mode} onChange={(e) => set({ hmp: (e.target as HTMLSelectElement).value as HmpMode }, "Change surround")} data-field="hmp">
          <option value="copy">Copy the base's</option>
          <option value="flat">Flat</option>
          <option value="none">None</option>
          <option value="paint">Painted (Terrain tab)</option>
        </select>
      </label>
      <p class="hint">The grid in the view marks where the surround lies; paint its heights in the Terrain tab.</p>
    </div>
  );
}
