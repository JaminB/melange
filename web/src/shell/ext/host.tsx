// Hosts a module's or mod's web panel: a sandboxed iframe with no cookie and no same-origin fetch, talking to
// the shell only through postMessage. The bridge lets it subscribe to its own mod.<id>.* channels plus the
// read-only "state" and "log" channels, and call its own mod.<id>.* methods. Everything else is refused here,
// before it ever reaches the real connection.
import { useEffect, useRef } from "preact/hooks";
import type { Client } from "../../sdk/client";
import type { PanelInfo } from "../../sdk/protocol";

export function extAllowed(panelId: string, name: string, kind: "channel" | "method"): boolean {
  if (name.startsWith(`mod.${panelId}.`)) return true;
  return kind === "channel" && (name === "state" || name === "log");
}

interface BridgeMsg {
  type: "call" | "subscribe" | "unsubscribe";
  id?: number;
  subId?: number;
  m?: string;
  p?: object;
  ch?: string;
  filter?: object;
}

export function ExtPanel({ info, client }: { info: PanelInfo; client: Client }) {
  const ref = useRef<HTMLIFrameElement>(null);

  useEffect(() => {
    const frame = ref.current;
    if (!frame) return;
    const unsubs = new Map<number, () => void>();

    const onMessage = (ev: MessageEvent) => {
      if (ev.source !== frame.contentWindow) return;
      const m = ev.data as BridgeMsg | undefined;
      if (!m || typeof m !== "object") return;
      const reply = (data: object) => frame.contentWindow?.postMessage(data, "*");

      if (m.type === "call") {
        if (typeof m.m !== "string" || !extAllowed(info.id, m.m, "method")) {
          reply({ type: "res", id: m.id, ok: false, error: "not allowed" });
          return;
        }
        client.call(m.m, m.p).then(
          (result) => reply({ type: "res", id: m.id, ok: true, result }),
          (e) => reply({ type: "res", id: m.id, ok: false, error: e instanceof Error ? e.message : String(e) }),
        );
      } else if (m.type === "subscribe") {
        if (typeof m.ch !== "string" || typeof m.subId !== "number" || !extAllowed(info.id, m.ch, "channel")) return;
        const subId = m.subId;
        const off = client.subscribe(m.ch, m.filter, (data, seq) => reply({ type: "ev", subId, data, seq }));
        unsubs.get(subId)?.();
        unsubs.set(subId, off);
      } else if (m.type === "unsubscribe" && typeof m.subId === "number") {
        unsubs.get(m.subId)?.();
        unsubs.delete(m.subId);
      }
    };

    window.addEventListener("message", onMessage);
    return () => {
      window.removeEventListener("message", onMessage);
      for (const off of unsubs.values()) off();
    };
  }, [info.id, client]);

  return <iframe ref={ref} class="ext-panel" sandbox="allow-scripts" src={info.url} title={info.title} />;
}
