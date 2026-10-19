#!/usr/bin/env python3
"""
tag_digest.py - reference implementation of Melange's texture-tag digest ("gl-level0-sha256-64").

Melange keys a texture-tag table (spice.json "graphics": {"textureTags": "<file>"}) by a digest of the texture's
base level as the engine uploads it (src/render/mirage/textags_logic.h is the C++ side):

    digest = SHA-256(header || pixels)[:8], written as 16 lowercase hex digits
    header = struct.pack('<5I', width, height, internalformat, format, type)   # glTexImage2D's arguments
    pixels = level 0, rows tightly packed, first byte = first texel of GL row 0 (texture v = 0)
             (glCompressedTexImage2D: format = type = 0 and pixels = the compressed level as given)

The engine (build #1077, 0x79dc50) uploads an XImage straight from its Data, whose rows are stored bottom-up:

    XImage Format 0     -> (GL_RGB8,  GL_RGB,  GL_UNSIGNED_BYTE)
    XImage Format 1, 2  -> (GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE)
    XImage Format 4     -> (GL_RGBA,  GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV)
    XImage Format 6     -> (GL_ALPHA, GL_ALPHA, GL_UNSIGNED_BYTE)
    XImage Format 9, 10 -> compressed (GL_COMPRESSED_RGBA_S3TC_DXT1 / DXT3)

A decoded image the usual way round (PNG, tools/xom ximage.extract_mip: top-down) is flipped back to bottom-up
before hashing.

    python tag_digest.py image.png [...]          # digest of each PNG (8-bit RGB or RGBA)
    python tag_digest.py --xom file.xom [...]     # digest of every XImage in each XOM, with its name
    python tag_digest.py --self-test

Library use:
    import tag_digest
    tag_digest.digest_image(w, h, channels, top_down_bytes)    # RGB / RGBA, rows top-down
    tag_digest.digest_ximage(fields)                            # an XImage's fields from tools/xom xom.load
    tag_digest.digest_upload(w, h, internalformat, fmt, type, data)
"""
import hashlib
import os
import struct
import sys

SCHEME = 'gl-level0-sha256-64'

GL_UNSIGNED_BYTE = 0x1401
GL_ALPHA = 0x1906
GL_RGB = 0x1907
GL_RGBA = 0x1908
GL_RGB8 = 0x8051
GL_RGBA8 = 0x8058
GL_UNSIGNED_SHORT_1_5_5_5_REV = 0x8366
GL_COMPRESSED_RGBA_S3TC_DXT1 = 0x83F1
GL_COMPRESSED_RGBA_S3TC_DXT3 = 0x83F2

# XImage Format -> (internalformat, format, type, bytes per pixel); bpp 0 = compressed.
ENGINE_FORMATS = {
    0: (GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE, 3),
    1: (GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4),
    2: (GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4),
    4: (GL_RGBA, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, 2),
    6: (GL_ALPHA, GL_ALPHA, GL_UNSIGNED_BYTE, 1),
    9: (GL_COMPRESSED_RGBA_S3TC_DXT1, 0, 0, 0),
    10: (GL_COMPRESSED_RGBA_S3TC_DXT3, 0, 0, 0),
}


def digest_upload(width, height, internalformat, fmt, gltype, data):
    """The digest of one level-0 upload; `data` is the level's bytes, rows tightly packed, in upload order."""
    h = hashlib.sha256(struct.pack('<5I', width, height, internalformat, fmt, gltype))
    h.update(data)
    return h.hexdigest()[:16]


def flip_rows(data, width, height, bpp):
    stride = width * bpp
    if len(data) != stride * height:
        raise ValueError('pixel data is %d bytes, expected %d' % (len(data), stride * height))
    return b''.join(data[y * stride:(y + 1) * stride] for y in range(height - 1, -1, -1))


def digest_image(width, height, channels, pixels, top_down=True):
    """An 8-bit RGB (channels 3) or RGBA (channels 4) image as the engine would upload it from an XImage."""
    if channels not in (3, 4):
        raise ValueError('only 8-bit RGB and RGBA images are supported')
    ifmt, fmt, gltype, bpp = ENGINE_FORMATS[0 if channels == 3 else 1]
    data = bytes(pixels)
    if top_down:
        data = flip_rows(data, width, height, bpp)
    elif len(data) != width * height * bpp:
        raise ValueError('pixel data is %d bytes, expected %d' % (len(data), width * height * bpp))
    return digest_upload(width, height, ifmt, fmt, gltype, data)


def _hex_bytes(field):
    if isinstance(field, dict) and 'hex' in field:
        return bytes.fromhex(field['hex'])
    if isinstance(field, (bytes, bytearray)):
        return bytes(field)
    raise TypeError('unexpected Data field encoding: %r' % (type(field),))


def digest_ximage(fields):
    """The digest of an XImage (its `fields` dict from tools/xom xom.load), as the engine uploads its level 0."""
    w, h, xfmt = fields['Width'], fields['Height'], fields['Format']
    if xfmt not in ENGINE_FORMATS:
        raise ValueError('XImage Format %r is not one the engine uploads' % (xfmt,))
    ifmt, fmt, gltype, bpp = ENGINE_FORMATS[xfmt]
    data = _hex_bytes(fields['Data'])
    offsets = list(fields.get('Offsets') or [0])
    off0 = offsets[0]
    if bpp:
        size = w * h * bpp
    else:
        size = (offsets[1] if len(offsets) > 1 else len(data)) - off0
    if off0 + size > len(data):
        raise ValueError('XImage Data is shorter than its level 0')
    return digest_upload(w, h, ifmt, fmt, gltype, data[off0:off0 + size])


def digest_png(path):
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'xom'))
    import png_codec  # noqa: E402
    with open(path, 'rb') as f:
        w, h, ch, top_down = png_codec.decode(f.read())
    return digest_image(w, h, ch, top_down)


def ximages(path):
    """(name, digest or error) for every XImage in a XOM file."""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'xom'))
    import xom  # noqa: E402
    doc = xom.load(path)
    for o in doc['objects']:
        if o.get('type') != 'XImage' or 'fields' not in o or 'Width' not in o['fields']:
            continue
        try:
            yield o['fields'].get('Name', ''), digest_ximage(o['fields'])
        except (ValueError, TypeError, KeyError) as e:
            yield o['fields'].get('Name', ''), 'error: %s' % e


# The same vectors are checked by tests/textags_selftest.cpp against the C++ implementation.
def _vector_image():
    w, h = 3, 2  # top-down RGB: row 0 = top
    return w, h, bytes((x * 40 + y * 7 + c * 3) & 255 for y in range(h) for x in range(w) for c in range(3))


SELF_TEST = {
    'rgb_3x2': 'a9daaeca15600965',
    'rgba_2x2': '9a467ea8b3f1fb7e',
}


def self_test():
    w, h, px = _vector_image()
    got = {
        'rgb_3x2': digest_image(w, h, 3, px),
        'rgba_2x2': digest_image(2, 2, 4, bytes(range(16))),
    }
    ok = True
    for k, v in got.items():
        if SELF_TEST[k] != v:
            print('FAIL %s: %s, expected %s' % (k, v, SELF_TEST[k]))
            ok = False
    # top_down=False takes bytes in upload order: the flipped vector must give the same digest.
    if digest_image(w, h, 3, flip_rows(px, w, h, 3), top_down=False) != got['rgb_3x2']:
        print('FAIL: bottom-up input')
        ok = False
    fields = {'Width': w, 'Height': h, 'Format': 0, 'Offsets': [0, 18], 'Data': {'hex': (flip_rows(px, w, h, 3) + b'\x00' * 6).hex()}}
    if digest_ximage(fields) != got['rgb_3x2']:
        print('FAIL: XImage path')
        ok = False
    print('tag_digest self-test: %s' % ('ok' if ok else 'FAILED'))
    for k, v in got.items():
        print('  %s %s' % (k, v))
    return ok


def main(argv):
    if not argv or argv[0] in ('-h', '--help'):
        print(__doc__)
        return 0
    if argv[0] == '--self-test':
        return 0 if self_test() else 1
    if argv[0] == '--xom':
        for path in argv[1:]:
            for name, d in ximages(path):
                print('%s\t%s\t%s' % (d, os.path.basename(path), name))
        return 0
    for path in argv:
        print('%s\t%s' % (digest_png(path), path))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
