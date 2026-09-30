# The Spice manifest (`spice.json`)

A Thumper mod is a folder under `<game>\Mods\<id>\` with a `spice.json` at its root. The machine-readable
schema is [spice-1.schema.json](spice-1.schema.json); this page explains the fields and how Thumper uses
them. A folder with no `spice.json` still loads as an implicit, client-only, dependency-free mod named
after its folder — every M1-era `Mods\` folder keeps working unchanged.

## Minimal example

```json
{
  "spiceVersion": 1,
  "id": "hello-spice",
  "version": "1.0.0",
  "name": "Hello Spice",
  "melange": { "range": ">=0.2.0 <0.3.0" },
  "kind": "client-only",
  "entry": { "client": "client/init.lua" }
}
```

## Fields

| Field | Notes |
|---|---|
| `spiceVersion` | Must be `1`. A manifest with a schema version Thumper doesn't know is `incompatible` and never gets a load-order slot. |
| `id` | Lowercase, `[a-z0-9_-]`, 1-64 characters. **Must equal the folder name** — one id, one folder. Used as the dependency-graph key and the `wum.storage`/`wum.config` namespace. |
| `version` | Semver `MAJOR.MINOR.PATCH[-prerelease][+build]`. |
| `name`, `authors`, `description`, `website` | Display only. |
| `melange.range` | An npm/Cargo-style semver range (`^1.2.0`, `>=0.5.0 <0.7.0`, space = AND) the running Melange version must satisfy, or the mod is `incompatible`. |
| `kind` | `client-only` (never touches the simulation, the wire, or `Data/Tweak/*`; no online handshake) or `content` (does at least one of those). `entry.sim`, `messages`, or `permissions.unsafe: true` all imply the mod counts as content for the online handshake even if `kind` says otherwise. |
| `dependencies`, `optional`, `conflicts` | Arrays of `"<id>"` or `"<id> <comparator><version>"` (e.g. `"weapon-toolkit >= 2.0.0 <3.0.0"`). `dependencies` are required and order the graph; `optional` orders the graph only when the id is present and version-compatible; `conflicts` blocks both mods, whichever side declares it. |
| `loadAfter` | Plain mod ids: an ordering-only edge with no existence requirement — nothing breaks if the named mod isn't installed. |
| `permissions.unsafe` | Deep Desert: `wum.unsafe.*` (raw memory read/write, calling game functions), client VM only. Enabling such a mod opens a consent modal in the overlay; declining still loads the mod, with `wum.unsafe` calls raising instead of working. |
| `permissions.filesystem` | `none` (default), `own-folder` (read) or `own-folder-write`, for Melange-mediated file helpers. Never raw Lua `io`. |
| `permissions.network` | Reserved for a later milestone. Accepted and ignored (with a log warning) by this build. |
| `entry.client` | Path, relative to the mod folder, loaded into the always-on Sandbox VM (Lua 5.4). |
| `entry.sim` | Path registered into the match's Lua VM at match start (Lua 5.0 dialect, float numbers). Requires `kind: "content"`. |
| `assets.root` / `.shaders` / `.effects` | Data-override folders, as M1's `Mods\<id>\` already used (defaults `assets`, `shaders`, `effects`). |
| `contentHash.include` | Glob list of files that feed the online content hash (default: `entry.sim`, `assets/**`, `*.spice.json`). Never computed for a client-only mod. |
| `messages` | Up to 16 engine message names this content mod registers (pattern `Prefix.Sub[.Sub...]`, 1-5 dotted segments after the first capitalised word). Checked against the **live** vanilla message registry at start-up, not a fixed list in the schema — a name that collides with a vanilla one is skipped and logged, not a hard error for the rest of the mod. The engine has 73 free slots; Thumper caps registrations at `[Thumper] MaxModMessages` (48) across every mod combined. |
| `settings` | `{key, type: bool\|int\|float\|string\|enum, default, min?, max?, options?, label}`. Drives `wum.config.get/set` and the per-mod widgets on the Mods page. |
| `weapons` | Weapon clones, `kind: "content"` mods only — see below and [weapons.md](weapons.md). |
| `defaultEnabled` | Honoured only the first time Thumper ever sees this mod id (default `true`). The shipped samples set it to `false`. |

## `weapons`: weapon clones

A `kind: "content"` mod can declare up to 3 weapon clones (schema: [spice-1.schema.json](spice-1.schema.json)),
in the free panel cells 29, 39 and 40 — shared across every enabled mod, not per mod. This is checked in two
passes, both at every Thumper rescan:

1. **Shape**, right here in `spice.cpp`: at most 3 entries, each an object with a known set of keys
   (`name`, `base`, `cell`, `bank`, `set`, `panelIcon`, `hudIcon`, `text`), `set` values that are a number, a
   boolean or a string. A shape error refuses the whole mod, naming the bad key or value.
2. **Names, bases, cells and field types**, in `weapons/manifest.cpp` — the base whitelist, the resource-name
   pattern, cell conflicts between mods (later load order loses), and each `set` field checked against the
   base's own container schema. A mod that fails here is `Incompatible` with the specific reason.

The full field reference, the base whitelist, the Lua side (`wum.sim.weapons`) and what does and doesn't work
yet are in [weapons.md](weapons.md); `dist\Mods\mega-bazooka` is a complete, working (disabled) example.

## `levels`: map packs

A `kind: "content"` mod can ship maps built with [Erg](erg.md), up to 32 per mod and 128 across every enabled mod:

```json
"levels": [{ "slug": "harbour", "title": "Harbour Brawl", "type": "multi", "chunk": true, "source": "src/harbour.ergpatch.json" }]
```

| Field | Notes |
|---|---|
| `slug` | `[a-z0-9]{1,24}`, unique within the mod. The level's file stem is `<prefix>_<slug>`, where `<prefix>` is the mod id with `-` turned into `_`. |
| `title` | Printable ASCII, 1-40 characters. What players see in the Prebuilt list. |
| `type` | `multi` only — other map types aren't supported yet. |
| `chunk` | `true` when the pack ships a generated script (`assets/levels/<stem>.lub`) for spawns, placed objects or a water level. |
| `source` | Path to the patch the map was built from (Erg writes this). When the built files are missing, the mod is `Incompatible` with "not built: open Erg and press Build, or run build.ps1". |

Two mods that resolve to the same prefix (their ids differ only by `-`/`_`) can't both ship maps; the later one in
load order is `Incompatible`. A map's own files never touch anything under `Data\`, are never named the same as one
of the game's own map files, and never include the shadow-cache files (`.csh`) the game itself generates.

## Resolution

Thumper resolves the whole `Mods\` folder as one pass, deterministically:

1. **Parse and validate** every folder. A manifest that fails the schema, has an unknown `spiceVersion`,
   or whose `melange.range` excludes the running build is `incompatible` and excluded from the graph
   entirely.
2. **Build edges** from `dependencies` (required), `optional` (soft, only when present and compatible)
   and `loadAfter` (ordering only).
3. **Topologically sort**, ties broken **by id, ascending** — the same mod set always resolves to the
   same order, regardless of folder creation order or scan order.
4. **Conflicts and consent.** A `conflicts` pair blocks both sides. A required dependency on a blocked
   mod blocks the dependant too ("blocked because X is blocked"). A cycle blocks every mod in it with
   one shared reason. An `unsafe` mod without a Deep Desert grant is `pending-consent`, not blocked — it
   still loads, with `wum.unsafe` raising until the user answers the prompt.

**Content-mod changes need a restart.** Message names are registered once per launch and never
unregistered, so enabling or disabling a `content`-relevant mod takes effect at the next launch (shown as
`restart-required` on the Mods page in the meantime). Client-only mods toggle live.

## State

Choices persist in `Mods\thumper-state.json` (falling back to `Documents\Melange\thumper-state.json` if
the game folder is read-only): which mods are enabled, load-order pins, and Deep Desert grants. It is
written atomically and is safe to delete — Thumper rebuilds it (re-applying `defaultEnabled` for every
mod as if freshly discovered).
