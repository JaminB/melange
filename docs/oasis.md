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
```

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
| `bus.names` | `[{id, name, posts, deliveries}]`: every registered engine message name and its counts |

The streams (`log`, `bus`, `state`, ...) and the other methods are listed in `welcome` as their providers load.

### Streams (`log`, `bus`, `net`, `lobby`, `stats`)

| Channel | Payload (`d`/`b`) | Filter | Overflow |
|---|---|---|---|
| `log` | `{seq, lvl, cat, ts, j}` (`j`: the raw jlog line); backlog of the last 500 matching records on subscribe | `minLevel`, `cats` (list), `text` (substring, matched against the raw line) | DropOldest + `drop` |
| `net` | as `log`, restricted to the `net` and `handshake` categories | as `log` | DropOldest + `drop` |
| `bus` | `{seq, frame, name, cls, path, handle, d?}` (`d`: `bus::Decode`'s fields, when the filter asks for it and a decoder exists) | `names` (required: exact name or `Prefix.*`), `path` (`"post"`; only Post is hooked), `decode` | DropOldest + `drop` |
| `bus.counts` | `{name: delta}` for every name whose post+deliver count changed, once a second | - | Coalesce |
| `lobby` | `{inLobby, local: {hash, contentMods, modMessages, vanilla}, peers: [{steamId, name, status, hash16, version}]}`, on change and at least once a second | - | Coalesce |
| `stats` | `oasis::GetStats()` plus `render::GetTiming()` (`busyMsP50`, `busyMsP95`, `fps`, `frames`), once a second | - | Coalesce |

The `bus` channel's hook is installed only while at least one client subscribes to it, and only messages named by
some subscriber's filter reach it at all (`Camera.HasUpdated` included). `bus.counts` reads the bus module's own
always-on counters, so it costs nothing extra to keep the hook installed or not.
