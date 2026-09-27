#!/usr/bin/env python3
"""
xom.py - reader/writer for Worms Ultimate Mayhem "MOIK" XOM container files.

Pure Python 3, no dependencies.  Format reference: re/notes/framework/xom-format.md.

    python xom.py xom2json <in.xom> [out.json]
    python xom.py json2xom <in.json> <out.xom>
    python xom.py dump <in.xom>            # short human-readable listing

Library use:
    import xom
    doc = xom.load('WEAPTWK.XOM')          # -> dict (JSON-able)
    blob = xom.dumps(doc)                  # -> bytes (byte-identical for untouched docs)

The JSON model
--------------
{
  "format": "wumfix-xom/1",
  "header": {...fields that are not recomputed...},
  "types": [ {"name", "version", "guid", "extra"}, ... ],   # file TYPE table, in order
  "guid_rec": [a, b, c], "schm_rec": [a, b, c],
  "strings": ["", "FE.MenuTextColour", ...],               # STRS table, index = string id
  "root": 5,                                               # 1-based object index
  "objects": [                                             # 1-based index = position + 1
     {"type": "XColorResourceDetails", "iflags": 0, "uflags": 0, "dxcount": 0,
      "fields": {"Value": [255,251,217,255], "Name": "FE.MenuTextColour", "Flags": 96}},
     {"type": "SomethingUnknown", "raw": "00 00 00 ..."},   # opaque (schema missing/failed)
  ]
}
Field values: numbers / bools; strings as text; object references as
{"ref": n} (n = 1-based object index, 0 = null); math structs (Vector3f,
Color4ub, Matrix4f, ...) as lists; arrays as lists.  Non-finite floats are
written as {"f32": "<hex bits>"} so the round trip stays exact.
"""
import json
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FORMAT = 'wumfix-xom/1'

# ---------------------------------------------------------------- schema

_SCHEMA = None


def schema():
    global _SCHEMA
    if _SCHEMA is None:
        with open(os.path.join(HERE, 'schema.json')) as f:
            _SCHEMA = json.load(f)
        _SCHEMA['classes'].setdefault('XContainer', dict(parent=None, fields=[], guid=None))
        _SCHEMA['by_guid'] = {c['guid']: n for n, c in _SCHEMA['classes'].items() if c.get('guid')}
    return _SCHEMA


def resolve_class(tname, guid):
    """TYPE names are truncated to 31 chars (strncpy 0x1f in FUN_0063c49f), so
    resolve the schema class by GUID first."""
    s = schema()
    n = s['by_guid'].get(guid)
    if n:
        return n
    if tname in s['classes']:
        return tname
    raise XomError('class %s {%s} not in schema' % (tname, guid))


# struct formats for the math TypeInfos (sizes are checked against schema.json)
MATH_FMT = {
    'Color3f': '3f', 'Color4f': '4f', 'Color1ub': 'B', 'Color2ub': '2B', 'Color3ub': '3B',
    'Color4ub': '4B', 'Color4444': 'H', 'Color1555': 'H',
    'Coord1f': 'f', 'Coord2f': '2f', 'Coord3f': '3f', 'Coord4f': '4f',
    'Coord1s': 'h', 'Coord2s': '2h', 'Coord3s': '3h', 'Coord4s': '4h',
    'Coord1b': 'b', 'Coord2b': '2b', 'Coord3b': '3b', 'Coord4b': '4b',
    'Normal2f': '2f', 'Normal3f': '3f', 'Normal4f': '4f',
    'Normal2s': '2h', 'Normal3s': '3h', 'Normal4s': '4h',
    'Normal2b': '2b', 'Normal3b': '3b', 'Normal4b': '4b',
    'Vector2f': '2f', 'Vector3f': '3f', 'Vector4f': '4f',
    'Vector2s': '2h', 'Vector3s': '3h', 'Vector4s': '4h',
    'Vector2b': '2b', 'Vector3b': '3b', 'Vector4b': '4b',
    'Matrix3f': '9f', 'Matrix43f': '12f', 'Matrix4f': '16f',
    'BoundSphere': '4f', 'BoundBox': '6f', 'Frustum': '7f', 'Ray': '7f', 'Quat': '4f',
}
PRIM_FMT = {
    'bool': '?', 'u8': 'B', 'i8': 'b', 'u16': 'H', 'i16': 'h', 'u32': 'I', 'i32': 'i',
    'u64': 'Q', 'i64': 'q', 'f32': 'f', 'f64': 'd', 'enum': 'I', 'bitfield32': 'I',
    'bitfield64': 'Q',
}


class XomError(Exception):
    pass


# ---------------------------------------------------------------- varint

def read_varint(d, o):
    """7-bit little-endian groups, high bit = continue (engine FUN_0063e939). [V]"""
    v = 0
    sh = 0
    while True:
        if o >= len(d):
            raise XomError('varint past end')
        b = d[o]
        o += 1
        v |= (b & 0x7f) << sh
        sh += 7
        if not b & 0x80:
            return v, o


def write_varint(v):
    out = bytearray()
    while True:
        b = v & 0x7f
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


# ---------------------------------------------------------------- float helpers

def _f_to_json(v, code):
    if isinstance(v, float) and not math.isfinite(v):
        return {code: struct.pack('<' + ('f' if code == 'f32' else 'd'), v).hex()}
    return v


def _f_from_json(v):
    if isinstance(v, dict):
        if 'f32' in v:
            return struct.unpack('<f', bytes.fromhex(v['f32']))[0]
        if 'f64' in v:
            return struct.unpack('<d', bytes.fromhex(v['f64']))[0]
    return v


# ---------------------------------------------------------------- class helpers

def class_chain(name):
    """Most-derived first, as the engine's reader loop walks it (FUN_006c4199)."""
    classes = schema()['classes']
    chain = []
    while name is not None:
        c = classes.get(name)
        if c is None:
            raise XomError('class %s not in schema' % name)
        chain.append(name)
        name = c.get('parent')
    return chain


def field_present(f, version):
    """Replicates the version gate in FUN_006c72b4. [V]"""
    if f['transient']:
        return False
    fv = None
    if f['obsolete']:
        a = f.get('attrs', {}).get('Obsolete')
        if a and 'FromVersion' in a:
            return version < int(a['FromVersion'])
        return False
    a = f.get('attrs', {}).get('Schema')
    if a and 'FromVersion' in a:
        fv = int(a['FromVersion'])
        return version >= fv
    return True


# ---------------------------------------------------------------- custom classes

# Non-XContainer XomObjects with hand-written serialisers (no "CTNR" tag, no
# InternalFlags/UserFlags/DxFieldCount prefix). Field order follows the engine's
# read functions (object vtable slot 5); base-class fields first.  [V decomp]
#   stream reads: 0x1c ref(varint), 0x28 16 bytes, 0x2c string(varint),
#                 0x40 u32, 0x48 u16, 0xa0 varint count
_DESC_BASE = [('ResourceId', 'string'), ('SectionId', 'u16')]   # FUN_006b53e0
CUSTOM = {
    # FUN_006df234: varint count x {GUID, ref Graph, string name}
    'XGraphSet': [('Graphs', ('array', 'varint', [('Guid', 'guid'), ('Graph', 'ref'), ('Name', 'string')]))],
    'XBaseResourceDescriptor': _DESC_BASE,
    'XNullDescriptor': _DESC_BASE,                                                        # thunk -> 006b53e0
    'XMeshDescriptor': _DESC_BASE + [('GraphSet', 'ref'), ('Flags', 'u16')],              # FUN_006b2db0
    'XBitmapDescriptor': _DESC_BASE + [('SpriteScene', 'ref'), ('ImageWidth', 'u16'),
                                       ('ImageHeight', 'u16')],                           # FUN_006aa2d0
    'XSpriteSetDescriptor': _DESC_BASE + [('SpriteSetGroup', 'ref')],                     # FUN_006b7750
    'XParticleSetDescriptor': _DESC_BASE + [('ParticleSetGroup', 'ref')],                 # FUN_006baa40
    'XCustomDescriptor': _DESC_BASE + [('Flags', 'u16')],                                 # FUN_0069e6a0
    'XTextDescriptor': _DESC_BASE + [('TextGroup', 'ref'),                                # FUN_006b1710
                                     ('Chars', ('array', 'u32', [('Index', 'u16'), ('MappedVal', 'u16'),
                                                                 ('Unicode', 'u16')]))],
}
# XGraphSet GUID 64bf3d0b-..., used to recognise truncated TYPE names as well
CUSTOM_GUIDS = {}


def is_container(cname):
    return cname in schema()['classes'] and cname not in CUSTOM


# ---------------------------------------------------------------- reader

class _Reader:
    def __init__(self, data, strings):
        self.d = data
        self.strings = strings

    def value(self, ty, o):
        d = self.d
        if isinstance(ty, tuple):          # ('array', countkind, elemtype) from CUSTOM
            _, ck, et = ty
            if ck == 'varint':
                n, o = read_varint(d, o)
            else:
                n = struct.unpack_from('<I', d, o)[0]
                o += 4
            out = []
            for _ in range(n):
                v, o = self.value(et, o)
                out.append(v)
            return out, o
        if isinstance(ty, list):           # struct: [(name, type), ...]
            out = {}
            for k, t in ty:
                out[k], o = self.value(t, o)
            return out, o
        if ty == 'string':
            i, o = read_varint(d, o)
            if i >= len(self.strings):
                raise XomError('string id %d out of range' % i)
            return self.strings[i], o
        if ty == 'ref':
            i, o = read_varint(d, o)
            return {'ref': i}, o
        if ty == 'guid':
            if o + 16 > len(d):
                raise XomError('short read')
            return d[o:o + 16].hex(), o + 16
        if ty in PRIM_FMT:
            fmt = '<' + PRIM_FMT[ty]
            n = struct.calcsize(fmt)
            if o + n > len(d):
                raise XomError('short read')
            v = struct.unpack_from(fmt, d, o)[0]
            if ty in ('f32', 'f64'):
                v = _f_to_json(v, ty)
            return v, o + n
        if ty in MATH_FMT:
            fmt = '<' + MATH_FMT[ty]
            n = struct.calcsize(fmt)
            if o + n > len(d):
                raise XomError('short read')
            v = list(struct.unpack_from(fmt, d, o))
            if 'f' in fmt:
                v = [_f_to_json(x, 'f32') for x in v]
            return v, o + n
        raise XomError('cannot decode type %s' % (ty,))

    def field(self, f, o):
        if f['array']:
            # XMF*Descriptor (FUN_006c5bf4): varint count, then elements  [V]
            n, o = read_varint(self.d, o)
            out = []
            for _ in range(n):
                v, o = self.value(f['type'], o)
                out.append(v)
            return out, o
        return self.value(f['type'], o)


class _Writer:
    def __init__(self, strings):
        self.strings = strings
        self.sid = {}
        for i, s in enumerate(strings):
            self.sid.setdefault(s, i)

    def string_id(self, s):
        i = self.sid.get(s)
        if i is None:
            i = len(self.strings)
            self.strings.append(s)
            self.sid[s] = i
        return i

    def value(self, ty, v, out):
        if isinstance(ty, tuple):
            _, ck, et = ty
            out += write_varint(len(v)) if ck == 'varint' else struct.pack('<I', len(v))
            for x in v:
                self.value(et, x, out)
        elif isinstance(ty, list):
            for k, t in ty:
                self.value(t, v[k], out)
        elif ty == 'string':
            out += write_varint(self.string_id(v))
        elif ty == 'ref':
            out += write_varint(v['ref'] if isinstance(v, dict) else (v or 0))
        elif ty == 'guid':
            b = bytes.fromhex(v)
            if len(b) != 16:
                raise XomError('guid must be 16 bytes')
            out += b
        elif ty in PRIM_FMT:
            if ty in ('f32', 'f64'):
                v = _f_from_json(v)
            out += struct.pack('<' + PRIM_FMT[ty], v)
        elif ty in MATH_FMT:
            fmt = MATH_FMT[ty]
            if 'f' in fmt:
                v = [_f_from_json(x) for x in v]
            out += struct.pack('<' + fmt, *v)
        else:
            raise XomError('cannot encode type %s' % (ty,))

    def field(self, f, v, out):
        if f['array']:
            out += write_varint(len(v))
            for x in v:
                self.value(f['type'], x, out)
        else:
            self.value(f['type'], v, out)


def _container_fields(tname, versions):
    """(key, fielddesc) in stream order for an XContainer subclass."""
    classes = schema()['classes']
    out = []
    seen = set()
    for cname in class_chain(tname):
        ver = versions.get(cname, 0)
        for f in classes[cname]['fields']:
            if not field_present(f, ver):
                continue
            key = f['name']
            if key in seen:
                key = '%s.%s' % (cname, key)
            seen.add(key)
            out.append((key, f))
    return out


def _decode_object(rd, tname, versions, o):
    """Decode one object at offset o. Returns (obj, new_offset)."""
    d = rd.d
    if tname in CUSTOM:
        obj = {'type': tname}
        obj['fields'], o = rd.value(CUSTOM[tname], o)
        return obj, o
    if d[o:o + 4] != b'CTNR':
        raise XomError('expected CTNR at %#x' % o)
    o += 4
    if o + 3 > len(d):
        raise XomError('short object header')
    # XContainer read FUN_006c4199: tag, InternalFlags, UserFlags, DxFieldCount [V]
    obj = {'type': tname, 'iflags': d[o], 'uflags': d[o + 1], 'dxcount': d[o + 2]}
    o += 3
    if obj['dxcount']:
        raise XomError('DxFieldCount != 0 not supported')
    fields = {}
    for key, f in _container_fields(tname, versions):
        fields[key], o = rd.field(f, o)
    obj['fields'] = fields
    return obj, o


def _encode_object(wr, obj, versions):
    if obj['type'] in CUSTOM and 'raw' not in obj:
        out = bytearray()
        wr.value(CUSTOM[obj['type']], obj['fields'], out)
        return bytes(out)
    out = bytearray(b'CTNR')
    if 'raw' in obj:
        out += bytes.fromhex(obj['raw'])
        return bytes(out)
    out += bytes([obj.get('iflags', 0), obj.get('uflags', 0), obj.get('dxcount', 0)])
    fields = obj['fields']
    for key, f in _container_fields(obj['type'], versions):
        if key not in fields:
            raise XomError('%s: missing field %s' % (obj['type'], key))
        wr.field(f, fields[key], out)
    return bytes(out)


# ---------------------------------------------------------------- container

def loads(data, strict=False):
    """Parse XOM bytes into the JSON-able document model."""
    d = data
    if d[:4] != b'MOIK':
        raise XomError('bad magic %r' % d[:4])
    ntypes, nobjects, root = struct.unpack_from('<III', d, 0x18)
    header = {'version': d[4:8].hex()}
    if d[8:0x18] != bytes(16) or d[0x24:0x40] != bytes(28):
        header['reserved_08'] = d[8:0x18].hex()
        header['reserved_24'] = d[0x24:0x40].hex()
    types = []
    o = 0x40
    for _ in range(ntypes):
        if d[o:o + 4] != b'TYPE':
            raise XomError('expected TYPE at %#x' % o)
        ver, cnt, c = struct.unpack_from('<III', d, o + 4)
        guid = d[o + 0x10:o + 0x20].hex()
        rawname = d[o + 0x20:o + 0x40]
        name = rawname.split(b'\0')[0].decode('latin1')
        t = {'name': name, 'version': ver, 'count': cnt, 'guid': guid}
        if c:
            t['c'] = c
        if rawname != name.encode('latin1').ljust(32, b'\0'):
            t['rawname'] = rawname.hex()
        try:
            cn = resolve_class(name, guid)
            if cn != name:
                t['class'] = cn
        except XomError:
            pass
        types.append(t)
        o += 0x40
    recs = {}
    for tag in (b'GUID', b'SCHM'):
        if d[o:o + 4] != tag:
            raise XomError('expected %r at %#x' % (tag, o))
        recs[tag] = list(struct.unpack_from('<III', d, o + 4))
        o += 16
    if d[o:o + 4] != b'STRS':
        raise XomError('expected STRS at %#x' % o)
    s0 = o
    scount, bsize = struct.unpack_from('<II', d, o + 4)
    offs = struct.unpack_from('<%dI' % scount, d, o + 12)
    b0 = o + 12 + scount * 4
    blob = d[b0:b0 + bsize]
    strings = []
    for x in offs:
        e = blob.index(b'\0', x)
        strings.append(blob[x:e].decode('latin1'))
    o = b0 + bsize
    doc = {'format': FORMAT, 'header': header, 'types': types,
           'guid_rec': recs[b'GUID'], 'schm_rec': recs[b'SCHM'],
           'strings': strings, 'root': root, 'objects': []}
    if _strs_bytes(strings) != d[s0:o]:
        doc['strs_raw'] = d[s0:o].hex()
    # object type sequence: TYPE order, count instances each (FUN_0063f535) [V]
    seq = []
    for t in types:
        seq += [t.get('class', t['name'])] * t['count']
    if len(seq) != nobjects:
        raise XomError('TYPE counts (%d) != nobjects (%d)' % (len(seq), nobjects))
    versions = {t.get('class', t['name']): t['version'] for t in types}
    rd = _Reader(d, strings)
    n = len(seq)
    i = 0
    while i < n:
        tname = seq[i]
        nxt_is_ctnr = i + 1 < n and is_container(seq[i + 1])
        try:
            obj, o2 = _decode_object(rd, tname, versions, o)
            if i + 1 == n:
                if o2 != len(d):
                    raise XomError('%d trailing bytes after last object' % (len(d) - o2))
            elif nxt_is_ctnr and d[o2:o2 + 4] != b'CTNR':
                raise XomError('decoded size mismatch: no CTNR after object')
        except (XomError, struct.error, ValueError, IndexError) as e:
            if strict:
                raise XomError('object #%d %s: %s' % (i + 1, tname, e))
            if d[o:o + 4] == b'CTNR' and (nxt_is_ctnr or i + 1 == n):
                # container of unknown/unsupported layout: keep its payload opaque;
                # its end is the next CTNR tag (or EOF for the last object)
                nxt = _next_ctnr(d, o + 4)
                if i + 1 == n:
                    nxt = len(d)
                obj = {'type': tname, 'raw': d[o + 4:nxt].hex(' '), 'error': str(e)}
                o2 = nxt
            else:
                # cannot delimit this object: keep everything from here on opaque
                doc['tail_raw'] = d[o:].hex()
                doc['tail_error'] = 'object #%d %s: %s' % (i + 1, tname, e)
                for tn in seq[i:]:
                    doc['objects'].append({'type': tn, 'in_tail': True})
                return doc
        doc['objects'].append(obj)
        o = o2
        i += 1
    if o != len(d):
        doc['trailer'] = d[o:].hex()
    return doc


def _next_ctnr(d, o):
    n = d.find(b'CTNR', o)
    return len(d) if n < 0 else n


def _strs_bytes(strings):
    """Canonical STRS: blob = "\\0" + sorted unique non-empty strings (what the
    engine's red-black-tree walk in FUN_0063cc92 produces; true for every
    shipped file)."""
    enc = [s.encode('latin1') for s in strings]
    uniq = sorted(set(s for s in enc if s))
    pos = {b'': 0}
    blob = bytearray(b'\0')
    for s in uniq:
        pos[s] = len(blob)
        blob += s + b'\0'
    offs = [pos[s] for s in enc]
    return (b'STRS' + struct.pack('<II', len(enc), len(blob)) +
            struct.pack('<%dI' % len(enc), *offs) + bytes(blob))


def dumps(doc):
    """Serialise the document model back to XOM bytes."""
    if doc.get('format') != FORMAT:
        raise XomError('not a %s document' % FORMAT)
    strings = list(doc['strings'])
    wr = _Writer(strings)
    versions = {t.get('class', t['name']): t['version'] for t in doc['types']}
    objs = [_encode_object(wr, ob, versions) for ob in doc['objects'] if not ob.get('in_tail')]
    # Recount instances per type from the object list (keeps the TYPE table
    # consistent). Objects must be grouped in TYPE-table order, because the
    # reader assigns each object its type purely by position (FUN_0063f535).
    order = [t.get('class', t['name']) for t in doc['types']]
    counts = {}
    pos = 0
    for ob in doc['objects']:
        if ob['type'] not in order:
            raise XomError('object type %s missing from the TYPE table' % ob['type'])
        p = order.index(ob['type'])
        if p < pos:
            raise XomError('objects are not grouped in TYPE-table order (%s)' % ob['type'])
        pos = p
        counts[ob['type']] = counts.get(ob['type'], 0) + 1
    out = bytearray()
    h = doc['header']
    out += b'MOIK' + bytes.fromhex(h['version'])
    out += bytes.fromhex(h.get('reserved_08', '00' * 16))
    out += struct.pack('<III', len(doc['types']), len(doc['objects']), doc['root'])
    out += bytes.fromhex(h.get('reserved_24', '00' * 28))
    for t in doc['types']:
        out += b'TYPE' + struct.pack('<III', t['version'], counts.get(t.get('class', t['name']), 0), t.get('c', 0))
        out += bytes.fromhex(t['guid'])
        if 'rawname' in t:
            out += bytes.fromhex(t['rawname'])
        else:
            out += t['name'].encode('latin1').ljust(32, b'\0')[:32]
    out += b'GUID' + struct.pack('<III', *doc['guid_rec'])
    out += b'SCHM' + struct.pack('<III', *doc['schm_rec'])
    if 'strs_raw' in doc and strings == doc['strings']:
        out += bytes.fromhex(doc['strs_raw'])
    else:
        out += _strs_bytes(strings)
    for b in objs:
        out += b
    if 'tail_raw' in doc:
        out += bytes.fromhex(doc['tail_raw'])
    if 'trailer' in doc:
        out += bytes.fromhex(doc['trailer'])
    return bytes(out)


def load(path, strict=False):
    with open(path, 'rb') as f:
        return loads(f.read(), strict)


def save(doc, path):
    with open(path, 'wb') as f:
        f.write(dumps(doc))


# ---------------------------------------------------------------- convenience

def obj(doc, ref):
    """Resolve a {'ref': n} (or int) to the object dict; None for null."""
    n = ref['ref'] if isinstance(ref, dict) else ref
    return doc['objects'][n - 1] if n else None


def to_json(doc):
    return json.dumps(doc, indent=1, ensure_ascii=False)


def _main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    cmd = argv[1]
    if cmd == 'xom2json':
        doc = load(argv[2])
        s = to_json(doc)
        if len(argv) > 3:
            with open(argv[3], 'w', encoding='utf-8', newline='\n') as f:
                f.write(s)
        else:
            sys.stdout.buffer.write(s.encode('utf-8'))
    elif cmd == 'json2xom':
        with open(argv[2], encoding='utf-8') as f:
            doc = json.load(f)
        save(doc, argv[3])
    elif cmd == 'dump':
        doc = load(argv[2])
        print('types:', ', '.join('%s v%d x%d' % (t['name'], t['version'], t['count']) for t in doc['types']))
        print('root: #%d  strings: %d' % (doc['root'], len(doc['strings'])))
        for i, ob in enumerate(doc['objects']):
            if 'raw' in ob:
                print('#%d %s RAW(%d bytes) %s' % (i + 1, ob['type'], len(ob['raw']) // 3 + 1, ob.get('error', '')))
            else:
                print('#%d %s %s' % (i + 1, ob['type'], json.dumps(ob['fields'], ensure_ascii=False)))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(_main(sys.argv))
