import "./shell/shell.css";
import "./launcher/launcher.css";
import "./panels";
import { render } from "preact";
import { useEffect, useState } from "preact/hooks";
import { createClient, type OasisClient } from "./sdk/client";
import { App as ShellApp } from "./shell/Shell";
import { LauncherApp } from "./launcher/App";

function reloadOnce() {
  try {
    if (sessionStorage.getItem("oasis.reloaded") === "1") return;
    sessionStorage.setItem("oasis.reloaded", "1");
  } catch {
    return;
  }
  location.reload();
}

// The launcher's "Oasis tools" and "Map editor" buttons open the browser on the token URL plus "#/<panel>"; such a
// hash shows the regular shell (whose hashPanel opens the panel) even though this server is Melange.exe's. Read once
// at load, and never for the launcher's own "#/plugins..." place (the Import page), which it writes while running.
const wantsShell = /^#\/(?!plugins(\/|$))/.test(location.hash);

// The page learns whether it is hosted by Melange.exe (caps includes "launcher") only once the welcome message
// arrives; until then (or if the connection never completes) it falls back to the regular Oasis shell.
function Root({ client }: { client: OasisClient }) {
  const [launcher, setLauncher] = useState<boolean>();
  useEffect(() => {
    const check = () => {
      if (client.state === "open") setLauncher(!!client.welcome()?.caps?.includes("launcher"));
    };
    check();
    return client.onState(check);
  }, [client]);
  useEffect(() => { if (launcher !== undefined) document.title = launcher && !wantsShell ? "Melange" : "Oasis"; }, [launcher]);
  if (launcher === undefined) return <div class="boot-blank" />;
  return launcher && !wantsShell ? <LauncherApp client={client} /> : <ShellApp client={client} />;
}

const client = createClient({ build: typeof __OASIS_BUILD__ === "string" ? __OASIS_BUILD__ : "dev", onStale: reloadOnce });
render(<Root client={client} />, document.getElementById("app")!);
