"""
ximage.py - XImage <-> PNG conversion for xomtool's Python parity CLI.

Layout formula validated exhaustively against every shipped XImage (docs/m5-assets-research.md
S1.1): levels are back-to-back, no padding. Row order and channel order were settled by the M5
scaffold's runtime check on the weapon panel atlas (docs/m5-design.md S1.9#6): XImage rows are
stored bottom-up, RGB order kept; PNG (like every other image tool) is top-down, so converting
either way flips rows. Mirrors src/xom/image.{h,cpp}.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import png_codec  # noqa: E402

FORMAT_BPP = {0: 3, 1: 4}  # RGB8, RGBA8


def mip_dims(width, height, level):
    return max(1, width >> level), max(1, height >> level)


def expected_layout(width, height, fmt, mip_levels):
    bpp = FORMAT_BPP[fmt]
    strides, offsets = [], []
    off = 0
    for i in range(mip_levels):
        w, h = mip_dims(width, height, i)
        stride = w * bpp
        strides.append(stride)
        offsets.append(off)
        off += stride * h
    return strides, offsets, off


def _hex_bytes(field):
    if isinstance(field, dict) and 'hex' in field:
        return bytes.fromhex(field['hex'])
    if isinstance(field, (bytes, bytearray)):
        return bytes(field)
    raise TypeError('unexpected Data field encoding: %r' % (type(field),))


def _flip_rows(raw, width, height, bpp):
    stride = width * bpp
    rows = [raw[y * stride:(y + 1) * stride] for y in range(height)]
    return b''.join(reversed(rows))


def extract_mip(fields, level=0):
    """Returns (width, height, bpp, top_down_bytes) for one mip level of a decoded XImage
    object's `fields` dict (as produced by xom.load). Recomputes Strides/Offsets from the
    formula rather than trusting the stored arrays (proven exact for every shipped file), and
    cross-checks the result against the object's actual Data length."""
    w0, h0 = fields['Width'], fields['Height']
    fmt = fields['Format']
    mips = fields['MipLevels']
    if not (0 <= level < mips):
        raise ValueError('mip level out of range')
    bpp = FORMAT_BPP[fmt]
    strides, offsets, total = expected_layout(w0, h0, fmt, mips)
    data = _hex_bytes(fields['Data'])
    if len(data) != total:
        raise ValueError('XImage.Data length does not match the Width/Height/Format/MipLevels formula')
    w, h = mip_dims(w0, h0, level)
    stride = strides[level]
    raw_bottom_up = data[offsets[level]:offsets[level] + stride * h]
    return w, h, bpp, _flip_rows(raw_bottom_up, w, h, bpp)


def ximage_to_png_bytes(fields, level=0):
    w, h, bpp, top_down = extract_mip(fields, level)
    return png_codec.encode(w, h, bpp, top_down)


def box_downsample(raw, w, h, bpp):
    """2x2 box filter (average, integer-truncated): the mip-generation policy for PNG ->
    XImage (docs/m5-assets-research.md S1.4), matching what every conventional texture-authoring
    tool does by default."""
    nw, nh = max(1, w // 2), max(1, h // 2)
    out = bytearray(nw * nh * bpp)
    for y in range(nh):
        for x in range(nw):
            sx0, sy0 = x * 2, y * 2
            for c in range(bpp):
                total = 0
                n = 0
                for dy in range(2):
                    sy = sy0 + dy
                    if sy >= h:
                        continue
                    for dx in range(2):
                        sx = sx0 + dx
                        if sx >= w:
                            continue
                        total += raw[(sy * w + sx) * bpp + c]
                        n += 1
                out[(y * nw + x) * bpp + c] = total // max(1, n)
    return bytes(out), nw, nh


def png_to_ximage_fields(png_bytes, name, generate_mips=True):
    """Builds a full XImage `fields` dict (schema field order) from PNG bytes."""
    w, h, ch, top_down = png_codec.decode(png_bytes)
    fmt = 0 if ch == 3 else 1
    levels_top_down = [top_down]
    lw, lh = w, h
    while generate_mips and (lw > 1 or lh > 1):
        nxt, lw, lh = box_downsample(levels_top_down[-1], lw, lh, ch)
        levels_top_down.append(nxt)
    mip_levels = len(levels_top_down)
    strides, offsets, total = expected_layout(w, h, fmt, mip_levels)
    data = bytearray(total)
    for i, level_top_down in enumerate(levels_top_down):
        lw_i, lh_i = mip_dims(w, h, i)
        data[offsets[i]:offsets[i] + strides[i] * lh_i] = _flip_rows(level_top_down, lw_i, lh_i, ch)
    return {
        'Name': name, 'Width': w, 'Height': h, 'MipLevels': mip_levels,
        'Flags': 0, 'Strides': strides, 'Offsets': offsets, 'Format': fmt,
        'Data': {'hex': bytes(data).hex()}, 'Palette': {'ref': 0},
    }
