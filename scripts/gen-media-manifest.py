#!/usr/bin/env python3
"""Writes src/media/pack_manifest.h (or a dev build's media_dev_manifest.h) from a media pack zip.

The header pins exactly one pack into melange.asi: the zip's URL, size and SHA-256, and every file's path, size and
SHA-256 (pack.json included). This script is also the reference reader of pack.json, the file CI's
scripts/media-pack.ps1 writes into the zip:

    {"format": "melange-media-pack-1", "version": "1",
     "files": [{"path": "holo-media.exe", "size": 123, "sha256": "<64 lowercase hex>"}, ...]}

`files` names every other file of the pack. The script checks the zip against it (the same names, nothing else, the
same sizes and hashes) and applies the rules melange.asi applies (src/media/pack_logic.h CheckManifest), so a pack it
accepts is a pack the build accepts.

Release (C7, after the user publishes media-v<ver>, R1):
    py -3 scripts/gen-media-manifest.py --zip melange-media-1.zip \
        --url https://github.com/JaminB/melange/releases/download/media-v1/melange-media-1.zip
Dev pack (scripts/make-dev-pack.ps1 runs this): --dev --assemble <staged folder> --version dev-<n> --zip <out.zip>
    --url file:///C:/.../pack.zip --lane-url file:///.../media-ytdlp.json --out build/x86-release/generated/media_dev_manifest.h
    (--assemble writes the folder's pack.json and zips the folder first.)
Standard library only.
"""
import argparse
import hashlib
import json
import math
import os
import re
import sys
import zipfile

PACK_FORMAT = "melange-media-pack-1"
ALLOWED_PREFIX = "https://github.com/JaminB/melange/releases/download/media-"
DEFAULT_LANE = "https://github.com/JaminB/melange/releases/download/media-ytdlp/media-ytdlp.json"
REQUIRED = ("holo-media.exe", "libmpv-2.dll", "deno.exe", "pack.json")
MAX_FILES = 64
MAX_FILE = 512 << 20
MAX_PACK = 1 << 30
MAX_ZIP = 768 << 20
SEGMENT = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
DEVICES = {"con", "prn", "aux", "nul", "conin$", "conout$"} | {f"com{i}" for i in range(1, 10)} | {f"lpt{i}" for i in range(1, 10)}
VERSION = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,31}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")


def fail(msg):
    sys.exit(f"gen-media-manifest: {msg}")


def safe_path(p):
    if not p or len(p) > 160:
        return False
    segs = p.split("/")
    if len(segs) > 3:
        return False
    for s in segs:
        if not SEGMENT.match(s) or s[0] == "." or s[-1] == "." or s.split(".")[0].lower() in DEVICES:
            return False
    return True


def valid_version(v):
    return bool(VERSION.match(v)) and not v.endswith(".") and ".." not in v and v.lower() not in ("ytdlp", "dl", "cache")


def allowed_url(u, allow_file):
    if any(ord(c) <= 0x20 or ord(c) >= 0x7F or c == "\\" for c in u):
        return False
    if allow_file and u.lower().startswith("file:///"):
        return ".." not in u
    if not u.startswith(ALLOWED_PREFIX) or len(u) == len(ALLOWED_PREFIX):
        return False
    rest = u[len(ALLOWED_PREFIX):]
    return ".." not in rest and "//" not in rest and re.match(r"^[A-Za-z0-9._/-]+$", rest) is not None


def sha_stream(f):
    h = hashlib.sha256()
    n = 0
    for block in iter(lambda: f.read(1 << 20), b""):
        h.update(block)
        n += len(block)
    return n, h.hexdigest()


def read_pack(zip_path):
    with zipfile.ZipFile(zip_path) as z:
        infos = [i for i in z.infolist() if not i.is_dir()]
        names = [i.filename for i in infos]
        if len(set(names)) != len(names):
            fail("the zip has a duplicate entry")
        if "pack.json" not in names:
            fail("the zip has no pack.json")
        for i in infos:
            if i.flag_bits & 0x1:
                fail(f"{i.filename}: encrypted")
            if i.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                fail(f"{i.filename}: only stored or deflate entries are allowed")
        for i in z.infolist():
            if i.is_dir() and not any(n.startswith(i.filename) for n in names):
                fail(f"{i.filename}: a folder entry with no file under it")
        pj = json.loads(z.read("pack.json").decode("utf-8"))
        if pj.get("format") != PACK_FORMAT:
            fail("pack.json: unknown format")
        version = pj.get("version")
        if not isinstance(version, str) or not valid_version(version):
            fail("pack.json: bad version")
        listed = pj.get("files")
        if not isinstance(listed, list):
            fail("pack.json: no file list")
        files = {}
        for e in z.infolist():
            if e.is_dir():
                continue
            with z.open(e) as f:
                n, h = sha_stream(f)
            files[e.filename] = (n, h)
        want = {}
        for f in listed:
            p, size, h = f.get("path"), f.get("size"), f.get("sha256")
            if not isinstance(p, str) or not safe_path(p) or p.lower() == "pack.json":
                fail(f"pack.json: bad path {p!r}")
            if not isinstance(size, int) or isinstance(size, bool) or size < 0 or not isinstance(h, str) or not HEX64.match(h):
                fail(f"pack.json: bad size or sha256 for {p}")
            want[p] = (size, h)
        for p, (size, h) in want.items():
            if p not in files:
                fail(f"{p}: in pack.json but not in the zip")
            if files[p] != (size, h):
                fail(f"{p}: the zip's file differs from pack.json")
        extra = set(files) - set(want) - {"pack.json"}
        if extra:
            fail(f"not in pack.json: {', '.join(sorted(extra))}")
    out = [(p, files[p][0], files[p][1]) for p in sorted(files, key=lambda s: (s.lower() != "holo-media.exe", s.lower()))]
    return version, out


def check(version, files, zip_size):
    if len(files) > MAX_FILES:
        fail("too many files")
    lower = [p.lower() for p, _, _ in files]
    if len(set(lower)) != len(lower):
        fail("two files differ only in case")
    for p, size, _ in files:
        if not safe_path(p):
            fail(f"unsafe path {p}")
        if size > MAX_FILE:
            fail(f"{p}: larger than 512 MiB")
        for q in lower:
            if q != p.lower() and q.startswith(p.lower() + "/"):
                fail(f"{p} is also a folder")
    if sum(s for _, s, _ in files) > MAX_PACK:
        fail("the pack is larger than 1 GiB unpacked")
    if not 0 < zip_size <= MAX_ZIP:
        fail("the zip is empty or larger than 768 MiB")
    for r in REQUIRED:
        if r not in lower:
            fail(f"the pack has no {r}")


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def render(version, url, lane_url, zip_size, zip_sha, files, dev, source):
    mib = 1 << 20
    download_mb = max(1, math.ceil(zip_size / mib))
    disk_mb = max(1, math.ceil(sum(s for _, s, _ in files) / mib))
    rows = "\n".join(f"    {{{c_str(p)}, {s}ull, {c_str(h)}}}," for p, s, h in files)
    what = "a DEVELOPER pack (scripts/make-dev-pack.ps1); never in a release" if dev else f"the published pack media-v{version}"
    body = f"""namespace melange::media::manifest {{
struct File {{
    const char* path;    // inside the zip and the pack folder, '/' separated, e.g. "holo-media.exe", "LICENSES/mpv.txt"
    uint64_t size;
    const char* sha256;  // lowercase hex
}};

inline constexpr bool kHavePack = true;
inline constexpr bool kDev = {"true" if dev else "false"};
inline constexpr const char kPackVersion[] = {c_str(version)};
inline constexpr const char kZipUrl[] = {c_str(url)};
inline constexpr uint64_t kZipSize = {zip_size}ull;
inline constexpr const char kZipSha256[] = {c_str(zip_sha)};
inline constexpr uint32_t kDownloadMB = {download_mb};  // shown in the consent text
inline constexpr uint32_t kDiskMB = {disk_mb};
inline constexpr File kFiles[] = {{
{rows}
}};
inline constexpr const char kYtdlpLaneUrl[] = {c_str(lane_url)};
}}  // namespace melange::media::manifest
"""
    head = f"""#pragma once
// GENERATED by scripts/gen-media-manifest.py from {source} ({what}). Do not edit.
#include <cstdint>
"""
    if dev:
        return head + "\n" + body
    # The release header yields to a dev build's generated media_dev_manifest.h (build/<cfg>/generated, written only by
    # scripts/make-dev-pack.ps1, so CI's clean builds never see one).
    return (head + '\n#if __has_include("media_dev_manifest.h")\n#include "media_dev_manifest.h"\n#else\n' + body +
            "#endif\n")


def assemble(folder, version, zip_path):
    """Writes <folder>/pack.json for every other file of the folder and zips the folder ('/' names, deflate)."""
    if not valid_version(version):
        fail(f"bad version {version!r}")
    files = []
    for base, dirs, names in os.walk(folder):
        dirs.sort()
        for n in sorted(names):
            full = os.path.join(base, n)
            rel = os.path.relpath(full, folder).replace(os.sep, "/")
            if rel == "pack.json":
                continue
            if not safe_path(rel):
                fail(f"unsafe file name in the staged pack: {rel}")
            with open(full, "rb") as f:
                size, h = sha_stream(f)
            files.append({"path": rel, "size": size, "sha256": h})
    pj = {"format": PACK_FORMAT, "version": version, "files": files}
    with open(os.path.join(folder, "pack.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(pj, f, indent=1)
        f.write("\n")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for e in files + [{"path": "pack.json"}]:
            z.write(os.path.join(folder, *e["path"].split("/")), e["path"])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--zip", required=True, help="the pack zip (its pack.json is read and checked)")
    ap.add_argument("--url", required=True, help="where melange.asi downloads the zip")
    ap.add_argument("--lane-url", default=DEFAULT_LANE, help="media-ytdlp.json")
    ap.add_argument("--dev", action="store_true", help="a developer pack: file:/// URLs allowed, kDev = true")
    ap.add_argument("--out", default="src/media/pack_manifest.h")
    ap.add_argument("--assemble", help="dev only: write this staged folder's pack.json and zip it to --zip first")
    ap.add_argument("--version", help="with --assemble: the pack version")
    a = ap.parse_args()
    if a.assemble:
        if not a.dev or not a.version:
            fail("--assemble needs --dev and --version")
        assemble(a.assemble, a.version, a.zip)
    if not allowed_url(a.url, a.dev):
        fail(f"--url must start with {ALLOWED_PREFIX}" + (" or file:///" if a.dev else ""))
    if not allowed_url(a.lane_url, a.dev):
        fail(f"--lane-url must start with {ALLOWED_PREFIX}" + (" or file:///" if a.dev else ""))
    version, files = read_pack(a.zip)
    with open(a.zip, "rb") as f:
        zip_size, zip_sha = sha_stream(f)
    check(version, files, zip_size)
    text = render(version, a.url, a.lane_url, zip_size, zip_sha, files, a.dev, a.zip.replace("\\", "/").split("/")[-1])
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"Wrote {a.out}: pack {version}, {len(files)} files, zip {zip_size} bytes")


if __name__ == "__main__":
    main()
