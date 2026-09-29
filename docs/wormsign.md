# Wormsign

Wormsign splits a match into 20 ms ticks (50 per second) and hashes the game state at the end of each one: the logic
RNG, the turn, the worms, the scheduled tasks, projectiles and teams. Two runs that agree on a tick's hash agree on
the game state at that tick. Recordings, replays and desync reports are built on these hashes.

## Replays

A recording (`Documents\Melange\replays\*.wsr`) holds what is needed to play a local match again: the seeds, the
random draws made in the menus before the match, every input with its time, the hash of every tick and a fingerprint
of the match setup. A replay plays the match again with those inputs and compares each tick with the recording.

1. Open the overlay panel *Wormsign/Replay* at the main menu and arm a recording.
2. Start a **Quick Game from the main menu**. The replay needs the same route into the match as the recording: the
   match setup is rebuilt from the recorded menu draws, not written directly.
3. At the first tick the setup (level, land, theme, scheme and teams) is checked against the recording. If it
   differs, the replay stops with "setup differs" and names the first difference. It never reports that as a
   divergence.
4. While the replay plays, your own inputs are ignored and the recorded ones are sent at their recorded times.
   Pause, change the speed (0.25x to 8x) or run to a tick. Running to a tick that has already passed needs a restart:
   *Restart* arms the recording again, and once you quit the match you start the Quick Game again.
5. The panel counts the ticks compared and matched. At the first tick that differs it shows "Diverged at tick N" and
   which parts of the state differ (*diff*). The replay keeps playing so you can watch what happens next.

Arming is refused while you are in a lobby or online, during a match, for a recording of an online match, for a
recording made with another game build, and for one made with different mod content (unless
`[Wormsign] ReplayAnyContent=1`). A recording whose hash version differs from this build's plays without being
compared.

The `wormsign.replay` test command does the same from scripts: `arm <file>`, `disarm`, `pause`, `resume`,
`speed <x>`, `runto <tick>`, `restart [tick]` and `status`.

| Setting (`[Wormsign]`) | Default | |
|---|---|---|
| `ReplayAnyContent` | 0 | 1 replays a recording made with different mod content |
| `ReplayPassLocal` | 0 | 1 lets local-only inputs (the camera) through while a replay plays; by default every live input is ignored |

## Record layouts in a `.wsr` file

The container (chunks, compression, index, recovery of truncated files) is described in `src/wormsign/format.h`.
These are the payloads of the record chunks, little-endian and packed:

| Chunk | Payload |
|---|---|
| `SEED` | `n × {u8 kind, u32 value, u32 caller, u32 t}`: `kind` 0 is the logic RNG, 1 the second RNG; `caller` is the return address of the seed call |
| `PDRW` | `n × {u8 rng, u32 ret, u32 stateAfter, u32 bits}`: the random draws made before the match, in order; `bits` is the result (an integer or float bits) |
| `INPT` | `n × {u8 type, u16 id, u32 a, u32 b, u32 time, u32 callT, u32 caller, u8 strLen, strLen bytes}`: `type` 0-5 is message, int, two ints, float, two floats, string; `a` and `b` are the values (float bits for floats); `time` is when the input takes effect and `callT` the logic time of the call |
| `TICK` | `u32 firstTick`, then records: `0` followed by `{u64 engine, u64 mods, u64 c[6], u32 rngLogic, u32 rng2, u16 fpucw, u16 inputs}` for one tick (the next tick number follows), or `1` followed by `u32 n` for `n` missing ticks |
| `SETP` | JSON: `v`, `level`, `landFile`, `landTheme`, `dataBank`, `timeOfDay`, `levelDetails`, `lastScheme`, `schemeName`, `scheme` and `init` (16-digit hex hashes of the scheme and team setup), `teams: [{name, worms}]` |

The `HEAD` chunk's `contributors` (an array of `{name, version}`) decides whether a replay also compares the `mods`
hash: only when the recording and the running game have the same set.
