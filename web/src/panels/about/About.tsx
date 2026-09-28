import { render } from "preact";
import { useEffect, useState } from "preact/hooks";
import type { Client, Welcome } from "../../sdk/client";
import { RpcError } from "../../sdk/client";

export function mount(el: HTMLElement, c: Client): () => void {
  render(<About client={c} />, el);
  return () => render(null, el);
}

interface Ping { rttMs: number; frame: number; }

function About({ client }: { client: Client }) {
  const [welcome, setWelcome] = useState<Welcome | undefined>(client.welcome());
  const [ping, setPing] = useState<Ping>();
  const [error, setError] = useState<string>();
  useEffect(() => {
    setWelcome(client.welcome());
    return client.onState(() => setWelcome(client.welcome()));
  }, []);

  const doPing = async () => {
    setError(undefined);
    const t0 = performance.now();
    try {
      const r = await client.call<{ frame: number; ms: number }>("sys.ping");
      setPing({ rttMs: performance.now() - t0, frame: r.frame });
    } catch (e) {
      setPing(undefined);
      setError(e instanceof RpcError ? `${e.message} (${e.code})` : String(e));
    }
  };

  const page = typeof __OASIS_BUILD__ === "string" ? __OASIS_BUILD__ : "dev";
  return (
    <div class="about">
      <h1>About Oasis</h1>
      <p class="muted">Oasis is Melange's local web app. It runs inside the game on 127.0.0.1 and nothing outside this computer can reach it.</p>
      <dl class="facts">
        <dt>Protocol</dt><dd data-fact="proto">{welcome ? welcome.proto : "—"}</dd>
        <dt>Server</dt><dd data-fact="server">{welcome ? welcome.server : "—"}</dd>
        {welcome?.game ? <><dt>Melange</dt><dd>{welcome.game.melange}</dd><dt>Game build</dt><dd>{welcome.game.exeBuild ? `#${welcome.game.exeBuild}` : "unrecognised"}</dd></> : null}
        <dt>Page build</dt><dd><code>{page}</code></dd>
        <dt>Server build</dt><dd><code>{welcome?.build ?? "—"}</code></dd>
        {welcome?.limits ? <><dt>Clients</dt><dd>up to {welcome.limits.maxClients}{welcome.limits.readOnly ? ", read-only" : ""}</dd></> : null}
      </dl>
      <div class="row">
        <button class="btn" data-action="ping" onClick={doPing} disabled={!welcome}>Ping the game</button>
        <span class="ping" data-result="ping" aria-live="polite">
          {ping ? `${ping.rttMs.toFixed(1)} ms round trip, frame ${ping.frame}` : error ? <span class="error">{error}</span> : null}
        </span>
      </div>
      <h2>Channels</h2>
      <List items={welcome?.channels} empty="No channels yet." />
      <h2>Methods</h2>
      <List items={welcome?.methods} empty="No methods yet." />
      <p class="muted small"><a href="/app/licenses.txt" target="_blank" rel="noreferrer">Third-party licences</a></p>
    </div>
  );
}

function List({ items, empty }: { items?: string[]; empty: string }) {
  if (!items || !items.length) return <p class="muted">{empty}</p>;
  return <ul class="chips">{[...items].sort().map((s) => <li key={s}><code>{s}</code></li>)}</ul>;
}
