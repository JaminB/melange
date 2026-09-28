// Preact hooks for panels: the connection state and the welcome, kept current across reconnects.
import { useEffect, useState } from "preact/hooks";
import type { Client, ClientState, Welcome } from "./client";

export interface Connection { state: ClientState; welcome: Welcome | undefined; open: boolean; }

export function useConnection(c: Client): Connection {
  const read = (): Connection => ({ state: c.state, welcome: c.welcome(), open: c.state === "open" });
  const [conn, setConn] = useState<Connection>(read);
  useEffect(() => {
    setConn(read());
    return c.onState(() => setConn(read()));
  }, [c]);
  return conn;
}

// The text of an RPC failure (RpcError or anything else), for showing next to the control that caused it.
export function errorText(e: unknown): string {
  if (e && typeof e === "object" && "message" in e) return String((e as { message: unknown }).message);
  return String(e);
}
