// One open project: toolbar, outliner, the 3D view, and the properties / level / checks side panel.
import { useEffect, useMemo, useRef, useState } from "preact/hooks";
import type { Client } from "../../../sdk/client";
import { errorText, useConnection } from "../../../sdk/hooks";
import { RemoveDetail, SetDetail, SetObject, type Command, type CommandStack, type Detail } from "../../../sdk/erg";
import { checkScene } from "../model/checks";
import { clearDraft, readDraft, writeDraft } from "../model/draft";
import { Duplicate, Group, setMany } from "../model/edits";
import { dropPoint } from "../model/ground";
import { withPatch, type Opened } from "../model/loader";
import { BUILTIN, canCopy, catalogOf, objectOf, paletteFrom, type ObjectCatalog, type PaletteEntry } from "../model/placing";
import { EditorStore } from "../model/store";
import type { Tool, Viewport } from "../view/viewport";
import type { ProjectInfo } from "./Home";
import { Checks } from "./Checks";
import { LevelSettings, themesOf, type ThemeInfo } from "./LevelSettings";
import { Outliner } from "./Outliner";
import { Properties } from "./Properties";
import { snapVec } from "../model/geometry";
import { ExportDialog } from "../test/ExportDialog";
import { TestPanel } from "../test/TestPanel";
import { ScriptDoc, ScriptPanel } from "../script";
import { Sculptor, SurroundTools, TerrainTool, TerrainTools, fetchAtlas, hexColors, materialNames, setAtlas } from "../terrain";
import { assetUrl } from "../../../sdk/erg/assets";

export const SNAPS: (number | null)[] = [null, 1, 0.5, 0.1];

function useVersion(store: EditorStore) {
  const [, set] = useState(0);
  useEffect(() => store.on(() => set((n) => n + 1)), [store]);
  return store.version;
}

interface Props { client: Client; info: ProjectInfo; opened: Opened; onClose(): void; }

export function Editor({ client, info, opened, onClose }: Props) {
  const conn = useConnection(client);
  const serverStore = useMemo(() => new EditorStore(info.id, opened.base, opened.current), [opened]);
  // A draft from an earlier page load of this project replaces the server state until it is saved or discarded.
  const draft = useMemo(() => {
    const d = readDraft(info.id, serverStore.base.base.sha256.xan, serverStore.savedText());
    if (!d) return undefined;
    try {
      const s = new EditorStore(info.id, opened.base, withPatch(opened.base, d.patch));
      s.markSaved(serverStore.savedText());
      return { store: s, at: d.at };
    } catch {
      clearDraft(info.id);
      return undefined;
    }
  }, [serverStore]);
  const [store, setStore] = useState(draft?.store ?? serverStore);
  const [restored, setRestored] = useState<number | undefined>(draft?.at);
  useVersion(store);
  const viewEl = useRef<HTMLDivElement>(null);
  const rootEl = useRef<HTMLDivElement>(null);
  const [view, setView] = useState<Viewport>();
  const [viewError, setViewError] = useState<string>();
  const [tool, setToolState] = useState<Tool>("translate");
  const [snap, setSnapState] = useState<number | null>(0.5);
  const [angle, setAngleState] = useState(true);
  const [placing, setPlacingState] = useState<PaletteEntry | null>(null);
  const [palette, setPalette] = useState<PaletteEntry[]>(BUILTIN);
  const [catalog, setCatalog] = useState<ObjectCatalog | undefined>();
  const [themes, setThemes] = useState<ThemeInfo>(() => themesOf(undefined));
  const [tab, setTab] = useState<"props" | "level" | "checks" | "terrain" | "script" | "export">("props");
  const scriptDoc = useMemo(() => new ScriptDoc(opened.session), [opened]);
  const hasScript = client.has("level.script.get") && client.has("level.script.put");
  const [, bumpScript] = useState(0);
  useEffect(() => {
    let dirty = scriptDoc.dirty;
    return scriptDoc.on(() => { if (scriptDoc.dirty !== dirty) { dirty = scriptDoc.dirty; bumpScript((n) => n + 1); } });
  }, [scriptDoc]);
  const [sculptOn, setSculptOn] = useState(false);
  const [atlasColors, setAtlasColors] = useState<string[]>();
  const [names, setNames] = useState<string[]>();
  const viewRef = useRef<Viewport>();
  viewRef.current = view;
  const terrainTool = useMemo(() => new TerrainTool(new Sculptor({
    scene: store.scene, voxels: store.voxels, base: store.baseVoxels, refOf: (id) => store.voxelRef(id),
    stack: { exec: (c: Command, m?: boolean) => store.exec(c, m) } as unknown as CommandStack,
    remesh: (ids) => { void viewRef.current?.remesh(ids); },
    freshRef: () => store.freshRef(),
  })), [store]);
  const [message, setMessage] = useState<string>();
  const [saving, setSaving] = useState(false);
  const [saveNote, setSaveNote] = useState<{ ok: boolean; text: string }>();

  useEffect(() => () => {
    if (client.has("level.close")) client.call("level.close", { project: info.id }).catch(() => {});
  }, []);

  useEffect(() => {
    let timer: ReturnType<typeof setTimeout> | undefined;
    const off = store.on((why) => {
      if (why !== "scene") return;
      clearTimeout(timer);
      timer = setTimeout(() => {
        if (store.dirty) writeDraft(info.id, { base: store.base.base.sha256.xan, saved: store.savedText(), patch: store.patch() });
        else clearDraft(info.id);
      }, 300);
    });
    return () => { off(); clearTimeout(timer); };
  }, [store]);

  useEffect(() => {
    let v: Viewport | undefined, dead = false;
    import("../view/viewport").then((m) => {
      if (dead || !viewEl.current) return;
      try {
        v = m.createViewport(viewEl.current, store, { onMessage: flash, onPlaced: () => {} });
        setView(v);
      } catch (e) {
        setViewError(errorText(e));
      }
    }, (e) => setViewError(errorText(e)));
    return () => { dead = true; v?.dispose(); setView(undefined); };
  }, [store]);

  // The view hears about tool changes at once (a click right after choosing must use the new tool), and again
  // whenever a new view starts.
  useEffect(() => { view?.setTool(tool); view?.setSnap(snap, angle); view?.setPlacing(placing); view?.setSculpt(sculptOn ? terrainTool : null); }, [view]);
  const setSculpt = (on: boolean) => { setSculptOn(on); view?.setSculpt(on ? terrainTool : null); if (on) { setPlacing(null); setTab("terrain"); } };
  const setTool = (t: Tool) => { setToolState(t); view?.setTool(t); };
  const setSnap = (s: number | null) => { setSnapState(s); view?.setSnap(s, angle); };
  const setAngle = (a: boolean) => { setAngleState(a); view?.setSnap(snap, a); };
  const setPlacing = (p: PaletteEntry | null) => { setPlacingState(p); view?.setPlacing(p); };

  const theme = store.scene.databank.theme;
  // Previews for every resource already in the loaded scene (any bundle, from level.load), merged with the current
  // theme's palette previews below: a placed detail keeps its preview even when its resource is not in this theme.
  const scenePreviews = useMemo(() => new Map(Object.entries(opened.current.scene.previews ?? {})), [opened]);
  useEffect(() => {
    if (!conn.open || !client.has("level.palette")) {
      view?.setPreviews(scenePreviews);
      return;
    }
    client.call<unknown>("level.palette", { theme }).then((r) => {
      const list = Array.isArray(r) ? r : (r as { entries?: unknown })?.entries;
      setPalette(paletteFrom(list));
      const byRes = new Map(scenePreviews);
      if (Array.isArray(list))
        for (const e of list as Record<string, unknown>[])
          if (typeof e?.resource === "string" && typeof e.preview === "string" && e.preview) byRes.set(e.resource, e.preview);
      view?.setPreviews(byRes);
      const atlas = assetUrl(String((r as { atlas?: unknown })?.atlas ?? ""));
      setAtlasColors(undefined);
      if (atlas) fetchAtlas(atlas).then((rgb) => {
        if (!rgb) return;
        setAtlas(theme, rgb);
        setAtlasColors(hexColors(rgb));
        void view?.remesh();
      }, () => {});
    }, () => view?.setPreviews(scenePreviews));
  }, [conn.open, theme, view, scenePreviews]);
  const materialFile = store.scene.databank.materialFile;
  useEffect(() => {
    setNames(undefined);
    if (!conn.open || !client.has("level.materials") || !materialFile) return;
    const { key, source } = store.base.base;
    client.call<unknown>("level.materials", { file: materialFile, base: key, source }).then((r) => setNames(materialNames(r)), () => {});
  }, [conn.open, materialFile, store]);
  useEffect(() => {
    if (!conn.open || !client.has("level.objects")) return;
    client.call<unknown>("level.objects").then((r) => setCatalog(catalogOf(r)), () => {});
  }, [conn.open]);
  useEffect(() => {
    if (!conn.open || !client.has("level.themes")) return;
    client.call<unknown>("level.themes").then((r) => setThemes(themesOf(r)), () => {});
  }, [conn.open]);

  useEffect(() => {
    const root = rootEl.current as (HTMLDivElement & { __erg?: unknown }) | null;
    if (root) root.__erg = { store, view, terrain: terrainTool };
  }, [store, view, terrainTool]);

  const flashTimer = useRef<ReturnType<typeof setTimeout>>();
  function flash(text: string) {
    setMessage(text);
    clearTimeout(flashTimer.current);
    flashTimer.current = setTimeout(() => setMessage(undefined), 4000);
  }

  const save = async (): Promise<boolean> => {
    if (saving) return false;
    if (!conn.open) {
      setSaveNote({ ok: false, text: "Not connected: the changes are kept here; save again when the connection is back." });
      return false;
    }
    const patch = store.patch();
    const text = JSON.stringify(patch);
    setSaving(true);
    try {
      const r = await opened.session.save(patch);
      if (r.saved === false) throw new Error("the server did not save the project");
      store.markSaved(text);
      clearDraft(info.id);
      setRestored(undefined);
      setSaveNote({ ok: true, text: r.warnings?.length ? `Saved with warnings: ${r.warnings.join("; ")}` : "Saved" });
      return true;
    } catch (e) {
      const details = (e as { details?: string[] }).details;
      setSaveNote({ ok: false, text: `Not saved: ${errorText(e)}${details?.length ? ` (${details.slice(0, 3).join("; ")})` : ""}` });
      return false;
    } finally {
      setSaving(false);
    }
  };

  const discardDraft = () => {
    clearDraft(info.id);
    setRestored(undefined);
    setStore(serverStore);
  };

  const sel = store.selected();
  const del = () => {
    if (!sel.length) return;
    store.exec(sel.length === 1 ? new RemoveDetail(sel[0].id) : new Group("Delete details", sel.map((d) => new RemoveDetail(d.id))));
    store.select([]);
  };
  const duplicate = () => {
    const src = sel.filter((d) => canCopy(d.role) && !objectOf(store.scene, d));
    if (!src.length) return flash(sel.length ? "Scenery and level objects cannot be duplicated; place a new object instead" : "Select something to duplicate");
    const cmd = new Duplicate(src, [1, 0, 1]);
    store.exec(cmd);
    store.select(cmd.ids);
    if (src.length < sel.length) flash("Scenery and level objects were left out: place new objects from the palette");
  };
  const drop = () => {
    const changes: [number, { pos: Detail["pos"] }][] = [];
    for (const d of sel) {
      const p = dropPoint(store.scene, store.frames, (id) => store.voxelsOf({ id }), store.frames.detailWorld(d));
      if (p) changes.push([d.id, { pos: snapVec(store.frames.toLocal(d.frame, p), null) }]);
    }
    if (!changes.length) return flash(sel.length ? "No terrain below the selection" : "Select something to drop");
    store.exec(setMany(changes, "Drop to ground"));
  };

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const root = rootEl.current;
      if (!root || !root.isConnected) return;
      const inField = e.target instanceof HTMLElement && /^(INPUT|SELECT|TEXTAREA)$/.test(e.target.tagName);
      const ctrl = e.ctrlKey || e.metaKey;
      const k = e.key.toLowerCase();
      if (ctrl && k === "s") { e.preventDefault(); void save(); return; }
      if (inField) return;
      if (sculptOn && terrainTool.key(e)) { e.preventDefault(); return; }
      if (ctrl && k === "z" && !e.shiftKey) { e.preventDefault(); store.undo(); return; }
      if (ctrl && ((k === "z" && e.shiftKey) || k === "y")) { e.preventDefault(); store.redo(); return; }
      if (ctrl && k === "d") { e.preventDefault(); duplicate(); return; }
      if (ctrl || e.altKey) return;
      if (k === "delete" || k === "backspace") { e.preventDefault(); del(); }
      else if (k === "w") setTool("translate");
      else if (k === "e") setTool("rotate");
      else if (k === "r") setTool("scale");
      else if (k === "g") drop();
      else if (k === "f") view?.focus();
      else if (k === "escape") { if (placing) setPlacing(null); else store.select([]); }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  });

  const issues = useMemo(() => checkScene(store.scene, store.frames, store.problems()), [store, store.version]);
  const errors = issues.filter((i) => i.level === "error").length;
  const dirty = store.dirty;

  return (
    <div class="erg" ref={rootEl} data-erg-editor={info.id} data-dirty={dirty ? "1" : "0"} data-depth={store.stack.depth}>
      <div class="erg-bar">
        <strong class="erg-title" title={`${info.id} · base ${store.base.base.key}`}>{store.scene.title}</strong>
        <button class={`btn${dirty ? " primary" : ""}`} onClick={() => void save()} disabled={saving} data-action="save" title="Save (Ctrl+S)">
          {saving ? "Saving…" : dirty ? "Save" : "Saved"}
        </button>
        <span class="erg-sep" />
        <button class="btn" onClick={() => store.undo()} disabled={!store.stack.canUndo} data-action="undo" title="Undo (Ctrl+Z)">Undo</button>
        <button class="btn" onClick={() => store.redo()} disabled={!store.stack.canRedo} data-action="redo" title="Redo (Ctrl+Shift+Z)">Redo</button>
        <span class="erg-sep" />
        {(["translate", "rotate", "scale"] as Tool[]).map((t, i) => (
          <button key={t} class={`btn${tool === t ? " on" : ""}`} onClick={() => setTool(t)} data-tool={t}
                  title={`${t[0].toUpperCase()}${t.slice(1)} (${"WER"[i]})`}>{t === "translate" ? "Move" : t === "rotate" ? "Rotate" : "Scale"}</button>
        ))}
        <label class="erg-inline" title="Snap positions to this step (.xan units; world = x20)">Snap
          <select value={String(snap)} onChange={(e) => { const v = (e.target as HTMLSelectElement).value; setSnap(v === "null" ? null : Number(v)); }} data-control="snap">
            {SNAPS.map((s) => <option key={String(s)} value={String(s)}>{s === null ? "off" : `${s} (${s * 20} world)`}</option>)}
          </select>
        </label>
        <label class="erg-inline"><input type="checkbox" checked={angle} onChange={(e) => setAngle((e.target as HTMLInputElement).checked)} /> 15°</label>
        <span class="erg-sep" />
        <button class="btn" onClick={duplicate} disabled={!sel.length} data-action="duplicate" title="Duplicate (Ctrl+D)">Duplicate</button>
        <button class="btn" onClick={drop} disabled={!sel.length} data-action="drop" title="Drop to ground (G)">Drop</button>
        <button class="btn danger" onClick={del} disabled={!sel.length} data-action="delete" title="Delete (Del)">Delete</button>
        <span class="erg-sep" />
        <button class={`btn${sculptOn ? " on" : ""}`} onClick={() => setSculpt(!sculptOn)} data-action="sculpt"
                title="Sculpt the terrain: drag to carve, fill or paint, or click to add a block (Alt-drag still orbits)">Sculpt</button>
        <span class="erg-sep" />
        <span class="muted small">Place</span>
        {palette.map((p) => (
          <button key={p.id} class={`btn${placing?.id === p.id ? " on" : ""}`} data-place={p.id}
                  onClick={() => { if (sculptOn) setSculpt(false); setPlacing(placing?.id === p.id ? null : p); }} title={`Click the terrain to place: ${p.label} (Esc stops)`}>{p.label}</button>
        ))}
        <span class="erg-grow" />
        {client.has("level.test") ? <TestPanel client={client} session={opened.session} projectTod={store.scene.databank.timeOfDay} beforeTest={async () => (!store.dirty || await save()) && (!scriptDoc.dirty || await scriptDoc.save())} /> : null}
        <button class="btn" onClick={onClose} data-action="close">Close</button>
      </div>
      {!conn.open ? <p class="erg-note warn" data-erg-offline>Not connected. Your changes and the undo history are kept; save once the connection is back.</p> : null}
      {restored ? (
        <p class="erg-note" data-erg-restored>Unsaved changes from {new Date(restored).toLocaleString()} were restored from this browser.
          <button class="link" onClick={discardDraft} data-action="discard-draft">Discard them</button></p>
      ) : null}
      {saveNote ? <p class={`erg-note${saveNote.ok ? "" : " bad"}`} data-erg-save={saveNote.ok ? "ok" : "fail"}>{saveNote.text}</p> : null}
      <div class="erg-body">
        <Outliner store={store} onVisible={(roles) => view?.setVisibleRoles(roles)} />
        <div class="erg-view" ref={viewEl} data-erg-view>
          {viewError ? <p class="error pad">The 3D view could not start: {viewError}</p> : !view ? <p class="muted pad">Loading the 3D view…</p> : null}
          {message ? <div class="erg-toast" role="status" data-erg-message>{message}</div> : null}
          {placing ? <div class="erg-hint">Click the terrain to place {placing.label}. Esc stops.</div> : null}
        </div>
        <aside class="erg-side">
          <div class="erg-tabs" role="tablist">
            <button class={`erg-tab${tab === "props" ? " on" : ""}`} onClick={() => setTab("props")} data-tab="props">Properties</button>
            <button class={`erg-tab${tab === "level" ? " on" : ""}`} onClick={() => setTab("level")} data-tab="level">Level</button>
            <button class={`erg-tab${tab === "checks" ? " on" : ""}`} onClick={() => setTab("checks")} data-tab="checks">
              Checks{issues.length ? <span class={`erg-count${errors ? " bad" : ""}`}>{issues.length}</span> : null}
            </button>
            <button class={`erg-tab${tab === "terrain" ? " on" : ""}`} onClick={() => setTab("terrain")} data-tab="terrain">Terrain</button>
            {hasScript ? <button class={`erg-tab${tab === "script" ? " on" : ""}`} onClick={() => setTab("script")} data-tab="script">Script{scriptDoc.dirty ? " •" : ""}</button> : null}
            <button class={`erg-tab${tab === "export" ? " on" : ""}`} onClick={() => setTab("export")} data-tab="export">Export</button>
          </div>
          <div class="erg-side-body">
            {tab === "props" ? <Properties store={store} set={(id, f) => store.exec(new SetDetail(id, f))} catalog={catalog}
                                           setObject={(knot, o) => store.exec(new SetObject(knot, o))} />
              : tab === "level" ? <LevelSettings store={store} themes={themes} />
              : tab === "terrain" ? <><TerrainTools tool={terrainTool} palette={atlasColors} names={names} /><SurroundTools store={store} /></>
              : tab === "script" && hasScript ? <ScriptPanel doc={scriptDoc} />
              : tab === "export" ? <ExportDialog client={client} project={info.id} defaultName={store.scene.title} />
              : <Checks issues={issues} onPick={(id) => { store.select([id]); view?.focus(); }} />}
          </div>
        </aside>
      </div>
      <div class="erg-status small muted">
        <span>{store.scene.frames.length} frames · {store.scene.details.length} details · {sel.length} selected</span>
        <span>Terrain is drawn from the voxels and only approximates the game's surface. Distances are shown in world units (20 per file unit).</span>
      </div>
    </div>
  );
}
