# Local content importers

Importers let plugins bring content (such as maps) from a download or a local file on the player's machine into Melange, with verification and safety checks. Melange provides the engine; the plugin supplies only a recipe and text.

## How plugins declare an importer

A plugin declares an importer in its `spice.json`:

```json
{
  "spiceVersion": 1,
  "id": "my-importer",
  "importer": { "recipe": "import.json" }
}
```

The `importer` object has exactly one key, `recipe`, a relative path inside the mod folder (no `..`) ending in `.json`, at most 64 characters. A mod with an importer may be `client-only` and have no `entry` (it registers nothing in the game). Melange versions that do not support importers ignore the `importer` key.

## Recipe format (version 1)

A recipe is a JSON file with these top-level fields:

| Field | Type | Purpose |
|---|---|---|
| `importVersion` | number | 1. Unknown keys anywhere fail validation. |
| `format` | number | The recipe format. Melange 0.3.3 supports `1`. |
| `id` | string | A stable id for this recipe: `^[a-z0-9][a-z0-9.-]{0,47}$`. One recipe per plugin. |
| `name` | string | User-facing name of what is being imported. |
| `content` | object | `{title, publisher, termsUrl, credit}`. Metadata shown to the user. |
| `sources` | array | 1 to 4 download sources. Each: `{id, name, urls: [https://...], fileName, size (bytes), sha256 (lowercase hex)}`. |
| `reader` | object | How to find content in the archive: `{type: "w4-registry", root, registry, titles?, descriptors, maps, previews?}`. Paths are relative, `/`-separated, with `*` globs only (no `..`). |
| `select` | object | Filtering rules: `{levelType, skipKeySuffix?, require?, exclude?, vanilla?, expect}`. `require` is empty or both `"descriptor"` and `"xan"`. |
| `categories` | array | 1 to 8 user-visible categories. Each: `{id, label, scriptsEqual?, scriptsWithin?, default?, hidden?}`. One must have `default: true`. |
| `groups` | array | 1 to 8 groups for filtering. Each: `{id, label}` plus exactly one of `match: [patterns]`, `vanilla: true` or `default: true`. Exactly one must be the default. |
| `modes` | object | (optional) Map mode tokens to user labels: `{"ModeToken": "Label"}`. |
| `transform` | object | How to transform content: `{stem, descriptor, timeOfDay?, title?, author?, textures, scripts, survivor?, previews?}`. |
| `output` | object | Output packs: `{packPrefix (must equal plugin id), version (semver), perPack (1-32 levels), order?, newPackBefore?, packName (holds {n}), packDescription}`. |

## Safety rules (enforced by Melange)

1. **No untrusted code.** The importer never loads, runs or writes a script, DLL, ASI, bundle or any executable. Only map data (descriptors, geometry, textures) and metadata are written.

2. **Verify before reading.** The download is verified (exact size and SHA-256) before any member is read. A local zip gets the same check.

3. **Member checks.** Before a byte is decompressed:
   - No absolute path, drive letter, `..`, backslash tricks, control characters or reserved names
   - Methods 0 (stored) and 8 (deflate) only; no encryption
   - Uncompressed size within the per-type cap (e.g., 4 MiB for map geometry)
   - Compression ratio at most 100:1
   - No executable magic bytes
   - Duplicate names (case-insensitive) fail the import

4. **Output location.** Content is written only under `Mods\<prefix>-<n>\` and the importer's own work folder. Nothing touches `Data\`, the save, or outside the game folder.

5. **Atomicity and rollback.** If any step fails, the previous import stays intact. All packs are generated into a staging folder, then moved atomically. If a move fails, all changes are rolled back.

6. **Path validation.** Every output path is built from validated components (pack id, stem, fixed sub-paths), never from zip entry names. All paths are canonicalized and checked to be under `Mods\` after full resolution.

## Determinism

For online play to work, every player who imports the same zip with the same Melange version must get byte-identical packs. Output depends only on:

- The verified zip bytes (all players have the same)
- The recipe bytes (all players have the same plugin version)
- The importer format (all players have the same Melange version)
- Pinned vanilla files (verified by hash, so identical)

The importer uses no timestamps, random values, locale-specific sorting (ASCII case folding only), or hash-map iteration order. JSON is written by a fixed writer. If the recipe needs to change the output, it must bump its `output.version` (a semver string).

## How the import works

1. **Acquire:** Download from the recipe's URLs or copy from a local file, hashing while streaming/copying. Size or SHA-256 mismatch fails the import. A cached zip is re-verified before every use.

2. **Open and list:** Read the archive's central directory; apply member checks to allowlisted members only. Members outside the allowlist are never decompressed.

3. **Read metadata:** Registry (if present), language banks and descriptors into memory, respecting size caps.

4. **Select:** Filter entries by the recipe's rules (type, key suffix, requirements, exclusions). Pinned vanilla files are read from the player's game folder and verified.

5. **Classify:** Assign each entry to a category based on its attributes. Assign it to a group if the recipe defines groups.

6. **Order and pack:** Sort entries by recipe order. Fill packs of `perPack` levels; start a new pack before entries in `newPackBefore` categories.

7. **Name:** Slugify file names; if longer than `slugMax`, keep the first `hashKeep` characters plus the first `hashHex` hex digits of SHA-256(name). Generate stems with the plugin's id as prefix.

8. **Transform:** Copy or rebuild map data, renaming stems, rewriting references, extracting titles from language banks, converting previews (e.g., TGA → PNG).

9. **Manifest:** Generate `spice.json` for each pack with `generated: {by, recipe, format}` (so Melange knows not to modify them).

10. **Catalogue:** Build local metadata for the UI (per-map title, author, group, category, preview).

11. **Place:** Move all packs from staging into `Mods\` atomically. Update the importer's state.

## Showing and hiding

Maps can be hidden from the picker and random pool without changing the content set (and thus online matching):

- A local file `<game>\Melange\hidden-levels.txt` lists stems, one per line.
- The game's level gate filters out hidden maps when building the landscape picker and random pool.
- Hiding a map does not prevent a host from picking it online; all peers still load it.

Default visibility is set by the recipe's `categories[].hidden` flag.

## UI disclosure

The importer always shows the player:

- Where the content comes from (publisher and URL)
- Whose terms apply
- That content stays on their PC
- That Melange is not affiliated with the content's authors
- What files are included and what are not

The player must check a box before the import runs.

## RPC interface

See [oasis.md](oasis.md) for the full `import.*` method reference. Key methods:

- `import.list` → all installed importers
- `import.status` → status of one importer (none, imported, stale, damaged, unsupported, busy)
- `import.start` → start an import (download or local file)
- `import.maps` → maps and packs from the current import
- `import.setHidden` → show/hide maps
- `import.setPacks` → enable/disable packs
- `import.uninstall` → remove all imported packs

Progress is published on the `import` channel with job phases: `downloading`, `copying`, `verifying`, `reading`, `building`, `placing`, `done`, `error`, `cancelled`.

## Example: Caravan

Caravan imports all 174 maps from Renewation HD 0.2A2 into a player's game folder. Its recipe:

- Pins the official zip from mod.worms.pro by SHA-256
- Reads Worms W4-format registry, descriptors and language banks
- Selects and categorizes maps (plays as designed, deathmatch only, mode not supported)
- Renames stems to `caravan_*` and rebuilds descriptors
- Splits maps into up to 9 generated packs (Melange limit)
- Generates `spice.json` with range `>=0.3.3 <0.4.0`

Players see:
- A disclosure listing mod.worms.pro's terms
- Download progress with cancellation
- A map browser filtered by source, category and visibility
- Per-pack enable/disable (changing online matching)
- Per-map show/hide (filtering locally only)
- An import fingerprint for online matching

Removing Caravan uninstalls all imported packs. The cached zip is deleted only when "Also delete its settings and saved data" is ticked.
