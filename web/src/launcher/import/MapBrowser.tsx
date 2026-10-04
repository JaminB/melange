// The "Maps" section of the Import page (spec §12.2 "Map browser"): search, native-select filters (accessibility
// note in §12.3) and per-row / bulk show-hide, all acting on `import.setHidden`.
import { useMemo, useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { errorText } from "../../sdk/hooks";
import { importMapsResultOf, importPreviewUrl, type ImportMap } from "../api";
import { MAP_BROWSER_EMPTY, MAP_SEARCH_PLACEHOLDER, MAP_SHOWN_OPTIONS, hideAllLabel, showAllLabel } from "../copy";
import {
  DEFAULT_FILTERS, bulkHideTargets, categoryOptions, countToHide, countToShow, filterMaps, groupOptions, playBadge,
  sortedByTitle, timeOfDayLabel, type MapFilters, type ShownFilter,
} from "./model";

export function MapBrowser({ client, plugin, maps, onChanged }: {
  client: Client; plugin: string; maps: ImportMap[]; onChanged: (maps: ImportMap[]) => void;
}) {
  const [filters, setFilters] = useState<MapFilters>(DEFAULT_FILTERS);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string>();

  const groups = useMemo(() => groupOptions(maps), [maps]);
  const categories = useMemo(() => categoryOptions(maps), [maps]);
  const filtered = useMemo(() => sortedByTitle(filterMaps(maps, filters)), [maps, filters]);
  const toShow = countToShow(filtered), toHide = countToHide(filtered);

  const setHidden = async (files: string[], hidden: boolean) => {
    if (!files.length || busy) return;
    setBusy(true);
    setError(undefined);
    try {
      await client.call("import.setHidden", { plugin, files, hidden });
      onChanged(importMapsResultOf(await client.call("import.maps", { plugin })).maps);
    } catch (e) {
      setError(errorText(e));
    } finally {
      setBusy(false);
    }
  };

  return (
    <section data-import-maps>
      <h2>Maps</h2>
      <div class="fb li-map-filters">
        <input class="fb-text" type="search" placeholder={MAP_SEARCH_PLACEHOLDER} aria-label={MAP_SEARCH_PLACEHOLDER} value={filters.query}
               onInput={(e) => setFilters((f) => ({ ...f, query: (e.currentTarget as HTMLInputElement).value }))} />
        <label class="small li-map-filter">Source
          <select aria-label="Source" value={filters.group} onChange={(e) => setFilters((f) => ({ ...f, group: (e.currentTarget as HTMLSelectElement).value }))}>
            <option value="">All</option>
            {groups.map((g) => <option key={g.value} value={g.value}>{g.label}</option>)}
          </select>
        </label>
        <label class="small li-map-filter">Plays
          <select aria-label="Plays" value={filters.category} onChange={(e) => setFilters((f) => ({ ...f, category: (e.currentTarget as HTMLSelectElement).value }))}>
            <option value="">All</option>
            {categories.map((c) => <option key={c.value} value={c.value}>{c.label}</option>)}
          </select>
        </label>
        <label class="small li-map-filter">Shown
          <select aria-label="Shown" value={filters.shown} onChange={(e) => setFilters((f) => ({ ...f, shown: (e.currentTarget as HTMLSelectElement).value as ShownFilter }))}>
            {MAP_SHOWN_OPTIONS.map((o) => <option key={o.value} value={o.value}>{o.label}</option>)}
          </select>
        </label>
        <div class="fb-actions">
          <button class="btn" data-show-all disabled={busy || !toShow} onClick={() => setHidden(bulkHideTargets(filtered, false), false)}>{showAllLabel(toShow)}</button>
          <button class="btn" data-hide-all disabled={busy || !toHide} onClick={() => setHidden(bulkHideTargets(filtered, true), true)}>{hideAllLabel(toHide)}</button>
        </div>
      </div>
      {error ? <p class="error" role="alert">{error}</p> : null}
      {filtered.length === 0 ? <p class="muted" data-empty>{MAP_BROWSER_EMPTY}</p> : (
        <ul class="li-map-list" data-maps>
          {filtered.map((m) => {
            const badge = playBadge(m);
            return (
              <li key={m.file} class="li-map-row" data-map={m.file} data-hidden={m.hidden}>
                <div class="li-map-preview">
                  {m.preview ? <img src={importPreviewUrl(plugin, m.stem)} alt="" loading="lazy" /> : <div class="li-map-preview-empty" aria-hidden="true" />}
                </div>
                <div class="li-map-main">
                  <div class="li-map-title">{m.title}{m.author ? <span class="muted small"> by {m.author}</span> : null}</div>
                  <div class="muted small">{m.theme} · {timeOfDayLabel(m.timeOfDay)}</div>
                  <div class="row" style="gap:6px;margin-top:2px">
                    {badge ? <span class="tag">{badge}</span> : null}
                    {m.survivor ? <span class="tag">Survivor</span> : null}
                  </div>
                </div>
                <label class="small li-map-show">
                  <input type="checkbox" checked={!m.hidden} disabled={busy} data-show={m.file}
                         onChange={(e) => setHidden([m.file], !(e.currentTarget as HTMLInputElement).checked)} /> Show
                </label>
              </li>
            );
          })}
        </ul>
      )}
    </section>
  );
}
