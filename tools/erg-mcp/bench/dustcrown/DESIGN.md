# Dustcrown: design

Foldspace's second map, after Caldera. The brief (the user's words): "A harsh, water-starved desert world featuring a
single large palace in the center. Two players spawn in the sand. One on the palace."

The user's answers (2026-11-11):

- **Teams**: any number of teams. Spawn areas lie in the sand and on the palace, and the game shuffles teams across them
  as on a stock map. The ratio is about two sand areas, on opposite sides, for each palace area, so a 3-team game usually
  reads 2 sand + 1 palace.
- **Hazard**: the sand is the hazard. The map is built for the Sandtide plugin: its sand-curse line is the threat, then
  its water. The sea must read as sand. Without Sandtide the map plays as any other map, with the scheme's sudden death.
- **Palace**: a desert fortress-palace. It is a massive sandstone palace with sheer walls, a stepped base, inner
  courtyards and tall towers, and it is hard to climb without tools. It is the single dominant landmark in the centre.
- **Desert**: dunes and rock outcrops (sandstone mesas, boulder fields) between the starts and the palace. There are no
  ruins and no ramp. Reaching the palace takes movement weapons (jetpack, rope, teleport, girder), and the stepped base
  gives some footholds.
- **Size**: about Caldera's size.

Like Caldera, the map is built only through the erg-mcp tools, by `build.mjs` here, and `pack/` is the source export of
the last build. Units: **map units** unless marked `w` (world = map x 20, the unit of the water level and of Sandtide).

## 1. The numbers that fix everything: water, tallest frame, sand line

Sandtide (1.0.1, `plugins/sandtide/mod/sim/main.lua` computeLine) fixes its line once per match:
`L = base + 0.25 x (maxY - base)`. Here `maxY` is the top of the engine's landscape box (`Land.MaxBounds`), and `base` is
`Water.Level` when the land reaches below it (otherwise `minY`). The map cannot set L directly, so it sets L through the
land's shape.

| Symbol | Value (map) | Value (w) | How it is held |
|---|---|---|---|
| W, water at match start | 1 | 20 | `set_water {level: 20}`, as Caldera; the pack sets nothing else on the water. |
| minY, the sea bed and the base floor | -9 | -180 | The skin's floor under the coast, so minY < W < maxY and `base = W`. |
| T, tallest voxel frame (the Crown tower's tip) | 55 | 1100 | The only frame above 50. The builder trims its frame box to the tip. |
| **L, the sand line** | **14.5** | **290** | 20 + (1100 - 20) / 4. |
| The dead band (no standable surface reachable from a spawn) | 12.5 - 16.5 | 250 - 330 | L +- 40 w, so standing on a surface never decides the curse by a hair. |

Procedure: build the Crown first, read `describe_level`'s extent (the landscape box, not meshes or details) and compute
L. Then place every tier against that L. Round 1 of the game tests reads the real line from the Sandtide log
(`sandtide: sand line at Y ...`). If the line is off by more than 20 w, move the Crown's tip (4 w of tip moves L by 1 w),
never the tiers. If the engine box turns out to be frame boxes rather than solid tops, the tallest frame's box top is the
thing to trim.

## 2. Layout

The arena is a square of 236 x 236 (box 118, as Caldera). The desert is clipped to a wobbled circle of radius 117, and
its coast runs at radius 100-112, where the dunes slope under the sand-coloured sea to the bed at -9. The palace stands at
(0, 0). +x is east and +z is south.

```
                                  N (-z)
        . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .
      .  ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~  .
     .  ~   ////   ////   ////  [ SAND N ]  ////   ////   ////     ~   .
     .  ~  ////  #MESA-NW#  ////   ////   ////  oo  (low mesa)     ~   .
     .  ~   //  ##(refuge)##   oo oo     ////   oo o    ////  ////  ~  .
     .  ~  ////  ######      ////  +---------------------+  ////   ~   .
     .  ~   ////   ////  ////     | step1 84x84 top 12  |   ////   ~   .
     .  ~  ////  (low mesa)  ////  | +-----------------+ |  ////    ~   .
     .  ~   oo o  ////   ////     | | step2 72 top 18  | |    oo     ~  .
  W  .  ~ [ SAND ]  ////  oo  //  | | +-------------+ | |  //  [ SAND ] ~ .  E
 (-x).  ~ [  W   ]   ////  ////   | | | T  court  T | | |      [  E   ] ~ . (+x)
     .  ~   ////  ////   oo  //   | | |  KEEP+CROWN  | | |  ////   //   ~  .
     .  ~  ////  ////  ////       | | | T  court  T | | |   oo o  ////  ~  .
     .  ~   //  oo o  ////  ////  | | +-------------+ | |  ////  ////   ~  .
     .  ~  ////   ////   ////     | +-----------------+ | (low mesa) ///~  .
     .  ~   ////  (low mesa)  //  +---------------------+   ######     ~   .
     .  ~  ////   ////   oo o   ////   ////   ////     ##(refuge)##    ~   .
     .  ~   ////   ////  ////   oo  ////  ////  //   #MESA-SE#  ////   ~   .
     .  ~  ////   ////  ////  [ SAND S ]   ////   ////   ////   ////  ~   .
      .  ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~  .
        . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .
                                  S (+z)
   ////  dune crests (transverse, running NNE-SSW; wind from the west)   oo  boulder fields   ~  sand-coloured sea
   ##    mesas (refuges NW and SE above the line; low mesas below it)    T   corner towers (domed)
```

### 2.1 The palace (the "Dustcrown"): about 84 x 84 at the foot, 55 high

The palace is one composition, symmetric under a quarter turn, so no side is favoured.

| Part | Footprint | Top (map / w) | Against L (290 w) | Build |
|---|---|---|---|---|
| Step 1 (lowest plinth) | 84 x 84 | 12 / 240 | L - 50 w: below. A foothold, but a cursed one. | Solid, scale 3, from the floor at 0 |
| Step 2 | 72 x 72 (6 deep ledge) | 18 / 360 | L + 70 w: safe | Solid, scale 3 |
| Step 3 (the palace terrace) | 60 x 60 (6 deep ledge) | 24 / 480 | L + 190 w | Solid, scale 3 |
| Curtain wall, 3 thick | 52 x 52 on step 3 | wall-walk 32, merlons 34 | safe | Scale 1.5, crenellated, one gate per side (6 wide, opened to 9 by a flat bore so no jamb catches a worm) |
| Four courtyards (L-shaped quadrants, about 11 wide) | inside the curtain | floor 24 / 480 | safe | Floor of step 3, StarMosaic paving, a fountain each |
| Two cross wings (galleries, 6 wide) | keep to the curtain's mid-sides | roof 32 | safe | Hollow, scale 1.5, an arch through each at court level |
| Keep (great hall) | 24 x 24 at the centre | roof terrace 40 | safe | Hollow hall, roof 2 voxels thick, windows |
| Four corner towers, round, radius 4 | on the curtain's corners | 46, onion domes to 53.5 | safe | Hollow, crenellated; BlueMosaic domes NE and SW, Gold domes NW and SE |
| The Crown (central tower), radius 6 | rises from the keep | tip 55 / 1100 = T | defines L | Gold-trimmed crown of merlons |

- **Sheer walls**: every riser is vertical and 6 high (step 1 stands 6 over the sand at its foot, 5.5-6.5 as the dunes
  run). The curtain rises 8 over step 3. Nothing is walkable from the sand to step 2: no ramp, no stair, no rubble slope.
  The dunes are flattened in a ring 8 wide round step 1, so no dune crest leans on a riser to make a ramp.
- **Footholds**: the three ledges, 6 deep, are the footholds. Step 1's ledge is under the line (a quick jetpack
  hop can land there, but a worm that ends its turn there after 3:00 is petrified). Step 2's ledge and above are safe.
  Each riser of steps 2 and 3 has two shallow niches (2 wide, 2 deep, 3 high) per side, offset from the side's middle.
  They give a rope anchor and a place to land, and they never make a stair.
- **Gates** (pinwheel): one gate on each side of the curtain at court level, offset so that it opens into a courtyard
  and not into a wing. Gates are N into the NE court, E into SE, S into SW and W into NW. A worm that reaches step 3 can
  walk in. A worm in the courts can walk through the wing arches to every court. Stairs inside the wings (`build_stairs`)
  lead from the courts to the wing roofs (32), and on to the wall-walk and the keep's roof.
- **Air strikes and holes**: the plinths are solid to the floor. The court floors are step 3's solid top. The keep's hall
  has a floor, so a bomb through the roof lands in a room, never in a void. No hollow structure has an opening to empty
  space below the courts.

### 2.2 The desert

- **Floor**: the sand floor stands at 5-7 (100-140 w). It is 80-120 w above W, so the sudden-death sea reaches it a few
  turns after it starts to rise.
- **Dunes**: a transverse dune field with crests running NNE-SSW and the wind from the west. Each dune has a gentle
  windward slope (about 12 degrees, 78% of the wave) and a steeper slip face (about 35 degrees, walkable). The crest
  spacing is 34 and the crests stand about 5 over the troughs, never above 11 (220 w, L - 70 w); a 2.5-unit skin holds
  that profile (round 2b's 18-wide dunes were 7 columns a wave and read as stripes). The bleached crest and the ochre
  trough are masks on the same `dunes()` wave as the height, edged per corner, so they follow the ridge lines. The ridged crests bend round the
  mesas and die down within 8 of the palace ring and within 6 of each spawn pad. A worm can cross every dune on foot.
- **Mesas** (terraced sandstone, 2.5 strata, `heightfield_region` terraces, cliff not drape):
  - Two **refuge mesas**, NW (-60, -60) and SE (60, 60), each about 30 x 22 at the foot, weathered buttes (sedimentary
    beds, gullies cut into a wobbled outline): a talus skirt to 10 (200 w), two sheer risers of 7.5 that step back 1.25
    (a ledge at 17.5, 350 w) and a top at 25 (500 w), so no ledge lies in the dead band. They are the only safe ground
    outside the palace, reached by jump, rope or jetpack, much as the palace is, but cheaper.
  - Four **low mesas and outcrops**, NE, SW, and two off the axes, 20-22 across, tops at 11.5 (230 w, L - 60 w). They give
    cover and a bit of height, and they are inside the curse. Since round 4 they are outcrops in the dune skin itself: a
    flattened, eroded dome over one steep sheared column of flank (darkyellowplaster by the skin's steepMaterial), with
    darker courses of uneven thickness on the flanks (round 3's build_mesa terraces read as stacked slabs).
- **Boulder fields**: six fields, one between each sand area and the palace and two by the low mesas. Each field is a
  pile of seven low rounded voxel domes (radius 1.8-3.2, 1-2 high over the sand, wider than tall: no stub pillars), plus
  sim-neutral boulder and scree meshes from the Dustcrown mesh bank (no slabs: they read as plinth blocks; no wedges:
  they read as pink slabs). The bank is a pale sandstone built on mod mesh section 514, which no Kindjal bank uses, so
  the detail layer is the same with any plugin set (round 4: game_shot_compare differs only in the sky).
- **The bed**: the rock shell under the dune skin stands at 2.2 or more inland (1.5 under the sand at least), so an air
  strike's crater bottoms out over the starting sea (1.0), not at it.
- **The coast**: from radius 95 the dunes slope gently under the sea to -2.5 by about radius 111 (held off round the
  refuges). The shore is sand on sand: the rim the sea meets is the crest's bleached sand (heightfield_drape coast),
  blended inland, and the crest and trough masks stop at the coast (round 3's overview showed an orange cliff ring).

### 2.3 Spawns

There are five spawn areas, each a flattened dune trough or terrace with room for four worms 3 apart:

| Area | Where | Ground | Against L |
|---|---|---|---|
| SAND W | (-88, 0) | 6 / 120 w | L - 170 w |
| SAND E | (88, 0) | 6 / 120 w | L - 170 w |
| SAND N | (0, -88) | 6 / 120 w | L - 170 w |
| SAND S | (0, 88) | 6 / 120 w | L - 170 w |
| PALACE | one knot in each of the four courtyards | 24 / 480 w | L + 190 w |

Each area has four knots (WORM slots), so up to four worms a team.

- **Target behaviour (the `areas` spawn mode, a feature below)**: per match, Melange deals areas to teams from the match
  seed, which every peer shares. The deal follows the pattern `sand, sand (opposite the first), palace, sand`, and the
  seed picks which axis (W/E or N/S), which side comes first, and which team gets which slot.
  - 2 teams: two opposite sand areas. They race each other to the palace.
  - 3 teams: 2 sand (opposite) + palace. This is the brief.
  - 4 teams: 3 sand + palace.
- **Fallback (today's `teams` mode)**: if the `areas` mode is not ready, the map ships with team 0 on SAND W, team 1 on
  SAND E, team 2 on PALACE and team 3 on SAND N. That gives the same team counts per area as above, without the shuffle.
  The README says this plainly.
- Every knot passes the spawn tread checks. No knot is in the dead band, under water, or within 8 of a riser's foot.

### 2.4 Hazard and water

- **With Sandtide (+ Kindjal)**: at the first turn end after 3:00, every resting worm with its feet under 290 w is
  petrified. This covers all four sand areas, the dunes, the low mesas and step 1. The palace's step 2 and above and the
  two refuge mesas are safe. At the round's end (5:00 in the sandworms scheme) the scheme's sudden death raises the sea.
  The sand floor (100-140 w) is covered after 80-120 w of rise, and cursed worms drown.
- **Without Sandtide**: plain sudden death on the same sea. The dunes flood first, then the mesas and the palace steps.
- The map sets only the start level, 20 w. It has no level script water schedule, and it sets nothing else on the sea.
- **The sea reads as sand**: `set_water_look` sets opacity 1, reflection 0, shade lighter, a sandy sky blend
  (target about #e3c27e, matched to Sand 24 under DAY light) and low specular. It is tuned with `game_shots` until the
  coast reads as dunes running into a flat sand plain with no blue. Sunstone's haze is left as it is.

### 2.5 Crates, mines, objects

- There is no mine factory and there are no map-placed mines or oil drums (the user asked for neither). Crates and mines
  follow the scheme. Mines explode more than 10 w under water, so mines on the sand-sea die as on any map.
- Scenery: lamps and torches (ARABIAN8/9/18) on the wall-walk and in the courts, a fountain (ARABIAN19/20) in each court,
  and sparse grass tufts (ARABIAN1) in dune troughs only. All of it is copied from the ARABIAN bases. DinerMight's neon,
  grass and backdrop are removed (`remove_scenery`, `remove_backdrop`, surround `none`).

## 3. Look: harsh sun, bleached sandstone, ochre dunes

- **Base and theme**: `new_project` on Multi.DinerMight, then clear, remove and trim the base terrain (as Caldera does),
  then `set_theme ARABIAN` with `materialFile: "theme"`.
- **Time of day**: DAY, for a high harsh sun, pinned for every match (`set_level_look {timeOfDay: DAY, lock: true}`,
  spice.json `levels[].timeOfDay`): round 2's Versus matches drew NIGHT and EVENING and the map took a violet wash. The
  pack carries its own menu picture (`set_level_thumbnail`, `thumb/dustcrown.png`).
- **Land light**: a slight warm `landDiffuse` (about 255, 244, 225), tuned in game. It must not push the palace orange.
- **Materials (ThemeArabian indices)**:

| Where | Material |
|---|---|
| Dune slopes | Sand 24, sand01 10, SandBlend 23 at the edges |
| Dune crests (bleached) | grass02blend 3, tinted cream #f2e3bd (sRGB) under Sunstone |
| Troughs (ochre) and the spawn pads | path02 9, the lowest 6% of each wave on ground under 22 degrees, tinted #d4b48e (sRGB; plays a muted ochre near #cfae7e in sun under Sunstone; round 3's #d8ad6a played a saturated #d9b43f, round 2b's #ce7a30 read as rust); path01 8 on the pads |
| Refuge mesa strata | stone 12 and sandy-plaster 15 beds, each followed by a darkyellowplaster 57 course (rock01/rock02 drew a grey cobble on the cliffs) |
| Low outcrops | stone 12, darkyellowplaster 57 courses and steep flanks |
| Mesa caps | stone 12 |
| Boulders | rock01 4 |
| Palace plinth risers | darkyellowplaster 57 (ochre courses), Bricks 14 at the base course |
| Plinth treads and ledges | stone 12 |
| Curtain, towers, wings, keep, Crown | sandy-plaster 15 below a GoldEdging 26 course, stone 12 (bleached sandstone) above; Plaster 21 read as grey concrete |
| Merlon caps and the Crown's crown | Gold 25, GoldEdging 26 |
| Court floors | StarMosaic 30, PlainTiles 32 at the edges |
| Tower dome caps | BlueMosaic 38 (NE, SW) and Gold 25 (NW, SE); DomeTile 18 read as red brick |
| The keep's door and windows | Door1 33, Window1 34 |

  Material names are not matched to their looks: check them once with `game_material_shots` before the palace pass.
- **Detail layer** (sim-neutral, later rounds, as Caldera's DETAIL.md): Dustcrown's own sandstone boulder and slab meshes
  in **one** mod mesh bank, seated with the lint tools. `sim_fingerprint` is unchanged by any detail.

## 4. Cell and block budget (65 535 cells, 64 added blocks)

| Part | Build | Cells (est.) | Blocks (est.) |
|---|---|---|---|
| Base remains after trim | | 700 | (base) |
| Desert skin: drape, voxel 2.5, 3 thick, circle 117, the offshore floor cut | as Caldera | 20 000 | 9-10 |
| Bedrock shell under the playable desert (r < 100): voxel 4, 3 thick, 6 under the skin, `keepFloor`, eroded edge | as Caldera's inner shell | 6 500 | 2-4 |
| Mesas (2 refuges + 4 low), terraces, voxel 2.5 | `heightfield_region` | 7 000 | 6-8 |
| Voxel boulders | small blocks, scale 2.5 | 1 500 | 3-4 |
| Palace plinths (3 solid steps), scale 3 | | 4 600 | 3-4 |
| Curtain, wings, keep, stairs, scale 1.5 | | 6 500 | 8-10 |
| Towers (4 corner + Crown) with caps, scale 1.5 | | 3 500 | 5 |
| Spawn pads, niches, trim | | 1 500 | 2-3 |
| **Total** | | **about 52 000** | **38-48** |

That leaves about 13 000 cells and 16 blocks spare. Every build call passes `maxCells` and `maxBlocks` and runs
`costOnly` first. The bedrock shell is required, not optional: a desert is dug open-air all game, and Caldera's hollow
skin let bombs fall into the void.

## 5. Rubric

Each item passes or fails, with in-game evidence (screenshots, logs) from testenv B, with Sunstone ON in every run, and
with Sandtide + Kindjal ON where stated.

| | Item |
|---|---|
| R1 | Builds only through the MCP from `tools/erg-mcp/bench/dustcrown/build.mjs`, reproducibly: two builds give a byte-identical `pack/`. `level.save` has no warnings. The source export succeeds. Within 65 535 cells and 64 blocks. One mod mesh bank. The ship gate passes. Caldera still builds byte-identically (its `ship.mjs` check). |
| R2 | Loads and plays with Sunstone ON and no asserts beyond a vanilla baseline (a vanilla Multi.DinerMight run in the same build and session, bazooka fire). fps is not worse than that run. |
| R3 | The palace reads, from the overview and from each sand area, as a single dominant fortress-palace: three sheer stepped plinths, a crenellated curtain, four courtyards, four domed corner towers and the Crown. It is the tallest and largest thing on the map, and no other landmark reaches half its height. |
| R4 | The dunes and outcrops read clearly in the overview and at worm height: crested transverse dunes with bleached crests and ochre troughs, two tall refuge mesas, four low mesas and at least four boulder fields. Nothing reads as ruins or as a ramp. |
| R5 | Spawns: with 2, 3 and 4 human teams, every worm stands on its area's knot (`fromKnot` 0), never in solid, water or air. 2 teams: two opposite sand areas. 3 teams: two opposite sand areas + the palace. 4 teams: three sand + the palace. With the `areas` mode: two matches with different seeds deal different areas and sides, and in a LocalNet game both peers agree. (Fallback, only if `areas` is not delivered: the fixed `teams` deal of 2.3, said in the README.) |
| R6 | Sand line with Sandtide + Kindjal ON: the Sandtide log's `sand line at Y` is within 20 w of the design (290 w). At the first turn end after 3:00, a resting worm on each sand area and on step 1 is petrified. Worms resting on step 2, in a court and on a refuge mesa are not. No spawn knot and no reachable standable surface lies within 40 w of the line (a `describe_level` / line-check listing). |
| R7 | The sea reads as sand: overview and shore shots with Sunstone ON show an opaque sand-coloured sea that continues the dunes, with no blue and no reflection. The sudden-death rise still shows as a rising sand plain. |
| R8 | Without Sandtide (the scheme's own sudden death): the water starts at 20 w, rises from sudden death at the scheme's speed, covers the sand floor within the expected turns (80-120 w of rise), and worms drown as on any map. The map sets nothing else on the water. |
| R9 | Reachability. **On foot**: every sand area reaches every other sand area and the foot of the palace (`check_reachability`, and one steered worm per area in game). No walking route leads from the sand to step 2 or above (`check_reachability` finds none, and a steered worm's jumps and backflips at a riser fail). **With tools**: from each sand area, a worm reaches a courtyard by jetpack within two turns (walk to the foot, then fly), and also by ninja rope from step 1. Teleport and girder each land a worm on a ledge once. From the palace, a worm gets down to the sand on foot by jumping off. |
| R10 | Explosions: an air strike over a court, the keep's roof and each plinth, plus a bazooka volley into the dunes. No bomb, worm or crate falls into a void or below the bed. Every hole opens onto solid rock below. |
| R11 | It looks good: a harsh DAY sun, bleached sandstone with gold trim and two blue domes, warm but not orange land light, and none of DinerMight's scenery or backdrop left. Detail meshes are seated (lints pass). `sim_fingerprint` is the same with and without the detail layer. |
| R12 | Nothing broken: gate 1 passes (`build.ps1`, web unit, `-Panels`, `-Erg`, C++ selftests within the known local failures, `tools/erg-mcp/test.ps1`). Old projects load. Every new builder feature is additive and off by default. |

## 6. Map-builder features to add before building

These are general tools that Dustcrown needs and that any later map can reuse. They are listed in build order.

1. **Height-line check** (`check_height_line`, plus `landBounds` in `describe_level`). `describe_level` reports the
   landscape box as the engine computes it from voxel frames (min/max Y, world and map), and the frame that sets each
   end. The new tool takes a `fraction` (or a fixed `y`) of the box over `base` (water at the start when the land reaches
   below it, else minY: Sandtide's rule is the default, `preset: "sandtide"`). It answers the line, then lists every
   spawn knot and standable surface (sampled tops) by side, and flags anything within `band` (default 40 w). It warns
   when the whole land is above W or W < minY. It is pure inspection. The builder calibrates it against the in-game
   Sandtide log once.
2. **`build_compound`**: one structure from a list of solid primitives on the existing Shape interface: `box`,
   `cylinder`, `cone`, `dome` (a sphere cap, an onion profile option), `tiers` (a stepped pyramid: base size, step
   count, riser height, ledge depth), and `carve` versions of each, in order. Each shape takes `material`, `part` and an
   optional `bands`. It supports `scale`, `parts` packing, `ground`/`sink`, `costOnly` and `maxCells`. It covers the
   stepped plinths, niches, dome caps and the Crown. It is additive and changes no existing tool.
3. **Material bands and trim on structures**: an optional `bands` ([{fromY, toY, material}], world or map) and `trim`
   ({`top`, `merlonCap`, `course`: {y, material}}) on `build_castle`, `build_wall`, `build_tower`, `build_keep` and
   `build_compound`, through each shape's `matAt`. With the options unset, the output is byte-identical to today's.
4. **Mesa generator** (`build_mesa`, on top of `heightfield_region`): a plateau from a wobbled outline (circle, ellipse
   or polygon plus noise), a top height, terraced cliffs (strata step in voxels, jitter per step, tread width so the
   treads stay unwalkable or walkable on request), an optional talus skirt, and strata material bands with a cap
   material. It also takes `clip`/`exclude` circles, so a later drape avoids it. One call per mesa.
5. **Dune expressions**: new built-ins for the heightfield expression language: `dunes(x, z, dirDeg, wavelength,
   height, asym, seed)` (transverse crests with an asymmetric windward and slip face, crest wander by noise) and
   `ridged(x, z, scale, octaves, seed)`. `heightfield_drape` also gets a crest/trough material rule (`crestMaterial`,
   `troughMaterial` by local curvature). Existing expressions evaluate as before.
6. **Water look presets**: `set_water_look {preset: "sand" | "lava" | "murk", tint}` expands to the verified fields for
   the project's theme and time of day, and lets explicit fields override it. "sand" is verified on ARABIAN DAY in this
   build. The tool says when a preset is unverified for the theme.
7. **Spawn areas mode** (hard: Melange core): `place_spawn_knots {mode: "areas", areas: [{name, group, knots: [...]}],
   pattern: ["sand", "sand:opposite", "palace", "sand"], opposite: [[a, b], ...]}`. At match start the generated level
   chunk deals one area to each team, in a pattern order permuted from the match seed. This is deterministic, so every
   online peer deals the same, and each team's worms go on its area's knots in slot order. This needs the C++
   generated-chunk change, the Erg schema and panel (an old server refuses the format with the existing "older than the
   project's format" error), docs, and a selftest for the deal. `random`, `knots` and `teams` are unchanged.
8. **Rock mesh library**: Caldera's `meshes/gen.mjs` and `merge.mjs` generalised into one module (faceted hull
   boulders, strata slabs, scree, with a palette and seed parameter) that writes one bank for a map. It is used for
   Dustcrown's sandstone boulders. Caldera's bank stays byte-identical.
9. **Movement-weapon reach check** (`check_tool_reach`): from each spawn area, a geometric check of whether a jetpack
   arc (rise and run limits), a ninja rope (anchor faces within its length) or a girder span reaches a named target
   surface. Its limits are calibrated once by game tests and listed in the docs. It reports estimates, which the game
   tests confirm.
