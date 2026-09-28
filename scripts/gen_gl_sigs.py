"""Generates src/render/mirage/trace_sigs.inc from the Khronos OpenGL registry (gl.xml).

Usage: python scripts/gen_gl_sigs.py [--xml gl.xml] [--out src/render/mirage/trace_sigs.inc]
Without --xml the registry is downloaded from KhronosGroup/OpenGL-Registry. The output is committed, so builds
never need the network.

Argument codes (one per parameter, x86 stdcall):
  e enum, b bitfield, z boolean, i signed int, u unsigned int, f float, d double (2 dwords),
  l int64 (2 dwords), L uint64 (2 dwords), s intptr/sizeiptr, p pointer or handle.
An enum argument may be followed by {n}: an index into kGlSmallEnumGroups, used to name values below 16
(GL_POINTS vs GL_NONE vs GL_ZERO) that the global table cannot tell apart.

Payload rule (the pointer argument a capture copies while recording):
  kind 1  fixed element count     count = n
  kind 2  count from an argument  count = arg[countArg] * n
  kind 3  byte size from an arg   size and hash only, never contents (void pointers)
elem is the pointee type: f float, d double, i int32, u uint32, h int16, H uint16, c int8, C uint8, l int64,
L uint64. Payloads are capped at 1024 bytes.
"""
import argparse
import hashlib
import re
import sys
import urllib.request
import xml.etree.ElementTree as ET

URL = "https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/xml/gl.xml"

TYPE_CODE = {
    "GLenum": "e", "GLbitfield": "b", "GLboolean": "z",
    "GLint": "i", "GLsizei": "i", "GLshort": "i", "GLbyte": "i", "GLfixed": "i", "GLclampx": "i",
    "GLuint": "u", "GLubyte": "u", "GLushort": "u", "GLhalfNV": "u", "GLhandleARB": "u", "GLchar": "i", "GLcharARB": "i",
    "GLfloat": "f", "GLclampf": "f",
    "GLdouble": "d", "GLclampd": "d",
    "GLint64": "l", "GLint64EXT": "l", "GLuint64": "L", "GLuint64EXT": "L",
    "GLintptr": "s", "GLsizeiptr": "s", "GLintptrARB": "s", "GLsizeiptrARB": "s",
    "GLsync": "p", "GLeglImageOES": "p", "GLeglClientBufferEXT": "p", "GLvdpauSurfaceNV": "s",
    "GLDEBUGPROC": "p", "GLDEBUGPROCARB": "p", "GLDEBUGPROCKHR": "p", "GLDEBUGPROCAMD": "p", "GLVULKANPROCNV": "p",
}
ELEM = {
    "GLfloat": "f", "GLclampf": "f", "GLint": "i", "GLsizei": "i", "GLfixed": "i", "GLuint": "u", "GLenum": "u",
    "GLdouble": "d", "GLclampd": "d", "GLshort": "h", "GLushort": "H", "GLhalfNV": "H", "GLbyte": "c", "GLubyte": "C",
    "GLboolean": "C", "GLint64": "l", "GLuint64": "L", "GLint64EXT": "l", "GLuint64EXT": "L",
}
ELEM_BYTES = {"f": 4, "d": 8, "i": 4, "u": 4, "h": 2, "H": 2, "c": 1, "C": 1, "l": 8, "L": 8}
MAX_PAYLOAD = 1024

# WGL entry points the game imports from OPENGL32.dll or resolves through wglGetProcAddress.
WGL = [
    ("wglCreateContext", "p"), ("wglDeleteContext", "p"), ("wglMakeCurrent", "pp"), ("wglGetProcAddress", "p"),
    ("wglGetCurrentContext", ""), ("wglGetCurrentDC", ""), ("wglShareLists", "pp"), ("wglSwapBuffers", "p"),
    ("wglUseFontBitmapsA", "puuu"), ("wglUseFontBitmapsW", "puuu"), ("wglCopyContext", "ppu"),
    ("wglCreateLayerContext", "pi"), ("wglSwapLayerBuffers", "pu"), ("wglDescribePixelFormat", "piup"),
    ("wglGetPixelFormat", "p"), ("wglSetPixelFormat", "pip"), ("wglChoosePixelFormat", "pp"),
    ("wglSwapIntervalEXT", "i"), ("wglGetSwapIntervalEXT", ""), ("wglGetExtensionsStringARB", "p"),
    ("wglGetExtensionsStringEXT", ""), ("wglCreateContextAttribsARB", "ppp"), ("wglChoosePixelFormatARB", "pppupp"),
    ("wglGetPixelFormatAttribivARB", "piiupp"), ("wglGetPixelFormatAttribfvARB", "piiupp"),
]

VENDOR = re.compile(r"_(NVX?|AMD|ATI|SGI[SX]?|APPLE|INTEL|MESAX?|IBM|HP|SUNX?|3DFX|PGI|OML|INGR|REND|S3|WIN|GREMEDY|OVR|"
                    r"QCOM|IMG|ANGLE|ARM|VIV|DMP|FJ|HUAWEI)$")
SUFFIX_RANK = [("", 0), ("_ARB", 1), ("_KHR", 1), ("_EXT", 2), ("_OES", 3)]


def suffix_rank(name):
    for suf, r in SUFFIX_RANK[1:]:
        if name.endswith(suf):
            return r
    if VENDOR.search(name):
        return 4
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--xml")
    ap.add_argument("--out", default="src/render/mirage/trace_sigs.inc")
    a = ap.parse_args()
    data = open(a.xml, "rb").read() if a.xml else urllib.request.urlopen(URL).read()
    digest = hashlib.sha256(data).hexdigest()
    root = ET.fromstring(data)

    wanted = set()
    for f in root.iter("feature"):
        if f.get("api") not in ("gl", "glcore"):
            continue
        for r in f.iter("require"):
            for c in r.iter("command"):
                wanted.add(c.get("name"))
    for x in root.iter("extension"):
        sup = x.get("supported", "").split("|")
        if "gl" not in sup and "glcore" not in sup:
            continue
        for r in x.iter("require"):
            if r.get("api") not in (None, "gl", "glcore"):
                continue
            for c in r.iter("command"):
                wanted.add(c.get("name"))

    enums = {}
    group_small = {}
    for e in root.iter("enum"):
        v, name = e.get("value"), e.get("name")
        if not v or not name or e.get("api") not in (None, "gl") or e.get("type") in ("ull",):
            continue
        try:
            val = int(v, 0)
        except ValueError:
            continue
        if val < 0 or val > 0xFFFFFFFF:
            continue
        key = (suffix_rank(name), len(name), name)
        if val not in enums or key < enums[val][0]:
            enums[val] = (key, name)
        if val < 16:
            for g in (e.get("group") or "").split(","):
                if g:
                    cur = group_small.setdefault(g, {})
                    if val not in cur or key < cur[val][0]:
                        cur[val] = (key, name)

    group_ids = {}
    sigs = []
    for c in root.iter("command"):
        proto = c.find("proto")
        if proto is None:
            continue
        name = proto.find("name").text
        if name not in wanted:
            continue
        params = c.findall("param")
        codes, pay = "", None
        pnames = [p.find("name").text for p in params]
        for idx, p in enumerate(params):
            pt = p.find("ptype")
            ptype = pt.text if pt is not None else ""
            text = "".join(p.itertext())
            is_ptr = "*" in text
            if is_ptr:
                codes += "p"
                const = text.strip().startswith("const")
                ln = p.get("len")
                if const and ln and pay is None and text.count("*") == 1:
                    if ptype in ELEM:
                        el = ELEM[ptype]
                        if ln.isdigit():
                            pay = (idx, 1, -1, int(ln), el)
                        else:
                            m = re.fullmatch(r"(\w+)(?:\*(\d+))?", ln)
                            if m and m.group(1) in pnames:
                                pay = (idx, 2, pnames.index(m.group(1)), int(m.group(2) or 1), el)
                    elif ptype == "" and "void" in text:
                        m = re.fullmatch(r"(\w+)", ln)
                        if m and m.group(1) in pnames:
                            pay = (idx, 3, pnames.index(m.group(1)), 1, "C")
                continue
            code = TYPE_CODE.get(ptype)
            if code is None:
                print("unknown type", ptype, "in", name, file=sys.stderr)
                code = "u"
            codes += code
            if code == "e":
                g = p.get("group")
                if g and g in group_small:
                    if g not in group_ids:
                        group_ids[g] = len(group_ids)
                    codes += "{%d}" % group_ids[g]
        sigs.append((name, codes, pay))
    for name, codes in WGL:
        sigs.append((name, codes, None))
    sigs.sort(key=lambda s: s[0])

    out = []
    out.append("// Generated by scripts/gen_gl_sigs.py from gl.xml (sha256 %s). Do not edit." % digest)
    out.append("// {name, args, payloadArg, payloadKind, countArg, n, elem}; tables sorted for binary search.")
    out.append("static const GlSig kGlSigs[] = {")
    for name, codes, pay in sigs:
        if pay:
            ai, kind, ca, n, el = pay
            if kind == 1 and n * ELEM_BYTES[el] > MAX_PAYLOAD:
                n = MAX_PAYLOAD // ELEM_BYTES[el]
            out.append("    {\"%s\", \"%s\", %d, %d, %d, %d, '%s'}," % (name, codes, ai, kind, ca, n, el))
        else:
            out.append('    {"%s", "%s", -1, 0, -1, 0, 0},' % (name, codes))
    out.append("};")
    out.append("static const GlEnumName kGlEnums[] = {")
    for val in sorted(enums):
        out.append('    {0x%X, "%s"},' % (val, enums[val][1]))
    out.append("};")
    out.append("static const GlSmallEnum kGlSmallEnumGroups[] = {")
    for g, gid in sorted(group_ids.items(), key=lambda t: t[1]):
        for val in sorted(group_small[g]):
            out.append('    {%d, %d, "%s"},' % (gid, val, group_small[g][val][1]))
    out.append("};")
    with open(a.out, "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    print("%d commands, %d enums, %d small-value groups -> %s" % (len(sigs), len(enums), len(group_ids), a.out))


if __name__ == "__main__":
    main()
