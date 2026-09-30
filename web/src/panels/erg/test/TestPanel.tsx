// The Test button and status line. Mounted by the Erg editor once a project is open; this file only needs a Client
// and an ErgSession, so it also works against the synthetic fixture server.
import { useEffect, useState } from "preact/hooks";
import type { ErgSession } from "../../../sdk/erg/session";
import type { Client } from "../../../sdk/client";
import { errorText, useConnection } from "../../../sdk/hooks";
import type { LobbyState } from "../../../sdk/streams";
import {
  IDLE_STATUS, reduceTestEvent, statusLine, testAvailability, type GameState, type TestStatus,
} from "./model";

function useGameState(client: Client): GameState {
  const conn = useConnection(client);
  const [inMatch, setInMatch] = useState(false);
  const [inLobby, setInLobby] = useState(false);
  useEffect(() => client.subscribe<{ match?: { inMatch?: boolean } }>("state", undefined,
    (m) => setInMatch(!!m?.match?.inMatch)), [client]);
  useEffect(() => client.subscribe<LobbyState>("lobby", undefined, (m) => setInLobby(!!m?.inLobby)), [client]);
  return { connected: conn.open, inMatch, inLobby };
}

export function TestPanel({ client, session, beforeTest }: {
  client: Client; session: ErgSession; beforeTest?: () => Promise<boolean>;
}) {
  const game = useGameState(client);
  const [status, setStatus] = useState<TestStatus>(IDLE_STATUS);
  const [error, setError] = useState<string>();
  useEffect(() => session.onTest((ev) => setStatus((s) => reduceTestEvent(s, ev))), [session]);

  const avail = testAvailability(game);
  const onClick = async () => {
    setError(undefined);
    try {
      if (beforeTest && !(await beforeTest())) return;
      const r = await session.test();
      setStatus((s) => reduceTestEvent(s, { state: r.state, key: r.key, detail: "" }));
    } catch (e) {
      setError(errorText(e));
    }
  };

  return (
    <div class="erg-test" data-erg-test>
      <button class="btn" data-action="test" disabled={!avail.ok || status.busy} title={avail.reason ?? ""} onClick={onClick}>
        Test
      </button>
      {!avail.ok ? <span class="muted"> {avail.reason}</span> : null}
      {status.phase !== "idle" ? <span class="status-line" data-test-status>{statusLine(status)}</span> : null}
      {error ? <span class="error" data-test-error> {error}</span> : null}
    </div>
  );
}
