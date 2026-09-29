# Sieve (`xomtool`) and the XOM format

Worms Ultimate Mayhem's data files — weapon stats, meshes, textures, sound banks — are all one container
format, `.xom` ("MOIK"). Sieve is Melange's toolchain for it: a portable reader/writer library
(`src/xom` in C++, `tools/xom/xom.py` in Python, kept byte-identical) plus a command-line tool built on it.

**Status in this build:** the library and its JSON model are real and used by Melange itself (weapon field
offsets and types come from the same schema, `src/xom/xom_schema.inc`). The full `xomtool` command-line
program — `unpack`/`pack`/`convert`/`bank`/`diff`/`report`, image and mesh conversion, and the C++/Python
parity CLI — is **not built yet**; the JSON model and command list below are the design it will implement, not
a tool you can run today. Until then, `tools/xom/xom.py` is the one thing that already works from the command
line, and it's how `dist/Mods/mega-bazooka`'s weapon stats can be cross-checked against `WEAPTWK.XOM` if you
want to see the JSON model for yourself.

## What already works: `tools/xom/xom.py`

Pure Python 3, no dependencies, using only your own local copy of the game's files (nothing about this tool
ships or requires any game asset):

```
python tools\xom\xom.py xom2json <in.xom> [out.json]   # a full JSON dump, byte-exact on the way back
python tools\xom\xom.py json2xom <in.json> <out.xom>   # the reverse
python tools\xom\xom.py dump <in.xom>                  # a short, human-readable listing
```

`import xom; doc = xom.load(path)` gives the same document as a Python `dict` for scripting; `xom.dumps(doc)`
serialises it back, byte-identical for anything you didn't change. This is the foundation the full `xomtool`
below is built on — the round trip it guarantees is exactly what the CLI's `unpack`/`pack` will wrap.

**Verified against a real install.** The C++ side of the same library (`src/xom`, built standalone per its
`CMakeLists.txt`) round-trips **721/721** of a retail build's `.xom`/`.xan` files byte-for-byte, with 0 opaque
(undecoded) objects across 134,764 decoded objects total (`xom_roundtrip --game <dir> --bundles`, run read-only
against the installed game — nothing here is shipped). Its edit test on the real `Data\Tweak\WEAPTWK.XOM`
confirms the schema and offsets this page and [weapons.md](weapons.md) describe:

```
edit test: kWeaponBazooka WormDamageMagnitude 50 -> 75, 1 byte(s) changed
ok     Data\Tweak\WEAPTWK.XOM (197 objects)
```

(50 is the vanilla Bazooka's own `WormDamageMagnitude` — the number [weapons.md](weapons.md) doubles for the
sample mod.)

## The JSON model (`melange-xom/1`)

Both the Python and (once shipped) C++ tools read and write the same JSON shape:

```json
{
  "format": "melange-xom/1",
  "header": { "...": "fields that are not recomputed on save" },
  "types": [ { "name": "...", "version": 1, "count": 1 } ],
  "root": 1,
  "objects": [ { "type": "...", "fields": { "FieldName": 1.0 } } ]
}
```

An object's `fields` are named and typed by the compiled-in schema (`src/xom/xom_schema.inc`, generated from
`tools/xom/schema.json`); an object the schema can't decode keeps its raw bytes instead of `fields`, so a
round trip never loses data even for a class Sieve doesn't understand yet.

## The planned `xomtool` CLI

Once Component D ships, `tools\xomtool.exe` (C++, built by `build.ps1`) and `tools/xom/xomtool.py` (the Python
parity CLI) will offer the same subcommands, with these exit codes: `0` ok, `1` usage, `2` input error, `3`
write refused. Every error will name the object index and the field.

| Subcommand | What it will do |
|---|---|
| `unpack <in.xom> <outdir>` (`--split`) | The JSON model above; `--split` also writes one file per top-level named resource plus an `index.json`. |
| `pack <indir> <out.xom>` | The reverse of `unpack`, from either form. |
| `inspect <in.xom>` | A short listing, like `xom.py dump` above. |
| `diff <a.xom> <b.xom>` | Every field that differs: object, path, old → new value. |
| `convert <in.(tga\|png)> <out.(png\|tga)>` (`--mips=N`) | XImage ↔ PNG, with rows flipped (XImage is bottom-up, PNG top-down) and channel order kept; mips regenerated with a 2×2 box filter unless `--mips=1`. |
| `convert <in.gltf\|glb> <out.xom>` (`--material-from <ResourceId>`) | A static mesh (glTF 2.0) into an `XMeshDescriptor` graph, with the shader subtree copied from a template mesh and its texture replaced by the glTF's base-colour image if one is given. One primitive per `XShape`, u16 indices only (over 65535 vertices is refused). |
| `bank --from <src.xom> --object <Name> --as <NewName> --set <Field>=<value> --out <bank.xom>` | A one- (or multi-) entry `XDataBank` built from a base container plus field overrides — this is exactly what a `weapons` manifest's `bank` field points at. |
| `report <in.xom>` | A human-readable summary (used for the weapons container report while building M5). |

**A limit that already affects mod authors today:** even once meshes convert cleanly offline, the game's
`LoadBank` only accepts an `XDataBank`, and a data bank has no mesh list — so a converted mesh can't be loaded
into a match yet, only inspected and round-tripped offline. See [weapons.md](weapons.md#meshes-and-sounds).

## Parity and safety

The C++ and Python implementations will be checked byte-for-byte against each other over every file the game
ships (943 `.xom`/`.xan` files) by `tools/xom/parity_test.py`, plus every hand-built conversion fixture. Both
readers bound-check every offset and index they follow, cap input sizes (PNG ≤ 4096², XOM ≤ 64 MB, glTF
buffers ≤ 64 MB), and never resolve a path outside the file (or mod folder) they were given.
