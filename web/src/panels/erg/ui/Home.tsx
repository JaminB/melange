// The project list (level.list) and the form that starts a project from a base level (level.new).
import { useEffect, useState } from "preact/hooks";
import type { Client } from "../../../sdk/client";
import { errorText, useConnection } from "../../../sdk/hooks";
import { printable } from "../../../sdk/erg";

export interface ProjectInfo { id: string; title: string; stem: string; base: string; modified?: string; built?: boolean; }
interface BaseInfo { key: string; stem: string; title: string; source: string; theme?: string; }

const str = (v: unknown) => (typeof v === "string" ? v : "");

export function listOf(v: unknown): { bases: BaseInfo[]; projects: ProjectInfo[] } {
  const o = (v && typeof v === "object" ? v : {}) as Record<string, unknown>;
  const bases = (Array.isArray(o.bases) ? o.bases : []).filter((b) => b && typeof b === "object").map((b) => {
    const r = b as Record<string, unknown>;
    return { key: str(r.key), stem: str(r.stem), title: str(r.title) || str(r.key), source: str(r.source), theme: str(r.theme) };
  }).filter((b) => b.key);
  const projects = (Array.isArray(o.projects) ? o.projects : []).filter((p) => p && typeof p === "object").map((p) => projectOf(p))
    .filter((p): p is ProjectInfo => !!p);
  return { bases, projects };
}

export function projectOf(p: unknown): ProjectInfo | undefined {
  if (!p || typeof p !== "object") return undefined;
  const r = p as Record<string, unknown>;
  const id = str(r.id), base = str(r.base) || str((r.base as Record<string, unknown> | undefined)?.key);
  if (!id || !base) return undefined;
  return { id, title: str(r.title) || id, stem: str(r.stem), base, modified: str(r.modified) || undefined, built: r.built === true };
}

export const validSlug = (s: string) => /^[a-z0-9]{1,24}$/.test(s);

interface Props {
  client: Client; busy?: string; error?: string; autoOpen?: string;
  onOpen(p: ProjectInfo): void; onAutoDone(): void;
}

export function Home({ client, busy, error, autoOpen, onOpen, onAutoDone }: Props) {
  const conn = useConnection(client);
  const [list, setList] = useState<ReturnType<typeof listOf>>();
  const [listError, setListError] = useState<string>();
  const [base, setBase] = useState("");
  const [slug, setSlug] = useState("");
  const [title, setTitle] = useState("");
  const [formError, setFormError] = useState<string>();
  const [creating, setCreating] = useState(false);

  const load = () => {
    if (!conn.open) return;
    client.call<unknown>("level.list").then((v) => {
      const l = listOf(v);
      setList(l);
      setListError(undefined);
      if (!base && l.bases[0]) setBase(l.bases[0].key);
      if (autoOpen) {
        const p = l.projects.find((x) => x.id === autoOpen);
        onAutoDone();
        if (p) onOpen(p);
      }
    }, (e) => setListError(errorText(e)));
  };
  useEffect(load, [conn.open]);

  const create = async (e: Event) => {
    e.preventDefault();
    if (!validSlug(slug)) return setFormError("The id is 1-24 characters: a-z and 0-9.");
    if (!printable(title, 1, 40)) return setFormError("The title is 1-40 printable ASCII characters.");
    setCreating(true);
    setFormError(undefined);
    try {
      const r = await client.call<unknown>("level.new", { base, slug, title }, 30000);
      const p = projectOf(r) ?? { id: slug, title, stem: "", base };
      onOpen(p);
    } catch (err) {
      setFormError(errorText(err));
    } finally {
      setCreating(false);
    }
  };

  return (
    <div class="erg-home pad" data-erg-home>
      <div class="row between">
        <h1>Erg</h1>
        <button class="btn" onClick={load} disabled={!conn.open}>Refresh</button>
      </div>
      <p class="muted">Edit a copy of a multiplayer level: move spawn knots and objects, set water and theme, then save the
        project. Levels are never changed in place.</p>
      {busy ? <p class="hint" data-erg-busy>Opening {busy}…</p> : null}
      {error ? <p class="error" data-erg-error>{error}</p> : null}
      {listError ? <p class="error">{listError}</p> : null}
      {!list ? <p class="muted">{conn.open ? "Loading…" : "Not connected."}</p> : (
        <>
          <h2>Projects</h2>
          {list.projects.length === 0 ? <p class="muted">No projects yet.</p> : (
            <table class="table" data-erg-projects>
              <thead><tr><th>Title</th><th>Id</th><th>Base</th><th>Saved</th><th /></tr></thead>
              <tbody>
                {list.projects.map((p) => (
                  <tr key={p.id} data-project={p.id}>
                    <td>{p.title}</td><td><code>{p.id}</code></td><td><code>{p.base}</code></td><td class="muted">{p.modified ?? ""}</td>
                    <td><button class="btn" disabled={!!busy || !conn.open} onClick={() => onOpen(p)}>Open</button></td>
                  </tr>
                ))}
              </tbody>
            </table>
          )}
          <h2>New project</h2>
          <form class="erg-new" onSubmit={create} data-erg-new>
            <label>Base level
              <select value={base} onChange={(e) => setBase((e.target as HTMLSelectElement).value)} data-control="base">
                {list.bases.map((b) => <option key={b.key} value={b.key}>{b.title} ({b.key})</option>)}
              </select>
            </label>
            <label>Id
              <input value={slug} maxLength={24} placeholder="harbour" data-control="slug"
                     onInput={(e) => setSlug((e.target as HTMLInputElement).value.toLowerCase())} />
            </label>
            <label>Title
              <input value={title} maxLength={40} placeholder="Harbour Brawl" data-control="title"
                     onInput={(e) => setTitle((e.target as HTMLInputElement).value)} />
            </label>
            <button class="btn primary" type="submit" disabled={creating || !conn.open || !base} data-action="create">Create</button>
          </form>
          {formError ? <p class="error" data-erg-form-error>{formError}</p> : null}
        </>
      )}
    </div>
  );
}
