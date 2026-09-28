// The entity list, the map and the data-variable browser.
import { useMemo, useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { RpcError } from "../../sdk/client";
import { FilterBar, VirtualTable, type Column } from "../../sdk/ui";
import type { Target } from "./Inspector";
import {
  KINDS, containerAddr, filterEntities, filterVars, fmtNum, fmtVec, hex32, kindCounts, project, speed, teamColour,
  valueText, type Entity, type Kind, type Snapshot, type Var, type VarType, type View, type Worm,
} from "./model";

export function EntityList({ entities, inMatch, target, onPick }: {
  entities: Entity[]; inMatch: boolean; target?: Target; onPick: (e: Entity) => void;
}) {
  const [kinds, setKinds] = useState<Set<Kind>>(() => new Set<Kind>(["Worm", "Projectile", "Crate", "Barrel"]));
  const [text, setText] = useState("");
  const counts = useMemo(() => kindCounts(entities), [entities]);
  const rows = useMemo(() => filterEntities(entities, kinds, text), [entities, kinds, text]);
  const cols: Column<Entity>[] = [
    { key: "h", title: "Handle", width: "7rem", render: (e) => <code>{hex32(e.handle)}</code> },
    { key: "k", title: "Kind", width: "6.5rem", render: (e) => <span class={`gs-kind k-${e.kind}`}>{e.kind}</span> },
    { key: "t", title: "Type", width: "minmax(8rem, 1.4fr)", render: (e) => e.type },
    { key: "l", title: "Label", width: "minmax(5rem, 1fr)", render: (e) => e.label || "" },
    { key: "p", title: "Position", width: "minmax(9rem, 1.2fr)", render: (e) => <span class="mono">{fmtVec(e.pos)}</span> },
    { key: "s", title: "Speed", width: "4.5rem", render: (e) => <span class="mono">{e.vel ? fmtNum(speed(e.vel), 2) : ""}</span> },
  ];
  const sel = target && "handle" in target ? rows.findIndex((e) => e.handle === target.handle) : -1;
  return (
    <div class="gs-list">
      <FilterBar text={text} onText={setText} placeholder="Filter by type, label or handle"
                 chips={KINDS.map((k) => ({ id: k, label: `${k} ${counts[k]}`, on: kinds.has(k) }))}
                 onChip={(id, on) => {
                   const next = new Set(kinds);
                   if (on) next.add(id as Kind); else next.delete(id as Kind);
                   setKinds(next);
                 }} />
      <VirtualTable rows={rows} columns={cols} rowKey={(e) => e.handle} selected={sel >= 0 ? sel : undefined}
                    onRowClick={onPick}
                    empty={inMatch ? "No entity matches the filter." : "Entities appear here during a match."} />
    </div>
  );
}

const W = 800, H = 480;

export function MapView({ snap, entities, onEntity, onWorm }: {
  snap?: Snapshot; entities: Entity[]; onEntity: (e: Entity) => void; onWorm: (w: Worm) => void;
}) {
  const [view, setView] = useState<View>(() => {
    try { return localStorage.getItem("oasis.gamestate.map") === "side" ? "side" : "top"; } catch { return "top"; }
  });
  const choose = (v: View) => {
    setView(v);
    try { localStorage.setItem("oasis.gamestate.map", v); } catch { /* storage unavailable */ }
  };
  type Item = { worm?: Worm; entity?: Entity };
  const items: Item[] = useMemo(() => [
    ...(snap?.worms ?? []).map((w) => ({ worm: w })),
    ...entities.filter((e) => e.kind !== "Worm" && e.kind !== "Other" && e.pos).map((e) => ({ entity: e })),
  ], [snap, entities]);
  const plot = project(items, (i) => (i.worm ? i.worm.pos : i.entity!.pos), view, W, H, 24, snap?.match.waterLevel);
  const active = snap?.match.activeWorm ?? -1;
  if (!snap?.match.inMatch) return <p class="muted gs-empty">The map shows worms and objects during a match.</p>;
  return (
    <div class="gs-map">
      <div class="gs-map-bar">
        <div class="gs-seg" role="group" aria-label="View">
          <button class={view === "top" ? "on" : ""} aria-pressed={view === "top"} onClick={() => choose("top")}>Top (X/Z)</button>
          <button class={view === "side" ? "on" : ""} aria-pressed={view === "side"} onClick={() => choose("side")}>Side (X/Y)</button>
        </div>
        <span class="muted small">{plot.points.length} plotted · X {fmtNum(plot.bounds.minA, 0)}…{fmtNum(plot.bounds.maxA, 0)}</span>
      </div>
      <svg class="gs-svg" viewBox={`0 0 ${W} ${H}`} role="img" aria-label={`${view === "top" ? "Top-down" : "Side"} map of worms and objects`}>
        <rect x="0" y="0" width={W} height={H} class="gs-svg-bg" />
        {plot.waterY !== undefined ? <rect x="0" y={plot.waterY} width={W} height={Math.max(0, H - plot.waterY)} class="gs-water" /> : null}
        {plot.points.map((p, i) => {
          const w = p.item.worm, e = p.item.entity;
          if (w) {
            return (
              <g key={`w${w.slot}`} class={`gs-dot worm${w.alive ? "" : " dead"}`} data-slot={w.slot} onClick={() => onWorm(w)}>
                {w.slot === active ? <circle cx={p.x} cy={p.y} r="12" class="gs-ring" /> : null}
                <circle cx={p.x} cy={p.y} r="7" fill={w.alive ? teamColour(w.team) : "none"} stroke={teamColour(w.team)} stroke-width="2" />
                <text x={p.x > W - 90 ? p.x - 10 : p.x + 10} y={p.y - 9} text-anchor={p.x > W - 90 ? "end" : "start"}
                      class="gs-label">{w.name}</text>
                <title>{`${w.name} · ${w.health} HP · ${fmtVec(w.pos)}`}</title>
              </g>
            );
          }
          const k = e!.kind;
          return (
            <g key={`e${e!.handle}-${i}`} class={`gs-dot k-${k}`} onClick={() => onEntity(e!)}>
              {k === "Crate" ? <rect x={p.x - 5} y={p.y - 5} width="10" height="10" /> :
               k === "Barrel" ? <rect x={p.x - 4} y={p.y - 6} width="8" height="12" rx="2" /> :
               <circle cx={p.x} cy={p.y} r="4" />}
              <title>{`${e!.label || e!.type} · ${fmtVec(e!.pos)}`}</title>
            </g>
          );
        })}
      </svg>
      <div class="gs-legend small muted">
        <span><i class="lg worm" /> worm (ring: active)</span><span><i class="lg k-Projectile" /> projectile</span>
        <span><i class="lg k-Crate" /> crate</span><span><i class="lg k-Barrel" /> barrel</span>
        {view === "side" ? <span><i class="lg water" /> water</span> : null}
      </div>
    </div>
  );
}

const TYPES: VarType[] = ["Int", "Uint", "Float", "Vector", "String", "Container", "Color", "StringTable", "Undefined"];

export function VarsView({ client, onContainer }: { client: Client; onContainer: (addr: number) => void }) {
  const [prefix, setPrefix] = useState("");
  const [vars, setVars] = useState<Var[]>();
  const [loadedAt, setLoadedAt] = useState<string>();
  const [error, setError] = useState<string>();
  const [busy, setBusy] = useState(false);
  const [text, setText] = useState("");
  const [types, setTypes] = useState<Set<VarType>>(() => new Set(TYPES));
  const rows = useMemo(() => filterVars(vars ?? [], text, types), [vars, text, types]);
  const load = async () => {
    setBusy(true);
    setError(undefined);
    try {
      const r = await client.call<Var[]>("state.vars", prefix ? { prefix } : {});
      setVars(r);
      setLoadedAt(new Date().toLocaleTimeString());
    } catch (e) {
      setError(e instanceof RpcError ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  };
  const cols: Column<Var>[] = [
    { key: "n", title: "Name", width: "minmax(10rem, 1.3fr)", render: (v) => <code>{v.name}</code> },
    { key: "t", title: "Type", width: "6.5rem", render: (v) => <span class="muted">{v.type}</span> },
    { key: "v", title: "Value", width: "minmax(8rem, 1.5fr)", render: (v) => {
      const addr = containerAddr(v);
      return addr !== undefined ? <a class="gs-link" href="#" onClick={(ev) => { ev.preventDefault(); ev.stopPropagation(); onContainer(addr); }}>{valueText(v)}</a>
                                : <span class="mono">{valueText(v)}</span>;
    } },
  ];
  return (
    <div class="gs-list">
      <form class="gs-vars-bar" onSubmit={(e) => { e.preventDefault(); load(); }}>
        <input class="fb-text" value={prefix} maxLength={63} placeholder="Name prefix, e.g. Wind. (empty: all)" aria-label="Name prefix"
               onInput={(e) => setPrefix((e.currentTarget as HTMLInputElement).value)} />
        <button class="btn" type="submit" disabled={busy} data-action="load-vars">{vars ? "Reload" : "Load variables"}</button>
        <span class="muted small" role="status">
          {error ? <span class="error">{error}</span> : vars ? `${vars.length} variables at ${loadedAt}` : "Reads the game's data store once (at most once a second)."}
        </span>
      </form>
      {vars ? (
        <>
          <FilterBar text={text} onText={setText} placeholder="Filter by name or value"
                     chips={TYPES.map((t) => ({ id: t, label: t, on: types.has(t) }))}
                     onChip={(id, on) => {
                       const next = new Set(types);
                       if (on) next.add(id as VarType); else next.delete(id as VarType);
                       setTypes(next);
                     }} />
          <VirtualTable rows={rows} columns={cols} rowKey={(v) => v.name} empty="No variable matches."
                        onRowClick={(v) => { const a = containerAddr(v); if (a !== undefined) onContainer(a); }} />
        </>
      ) : null}
    </div>
  );
}
