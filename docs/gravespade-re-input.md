# Grave Spade RE: melee fire input, hit events and turn end

> **Status:** built and shipped in Melange 0.16.0. These are the research notes written before and during the
> work; the API as built is documented in [lua-api.md](lua-api.md), and where the two differ lua-api.md is right.

Reverse-engineering notes for the Kindjal Grave Spade (Melange 0.16.0). They cover how a melee weapon gets its fire
input, how its strike finds and damages a worm, and how its turn ends. They end with the sim API and C++ hooks that
0.16.0 should add. All of it was built in 0.16.0 (see the status note above).

- **Build:** WormsMayhem.exe build #1077, the build `src/gameplay/worm_anchor.cpp` targets.
- **Sources:** static analysis only: the Ghidra export in `melange-private/re/out`, plus capstone disassembly of the
  exe. I mapped message-id globals to names by scanning every `push "<name>"; mov ecx, <global>; call 0x68bb9f`
  static initialiser (3635 of them).
- **Tags:** items marked **[verify]** still need an in-game check.

> **Correction after the in-game run (r1): fire does not reach the melee entity through the worm.** With a logging
> filter in the worm input hook `0x5ac6e0`, a human turn showed only broadcast messages there (AI.IssueWormCommand,
> Weapon.Selected, Weapon.Delete, Worm.OverridePhysics) and no `Input.*` at all. A spade press still swung and ended
> the turn. The disassembly shows why: `MeleeWeaponLogicEntity::Initialize 0x568860` subscribes the entity itself
> on the message bus (`0x690e3e`) to `Weapon.Delete` (`0x568960`), `Input.FirePressed` (`0x568988`, id global
> `0x95deac`) and `Input.FireReleased` (`0x5689b0`, `0x95deb4`). The bus delivers them straight to HandleMessage
> `0x569930`. Section 1.1 below describes a path that the melee weapon does not use. The 0.16.0 gate therefore lives
> in the HandleMessage hook. For a press or release that the active worm's entity (its `+0xc8` task resolves to
> `*(0x95fb98)`) gates as a dig, the hook runs the base handler `0x68b79e` (stdcall(this, msg), `ret 8`) instead of
> the original. That is exactly what the original does with a press it ignores, so the swing never starts. It needs
> no previously tracked entity, so the first press after a weapon switch is covered too. `Input.FirePressed` is an
> exact id compare (`0x68bcce`); the Joypad ids are separate messages that the entity does not subscribe to.

## 0. Summary

- **No vanilla digger.** The weapon table has no drill, blowtorch or digging weapon. The only melee weapons are the
  four `MeleeWeaponPropertiesContainer` weapons:

  | Weapon | MeleeType |
  |---|---|
  | `kWeaponBaseballBat` | 1 |
  | `kWeaponFirePunch` | 2 |
  | `kWeaponProd` | 3 |
  | `kWeaponNoMoreNails` | 4 |

  All four run on one logic class, `MeleeWeaponLogicEntity` (vtable `0x85a82c`). The dig has to come from Melange
  (the carve API is a separate RE task). The melee entity can provide the aim, the held mesh and the strike.
- **Use the Bat as the base.** It is the only melee weapon with `IsAimedWeapon=yes`, so its swing follows the worm's
  aim pitch. The Prod uses a fixed angle (`ImpulseDirection.x` = 30 degrees). Choosing the Prod would also mean
  setting `IsAimedWeapon=true` on it (a Bool field, which `weapon:set` accepts).
- **Fire input path.** Fire reaches the melee entity through the active worm:
  1. `WXWormLogicEntity::HandleMessage 0x5b07c0` calls the worm input handler `0x5ac6e0`.
  2. If that handler doesn't handle the message, `0x5adcf0` gets it.
  3. If that doesn't either, it goes on to the worm's sub-entity at `+0x20` (vtable `+0x1c`), and from there to the
     weapon.

  Melange already hooks `0x5ac6e0` for the anchor and returns 1 there to swallow rope and jetpack fire. The same hook
  can **swallow `Input.FirePressed` and `Input.FireReleased` for the spade**. Then the melee entity never swings: no
  ammo is used, no `Timer.EndTurn` is posted, no `Weapon.Fired` is posted and the turn is not ended. Melange records
  press, release and held ticks for sim Lua instead. Inputs reach this handler on every machine at the same point in
  the deterministic phase (the anchor already relies on this), so the swallow decision is lockstep-safe.
- **Hit path.** The strike calls `0x568ec0` (sweep loop). For each new collider it calls `0x567a40` once
  (per-target impact). That function builds a `DamageImpulseMessage` and sends it to the victim's task. A
  pre-hook on `0x567a40` sees attacker, victim, hit point and the vanilla damage and impulse. It can **cancel** the
  hit (skip the original), **override damage/impulse** (patch the container fields around the original call) or
  force the engine's own **`InstantKill`** path.
- **Turn end.** A melee strike ends the turn by itself:
  1. At the swing, `0x549ad0(1)` posts `Timer.EndTurn` and `Weapon.DisableWeaponChange`.
  2. After `PostLaunchDelay`, `0x549bb0` posts `Weapon.Delete`, `Timer.StartRetreatTimer` and `Weapon.Fired`.
  3. The retreat time is the container's **`RetreatTimeOverride`** when it is 0 or more. Otherwise it is
     `DefaultRetreatTime`. MeleeWeaponLogicEntity::Initialize `0x568860` sets the `RetreatTime` data from it.

  So "the strike ends the turn" is free. "End it now" is a data setting: `RetreatTimeOverride = 0`, an I32 field that
  `weapon:set` accepts. A scripted `endTurn` is an optional extra with network risk (section 5).

## 1. Where fire reaches the melee weapon

### 1.1 The worm side

`WXWormLogicEntity::HandleMessage 0x5b07c0` (stdcall(this, msg)) handles a message in this order:
1. Its own messages first: Land.NewShape, Worm.DamageComplete, and others.
2. Then the input chain:

```
cVar4 = WormInput_0x5ac6e0(this; msg);           // movement, aim, jump, AI.IssueWormCommand
if (!cVar4) { cVar4 = 0x5adcf0(this; msg);       // state-specific
              if (!cVar4) this->[+0x20]->vt[0x1c](msg); }   // forwarded on: the weapon gets it from here
```

- `0x5ac6e0` (thiscall(this; msg), `ret 4`) returns 0 at once unless `this == *(0x95fb98)`, the active worm's
  entity. Only the active worm's inputs reach the weapon.
- Its own dispatch handles `Input.MoveX/MoveY/AimMouse/AimMoveX/FirstPersonPressed/JumpBack*/AutoAim*/Jump*` and
  `AI.IssueWormCommand`. It does not handle `Input.FirePressed` or `Input.FireReleased`, so those normally go to the
  weapon.
- `worm_anchor.cpp`'s `HkInput` returns 1 for a swallowed input. That skips `0x5adcf0` and the forward, which is why
  an anchored worm's rope and jetpack fire is dropped.
- The hook is enabled for every match at VM creation (`OnContext -> SetHooks(true)`), so a spade filter costs
  nothing new to install.

### 1.2 MeleeWeaponLogicEntity

These are the melee entity's methods. Ghidra drops `this` on several of them, so the conventions below come from
the disassembly.

| Address | What | Convention |
|---|---|---|
| `0x569930` | HandleMessage (vtable slot 7) | stdcall(this, msg), `ret 8` |
| `0x569b00` | Update (slot 6): the state machine | stdcall(this, time), `ret 8` |
| `0x568860` | Initialize (on message 0x40) | thiscall |
| `0x568100` | Can fire (properties checks) | thiscall, returns bool |
| `0x568180` | Begin the swing | thiscall |
| `0x5683b0` | Idle tick: the "target in range" sweep | thiscall, `ret` |
| `0x568ec0` | Strike: the sweep, then each new target | thiscall, `ret` |
| `0x567a40` | Impact on one target | thiscall, `ret` |
| `0x5693e0` | Fire Punch flight tick (`WormIsWeapon`) | thiscall(arg) |

**HandleMessage `0x569930`.** The message id is the u16 at `msg+4`.

| Message | Condition | What it does |
|---|---|---|
| `0x40` | | Initialize (`0x568860`) |
| `0x42` | | Shutdown: if state != 0, `0x549bb0(1)`; then `0x567870` |
| `Input.FirePressed` (id global `0x95deac`) | `0x568100()` is true and `state(+0x50) == 0` | Post `Weapon.Wield`, `0x568180()`, `0x612d40()` |
| `Input.FireReleased` (`0x95deb4`) | `state == 1` | `0x568180()`. State 1, the accuracy bar, is **never entered** in this build: nothing stores 1 to `+0x50`. A release does nothing. |
| `Weapon.Delete` (`0x95de8c`) | | Delete the entity |

**Begin the swing (`0x568180`).** It does these in order:
- posts `Weapon.DisableWeaponChange` and the worm comment "WeaponFired" (or "Punch");
- posts `Weapon.PlayFireAnim`;
- sets `+0x80 = now + LaunchDelay`;
- for MeleeType 4 only, posts `Worm.OverridePhysics`;
- sets `state = 2`.

**Update `0x569b00`** switches on `this+0x50`:

| State | What happens |
|---|---|
| 0 (default) | `0x5683b0`: the idle sweep, which writes data `Weapon.Melee.TargetInRange` (descriptor at `this+0x34`) |
| 2 | When `time >= +0x80` (LaunchDelay over): calls `0x549ad0(1)` (the turn-end start, see section 3), posts `GameLogic.DecrementInventory`, then runs `0x568ec0` (**the strike**). Next state is 3 if `WormIsWeapon`, else 5. MeleeType 4 also posts `Worm.OverridePhysics`. |
| 3 / 4 | Fire Punch flight (`0x5693e0`, which calls `0x568ec0` every tick) |
| 5 | Moves to state 7 |
| 7 | Moves to state 8 |
| 8 | Once `PostLaunchDelay` (`props+0x44`) has passed: state 0, then `0x549bb0(1)` |

### 1.3 Fields used (entity `this`, properties `P = this+0x58`)

The offsets below were observed in the code. 0.16.0 should still resolve the container offsets by name with
`weapons::Field()` and log any mismatch.

**Entity fields (`this`):**

| Offset | Meaning |
|---|---|
| `+0x14` | Own task id (the stats attacker) |
| `+0x34` | Data descriptor `Weapon.Melee.TargetInRange` |
| `+0x50` | State |
| `+0x58` | Properties container `P` |
| `+0x5c` | Swing dir |
| `+0x68` | Impulse dir |
| `+0x74` | Swing origin |
| `+0x80` | Launch time |
| `+0x9c` | Accuracy angle |
| `+0xb8` | Collider handle (u16 + ptr) |
| `+0xbc..+0xc4` | Already-hit task list |
| `+0xc8` | **Own worm's task id**. The idle sweep skips it, and it identifies the attacker. |
| `+0x2b8` | XString weapon container name (for example `"kWeaponBaseballBat"`) |
| `+0x2bc` | In-sweep flag |

**Properties fields (`P`):**

| Offset | Field |
|---|---|
| `+0x40` | LaunchDelay |
| `+0x44` | PostLaunchDelay |
| `+0x50` | RetreatTimeOverride |
| `+0x80` | LogicalPositionOffset (vec3) |
| `+0x8c` | ImpulseDirection (vec3; `.x` is the fixed swing pitch in degrees when not aimed) |
| `+0x98` | MeleeType |
| `+0x9c` | Radius |
| `+0xa0` | MinAimAngle |
| `+0xa4` | MaxAimAngle |
| `+0xb0` | WormCollisionFX |
| `+0xbc` | LogicalLaunchYOffset |
| `+0xc0` | WormDamageMagnitude |
| `+0xc8` | ImpulseMagnitude |
| `+0xd8` | IsAimedWeapon |
| `+0xd9` | DamageIsPercentage |
| `+0xda` | WormIsWeapon |
| `+0xdb` | InstantKill |

Initialize (`0x568860`) sets two things that matter here:
- With `IsAimedWeapon`, it sets data `Weapon.MinAimAngle`/`Weapon.MaxAimAngle` from `P+0xa0`/`P+0xa4`
  (degrees to radians). Otherwise it uses -pi/2 and +pi/2.
- It sets data `RetreatTime` = `P+0x50` when that is 0 or more, else `DefaultRetreatTime`.

The Bat's MinAimAngle is 45 and its MaxAimAngle is 50. **[verify]** the sign convention. To dig straight down, the
spade probably needs MinAimAngle and MaxAimAngle of about 85 (F32 fields, settable at match start).

### 1.4 The aim the strike uses (bit-exact recipe)

`0x5683b0` (idle) and `0x568ec0` (strike) compute the same ray. `C` is the attacker's worm container
(`0x5b26a0()`; Wormsign hashes every field used here).

```
pitch = P.IsAimedWeapon ? C+0xd0 : P.ImpulseDirection.x * pi/180      (radians)
yaw   = C+0x90
M     = 0x6df502(euler = {-pitch, yaw, 0})       -> dir = M * (0,0,1)   (constants at 0x95de74/0x95de64)
origin = C+0x38 (feet) + (0, Worm.EyeLevelOffset + P.LogicalLaunchYOffset, 0) + P.LogicalPositionOffset (*) dir
sweep  = collider(this+0xb8): 0x519d10(origin); 0x519db0(dir, 10); i = 0x519e90() ... (i == -1 ends); 0x519ec0()
```

Here `(*)` multiplies component by component. The strike's impulse direction (`+0x68`) is the same ray with yaw
lowered by `rand * 1.309` (75 degrees).

Each hit record is `*(0x95c2ec) + 0x20 * u16[0x95c2a0]` (`0x5160e0`):

| Offset | Meaning |
|---|---|
| `+0..+8` | Hit point |
| `+0x14` | Flags. Bit 0 is a worm; bits 1-2 are other objects (barrels, crates). |
| `+0x18` | Task id |

Task to worm slot: `*(u8*)(0x68e4ca(taskId) + 0x30)`. `0x68e4ca` is cdecl and returns the logic entity. `+0x30`
is the worm index, the same `kEntitySlot` the anchor uses. The 0x567a40 path proves this: it reads the victim's
container with `0x50bdf0(*(u8*)(task+0x30))`.

## 2. The hit

`0x568ec0` is called by Update state 2 (Bat, Prod, nails) and by every Fire Punch flight tick. It works like this:
- It sweeps.
- It skips task ids already in `this+0xbc..` (one hit per target per swing).
- For every other collider it calls **`0x567a40`** and appends the task id.
- It stops if the collider handle `this+0xb8` was released.

The Bat hits everything in the sweep, which can be more than one worm.

`0x567a40` (thiscall(this), no arguments; the target is the current record `u16[0x95c2a0]`) does this:

1. **Rumble.** It calls `0x4bc410` (rumble).
2. **Worm-only effects.** If the record's flags have bit 0 set (a worm):
   - spawns the `WormCollisionFX` particle at the hit (`0x5c1410`);
   - for MeleeType 4, nails the worm (a `DirtBallLogicEntity`);
   - computes the damage:
     - `InstantKill` (`P+0xdb`): damage = victim `health(+0x11e) - (+0xcc)`, all of it;
     - `DamageIsPercentage` (`P+0xd9`): a share of the victim's health;
     - otherwise: `ftol(P+0xc0)`.
3. **Impulse.** `P+0xc8 * (accuracy +0x9c / (pi/2))`.
4. **Non-worm objects.** If flags & 6 (another object) and the object's class matches `0x9602ac`, it takes a
   different branch (`0x5c8370`).
5. **Stats.** `0x688eec(victimTask, this+0x14, name == "kWeaponBaseballBat" ? 0x43 : 10)` records the hit.
6. **The damage message.** `0x518c20` builds a **`DamageImpulseMessage`** (0x30 bytes) and `0x68b89e(msg,
   victimTask, 0)` sends it to the victim task. That is what `Worm.Damaged` and the knockback come from. The message
   layout:

   | Offset | Field |
   |---|---|
   | `+8` | Damage (int) |
   | `+0xc` | Dir |
   | `+0x18` | Attacker position |
   | `+0x24` | Impulse |
   | `+0x29` | Prod flag |
   | `+0x2c` | 5 = melee |

   `DoubleDamage` doubles the damage and the impulse.
7. **Sound.** It plays `weapons/BaseballBatImpact`.

Skipping `0x567a40` entirely is safe. The caller only reads the collider handle and the task id afterwards. The
result is a clean miss: no damage, no knockback, no FX, no stats. The swing, the ammo use and the turn end still
happen.

## 3. How a melee strike ends the turn

| Helper | Called from | Effect |
|---|---|---|
| `0x549ad0(endTurn)` | Update state 2, at the swing | Data `Worm.WeaponDisableMovement = 1`. If the argument is set, posts **`Timer.EndTurn`** (stops the turn clock). Posts `Weapon.DisableWeaponChange`. |
| `0x549bb0(x)` | Update state 8 (after PostLaunchDelay) and Shutdown | `Worm.WeaponDisableMovement = 0`. Posts `Weapon.Delete`, **`Timer.StartRetreatTimer`** and **`Weapon.Fired`**. `jlog_adapters.cpp` already knows this is the only `Weapon.Fired` source. |

The retreat time comes from `RetreatTime`, set at Initialize from `RetreatTimeOverride`. `Timer.RetreatTimedOut`
then ends the turn as usual. A swallowed press reaches neither helper, so digging never ends the turn and never
posts `Weapon.Fired`.

That last point has a side effect on Kindjal's client melee flourish. The flourish keys on `Weapon.Fired` plus
`Worm.Damaged` within 0.6 s (`plugins/kindjal/mod/client/init.lua`: `onFired` 1394, `resolveMelee` 1232,
`flourish` 1203). It still fires for a real strike and never for a scoop. It is a client heuristic, not a sim
source.

The Kanly Melee feature (melange-plugins#43) is scheme data only: Ammo and Crate are 0 for banned weapons
(`plugins/kanly/mod/schemes/kanly-melee.json`). It has no melee hook to reuse.

## 4. The sim API (proposed here, built in Melange 0.16.0, `wum.sim.melee`, Lua 5.0)

It mirrors `wum.sim.weapons`. Every call returns `nil, reason` on a refusal.

```lua
-- Top-level chunk only (match start, like weapon:set). Puts one vanilla melee weapon under the mod's control.
-- opts.dig = true turns on the fire gate (below). One mod per weapon per match.
ok, err = wum.sim.melee.claim("kWeaponBaseballBat", { dig = true })
-- err: "not melee" | "unknown weapon" | "claimed by <mod>" | "too late"

h = wum.sim.melee.on(event, weapon, fn)      -- weapon: a claimed name or "*"; returns a handle
wum.sim.melee.off(h)
--   "press"   fn(ev, weapon, tick, slot, mode)              mode = "dig" | "strike" (what the gate did)
--   "release" fn(ev, weapon, tick, slot, heldTicks, mode)
--   "strike"  fn(ev, weapon, tick, slot)                     the vanilla swing reached its strike (state 2 -> 0x568ec0)
--   "hit"     fn(ev, weapon, tick, attacker, victim, x, y, z, damage, impulse)   once per worm per swing
--   "done"    fn(ev, weapon, tick, slot)                     0x549bb0: swing over, retreat started

slot, heldTicks, pressTick = wum.sim.melee.held(weapon)     -- nil while not held (a dig hold in progress)
victim = wum.sim.melee.reach(weapon)       -- worm slot in the strike sweep at the last melee Update, or nil
wum.sim.melee.mode(weapon, "auto" | "dig" | "strike")       -- the gate; default "auto"
ok, err = wum.sim.melee.strike(weapon)     -- start the vanilla swing now (from a dig hold); err "not wielded" | "busy"
ok, err = wum.sim.melee.result(t)          -- INSIDE a "hit" handler only, like weapons.explode:
--   { cancel = true }                  no damage, no knockback, no FX (0x567a40 skipped)
--   { damage = n, impulse = f }        exact numbers (patched P+0xc0/P+0xc8, DamageIsPercentage/InstantKill off)
--   { kill = true }                    the engine's InstantKill path (P+0xdb = 1 for this call)
--   err "not in hit"

pitch, yaw, dx, dy, dz, ox, oy, oz = wum.sim.worm(slot):aim()   -- the strike ray of section 1.4 (works for any weapon:
--                                     the pitch from C+0xd0, the origin without the melee offsets when no melee is wielded)
```

**The gate**, when `dig = true` and the active worm's weapon (`C+0xf4`) is the claimed weapon:

| Mode | On `Input.FirePressed` |
|---|---|
| `"auto"` | Strike if `reach()` holds a worm, otherwise dig |
| `"dig"` | Always dig |
| `"strike"` | Always pass to vanilla |

- A dig press is swallowed (return 1 from the worm input hook). Melange records `held = {slot, pressTick}`.
- The matching `Input.FireReleased` is swallowed too. Melange clears `held` and queues a "release".
- A strike press passes through untouched, and so does its release.
- Only when the melee entity's state is 0. Otherwise everything passes to vanilla.
- `held` clears itself on weapon change, on a new active worm, on turn end and on match end, each with a "release"
  (`mode` set to the reason).

**Events are queued, not run inside the engine call.**
- "press", "release", "strike" and "done" queue from inside engine handlers. They are dispatched from a Melange sim
  tick hook after the engine's Update, in the same tick on every machine. So Lua never runs inside HandleMessage.
- "hit" is the exception. It runs synchronously inside the `0x567a40` hook, as "explosion" does inside
  CreateExplosion, because `result()` must decide the original call.

**Tap versus hold** is Lua policy. A typical policy:
- one scoop on "press";
- more scoops every N ticks while `held()`;
- `heldTicks` on "release" separates a tap from a hold.

**Client side.** Add `aim` (pitch, `C+0xd0`) to `wum.game.worms()` entries, so the dig hands and blade can follow
the real aim. Dig and hit facts reach the client the existing way, through a registered mod message and `sendString`.

## 5. C++ hook plan (new module `src/gameplay/melee.cpp`, order after WormAnchor)

| Hook | Address and convention | Job |
|---|---|---|
| Input filter | Existing `HkInput` (`0x5ac6e0`, thiscall(this; msg)). Add a filter callback `simworms::SetInputFilter(fn)`, so worm_anchor and melee share the one hook. | The gate (section 4). Resolves `Input.FirePressed`, `Input.FireReleased`, `Joypad.Input.FirePressed` and the Joypad release (if registered) per match with `bus::IdOf`, as `ResolveIds` does. |
| Melee HandleMessage | `0x569930`, stdcall(this, msg), `ret 8`. Prologue `64 a1 00 00 00 00 6a ff 68 eb 47 7d`. | Track the live entity per claimed name. Read `this+0x2b8` on `0x40` (after the original); clear on `Weapon.Delete` or `0x42`. `strike()` calls the trampoline with a stack `Message {vt 0x81aa14, id Input.FirePressed}` from the tick hook when `state == 0` and `0x568100` is true. **[verify]** |
| Idle sweep | `0x5683b0`, thiscall, `ret`. Prologue `64 a1 00 00 00 00 6a ff 68 2e 47 7d`. | After the original, for a claimed entity only, re-run the sweep with the stored `+0x74`/`+0x5c` and keep the first record with flags bit 0 and task != `+0xc8`. That is `reach()`. Set `+0x2bc` around the sweep as the engine does. |
| Melee Update | `0x569b00`, stdcall(this, time). Prologue `64 a1 00 00 00 00 6a ff 68 3a 48 7d`. | Watch `+0x50` before and after. On a 2 to 3/5 change, queue "strike". The "done" event comes from the state 8 to 0 change. |
| Per-target impact | `0x567a40`, thiscall, `ret`. Prologue `6a ff 68 0b 47 7d 00 64 a1 00 00 00`. | For a claimed entity with a worm record (see the flow below). |
| Wormsign | `melange.melee` contributor | Claims, `held`, gate mode, `reach` slot and the "hit" count, once per tick. |

**Impact hook flow.** For a claimed entity whose record has a worm flag:
1. Build the attacker slot (from `+0xc8`), the victim slot (record `+0x18`), the hit point and the vanilla
   damage/impulse (`P+0xc0`, `P+0xc8`).
2. Open the hit window and dispatch "hit" synchronously. Close the window.
3. Apply the result:
   - `cancel`: return without calling the original.
   - Otherwise: save `P+0xc0`, `+0xc8`, `+0xd9` and `+0xdb`; write the override; call the original; restore.

Notes on this hook:
- `P` is shared by every entity of that weapon. The sim is single-threaded and the write lasts one call, so this is
  deterministic.
- Resolve the offsets with `weapons::Field(cls, "WormDamageMagnitude" | "ImpulseMagnitude" | "InstantKill" |
  "DamageIsPercentage")`, not hard-coded.

Every hook uses SafetyHook inline, `StartDisabled`, and is enabled per match only when a claim exists. Each one needs
a `mem::Expect` on the prologue bytes above. The prologue bytes were read from the exe on disk; check them at run
time, as the 0x705fb4 lesson showed.

**Ending the turn on demand.** Use data, not a call:
- set `RetreatTimeOverride = 0` (I32, `weapon:set` at match start) on the spade's base, so the turn ends
  PostLaunchDelay (1 s on the Bat) after the swing;
- anchor attacker and victim from the "hit" handler (`wum.sim.worm(s):anchor(true)`);
- arm `wum.sim.holdTurn` (the Sandtide pattern) at `GameLogic.Turn.Ended` or in the hit handler.

An optional `wum.sim.endTurn()` could send `Timer.TurnTimedOut`, as `level.test.endTurn` does from the match VM. The
retreat phase would need `Timer.RetreatTimedOut` instead. Online, the turn end goes through the active player's
validation protocol (see the WUM net time-sync notes). Sending these timers from every peer is untested, so leave
`endTurn` out of 0.16.0 unless a LocalNet run proves it.

**Stopping the base weapon's own effect.** No vanilla effect needs suppressing beyond the gate:
- A swallowed press never starts the swing, so there is no LaunchDelay, no `DecrementInventory`, no
  `Timer.EndTurn` and no retreat.
- Ammo is spent only at a real strike, so the scheme needs at least 1 Bat, or infinite.
- The Fire Punch and No More Nails specials (flight and nailing) depend on MeleeType 2 and 4, which the Bat base
  doesn't have.

## 6. Risks and checks for the implementation agent

1. **[verify] The swallow is enough.** Check that swallowing `Input.FirePressed` at `0x5ac6e0` stops the melee
   entity. It relies on the weapon getting input only through the worm's `+0x20` forward. No direct bus
   subscription of the melee entity was found (its handler is reached through the vtable), but it is unproven in
   game. Log `[melee] press swallowed (dig)` next to the absence of `Weapon.DisableWeaponChange`.
2. **[verify] Inputs while the worm is airborne or sliding.** Lua should only carve in state 0. The C++ gate still
   swallows, so a mid-air tap does nothing rather than swinging.
3. **[verify] AI.** CPU worms reach melee through `AI.IssueWormCommand` and `CAIPlanAttackBaseballBat`. If the AI
   fires through `Input.FirePressed` with a worm in reach, "auto" passes it to the strike. If it fires through
   another path, the gate never sees it, which is also fine (a vanilla strike, then the hit hook).
4. **Hit dispatch inside the engine.** Lua runs inside `0x567a40`, which runs inside the weapon Update. The
   `wum.sim.weapons` explosion dispatch is the precedent. Refuse `strike()`, `claim` and `holdTurn` re-entry from
   inside a hit handler if they misbehave.
5. **Multiple victims per swing.** The Bat hits every worm in the sweep. The once-per-match execution must pick
   the first "hit" and `cancel` or Shiv-damage the rest.
6. **`reach()` is one Update old.** It uses the pose at the last idle Update. That is deterministic, but it differs
   from the strike sweep by LaunchDelay (200 ms on the Bat). A worm that walks away gets a swing that misses, as in
   vanilla.
7. **`DoubleDamage`.** It still doubles an overridden damage. Lua can halve it, or accept the vanilla rule.
8. **The repurposed weapon is not only the spade.** Claiming `kWeaponBaseballBat` makes Kindjal's Nail Bat the spade.
   kindjal-heavy and kindjal-brutal also tune the Bat (`plugins/kindjal-heavy/mod/sim/main.lua:20`,
   `plugins/kindjal-brutal/mod/sim/main.lua:19`). A true melee clone needs separate RE: `kBases` in
   `src/weapons/manifest.cpp`, the melee name compare at `0x567a40`, and `0x5c68e0`/`0x5d6420`.
9. **LocalNet gate.**
   - A dig hold and a strike, each on A's turn and on B's turn.
   - Wormsign `melange.melee` and `mod.kindjal.env` equal on both machines.
   - No `NETWORK TIMES NOT IN SYNC` after a strike with `RetreatTimeOverride = 0`.

## Appendix: message-id globals used above

These come from the static initialisers (`push name; mov ecx, global; call 0x68bb9f`).

| Global | Message |
|---|---|
| `0x95deac` | Input.FirePressed |
| `0x95deb4` | Input.FireReleased |
| `0x95de8c` | Weapon.Delete |
| `0x95decc` | Weapon.DisableWeaponChange |
| `0x95ded4` | Weapon.EnableWeaponChange |
| `0x95dee4` | GameLogic.DecrementInventory |
| `0x95deec` | Weapon.PlayFireAnim |
| `0x95df1c` | Worm.OverridePhysics |
| `0x95df24` | Weapon.PlayWindupAnim |
| `0x95df2c` | Weapon.Wield |
| `0x95dea4` | Weapon.LaunchPayload |
| `0x95dedc` | Firepunch.End |
| `0x95d03c` | Timer.EndTurn (in `0x549ad0`) |
| `0x95d04c` | Timer.StartRetreatTimer (in `0x549bb0`) |
| `0x95d054` | Weapon.Fired (in `0x549bb0`) |
| `0x95cffc` | Weapon.Delete (in `0x549bb0`) |

The data keys are `Worm.WeaponDisableMovement` (`0x857ad8`), `Weapon.Melee.TargetInRange`, `Weapon.MinAimAngle`,
`Weapon.MaxAimAngle`, `RetreatTime` and `DefaultRetreatTime`.
