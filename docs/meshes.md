# Mod meshes

Status: **shipping as the manifest field [`meshes`](spice.md#meshes-custom-3d-models); the core chain is proven in
game.** This page records how the engine loads 3D meshes from `Data/Bundles/BundlNN.xom`, why an `xomtool`-built mesh
bank could not be loaded before, the `Meshes` module that loads one now, how to try it, and what is still unverified.
Confidence is stated per claim: **[V]** read from the decompiled code and consistent with the data files; **[G]**
verified in game; **[P]** implemented on the reading above, not yet run in game.

Verified in game [G] (build #1077, one run): `mesh.load Mods/kindjal/assets/data/kindjal.NailBat.xom kindjal` at the
main menu loaded the bank as section 480; `mesh.resolve kindjal.NailBat` then showed section 480, bin 8, **loaded 1**
and a graph set; and a sim script's `wum.sim.weapon("kWeaponBaseballBat"):set("WeaponGraphicsResourceID",
"kindjal.NailBat")` made the worm hold the mod's bat mesh in a match. Everything the section below calls **[P]**
and the manifest-driven load (below) is that same chain driven by the manifest instead of a test verb; the new parts
(automatic load, section relocation) have been compiled and unit-tested only.

Everything below is build #1077 (`WormsMayhem.exe`, Steam/GOG). Decompiles referred to are
`re/out/decomp/decomp_<chunk>.c` in the private research checkout, one file per 64 KiB of code
(`FUN_006ae6a0` is in `decomp_6a0000.c`, `FUN_006b2550` in `decomp_6b0000.c`, `FUN_004d4220` in `decomp_4d0000.c`).

## What the engine does

### The graphical resource manager [V]

Meshes, bitmaps, sprite sets, fonts and "custom" resources are owned by one object, `XGraphicalResourceManager`
(vtable `0x88948c`, constructor `0x6af930`, singleton reached through `XomGetApp()->QueryInterface(0x888e8c)`,
helper `0x6acae0`). This is **not** the data resource manager that `weapons/engine.cpp` talks to (`0x50b8b0` /
`0x50b760` look up *data* resources - weapon containers, strings - through the DRM IID `0x888288`); mesh names on
a weapon container are resolved through the GRM.

The GRM keeps every resource in an open-addressed hash table of 7500 slots (`this+0x18`, name hash `0x6ad050`,
linear probing; lookup `FindResourceIndex` `0x6adc60` with a 256-entry pointer cache at `this+0x7548`). Each slot
holds a descriptor object; for a mesh that is an `XMeshDescriptor` (vtable `0x8897d4`), laid out as:

| Offset | Field | Set by |
|---|---|---|
| `+0x14` | `XString` name (`ResourceId`) | startup record, or the bundle |
| `+0x18` | `u16` SectionId | startup record, or the bundle |
| `+0x1a` | `u8` scene bin (which of the 88 "Scene Bin %d" scenes an instance renders in; weapons use 8) | startup record only |
| `+0x1b` | bit 0 = graph present ("loaded") | adopt step |
| `+0x20` | `const char*` source file name (`"BaseballBat.xom"`, used when the game runs without bundles) | startup record only |
| `+0x24` | `u16` Flags (8 for weapon meshes) | startup record, or the bundle |
| `+0x28` | `XGraphSet*` the mesh graph | the bundle |

### Startup: stubs from a table compiled into the exe [V]

`AppDataService::RegisterResources` (`0x4d4220`) calls GRM vtable slot 10 (`AddMeshDescriptors`, `0x6ad510`,
`stdcall(this, records, count)`) with a static table at `0x910f00` of **1101 twenty-byte records**, one per
vanilla mesh (`W4.Worm`, ..., `Bazooka.Weapon` #26, `BaseballBat` #82), plus a second table at `0x9480f8` for
frontend meshes. Record layout, read by `XMeshDescriptor::InitFromRecord` (`0x6b2550`):

```
+0x00 const char* name          "BaseballBat"
+0x04 u16  sectionId            9  (kSectionIngame -> Bundl09.xom)
+0x06 u8   sceneBin             8
+0x08 const char* sourceFile    "BaseballBat.xom"
+0x0c u16  flags                8
+0x10 u32  (0 for every mesh)
```

Each record becomes a **stub** descriptor (no graph) inserted by name (`AddResource` `0x6ad270`; a duplicate name
only logs "The specified resource name has already been used!"). Bitmaps (slot 11, `0x6ad450`), sprite sets (slot
9, `0x6ad5b0`), fonts (slot 8) and custom resources (slot 7) get the same treatment from their own tables. This is
the "write side" of the resource store that `m5-assets-research.md` §4.3 asked for (U-ASSET-3): **names are not
discovered from the bundle files; they are compiled in.**

The same function then loops sections 0..474 (`kSectionCount` = 475, the enum's names are in the string table at
`0x90cc00`: 0 `kSectionLoading`, 9 `kSectionIngame`, 10 `kSectionPermanent`, 126..166 `kSectionCustom01..41`,
472..474 team/fort health and `PermanentPart2`) and marks every one "must be explicitly loaded" (slot 21,
byte array `0x96ef58`) with auto-unload off (slot 18, `0x96f160`).

### On demand: a section's bundle is loaded when a resource in it is first created [V]

`CreateResource` (slot 35, `0x6aeac0`): find the descriptor by name; if it has no graph, log
"Auto loading section N" and call `LoadSection` (slot 13, `0x6aefd0`, `stdcall(this, u16* section)`); then
`descriptor->Create(...)` (slot 12, `0x6b2c70`), which takes one of the fixed pool of mesh instances
(`0x970008`, 0xd0 bytes each, "Out of mesh instances" when empty). So **bundles are loaded by SectionId on first
use, not enumerated and not from a fixed list of 475 names** - the 475 files simply exist because every section
has one (even the empty ones: `Bundl01.xom` is 178 bytes, a bare `XGraphSet`).

`LoadSection` refuses a section whose flag in `0x96f368` (byte[520]) is already set ("Attempting to load section
N but it is already loaded"), sets it, and calls the bundle loader `0x6ae6a0`:

1. `sprintf(GRM+0x7dd0, section)` - `+0x7dd0` is an `XString` holding the file-name format. The constructor sets
   `"Bundles/"` / `"Bundl%03d.xom"`; `AppInit` (`0x4d7aa0`) then calls `SetBundlePath` (slot 24, `0x6acd00`,
   `stdcall(this, dir, fmt, flag)`) with `"Bundles/"`, **`"Bundl%.2d.xom"`** (hence `Bundl09.xom` and
   `Bundl100.xom`), guarded by the "use bundles" bit (`[0x95a100+0x94] & 4`; without it the game asserts "Must
   use bundles for final version").
2. `XomLoadObject` (`0x63e107`): opens the name through the engine file system (`0x63aa1e`, the same
   search-path open that `LoadBank` `0x6a37a0` uses, so a game-relative path like `Mods/x/y.xom` works), reads
   the root object, and `QueryInterface`s it to `0x98c0f0` = **`XGraphSet`**. (A data bank's root is an
   `XDataBank`; that is the only difference between the two file kinds - no header flag.)
3. For each `Graphs[i]` of the root: `QueryInterface(0x98c310)` (resource descriptor), `GetName`,
   `FindResourceIndex`. **An unknown name is an error**: "Bundle contains unrecognised resource: <name>",
   result `0x80004005`. For a known name, the stub in the slot must have the same SectionId as the one being
   loaded, the same class, no graph yet, and no instances - otherwise ") from bundle (" is logged with the reason
   bits and the entry is skipped.
4. Otherwise the loaded descriptor **adopts** the stub's scene bin, source file and `+0x2c`
   (`XMeshDescriptor::AdoptFrom` `0x6b3500`), fetches its geometry graph from its `XGraphSet` by the GUID
   `6ae6dbe4fa866b45a73ff9130e12dfeb` (the IID at `0x98c100`; in every vanilla bundle that is the entry named
   `"world"`), sets bit 0 of `+0x1b`, and **replaces the stub in the hash slot**. Registration is therefore a
   side effect of loading *only for names the table already holds*.

Consequences for a mod:

- Dropping a `BundlNNN.xom` into a mod root can never add a mesh: its names would be "unrecognised".
- Overwriting a vanilla bundle wholesale works (bundles are not CRC-checked) but is the blunt instrument the
  research already rejected.
- The GRM's public methods are enough to do what startup does for a mod: **register stubs, then load the file as
  the bundle of a section of the mod's own.** No hook, no patch.

### Free sections [V]

Every per-section array is 520 entries (`0x96ef58`, `0x96f160`, `0x96f368` bytes; `0x96f570` ints; the auto-unload
pass `0x6acbb0` and the "already loaded" checks all iterate or index up to `0x208`), while the vanilla enum ends at
`kSectionCount` = 475, which the game also uses as a "no section" sentinel (`0x1db` stored as a "current section" default in `0x4d5730` and `0x4d7ca0`).
Sections **476..519** are therefore addressable, never named, never loaded by the game and never auto-unloaded
(their auto-unload byte is 0): the module reserves them for mods, one section per bank.

## The `Meshes` module [G for the engine calls, P for the manifest load]

### Loading from the manifest

A content mod lists banks in `spice.json` (`"meshes": [{ "file": "assets/meshes/kindjal.NailBat.xom" }]`, at most 64,
under the assets root, see [spice.md](spice.md#meshes-custom-3d-models)). The files are under `assets/**`, which
`WalkModFileList` in `src/mods/handshake.cpp` hashes into the mod's content hash, so peers must hold identical banks.

`src/assets/meshes_module.cpp` loads them once per launch from the `Frame` event, on the main thread: once the frontend
has been up for 30 frames, the app is ready (`weapons::engine::AppReady`) and the GRM answers (`Available()`). Thumper
has resolved the mod list long before (at startup), and the GRM exists from app init; the proven manual call was at the
same point. It goes through every Thumper entry that is active this session (`sessionActive`) and of `kind: "content"`,
sorted by load order, and calls `LoadModBank(..., relocate = true)` for each `meshes` entry. A failing bank logs
`[meshes] <mod>/<file>: <reason>` and is skipped (the mod is not refused); a launch logs one summary line
(`[meshes] N bank(s) loaded from M mod(s), K mesh(es), sections ...`). A section is burnt by one attempt, and nothing is
ever unloaded, so a mod disabled later keeps its banks and sections for the session; content-mod changes need a
restart anyway.

Sections: a bank keeps the `SectionId` it was built with (`xomtool ... --section N`) when that is in 476..519 and the
engine has not loaded it. Otherwise (outside the range, or taken by an earlier bank or a failed attempt) `RelocateBank`
rewrites that one field in a copy at `Melange\cache\meshes\<modId>.<section>.xom` and the copy is loaded as the first
free section from 476 up. Two mods that both built their bank as section 480 therefore both load.

### The section budget

One bank takes one section for the whole session, and a section is never given back, so the budget is shared by every
mod and is spent in load order:

| | |
|---|---|
| Sections the engine can address for mods | 476..519 = **44**. The per-section arrays have 520 entries (`0x96ef58`, `0x96f160`, `0x96f368`), 475 is the engine's "no section" sentinel, 0..474 are vanilla. |
| `meshes` entries one mod may declare | **64** (a parse limit, `spice::kMaxMeshes`). More than 44 is allowed on purpose: a mod that lists spare banks still parses, and the budget is spent only by banks that actually load. |
| Banks that load across all mods | at most 44, first come in load order (mod order, then array order). A bank that fails before the engine call (unreadable, a name outside `<modId>.`) burns nothing; one that reaches `LoadSection` burns its section even if the engine then refuses it. |
| Many meshes in one bank | A bank may hold any number of `XMeshDescriptor`s as long as they share one `SectionId`, so a mod that needs more than its share of 44 puts several meshes in a bank (`xomtool convert ... --bundle` writes one mesh per file today; merging banks is a separate change). |

When the budget runs short the load says so before it starts and again per bank, instead of 20 identical failures:

```
[meshes] 52 bank(s) declared by 3 mod(s), but only 44 of the 44 mod sections are free: the last 8 in load order will not load
[meshes] bigmod/assets/meshes/m45.xom: not loaded, all 44 mod sections (476..519) are in use
[meshes] 44 bank(s) loaded from 3 mod(s), 44 mesh(es), sections 480, 476, ..., 8 not loaded (section budget exhausted)
```

(`mesh.state` shows the sections left.) A mod whose bank did not load keeps working: its `set` and `vehicleMeshes` entries
that name a missing mesh are skipped for that match with a `[weapons] ... not loaded` line and the vanilla mesh is drawn.

`src/assets/meshbank.{h,cpp}` (engine side; `FreeSections()` counts what is left of the 44), `src/assets/meshbank_inspect.cpp` (pure parsing, offline-testable),
`src/assets/meshes_module.cpp` (the module and test verbs). `[Meshes] Enabled=1`, `RequiresKnownBuild` true.

```cpp
namespace melange::assets::meshes {
bool LoadModBank(const std::wstring& absPath, const std::string& modId, std::string* err, uint8_t sceneBin = 8);
bool Resolves(const char* resourceName);               // GRM FindResource (slot 36, 0x6aec90) != 0
bool Describe(const char* resourceName, Info* out);    // + section / scene bin / loaded bit / graph set
bool InspectBank(bytes, &entries, &section, &err);     // pure: root XGraphSet -> XMeshDescriptors, one SectionId
bool CheckEntries(modId, entries, section, &err);      // "<modId>." prefix rule, section in 476..519
}
```

`LoadModBank` does, in order, refusing (false, `*err`, nothing touched) at the first failure:

1. Site check: known build, first bytes of `0x639b1d`, `0x6ad510`, `0x6aefd0`, `0x6aec90`, `0x6ae6a0`,
   `0x6adc60`, `0x6b2550` intact; the GRM's vtable is `0x88948c` and slots 10/13/36 are those functions.
2. Reads and parses the file with `src/xom` (64 MiB cap): root must be an `XGraphSet` of `XMeshDescriptor`s that
   all carry one `SectionId`; every `ResourceId` must start with `<modId>.`; the section must be in 476..519 and
   not yet loaded (`0x96f368[section] == 0`); no name may already resolve in the GRM (the same "already live"
   rule `assets/banks` applies to data banks).
3. **Stubs:** one 20-byte record per descriptor (`name`, `section`, scene bin 8 by default, `sourceFile =
   "<name>.xom"`, the bank's `Flags`), strings kept alive for the process, `AddMeshDescriptors(records, n)`;
   each name is then re-resolved.
4. **Bundle:** the GRM's format `XString` at `+0x7dd0` is read (`"Bundl%.2d.xom"`), replaced with the bank path
   (game-relative when the file is under the game folder, e.g. `Mods/kindjal/assets/data/kindjal.NailBat.xom`;
   `'%'` in a path is refused since the engine `sprintf`s it), `LoadSection(&section)` is called, and the format
   string is restored whatever the result. `Describe` must then report a loaded graph for every entry.

Every engine call is behind `__try`; a failure logs `[meshes] ...` and returns false. Stubs that were registered
stay (there is no removal API): a weapon naming one of them after a failed load fails to create its mesh ("Failed
to load mesh from stream" in the game's own log) rather than crashing, as far as the code reads - **not
verified**. A section is burnt by one attempt (the engine sets its loaded byte even on failure): rebuild the bank
with another `--section` to retry in the same session.

Test verbs (`melange/testcmd.h`, run through the automation driver or the Lua console's test-command path):

| Verb | Does |
|---|---|
| `mesh.state` | enabled / sites verified / banks and stubs this session / the mod section range |
| `mesh.resolve <name>` | `Describe`: descriptor address, section, scene bin, loaded bit, graph set, source file |
| `mesh.load <bank.xom> [modId] [sceneBin]` | `LoadModBank`; a path with spaces is quoted; `modId` defaults to the file name's first dot-part |

## Engine-picked meshes: the Airstrike and Super Airstrike helicopters

The Airstrike (`kWeaponAirstrike`, `IsBomberWeapon`) and Super Airstrike (`kWeaponSuperAirstrike`, `IsControlledBomber`)
containers name only the radio (`Radio`) and the bomb (`Airstrike.Payload`, `Cow.Payload`); the aircraft that flies over is
not a container field. This section records where the engine picks its mesh and the hook built on it. Everything here is
**[V]** (read from the decompile and the `.data` bytes of `WormsMayhem.exe` #1077) unless marked otherwise.

### What the engine does

- **`Bomber.Mesh` in WEAPTWK is not a mesh index.** `BomberLogicEntity::Initialize` (`0x54d720`) writes the data resource
  `Bomber.Mesh` as a literal `0`, builds its `BomberGraphicEntity`, and then reads `Bomber.Mesh` back and calls it as an
  object (`GetNode(…, "perspShape")` on it); the last thing `BomberGraphicEntity::Setup` (`0x54cab0`) does is store the mesh
  instance it created under that name. So it is a pointer channel from the graphic entity to the logic entity that happens to
  have a tweak slot, not a selector. Nothing reads the WEAPTWK value as a choice.
- **The mesh name is a static `const char*` in `.data`, and `Bomber` is not used.** Each graphic entity's `Setup` creates its
  mesh with `push <&name>; call 0x6f3d95` (the wrapper that calls GRM `CreateResource`, slot 35 `0x6aeac0`, whose second
  argument is a pointer to a `char*`: the first thing it does is `FindResourceIndex(*param_2)`):

  | Vehicle (manifest key) | Entity, `Setup` | `push` of the variable | Variable (`.data`) | Vanilla value | Stub record |
  |---|---|---|---|---|---|
  | `BomberHelicopter` | `BomberGraphicEntity`, `0x54cab0` | `0x54cb40`: `68 88 f3 91 00 e8 4b 72 1a 00` | `0x91f388` | `0x850938` "BomberHelicopter" | `0x9113d8`: section 9, bin 8, flags 8, `AirStrike.xom` |
  | `SuperAirstrike` | `SuperBomberGraphicEntity`, `0x5897e0` | `0x589870`: `68 28 fc 91 00 e8 1b a5 16 00` | `0x91fc28` | `0x850cf0` "SuperAirstrike" | `0x91116c`: section 9, bin 8, flags 8, `SuperAirstrike.xom` |

  Each variable has exactly one code reference (a scan of the whole image for the 4-byte address finds the `push` and
  nothing else), and each sits in a run of sibling variables (`bombrun_start`, `trail1`, `trail2`, `rear_rotor`, `top_rotor`,
  `Chopper`) that the same `Setup` and the logic entity read for node and animation names.
- **The `Bomber` mesh (2731 triangles, 512x512 texture, `Airstrike.xom`) is never created.** Its string at `0x850984` is
  referenced only by its own stub record (`0x9113b0`); no code loads it. In the bundle it is the same helicopter as
  `BomberHelicopter` (same node tree and the same `CULLEDAirstrike` clip library). The plane the task brief called "Bomber" is
  therefore a helicopter, and the Airstrike's aircraft is the `BomberHelicopter` mesh. Three more references to the
  `SuperAirstrike` string (`0x92f7f0`, `0x92ffd0`, `0x933cc8`) are `{name, id, vtable}` class-registration records, not mesh
  lookups.
- **What a replacement mesh must keep.** The hierarchy of all three vanilla meshes is
  `helicopter` > `Chopper` > { `ChopperShape`, `rear_rotor`, `Bombbaydoor_left`, `bombbaydoor_right`, `locator1` > `top_rotor`,
  `trail1`, `trail2` }, plus a `persp` camera group beside `Chopper`. `Setup` looks up `rear_rotor` and `top_rotor` (the mesh
  instance's node lookup, an `HRESULT` assert on failure) and `trail1` / `trail2` (the smoke-trail locators); the logic entity
  finds the camera as `perspShape`;
  the `bombrun_start*` / `bombrun_end*` clips of `CULLEDAirstrike` animate nodes by name. A mod mesh should therefore be the
  vanilla mesh with edited geometry and textures, node names and transforms untouched
  (`xomtool convert BomberHelicopter --from Bundl09.xom --out chopper.gltf`, edit, `convert ... --bundle`).
  Both stub records say scene bin 8, the loader's default, so no bin override is needed.
  The loader checks this offline: when it loads a bank it records, per mesh, which of `rear_rotor`, `top_rotor`, `trail1`,
  `trail2` and `perspShape` no object in the mesh's graph-set closure carries as a `Name` (`meshes::MissingNodes`), and a
  `vehicleMeshes` rule whose mesh lacks any is not armed (the vanilla vehicle is drawn, with a log line naming the nodes).
  That checks names only, not that the animation clips still bind to them, which is still unverified in a match.

### The hook: `vehicleMeshes`

```json
"meshes": [{ "file": "assets/meshes/kindjal.Chopper.xom" }],
"vehicleMeshes": { "BomberHelicopter": "kindjal.Chopper", "SuperAirstrike": "kindjal.Chopper" }
```

Keys are exactly the two vehicles above (`Bomber` is refused with the reason; anything else is an unknown key). Values must
be `<modId>.<Name>` (letters, digits, `.`, `_`, `-`, at most 96 characters), and the mod must list at least one `meshes`
bank: a pure manifest parse cannot open the banks, so "the bank really holds that mesh and it loaded" is checked when the
match starts (the mesh must resolve in the GRM with its graph loaded; otherwise that vehicle stays vanilla and the log says
so). Two mods setting the same vehicle: the later one in load order is refused as a whole, like `weaponIcons`.

How it is applied (`weapons::engine::SetVehicleMesh`, called by the clone registry; **no code is patched and no hook is
created**): when the registry goes live for a match (the same gate as clones, `weaponText` and `weaponIcons`, so every peer
has the same rules), for each rule whose mesh is loaded it checks the build, that the `push` site still has the bytes in the
table above, and that the variable still holds the vanilla pointer to the vanilla name, then writes the address of the mod's
mesh name (a string kept alive for the process) into the variable. `Setup` then asks the GRM for the mod's mesh instead.
At the match end (and when a new match starts over an unclosed one) the vanilla pointer is written back. Any mismatch, or an
unknown build, leaves the vanilla vehicle and logs `[weapons] vehicleMeshes <vehicle> (<mod>): ... not used (...)`.

The rules travel in the content identity as `vehicle <key> mesh="<name>"` lines beside the `text` and `icon` lines (the
banks themselves are already hashed as files under `assets/**`), and count as weapon content for the lobby gate: a peer
with a different vehicle mesh is held, exactly as for a different icon.

Verified offline: the manifest rules, the cross-mod assignment, the registry lifecycle against a fake engine, the identity
lines. **Not run in the game (unverified):** that `CreateResource` accepts a mod-section mesh at the helicopter's scene bin
and that nothing else caches the vanilla mesh; that the helicopter's animations and rotor nodes bind on a re-exported mesh;
that a `.data` write to the two variables is not reverted by another writer (the scan found none); the Super Airstrike
path in a real match; behaviour when the mesh is missing a required node (the assert path was not exercised).

## Building a bank with xomtool

`xomtool convert <mesh> --into` writes the mesh and its `XMeshDescriptor` but (a) needs an existing file to write
into, (b) leaves the file's root where it was and (c) writes the `"world"` graph entry with a zero GUID, while the
engine finds the geometry graph by GUID `6ae6dbe4fa866b45a73ff9130e12dfeb`. Until `xomtool` grows a
`--bundle`/`-o new.xom` form that does this itself, a loadable bank is the `convert` output plus one JSON edit.
This is exactly how the test asset was made (paths relative to a scratch folder; the game copy is
`C:\Users\Jamin\Desktop\WUMFix\testenv\A\Data`):

```
# 1. the vanilla mesh as glTF (BaseballBat.gltf + BaseballBat.bin)
xomtool convert BaseballBat --from Data/Bundles/Bundl09.xom --out BaseballBat.gltf

# 2. its texture. NOTE: ten XImages in Bundl09 are named "maya:file11/-1" and `convert <Name> --from` takes the
#    first, which is not the bat's. Walk the graph instead: descriptor #102 -> XGraphSet #646 -> ... -> XShape ->
#    XSimpleShader #6263 "baseballbat_shader" -> XOglTextureMap #6964 -> XImage #6819 (64x64 RGB8, rows bottom-up).
xomtool unpack Data/Bundles/Bundl09.xom -o bundl09.json
python extract_tex.py 6819            # writes bat_orig.png (level 0) and nailbat_rust.png (darker, rust-red tint
                                      # keeping the grain: R = lum*150+25, G = lum*55+8, B = lum*35+6)

# 3. a seed file to write into (any small XOM; a one-entry data bank is handy), then the mesh with the bat's own
#    shader/material copied over and the texture swapped, as section 480
xomtool bank --from Data/Tweak/WEAPTWK.XOM --object kWeaponBaseballBat --as kindjal.NailBatSeed --out seed.xom
xomtool convert BaseballBat.gltf --into seed.xom --as kindjal.NailBat --section 480 \
    --material-from BaseballBat --material-file Data/Bundles/Bundl09.xom --texture nailbat_rust.png -o stage1.xom

# 4. bundle shape: drop the 3 seed objects, set the "world" entry GUID, add a root XGraphSet listing the
#    descriptor by name (entry GUID 99cc436e6fbef54b85d2bfcdf9ae4283 as in vanilla roots), fix TYPE counts
xomtool unpack stage1.xom -o stage1.json
python bundleize.py stage1.json stage2.json 3
xomtool pack stage2.json kindjal.NailBat.xom
```

The helper scripts (`extract_tex.py`, `bundleize.py`) are in the session scratch folder next to the result:
`C:\Users\Jamin\AppData\Local\Temp\claude\C--Users-Jamin-Desktop-melange-plugins\2e56fa3c-e59c-4870-9357-c436ad2eadcc\scratchpad\meshes\kindjal.NailBat.xom`
(44 798 bytes, 16 objects: root `#15 XGraphSet` -> `#16 XMeshDescriptor kindjal.NailBat` SectionId 480 Flags 8 ->
`#14 XGraphSet` "world" -> `XInteriorNode` -> `XGroup` -> `XShape` with the copied `baseballbat_shader` and the
recoloured `XImage`). `meshbank_selftest` accepts it (`InspectBank` + `CheckEntries("kindjal")`).

## Trying it in the game

1. Copy the bank somewhere under the game folder, e.g. `<game>\Mods\kindjal\assets\data\kindjal.NailBat.xom`
   (an absolute path outside the game folder is passed to the engine as-is and may not open - unverified).
2. At the main menu (the GRM exists from app init): `mesh.state` (expect `available=1`),
   `mesh.resolve BaseballBat` (expect section 9, bin 8, loaded 0 before any match),
   `mesh.load <game>\Mods\kindjal\assets\data\kindjal.NailBat.xom`,
   `mesh.resolve kindjal.NailBat` (expect section 480, bin 8, **loaded 1**, a non-zero graph set).
   `Melange.log` gets one `[meshes] ... ready` line per mesh; the game's own log says "Loading section 480".
3. Point a weapon at it for a match. A content mod's `entry.sim` (the bat is not a clonable base, so use the
   in-match override, which is undone at match end):
   ```lua
   wum.sim.weapon("kWeaponBaseballBat"):set("WeaponGraphicsResourceID", "kindjal.NailBat")
   ```
   then start an offline match with the mod enabled, select the baseball bat and fire. The expected result is the
   bat drawn in dark rust-red; `Bazooka.Weapon`-style weapons use `WeaponGraphicsResourceID` for the held model
   and `PayloadGraphicsResourceID` for the projectile, so a bazooka clone's `set` can name a mod mesh the same way.
4. If the bat is invisible or vanilla-coloured, check the game log for "Failed to load mesh", "Bundle contains
   unrecognised resource", ") from bundle (" or "XomLoadObject: failed to open file", which pin the step.

## Self-test

`tests/meshbank_selftest.cpp` (synthetic bundle documents: order, section, naming rule, sentinel and range edges,
mixed sections, bad root, duplicates, `RelocateBank`; plus `--bank <file> --mod <id>` for a real bank) is the
`meshbank_selftest` target in `CMakeLists.txt` and in `scripts/selftest.ps1`; it links
`src/assets/meshbank_inspect.cpp` and `melange_xom` only. The manifest field's parsing (shape, `.xom`, under the
assets root, the 64 limit, kind) is tested in `tests/thumper_selftest.cpp` (`TestMeshes`, and `TestVehicleMeshes` for
`vehicleMeshes`).

## Unverified and open

- **Verified in game:** the chain (`AddMeshDescriptors` on a mod record, the format-string swap, `LoadSection` on
  section 480, `AdoptFrom` finding the "world" graph) at the main menu, and a weapon creating a held instance from it
  (the baseball bat via `WeaponGraphicsResourceID`).
- **Not yet run in game:** the automatic load from the manifest (it calls the same function from a `Frame` handler);
  the section relocation (a rewritten copy under `Melange\cache\meshes\`; the pure rewrite is unit-tested and
  round-trips, but the engine has only loaded an unmodified bank); a second bank in one session; a failed bank's
  stubs; a match with other players.
- The engine file open with an **absolute** path outside the game folder is unverified; game-relative paths are the
  proven form (`LoadBank` uses the same open). A mod folder under `<game>\Mods` is converted to game-relative.
- `XMeshDescriptor.Flags` (8) and record `+0x2c` (0) are copied, not understood. Scene bin 8 is what every vanilla
  weapon mesh uses; `Bazooka.Flames` (an `AttachedMesh`) uses bin 23 with Flags 0, so an attached-effect mesh may
  need `mesh.load ... <modId> 23` - untested.
- **Instances and sections:** a loaded section is never unloaded by the module; the mod section's auto-unload byte
  is 0 so the engine will not either. What the Ingame section's unload between matches does to a mesh *instance*
  whose descriptor lives in another section is not known.
- Skinned meshes, animation and bitmaps/sprite sets (other descriptor classes with their own record layouts,
  slots 7-11) are out of scope; the same stub-then-bundle protocol should apply to bitmaps (slot 11 records are
  20 bytes with a type bit at `+0x10`), not checked.
- `xomtool convert ... --bundle out.xom` writes the bundle shape directly (see [xomtool.md](xomtool.md)); the
  hand-made pipeline above (`--into`, `bundleize.py`) is how the proven test asset was built.
- A mod mesh as a projectile (`PayloadGraphicsResourceID`) or an `AttachedMesh`, and on a weapon clone, is untried.
- `vehicleMeshes` (the Airstrike and Super Airstrike helicopters) is implemented from the decompile and the `.data` bytes
  and unit-tested, not run in the game; see [the section above](#engine-picked-meshes-the-airstrike-and-super-airstrike-helicopters).
- Only content mods load banks, once per launch; there is no live reload and no unload.
