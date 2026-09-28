import { render } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import type { EditorView } from "@codemirror/view";
import { RpcError, type Client } from "../../sdk/client";
import { errorText, useConnection } from "../../sdk/hooks";
import { createEditor, setText } from "./editor";
import { History, loadHistory, parseTarget, saveHistory, targetLabel, targetParams, targetValue, type Target } from "./history";

export function mount(el: HTMLElement, c: Client): () => void {
  render(<Console client={c} />, el);
  return () => render(null, el);
}

interface Entry { id: number; target: string; code: string; ok: boolean; text: string; ms?: number; refused?: boolean; }
interface ModItem { id: string; name: string; state: string; hasClient: boolean; }

const TARGET_KEY = "oasis.console.target";
const MAX_ENTRIES = 300;

function loadTarget(): Target {
  try {
    return parseTarget(localStorage.getItem(TARGET_KEY) ?? "client");
  } catch {
    return { kind: "client" };
  }
}

function Console({ client }: { client: Client }) {
  const conn = useConnection(client);
  const host = useRef<HTMLDivElement>(null);
  const out = useRef<HTMLDivElement>(null);
  const view = useRef<EditorView>();
  const hist = useRef(new History(loadHistory()));
  const [target, setTargetRaw] = useState<Target>(loadTarget);
  const targetRef = useRef(target);
  targetRef.current = target;
  const [mods, setMods] = useState<ModItem[]>([]);
  const [entries, setEntries] = useState<Entry[]>([]);
  const [busy, setBusy] = useState(false);
  const nextId = useRef(1);
  const canEval = conn.open && client.has("lua.eval");
  const readOnly = !!conn.welcome?.limits?.readOnly;

  const setTarget = (t: Target) => {
    setTargetRaw(t);
    try {
      localStorage.setItem(TARGET_KEY, targetValue(t));
    } catch {
      /* storage unavailable */
    }
  };

  const push = (e: Omit<Entry, "id">) => {
    setEntries((xs) => {
      const next = [...xs, { ...e, id: nextId.current++ }];
      return next.length > MAX_ENTRIES ? next.slice(next.length - MAX_ENTRIES) : next;
    });
  };

  const run = async () => {
    const v = view.current;
    if (!v) return;
    const code = v.state.doc.toString();
    if (!code.trim()) return;
    const t = targetRef.current;
    hist.current.add(code);
    saveHistory(hist.current.list());
    setText(v, "");
    const t0 = performance.now();
    setBusy(true);
    try {
      const r = await client.call<{ ok: boolean; text: string }>("lua.eval", { ...targetParams(t), code }, 15000);
      push({ target: targetLabel(t), code, ok: !!r.ok, text: String(r.text ?? ""), ms: performance.now() - t0 });
    } catch (e) {
      const refused = e instanceof RpcError && (e.code === -32000 || e.code === -32001 || e.code === -32003);
      push({ target: targetLabel(t), code, ok: false, text: errorText(e), refused });
    } finally {
      setBusy(false);
    }
  };
  const runRef = useRef(run);
  runRef.current = run;

  useEffect(() => {
    if (!host.current) return;
    const v = createEditor(host.current, {
      run: () => runRef.current(),
      historyStep: (dir, current) => (dir < 0 ? hist.current.older(current) : hist.current.newer()),
      complete: (prefix) => client.call<string[]>("lua.complete", { ...targetParams(targetRef.current), prefix }, 3000)
        .then((r) => (Array.isArray(r) ? r.filter((x) => typeof x === "string") : [])),
    });
    view.current = v;
    v.focus();
    return () => {
      v.destroy();
      view.current = undefined;
    };
  }, []);

  const loadMods = () => {
    if (!conn.open || !client.has("mods.list")) return;
    client.call<unknown>("mods.list").then((v) => {
      if (!Array.isArray(v)) return;
      setMods(v.filter((m): m is ModItem => !!m && typeof m === "object" && typeof (m as ModItem).id === "string")
        .filter((m) => m.state === "enabled" && m.hasClient));
    }, () => {});
  };
  useEffect(loadMods, [conn.open]);

  useEffect(() => {
    const el = out.current;
    if (el) el.scrollTop = el.scrollHeight;
  }, [entries.length]);

  const value = targetValue(target);
  const modMissing = target.kind === "mod" && conn.open && mods.length > 0 && !mods.some((m) => m.id === target.mod);

  return (
    <div class="console" data-console>
      <div class="fb">
        <label class="field">
          <span>Target</span>
          <select value={value} data-control="target" onFocus={loadMods} onChange={(e) => setTarget(parseTarget((e.currentTarget as HTMLSelectElement).value))}>
            <option value="client">Client (the console environment)</option>
            <option value="match">Match (the game's Lua, in a match)</option>
            {mods.map((m) => <option key={m.id} value={`mod:${m.id}`}>Mod: {m.name || m.id}</option>)}
            {target.kind === "mod" && !mods.some((m) => m.id === target.mod) ? <option value={value}>Mod: {target.mod}</option> : null}
          </select>
        </label>
        <div class="fb-actions">
          <button class="btn primary" data-action="run" disabled={!canEval || busy || readOnly} onClick={run}>Run</button>
          <button class="btn" data-action="clear" onClick={() => setEntries([])}>Clear</button>
        </div>
      </div>
      {target.kind === "match" ? <p class="hint">Match code runs in the game's own Lua. Online it is refused unless [LuaConsole] MatchConsoleOnline=1.</p> : null}
      {modMissing ? <p class="hint warn">Mod '{target.mod}' is not enabled with client code.</p> : null}
      {readOnly ? <p class="hint warn">Oasis is read-only ([Oasis] ReadOnly=1): the console cannot run code.</p> : null}
      {conn.open && !client.has("lua.eval") ? <p class="hint">This server has no Lua console.</p> : null}
      <div class="console-out" ref={out} aria-live="polite" data-output>
        {entries.length === 0 ? <p class="muted">Results appear here. Try <code>return wum.game.scene()</code>.</p> : null}
        {entries.map((e) => (
          <div key={e.id} class={`entry${e.ok ? "" : e.refused ? " refused" : " failed"}`} data-entry={e.ok ? "ok" : e.refused ? "refused" : "error"}>
            <div class="entry-code"><span class="muted">{e.target}&gt;</span> <code>{e.code}</code></div>
            <pre class="entry-text">{e.text || (e.ok ? "(no value)" : "")}</pre>
            {e.ms !== undefined ? <div class="muted small">{e.ms.toFixed(1)} ms</div> : null}
          </div>
        ))}
      </div>
      <div class={`console-in${busy ? " busy" : ""}`} ref={host} />
    </div>
  );
}
