# Melange GL capture format (.mcap v1)

A `.mcap` file is one to eight consecutive frames of the game's OpenGL calls, recorded by Melange's `MirageTrace`
module. It is a plain zip archive; every file inside is JSON, JSON Lines, PNG or text, so a tool can read it without
any Melange code.

Captures are written to `Documents\Melange\captures\<local date>_<time>_f<frame>.mcap` (or `[MirageTrace] CaptureDir`)
by the overlay button "Capture frame" in the "Mirage/GL" panel, the hotkey `[MirageTrace] CaptureHotkey`
(`Ctrl+Shift+F9` by default), the menu item "Mirage/Capture GL frame", the test command `gltrace.capture [frames]
[notex]`, or `melange::gltrace::RequestCapture()`. A capture contains the game's textures: keep it on your PC.
Captures are never added to "Save logs" exports.

## Layout

```
manifest.json          what was captured, counts, and the list of files
calls.jsonl            one line per GL call, in call order
events.jsonl           markers placed between calls: frames, render passes, scene stages, Cg program binds, notes
state/begin.json       GL state at the start of the first frame
state/end.json         GL state at the end of the last frame
programs/index.json    every Cg program the engine has created
programs/<n>.asm       compiled program text (ARB assembly on most drivers)
textures/index.json    every 2D texture bound during the capture
textures/<gl>.png      level 0 of that texture
frame.png              the back buffer at the end of the last frame, before the Melange overlay
```

`programs/*` is missing when the capture was made without shaders, `textures/*` without textures, and `frame.png`
without the frame image (see `options` in the manifest). Every `.json` file is an object with
`"format":"melange-capture"` and `"version":1`; the `.jsonl` files carry these only in `manifest.json`.

A reader must ignore members it does not know. A later version that changes the meaning of an existing member will
raise `version`.

## manifest.json

| Member | Meaning |
|---|---|
| `format`, `version` | `"melange-capture"`, `1` |
| `melangeVersion` | the Melange build that wrote the file |
| `exeSha256`, `exeBuild` | the game executable |
| `gl` | `vendor`, `renderer`, `version` strings |
| `window` | `w`, `h`: the game window's client size in pixels |
| `frames` | the frame numbers recorded (Melange's frame counter, one per presented frame) |
| `scene` | `"menu"` or `"match"` (or a label forced with `gltrace.scene`) |
| `counts` | `calls`, `draws`, `textures` (PNG files written), `programs`, `programsWithAsm`, `payloads`, `events` |
| `options` | the request: `frames`, `textures`, `shaders`, `frameImage`, `bufferSizes`, `maxTextureMB` |
| `files` | every file in the archive, including `manifest.json` |
| `droppedRecords` | calls lost because the recording ring overflowed within one frame (normally 0) |
| `droppedPayloads` | payloads not stored because the 32 MB payload budget was used up |
| `readbackMs` | how long the game paused at the end of the capture to read back state, shaders, textures and the frame |
| `notes` | free-text remarks, for example texture binds of other targets that were not read back |

`counts.calls` equals the number of lines in `calls.jsonl`.

## calls.jsonl

One JSON object per line:

```json
{"i":0,"f":11615,"fn":"glBindFramebuffer","src":"exe-proc","caller":"0x6f64be","pass":1,
 "args":[36160,1],"text":"GL_FRAMEBUFFER, 1","payload":null,"tsc":123456}
```

| Member | Meaning |
|---|---|
| `i` | index of the call in the capture, from 0 |
| `f` | frame number |
| `fn` | the GL or WGL function name |
| `src` | how the game reached it: `exe-import` (the executable's import table), `exe-proc` (a pointer the executable got from `wglGetProcAddress`), `cggl-import`, `cggl-proc` (the same for NVIDIA's `cgGL.dll`, which issues most shader parameter uploads) |
| `caller` | the return address of the call, as a hex string |
| `pass` | the engine's render pass at the time: 0 none, 1 shadow, 2 picture-in-picture, 3 main |
| `args` | the decoded arguments, see below |
| `text` | the same arguments in readable form: enum names (`GL_TRIANGLES`), `GL_TRUE`/`GL_FALSE`, hex bitfields, `NULL` |
| `payload` | data behind a pointer argument, see below; `null` when none was recorded |
| `tsc` | the CPU time stamp counter when the call was made |
| `raw` | `true` when the function has no known signature: `args` then holds the first 8 stack dwords as unsigned numbers |
| `truncated` | `true` when the function takes more than 8 dwords of arguments: `args` holds only those that fit |
| `bytes` | for `glTexImage1D/2D` and `glTexSubImage1D/2D`: the size of the pixel data the call reads from memory |

Argument encoding in `args`, by parameter type:

| Type | JSON |
|---|---|
| `GLenum`, `GLbitfield`, `GLuint`, `GLboolean`, and the other unsigned types | number |
| `GLint`, `GLsizei`, `GLintptr`, `GLsizeiptr`, and the other signed types | number |
| `GLfloat`, `GLdouble` | number, or the string `"nan"`, `"inf"`, `"-inf"` |
| `GLint64`, `GLuint64` | number (may exceed 2^53) |
| pointers, `GLsync`, handles | string `"0x…"` |

Signatures come from the Khronos OpenGL registry (`gl.xml`) through `scripts/gen_gl_sigs.py`.

### Payloads

For calls with a `const` pointer argument whose size the registry defines, the data at the pointer is copied when the
call is made:

- a JSON array of the elements (floats, integers) for matrices (`glLoadMatrixf`: 16 floats), vectors
  (`*4fv`: 4 floats), and counted arrays (`glNamedProgramLocalParameters4fvEXT`: `count` × 4 floats);
- `{"data":[…],"capped":true}` when the data is longer than 1024 bytes (the array holds the first 1024 bytes);
- `{"bytes":N,"hash":"<sha256 hex>"}` for untyped buffers (`glBufferData`, `glBufferSubData`, `glProgramStringARB`,
  …): the size the call names and the SHA-256 of its first 64 bytes. Buffer contents are never stored. With
  `options.bufferSizes` false these are left out.

## events.jsonl

One JSON object per line, each with `at` (the index of the call the event precedes; the number of calls for events
after the last call) and `type`:

| `type` | Members | Meaning |
|---|---|---|
| `frame-begin`, `frame-end` | `frame` | frame boundaries |
| `pass` | `pass`, `frame` | the render pass changed (see `pass` above) |
| `stage` | `stage`, `frame` | a Melange scene stage ran here: `World`, `WorldLate`, `PostWorld`, `Hud`, `Final` |
| `cg-bind` | `target`, `arb`, `program`, `frame` | `glBindProgramARB`; `program` is `"<file>:<entry>"` of the Cg program that owns that ARB program, or empty |
| `note` | `text` | a remark from the recorder |

## state/begin.json, state/end.json

`{"format":"melange-capture","version":1,"which":"begin"|"end","frame":N,"values":{…}}`. `values` holds about 200
GL state values by name: enables, blend, depth, stencil and alpha-test state, viewport and scissor, bindings
(framebuffers, buffers, ARB and GLSL programs), the modelview, projection and texture matrices, per-unit texture
bindings (`textureUnits`), client arrays and pixel-store settings. Numbers are GL enum values or plain values;
matrices are 16-element arrays in OpenGL (column-major) order. Values a driver does not support are omitted or 0.

## programs/index.json

`{"format":"melange-capture","version":1,"programs":[…]}`, each entry:

| Member | Meaning |
|---|---|
| `n` | index; `programs/<n>.asm` holds its compiled text |
| `file`, `entry` | Cg source file and entry function, e.g. `Landscape.cg`, `LandscapeFragmentMain` |
| `stage` | `"vertex"` or `"fragment"` |
| `source` | the path the engine loaded, e.g. `CG/Landscape.cg` |
| `cgProgram` | the Cg runtime handle |
| `arbName` | the GL program name Cg uses (matches `glBindProgramARB` arguments), 0 if none |
| `binds` | how often the engine bound it since it was loaded |
| `failed` | the engine could not compile it |
| `overridden`, `glsl`, `owner` | set when a Melange shader override or replacement is active (`owner` = `"builtin"` or a mod id) |
| `asm` | the path of the `.asm` file, or `""` |

## textures/index.json

`{"format":"melange-capture","version":1,"textures":[…]}`, each entry:

| Member | Meaning |
|---|---|
| `gl` | the GL texture name |
| `w`, `h`, `levels` | size of level 0 and the number of mip levels |
| `internalFormat` | the GL internal format |
| `depth` | a depth texture: the PNG is 16-bit greyscale (the depth value scaled to 0..65535) |
| `name` | the engine's image name when Melange saw the upload (after a texture dump), else `""` |
| `file` | `textures/<gl>.png`, or `""` when it was not read back |
| `skipped` | why it was not read back: `deleted`, `not a 2D texture`, `maxTextureMB reached`, `glGetTexImage failed` |

Colour textures are 8-bit RGBA PNGs. All images, `frame.png` included, are stored top row first (flipped from
OpenGL's bottom-up order), so they look upright in an image viewer. `frame.png` has alpha forced to 255.
