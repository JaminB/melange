"""
png_codec.py - minimal, dependency-free PNG reader/writer for xomtool's Python parity CLI.

Only stdlib `zlib` and `struct` are used, matching xom.py's "pure Python 3, no dependencies"
convention. Supports exactly what the XImage <-> PNG converter needs: 8-bit RGB and RGBA, no
interlacing, no palette.

Not a general PNG library: encode() always emits filter-type 0 (None) scanlines, and decode()
only understands filter types 0-4 (required for any compliant encoder) for colour types 2 (RGB)
and 6 (RGBA), bit depth 8.
"""
import struct
import zlib

_SIG = b'\x89PNG\r\n\x1a\n'


def _chunk(tag, data):
    return (struct.pack('>I', len(data)) + tag + data
            + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))


def encode(width, height, channels, raw_rows):
    """raw_rows: bytes of tightly packed rows (no filter byte), row-major, top row first,
    `channels` bytes per pixel (3=RGB, 4=RGBA), 8 bits/channel. Returns PNG bytes."""
    assert channels in (3, 4)
    stride = width * channels
    assert len(raw_rows) == stride * height
    color_type = 2 if channels == 3 else 6
    ihdr = struct.pack('>IIBBBBB', width, height, 8, color_type, 0, 0, 0)
    filtered = bytearray()
    for y in range(height):
        filtered.append(0)  # filter type 0 = None
        filtered += raw_rows[y * stride:(y + 1) * stride]
    idat = zlib.compress(bytes(filtered), 9)
    return (_SIG + _chunk(b'IHDR', ihdr) + _chunk(b'IDAT', idat)
            + _chunk(b'IEND', b''))


def _unfilter(data, width, height, channels):
    stride = width * channels
    out = bytearray(stride * height)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        ftype = data[pos]
        pos += 1
        cur = bytearray(data[pos:pos + stride])
        pos += stride
        if ftype == 0:
            pass
        elif ftype == 1:  # Sub
            for i in range(channels, stride):
                cur[i] = (cur[i] + cur[i - channels]) & 0xff
        elif ftype == 2:  # Up
            for i in range(stride):
                cur[i] = (cur[i] + prev[i]) & 0xff
        elif ftype == 3:  # Average
            for i in range(stride):
                a = cur[i - channels] if i >= channels else 0
                b = prev[i]
                cur[i] = (cur[i] + ((a + b) >> 1)) & 0xff
        elif ftype == 4:  # Paeth
            for i in range(stride):
                a = cur[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                if pa <= pb and pa <= pc:
                    pr = a
                elif pb <= pc:
                    pr = b
                else:
                    pr = c
                cur[i] = (cur[i] + pr) & 0xff
        else:
            raise ValueError('unsupported PNG filter type %d' % ftype)
        out[y * stride:(y + 1) * stride] = cur
        prev = cur
    return bytes(out)


def decode(png_bytes):
    """Returns (width, height, channels, raw_rows), raw_rows tightly packed top-row-first
    bytes, matching encode()'s input format."""
    assert png_bytes[:8] == _SIG
    pos = 8
    width = height = channels = None
    idat = bytearray()
    while pos < len(png_bytes):
        length, = struct.unpack_from('>I', png_bytes, pos)
        tag = png_bytes[pos + 4:pos + 8]
        body = png_bytes[pos + 8:pos + 8 + length]
        pos += 12 + length
        if tag == b'IHDR':
            width, height, depth, color_type = struct.unpack('>IIBB', body[:10])
            if depth != 8:
                raise ValueError('only 8-bit PNG supported')
            if color_type == 2:
                channels = 3
            elif color_type == 6:
                channels = 4
            else:
                raise ValueError('only RGB/RGBA PNG supported (color_type=%d)' % color_type)
        elif tag == b'IDAT':
            idat += body
        elif tag == b'IEND':
            break
    raw = zlib.decompress(bytes(idat))
    return width, height, channels, _unfilter(raw, width, height, channels)
