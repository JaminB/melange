# Oasis

Oasis is Melange's local web app: a page in your browser that talks to the running game. It is served by
`melange.asi` itself, on `127.0.0.1` only, and nothing listens until you open it.

## Using it

- **Open it** with `AutoStart=1` (the overlay button and `Ctrl+Shift+O` are on the way). The link has the form
  `http://127.0.0.1:8765/?k=...`.
- **The link carries a secret token.** The first visit swaps it for a session cookie and removes it from the address
  bar. Anyone with the link can use Oasis while the game runs, so do not share it. A new game session makes a new
  token.
- **Nothing outside this computer can connect.** The server binds `127.0.0.1` only, answers only requests whose
  `Host` is `127.0.0.1:<port>` or `localhost:<port>`, and accepts a WebSocket only from its own page's origin.
  It never sends CORS headers, so other web sites you visit cannot read from it.
- If port 8765 is taken, the next free port up to `Port + PortRange - 1` is used.

### The panels

| Panel | What it does |
|---|---|
| **Logs** | The live log (the `log` stream) in a virtualised table of up to 50 000 records: filter by level, category and text, pause, follow the newest, open a record as a JSON tree. The source menu opens a past session's `events.jsonl`. |
| **Events** | Pick bus messages by name or `Prefix.*` and watch them arrive with their decoded payloads. Nothing is streamed until you pick something. The Counts view shows every message's rate from `bus.counts`. |
| **Console** | Lua, as the overlay console: the client environment, a mod's environment or the match. Enter runs, Shift+Enter adds a line, Tab completes, Up and Down recall your history (kept in the browser). Match code follows the console's rule: refused online unless `[LuaConsole] MatchConsoleOnline=1`. |
| **Mods** | Enable and disable mods (content mods take effect after a restart), see load errors, and revoke Deep Desert. **Deep Desert is never granted from the browser**: a mod waiting for consent asks in the game's overlay. |
| **Settings** | Every `Melange.ini` key a module declares, with its default and whether it applies live or after a restart, plus the file as raw text. Saving changes that one line in place and keeps every comment and other byte. `[Thumper] GrantSalt` is hidden and cannot be changed, and `[Thumper] AutoGrantDeepDesert` can only be set to `0`. |
| **About** | Versions, the protocol, and the channels and methods the server offers. |

Tabs can be opened side by side (the ⧉ button next to a tab, or the command palette, `Ctrl+K`). The layout and the
colour theme (system, light or dark) are remembered by the browser. With `ReadOnly=1` every control that changes
something is disabled, the console included.

### Settings (`[Oasis]` in `Melange.ini`)

| Key | Default | Meaning |
|---|---|---|
| `Enabled` | `1` | Load the Oasis module. With `0`, nothing of Oasis runs. |
| `AutoStart` | `0` | Start the server when the game starts instead of on first open. |
| `AutoOpen` | `0` | Open a browser tab when the server starts. |
| `Port`, `PortRange` | `8765`, `10` | The first port to try and how many to try. |
| `MaxClients` | `4` | Browser tabs (WebSocket clients) at once. |
| `ReadOnly` | `0` | Refuse every call that changes something (ini, mods, captures). |
| `RawInspect` | `1` | Allow the read-only memory view in the entity inspector. |
| `WebRoot` | *(empty)* | Serve the web app from this folder instead of the copy built into `melange.asi` (development; relative to the game folder). |
| `Hotkey` | `Ctrl+Shift+O` | Opens Oasis (reserved; not active yet). |

## Building the web app

The web app lives in `web/` (TypeScript and Preact) and is built into `melange.asi` as a zip. The toolchain is
portable and needs no npm:

```powershell
.\scripts\web\fetch.ps1     # once: Node.js and esbuild into tools\, the packages of web\web.lock.json into web\node_modules
.\build.ps1                 # type-checks, bundles and embeds the web app
.\scripts\web\test.ps1      # unit tests; add -Url <launch url> for the browser tests against a running game
.\scripts\web\test.ps1 -Panels   # plus every panel in headless Edge against a mock server (no game needed)
```

`web/test/e2e/mock-server.mjs` is a stand-in for the game's server that serves a built app and fakes the streams and
methods; `node web/test/e2e/mock-server.mjs --root web/dist` prints a link you can open by hand.

- `web/toolchain.lock.json` pins Node.js and esbuild by SHA-256. `web/web.lock.json` lists every package,
  transitive ones included, with its registry tarball, `sha512` integrity and licence.
- `fetch.ps1` downloads the tarballs straight from the registry, checks each against the lock, checks every licence
  against the allowlist (MIT, ISC, BSD-2-Clause, BSD-3-Clause, Apache-2.0, 0BSD), and unpacks them with the system
  `tar.exe`. No package manager runs and no install script executes. It also writes `THIRD_PARTY.md`.
- `scripts/web/lock.mjs` (under `tools\node\node.exe`) resolves new versions from the registry and rewrites
  `web.lock.json`; updating a dependency is a reviewed diff of that file.
- Without the toolchain, or with `-DMELANGE_WEB=OFF`, `melange.asi` embeds a one-page placeholder instead.
- The build is deterministic: the same sources and lock give the same bundle, and the bundle's build id is a hash of
  its inputs.

## Writing a panel

Panels are TypeScript modules under `web/src/panels/<name>/`. Each registers itself and is loaded on first open:

```ts
import { registerPanel } from "../../sdk/panels";
registerPanel({ id: "hello", title: "Hello", order: 50, needs: ["game"], load: () => import("./Hello") });
```

```tsx
// Hello.tsx
import { render } from "preact";
import type { Client } from "../../sdk/client";
export function mount(el: HTMLElement, c: Client) {
  const off = c.subscribe<{ n: number }>("mod.hello.ticks", undefined, (m) => { el.textContent = `tick ${m.n}`; });
  return () => { off(); render(null, el); };
}
```

A panel imports only from `web/src/sdk/` and its own folder. `sdk/ui` has a virtualised table, a JSON tree, a filter
bar and a split pane.

## Adding channels and methods from C++

`melange/oasis.h`:

```cpp
#include "melange/oasis.h"
namespace oasis = melange::oasis;

oasis::ChannelId g_ticks = oasis::AddChannel("mod.hello.ticks");
void OnFrame() {
    if (oasis::HasSubscribers(g_ticks)) oasis::Publish(g_ticks, "{\"n\":42}");  // costs one atomic load when unwatched
}

void Echo(const oasis::Call& c, oasis::Result& r, void*) { r.json = std::string(c.paramsJson); }
oasis::AddMethod("hello.echo", &Echo, nullptr);  // runs on the main thread, in the Frame event
```

- `Publish` copies the message into each subscriber's queue and never blocks. Each client has a bounded queue per
  channel: `DropOldest` drops from the front and tells the client how many it missed; `Coalesce` keeps only the latest.
- Methods run on the main thread by default, at most 0.5 ms of them per frame; `kRpcServerThread` runs one on the
  server thread instead (it must not touch the game). A method that faults three times is disabled.
- `kRpcMutating` methods are refused when `ReadOnly=1`.

## Protocol (version 1)

### HTTP

HTTP/1.1 with keep-alive, `GET` and `HEAD` only. Requests have no body; `Transfer-Encoding`, folded header lines,
a duplicate `Host`, control characters and non-ASCII bytes in the target are refused with 400, more than 64
headers or 16 KB of them with 431 or 413.

| Route | Auth | Response |
|---|---|---|
| `GET /?k=<token>` | token | sets the session cookie, `303 /` |
| `GET /`, `/app/*` | cookie | the app; `ETag`, `Cache-Control: no-cache`; `.gz` copies sent as-is to clients that accept gzip |
| `GET /ws` (upgrade) | cookie + Origin, or `?k=` with no Origin | WebSocket |
| `GET /captures/<name>.mcap` * | cookie | a capture file, `Range` supported |
| `GET /ext/<panel>/*` * | cookie | a module's or mod's web panel, sandboxed |
| `GET /logs/<session>/<file>` * | cookie | past session logs |
| anything else | - | 404 |

Routes marked * are added by the providers that serve them.

Every response carries `Content-Security-Policy` (`default-src 'self'`, `frame-ancestors 'none'`, ...),
`X-Content-Type-Options: nosniff` and `Referrer-Policy: no-referrer`, and never any `Access-Control-*` header. The
session cookie is `oasis_s`, `HttpOnly; SameSite=Strict; Path=/`.

### WebSocket

RFC 6455 version 13. Client frames must be masked; no extensions; text messages must be valid UTF-8; fragmented
messages are reassembled up to 1 MB; control frames are at most 125 bytes. The server pings after 20 s of silence
and closes after 60 s without an answer.

Every message is one JSON object with a `t` member:

| `t` | Direction | Members |
|---|---|---|
| `hello` | client -> server | `proto` (1), `build` (the page's build id), `client` |
| `welcome` | server -> client | `proto`, `build`, `server` (`game` or `standalone`), `game` (`exeBuild`, `melange`), `channels`, `methods`, `panels`, `limits` |
| `bye` | server -> client | `reason`, `want`: sent before a protocol refusal |
| `sub` | client -> server | `ch`, `filter` (object, optional), `id` (optional; acknowledged by `res`) |
| `unsub` | client -> server | `ch`, `id` (optional) |
| `ev` | server -> client | `ch`, `seq`, then `d` (one value) or `b` (a batch; the items have `seq`, `seq+1`, ...) |
| `drop` | server -> client | `ch`, `n` (messages this client missed), `why` (`queue` or `rate`) |
| `call` | client -> server | `id` (integer chosen by the client), `m` (method), `p` (object, optional) |
| `res` | server -> client | `id`, `r` (the result) |
| `err` | server -> client | `id` (or `null`), `code`, `msg`, `data` (optional) |

The client sends `hello` first; anything else first closes the socket with 4002. `seq` counts every message a
client was due on a channel, dropped ones included, so a gap equals the `n` of the preceding `drop`. Batches are
flushed at most every 50 ms per channel. `res` and `err` are never dropped.

`proto` rises only for a breaking change to this envelope. New channels, methods and members are additive: readers
ignore members they do not know, and clients check `welcome.channels` and `welcome.methods` before using a feature.
A page whose `build` differs from the server's reloads once.

Error codes: `-32600` bad envelope, `-32601` unknown method, `-32602` bad parameters, `-32000` refused by policy
(`msg` says why), `-32001` not in a match or game not running, `-32002` busy (at most 16 queued calls per client),
`-32003` read-only, `-32004` the handler faulted.

Close codes: 4000 closed by the game (the page reconnects), 4001 protocol version, 4002 no hello, 4003 auth,
4008 the client stopped reading, 4029 too many clients.

### Core methods

| Method | Result |
|---|---|
| `sys.ping` | `{frame, ms}`: the game's frame counter and the server's clock |

### Console, mods and settings

| Method | Params → result | Notes |
|---|---|---|
| `lua.eval` | `{target: "client"\|"match"\|"mod", mod?, code}` → `{ok, text}` | Main thread, one per frame across clients; `code` at most 64 KB; `=expr` is shorthand for `return expr`. A Lua error is `ok:false` with the message. `match` outside a match is `-32001`, and refused online (`-32000`, the console's reason) unless `[LuaConsole] MatchConsoleOnline=1`. Refused with `ReadOnly=1`. |
| `lua.complete` | `{target, mod?, prefix}` → `[string]` | The names that can follow the last `.` or `:` of `prefix`. |
| `mods.list` | `{}` → `[ModInfo]` | `ModInfo`: `{id, name, version, authors, dir, kind: "client"\|"content", state, reason, on, restartRequired, implicitManifest, hasClient, hasSim, deepDesert: {declared, granted}, order, sandbox?: {loaded, error, callbacks, disabledCallbacks, faults, bytes}}`. `state` is `enabled`, `disabled`, `blocked`, `pending-consent`, `incompatible` or `restart-required`. |
| `mods.setEnabled` | `{id, on}` → `ModInfo` | As the overlay's checkbox: persisted; client-only mods apply at once, content mods at the next launch. Enabling a Deep Desert mod does not grant it: the consent prompt appears in the game. |
| `mods.revokeDeepDesert` | `{id}` → `ModInfo` | There is no method to grant Deep Desert. |
| `ini.get` | `{}` → `{path, encoding, text, keys}` | `keys`: `[{section, key, def, live, declared, current, line?}]`, every declared key plus the undeclared ones in the file; `def` is `null` for undeclared keys and `current` is `null` for keys missing from the file. The grant salt reads `********`. |
| `ini.set` | `{section, key, value}` → `{live, restart, changed}` | One line is changed in place (indentation, spacing, key spelling and an inline `; comment` kept) or added after the section's last key, and the file is replaced atomically in its own encoding. The key must be declared, present in the file, or in a `[Mod.<id>]` section. Values cannot contain line breaks or `;` or start or end with a space. `[Thumper] GrantSalt`, and `[Thumper] AutoGrantDeepDesert` other than `0`, are refused (`-32000`). |

The `mods` channel sends the whole `mods.list` result to a new subscriber and again on every change (Coalesce).

The streams (`log`, `bus`, `state`, ...) and the other methods are listed in `welcome` as their providers load.
