# The media helper (holo-media.exe)

Melange 0.12 plays video for client mods ([`wum.media`](lua-api.md#wummedia)) in a separate 64-bit process,
`holo-media.exe`, so a crash in a decoder or the torrent engine can never end a match, and the 32-bit game spends only
about 25 MB of its address space on it. This page describes the media pack the helper comes in, how Melange starts and
checks it, and the protocol between the two. The protocol is defined by four headers that both builds compile:
`src/media/media_proto.h` (names, caps, timings), `src/media/pipe_codec.h` (the messages), `src/media/shm_logic.h`
(the frame section) and `src/media/url_logic.h` (which links are accepted). A change to any of them needs both sides
and a new protocol number.

## The media pack

The pack is downloaded once per machine, on the user's click in a mod's own panel, into `Documents\Melange\media\<ver>\`:

| File | What | Licence |
|---|---|---|
| `holo-media.exe` | The helper, built and Authenticode-signed by Melange's CI (`helper/holo-media`), libtorrent statically linked | MIT (libtorrent BSD, Boost BSL, OpenSSL Apache 2.0) |
| `libmpv-2.dll` | mpv and ffmpeg, the unmodified LGPL x86_64 build of a pinned zhongfly release | LGPL 2.1+ |
| `deno.exe` | The JavaScript runtime yt-dlp needs for YouTube | MIT |
| `LICENSES\*`, `SOURCES.md` | Every licence, and where each component's source is | |
| `pack.json` | The pack's own manifest | |

yt-dlp is not in the pack: it lives in its own folder `Documents\Melange\media\ytdlp\<ver>\yt-dlp.exe` and is updated
from a rolling manifest (`media-ytdlp.json`) at most once a day, or at once after yt-dlp reports a YouTube change. Both
come from `https://github.com/JaminB/melange/releases/download/media-...` only, over https. Every file's size and
SHA-256 are compiled into `melange.asi` (`src/media/pack_manifest.h`), so a Melange build accepts exactly one pack; the
yt-dlp manifest carries a serial number that may never go down. `[Media] AllowDownload=0` blocks both.

Installing streams the zip to disk with a running hash (a dropped download resumes), then extracts only the exact names
of the manifest, each checked against its size and hash, into a temporary folder that is renamed into place. No step
holds more than a few MB of memory, since the game is a 2 GB process.

## Starting the helper

Before every start Melange (on its media worker thread, never the game thread) opens `holo-media.exe`,
`libmpv-2.dll`, `deno.exe` and `yt-dlp.exe` so they cannot be changed or deleted, hashes them again, and checks that
`holo-media.exe` is signed by the same publisher as `melange.asi` (skipped, and logged, on an unsigned developer
build). `[Media] AllowCustomLibmpv=1` with `CustomLibmpvPath` loads a library of the user's choice instead of the pack's
`libmpv-2.dll`, unchecked (the LGPL's relinking right); Melange logs that it did.

The helper is started suspended, with no inherited handles, in a job object that kills it (and the yt-dlp and deno
processes it starts) when the game exits, then resumed. It runs at Low integrity (`[Media] HelperLowIntegrity=1`): it can
write only its own folder `%USERPROFILE%\AppData\LocalLow\Melange\media-cache\<pid>-<random>\` (`[Media] CacheDir`), and
its environment is rebuilt from scratch (no `PYTHON*`, `YTDLP*` or `MPV_HOME` from the user's). Command line:

```
holo-media.exe --pipe \\.\pipe\melange-holo-<pid>-<rand64> --parent <game pid> --cache <folder>
               --libmpv <path> --ytdlp <path> --deno <path> [--sweep]
```

`--sweep` (the first start in a game session only) deletes the cache folders of helpers that are no longer running.
The helper exits when the game's process ends or on `quit`, never because pings stop.

mpv runs with no configuration, scripts or user files (`config=no`, an empty `config-dir`, `load-scripts=no` and every
built-in script off, no input bindings or IPC, `access-references=no`, `ordered-chapters=no`, no automatic subtitle or
audio files), `vo=libmpv` with the software renderer, `hwdec` from `[Media] Hwdec`, and `ytdl` only for YouTube items.
yt-dlp options are built inside the helper from a fixed list (`ignore-config`, `no-plugin-dirs`, `no-exec`,
`no-cache-dir`, `use-extractors=youtube,youtube:tab`, deno as the JavaScript runtime, and the subtitle language when it
is a plain language code); nothing a mod or another player sends ever becomes a yt-dlp or mpv option. URLs are rebuilt
from the parsed video or playlist id on both sides.

## The pipe

Melange creates the pipe (first instance, local clients only, current-user access) and accepts only the helper's own
process id. Each line is one JSON object of at most 64 KB, named by `"t"`; a longer line is dropped whole, and every
message is range-checked and capped on receipt (each side treats the other's lines as untrusted). Melange reads the pipe
on its own thread and writes it from another; the game thread never waits on the pipe.

Melange to the helper:

| `t` | Fields | |
|---|---|---|
| `hello` | `proto`, `hwdec`, `audioDelayMs`, `maxHeight` | first line |
| `open` | `id` (player), `gen` (load), `shm` (the player's frame section), `url` or `bt {ih, index}`, `start`, `paused`, `volume`, `mute`, `speed`, `height`, `alang`, `slang` | loads an item (again, for a quality change) |
| `set` | `id`, `prop` (`pause`, `volume`, `mute`, `speed`, `aid`, `sid`), `value` | |
| `seek` | `id`, `pos` (seconds), `mode` (`exact`, `keyframes`, `relative`) | |
| `size` | `id`, `w`, `h` | the drawn size, at most 1280x720 |
| `close` | `id` | |
| `resolve` | `rid`, `url` | a playlist or video, through yt-dlp |
| `bt.add` | `ih`, `magnet`, `uploadKBps`, `upnp`, `port` | |
| `bt.select` | `ih`, `index` | |
| `bt.remove` | `ih` | closes the streams, removes the torrent and its files |
| `ping` | | every 2 s, from the writer thread |
| `quit` | | |

The helper to Melange:

| `t` | Fields | |
|---|---|---|
| `hello` | `proto`, `mpv`, `ffmpeg`, `libtorrent`, `hwdec` | first line |
| `prop` | `id`, `gen`, `name`, `value`, `qpc` | `time-pos` and `audio-pts` (at most 10 a second, with the helper's QueryPerformanceCounter time in ms, which Melange uses to extrapolate), `pause`, `paused-for-cache`, `duration`, `track-list`, `demuxer-cache-time`, `eof-reached`, `video-params`, `media-title`, `speed` |
| `event` | `id`, `gen`, `name` (`file-loaded`, `end-file`), `reason` (`eof`, `error`, `stop`) | |
| `error` | `id`, `gen`, `rid`, `ih`, `code`, `message` | `load`, `shm`, `proto`, `torrent`, `disk`, `internal` |
| `resolved` | `rid`, `title`, `items [{url, id, title, duration}]`, or `error` (`ytdlp`, `network`, `private`, `timeout`) and `message` | |
| `bt.files` | `ih`, `name`, `files [{index, path, size, video}]` | once the metadata arrived |
| `bt.status` | `ih`, `down`, `up`, `peers`, `progress`, `have`, `metadata` | once a second |
| `bt.removed` | `ih` | |
| `pong` | | |

Melange keeps only the latest value of each property, and at most 256 other messages, so a stalled game thread never
blocks the helper. A level load can stall the game for many seconds, so the watchdog runs on the pipe threads: the
helper is restarted only after 6 seconds without a `pong` while its process is alive, at most once a minute, and the
player is then reopened where it was.

## The frame section

Each player has a section `Local\melange-holo-<pid>-<random>-<id>` that Melange creates (current-user access, Low
label): a 128-byte header and three slots of 1280x720 BGR0 pixels from offset 4096, about 11 MB. The header holds
`magic` ("HMF1"), `version`, the size limits, `latest` (slot plus a sequence number), `reading` (the slot Melange is
copying) and each slot's `{w, h, stride, fmt, frameNo, pts}`. The helper renders into the slot that is neither
`latest`'s nor `reading`'s and then publishes it; Melange announces the slot it reads and re-checks `latest`, so the
helper never writes a slot being copied. Melange trusts nothing in the header: it uses its own limits, and copies a
frame only if `1 <= w <= 1280`, `1 <= h <= 720` and `stride = 4 * w`; anything else is skipped and counted.

Melange uploads a new frame only for a player that was drawn in the last 250 ms, through a pixel buffer object into a
texture allocated once at 1280x720 (about 0.3 ms at 960x540 on the game thread).

## Torrents

libtorrent adds a magnet with every file at priority 0; the chosen file then downloads sequentially, with the start
and the last 2 MB first (an MP4's index) and about 30 seconds read ahead. mpv reads it through a stream callback, with
no local web server. The magnets are canonical (infohash, name and at most 8 udp/http/https trackers; web seeds, direct
peers and other parameters are dropped), url seeds from the metadata are removed, and trackers in loopback, link-local
or private address ranges are ignored. DHT, local peer discovery and trackers are on; UPnP and NAT-PMP are off
(`[Media] TorrentUPnP`). Data lives under the helper's own cache folder and is deleted when the torrent is closed, when
the party ends, and at the next start after a crash. Every file any open torrent has selected, now or earlier (a
deselected file keeps its data until the torrent closes), counts against `[Media] TorrentCacheMB`, together with the
file being opened.

## Tests

`media_selftest` (the core, x86) and `holo_media_shared_test` (the helper project, x64) run the same cases against the
shared headers: the accepted and refused links, magnet canonicalisation, the frame section's checks and a two-thread
triple-buffer run, and every pipe message's round trip, hostile values and a mutation fuzz. `holo-media.exe --selftest`
plays `av://lavfi:testsrc` into a private section in CI.
