# Erg: the map editor

Erg is Oasis's level editor: it loads one of the game's own maps or a map you're already building, lets you move
spawns and a handful of objects, set water and theme, and export the result as a mod. It never touches your game
files directly — everything happens through the local server (`melange.asi` with the game running, or `oasis.exe`
with it closed), which is the only thing that reads `Data\` and writes `Mods\`.

## Opening it

Open Oasis (see [oasis.md](oasis.md)) and pick the *Erg* panel, either with the game running or from `oasis.exe`
with it closed. Editing and building a pack work either way; **Test** needs the game running, at the main menu, and
not in a lobby.

## Projects

A project is one map you're editing: a base level plus your changes, saved under
`Documents\Melange\erg\projects\<id>\`. Opening the panel lists the game's own multiplayer maps and any project you
already started; pick one to start a new project from it, or an existing project to keep going. **Save** (Ctrl+S)
writes the project atomically; until then your edits and the undo history live in the page, and unsaved edits come
back as a draft if you reload it. Only one server (the game or `oasis.exe`, not both) can have a given project open at
a time. Map packs that ship only their patches (see Export) are listed at the top with a **Build** button.

## The view and tools

- **Camera:** orbit, pan and zoom with the mouse.
- **Selecting and moving:** click a spawn, object or scenery piece to select it; drag its gizmo to move, rotate or
  scale it, with snapping (1, 0.5 or 0.1 map units, and 15° steps).
- **Outliner and properties:** a tree of everything in the map on one side, and the selected thing's fields (name,
  position, rotation, scale) on the other.
- **Units:** the editor works in world units, which are 20× the numbers stored in the level file. A worm 1 map unit
  from a wall in the file is 20 world units away on screen — the same units the game's own camera and physics use.
- **Placing:** pick *Spawn knot*, *Oil drum* or *Mine* in the toolbar, then click the terrain. *Drop* (G) puts the
  selection on the ground below it; *Duplicate* (Ctrl+D) copies spawns and objects (not scenery).
- **Undo and redo:** Ctrl+Z and Ctrl+Shift+Z, up to 500 steps.
- The 3D view is an approximation of the game's own geometry, close enough to place things accurately but not a
  pixel-identical render of the game.

## Spawns

A map's spawn mode is either:

- **Random** (the default): worms land wherever the game's own random spawn logic puts them, same as an unedited
  map.
- **Knots:** place up to 8 numbered markers (`WORM0`..`WORM7`); worm *i* always starts on marker *i*. Every match
  needs all 8, even in smaller games — the map simply won't use the markers past the worm count.

## Objects

Erg can place **mines** and **oil drums**, which work in any match on any map that uses them, and **crates**,
**telepad pairs**, **triggers** and one **mine factory**, whose settings are on the Properties tab.

## Water

Set a water level or leave it at the map's own default. The number is in world units, with sea level at 0; on Diner
Might, 150 floods nearly everything.

## Theme and time of day

Changing the theme swaps the terrain's material and texture set (for example, Camelot's stone-and-banner look) and
loads that theme's own height-map textures; it doesn't rewrite the terrain itself. Quick Game always plays your map
in daytime regardless of the time-of-day setting you pick — that only affects how the map looks in other places the
game shows it.

## Terrain

Press **Sculpt**, then drag over the terrain to carve, fill or paint with a box or sphere brush (the *Terrain* tab sets
the mode, shape, size and material; `[` and `]` resize the brush, and Alt-drag still orbits). Fill with *Match
column* takes the material of the terrain above. Erg doesn't add new terrain shapes or resize what's there — it only
edits what the base map already has, and a brush reaches every piece of terrain it overlaps.

## Surround

The *Surround* setting keeps the base map's far-off scenery ring (*copy*), removes it (*none*) or flattens it
(*flat*).

*Painted* lets you reshape it: the **Terrain** tab shows the surround as a 100×100 top-down grid, starting from the
base map's heights. Drag on it with *Raise*, *Lower*, *Flatten* (shift-click a cell to pick its height) or *Smooth*.
Heights run from 0 to 1 and are relative; test the map to see them in the game. A changed surround also rebuilds
the level's shadow cache.

## Testing your map

Press **Test** to play the map as it stands, without exporting or restarting anything:

1. Erg builds your changes into a private, offline-only copy of the map (never your `Mods` folder).
2. It registers that copy for one session and arms a one-time override so the very next match loads it.
3. If your game build supports starting a match from the editor directly, Test starts Quick Game itself. Otherwise
   the status line asks you to press Quick Game yourself — the override is already armed, so that press loads your
   map.

A Test map never appears to anyone else and never starts in an online match; it's for checking your own work before
you export. Testing again after another edit simply overwrites the same private copy.

## Level scripts

The **Script** tab holds the map's own Lua, saved in the project as `script.lua`. It runs in the match's sandbox,
the same one a content mod's `entry.sim` gets (see *Sim scripts* in [developer-guide.md](developer-guide.md#sim-scripts)),
after every mod's sim script and only when this map is the one being played. **Save script** (Ctrl+S) checks the text
first and marks the line of any problem; Test saves it and runs the saved text, so an edit followed by another Test
needs no restart. Export ships it as `sim/<slug>.lua` and names it in the level's `levels[].sim` in `spice.json`.

```lua
wum.level.trigger("GOAL", { radius = 80 })
wum.events.on("Trigger.Collected", function()
  wum.sim.setData("Water.Level", 30)
end)
wum.events.on("sim.turnStarted", function(name, turn)
  if math.mod(turn, 5) == 0 and wum.level.knots["CRATE1"] then
    wum.level.crate("CRATE1", { kind = "health", amount = 50 })
  end
end)
```

| Name | Purpose |
|---|---|
| `wum.level.stem`, `wum.level.key` | This map's file stem and registry key (`Multi.<stem>`) |
| `wum.level.knots` | Read-only table of the map's named knots, name → kind (no positions) |
| `wum.level.trigger(knot [, opts])` | A trigger at a knot; `opts`: `index`, `radius`, `teamCollect`, `teamDestroy`, `hitpoints`, `wormCollect`. `true`, or `nil` and a reason |
| `wum.level.crate(knot [, opts])` | A crate at a knot; `opts`: `kind` (`weapon`, `health`, `utility`), `contents`, `count`, `amount`, `hitpoints`, `parachute` |
| `wum.events.on("sim.turnStarted", fn)` | `fn(name, turn)` at the first tick of each turn, on every machine at the same tick |

Everything else (`wum.events`, `wum.sim.*`, `wum.log`) is the sim script API. In logs and in Wormsign the script is
named `<mod id>:<slug>` (`erg:<project>` while testing).

**Sandbox limits.** No engine globals (`SendMessage` and friends are `nil`), only the messages the sim allows
(`GameLogic.PauseGame` and the like are refused), and an instruction budget per call (`[SimBridge] InstrPerCall`): a
callback that errors or runs out of budget three times is switched off, and the match goes on. A script is at most
256 KB of UTF-8 text with no byte order mark; compiled Lua is refused.

**Determinism.** Every machine in a match runs the script and must reach the same result, so:

- use `wum.sim.random` (`math.random` is the same stream), never the clock or anything local to one machine;
- numbers are floats (exact integers only up to 2^24), and it's Lua 5.0: `table.getn(t)` not `#t`, `math.mod` not `%`;
- a closure inside a loop sees the loop variable's last value, so copy it into a `local` first;
- keep state in globals or `wum.sim.storage` (Wormsign hashes those) or pass it to `wum.sim.hash`.

Online, everyone must have the same pack version, which covers the script's bytes, and a replay refuses to play if
the level script has changed since it was recorded.

## Export

Exporting turns a project into a mod under `Mods\`, in one of two forms:

- **Source** (recommended for sharing): just your edits — a small JSON file plus a build script. Whoever receives it
  runs the build script against their own copy of the game to produce the actual map files. Nothing of the game
  itself travels in this form, so it's the right way to share a map with someone else.
- **Install** (for your own machine only): the full, ready-to-play map files, generated from your edits against your
  own install. This form contains modified copies of game files and should stay on your computer — don't redistribute
  it.

Either way you give the export a mod id, a display name and a version; exporting into a mod id you've already used
adds the new map alongside any others already in it (or updates it, if you export the same map again). A newly
exported mod needs a restart to take effect, the same as any other content mod — the dialog tells you when that's
needed.

If you choose Source, the recipient enables the mod and either presses *Build* in Erg or runs its `build.ps1`, which
needs nothing but their own game install; either produces byte-identical files to what Install would have written
on your machine.

## Playing an exported map

An exported map appears under *Prebuilt* in Versus, exactly like one of the game's own maps, once the mod that ships
it is enabled and the game has restarted.

**Online rules:** a map you made plays online only when everyone in the lobby has the exact same version of the mod
that ships it — Melange checks this automatically. If anyone doesn't match, the host's selection of that map is held
with a banner naming who's missing it, and maps you've only Tested never start online at all. A vanilla player simply
can't pick your map to begin with.

## Limits

- Up to 32 maps in one mod, 128 across every enabled mod at once.
- A map title is 1-40 plain-ASCII characters.
- A patch (your saved edits) is capped at 20 000 operations and 4 MB — enough for any hand-made edit; if you hit
  this, split the changes into more than one exported map.
- Not yet supported: per-team or story spawn points, survivor/story/challenge
  map types, new terrain shapes (only carving, filling and painting the terrain the base map already has), editing
  the map's generated chunk (Erg rewrites it every export; use a [level script](#level-scripts)), and enabling a
  freshly exported mod that has level scripts without a restart.
- A replay recorded on a Test map only plays back correctly while your Test workspace still has the same files —
  moving on to a different edit, or exporting for real, can make an older Test recording unplayable.

## Troubleshooting

- **A map you enabled doesn't show up:** it needs a restart after being enabled, like any content mod; check the
  Mods page for "restart required".
- **"Not built"** on a Source-form mod: press *Build* in Erg, or run the mod's `build.ps1`, then restart.
- **Test is greyed out:** the game needs to be running, sitting at the main menu, and not in a lobby.
- **A map is missing online, or a teammate's start is held:** everyone needs the exact same version of the mod; check
  who's missing it in the lobby panel.
- **Something looks wrong in the 3D view but fine in-game (or the reverse):** the editor's view is an approximation;
  trust Test (which runs the real game) over the preview for anything that matters.
- For anything else, the game's log has an `[erg]`/`levels` line naming what happened at each step (registration,
  test starts, exports) — attach it if you report a problem (see the main [README](../README.md#reporting-a-bug)).
