// Erg, the map editor: the project list, then one open project (the 3D view, outliner, properties, level settings
// and checks). Edits go through commands; Save sends the patch against the pinned base.
import { render } from "preact";
import { useRef, useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { errorText, useConnection } from "../../sdk/hooks";
import { Editor } from "./ui/Editor";
import { Home, type ProjectInfo } from "./ui/Home";
import { lastProject, setLastProject } from "./model/draft";
import { openProject, type Opened } from "./model/loader";

export function mount(el: HTMLElement, c: Client): () => void {
  render(<Erg client={c} />, el);
  return () => render(null, el);
}

function Erg({ client }: { client: Client }) {
  const conn = useConnection(client);
  const [open, setOpen] = useState<{ info: ProjectInfo; data: Opened }>();
  const [loading, setLoading] = useState<string>();
  const [error, setError] = useState<string>();
  const [auto, setAuto] = useState(() => lastProject());

  // Opens can overlap (the last project reopening while another is created): only the newest applies, so a late
  // result never replaces the editor opened since, with the edits made in it.
  const latest = useRef({ n: 0, id: "" });
  const openIt = async (info: ProjectInfo) => {
    const n = ++latest.current.n;
    latest.current.id = info.id;
    setLoading(info.title || info.id);
    setError(undefined);
    let data: Opened;
    try {
      data = await openProject(client, info.id, info.base);
    } catch (e) {
      if (n !== latest.current.n) return;
      setError(`Could not open ${info.title || info.id}: ${errorText(e)}`);
      setLoading(undefined);
      return;
    }
    if (n !== latest.current.n) {
      if (latest.current.id !== info.id && client.has("level.close")) client.call("level.close", { project: info.id }).catch(() => {});
      return;
    }
    setLastProject(info.id);
    setOpen({ info, data });
    setLoading(undefined);
  };

  if (conn.open && !client.has("level.list"))
    return <div class="pad"><p class="muted" data-erg-unavailable>This server has no level service, so Erg cannot open levels here.</p></div>;
  if (open)
    return <Editor key={open.info.id} client={client} info={open.info} opened={open.data}
                   onClose={() => { setLastProject(undefined); setAuto(undefined); setOpen(undefined); }} />;
  return (
    <Home client={client} busy={loading} error={error} onOpen={openIt} autoOpen={auto}
          onAutoDone={() => setAuto(undefined)} />
  );
}
