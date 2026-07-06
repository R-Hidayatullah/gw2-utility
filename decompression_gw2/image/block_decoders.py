#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
block_decoders.py -- decode raw ArenaNet block surfaces into RGBA8888.

Supports every format the ATEX codec emits:
    DXT1 (BC1)      DXT2/DXT3 (BC2)     DXT4/DXT5 (BC3)
    DXTA            DXTL                DXTN / 3DCX / BC5   BC7

All decoders take a flat `bytes`/`bytearray` surface laid out as a row-major
grid of 4x4 blocks and return a numpy uint8 array of shape (H, W, 4) RGBA.
"""

import numpy as np

# --------------------------------------------------------------------------
#  Small helpers
# --------------------------------------------------------------------------
def _expand565(c):
    r = (c >> 11) & 0x1F
    g = (c >> 5) & 0x3F
    b = c & 0x1F
    r = (r << 3) | (r >> 2)
    g = (g << 2) | (g >> 4)
    b = (b << 3) | (b >> 2)
    return r, g, b


def _grid_iter(bw, bh):
    for by in range(bh):
        for bx in range(bw):
            yield bx, by


def _place(img, bx, by, px, W, H):
    """px : list of 16 (r,g,b,a) in row-major 4x4 order -> write to img."""
    x0, y0 = bx * 4, by * 4
    for j in range(4):
        y = y0 + j
        if y >= H:
            break
        row = img[y]
        base = j * 4
        for i in range(4):
            x = x0 + i
            if x >= W:
                break
            row[x] = px[base + i]


# --------------------------------------------------------------------------
#  BC1 colour block  ->  16 (r,g,b,a)
# --------------------------------------------------------------------------
def bc1_colors(block, off, dxt1_alpha=True):
    c0 = block[off] | (block[off + 1] << 8)
    c1 = block[off + 2] | (block[off + 3] << 8)
    bits = block[off + 4] | (block[off + 5] << 8) | \
           (block[off + 6] << 16) | (block[off + 7] << 24)
    r0, g0, b0 = _expand565(c0)
    r1, g1, b1 = _expand565(c1)
    pal = [(r0, g0, b0, 255), (r1, g1, b1, 255), None, None]
    if c0 > c1 or not dxt1_alpha:
        pal[2] = ((2 * r0 + r1) // 3, (2 * g0 + g1) // 3, (2 * b0 + b1) // 3, 255)
        pal[3] = ((r0 + 2 * r1) // 3, (g0 + 2 * g1) // 3, (b0 + 2 * b1) // 3, 255)
    else:
        pal[2] = ((r0 + r1) // 2, (g0 + g1) // 2, (b0 + b1) // 2, 255)
        pal[3] = (0, 0, 0, 0)
    out = []
    for k in range(16):
        out.append(pal[(bits >> (2 * k)) & 3])
    return out


# --------------------------------------------------------------------------
#  BC3-style interpolated alpha (also BC4) ->  16 values 0..255
# --------------------------------------------------------------------------
def bc3_alpha(block, off):
    a0 = block[off]
    a1 = block[off + 1]
    lut = [a0, a1, 0, 0, 0, 0, 0, 0]
    if a0 > a1:
        for i in range(1, 7):
            lut[i + 1] = ((7 - i) * a0 + i * a1) // 7
    else:
        for i in range(1, 5):
            lut[i + 1] = ((5 - i) * a0 + i * a1) // 5
        lut[6] = 0
        lut[7] = 255
    bits = int.from_bytes(block[off + 2:off + 8], "little")
    return [lut[(bits >> (3 * k)) & 7] for k in range(16)]


# --------------------------------------------------------------------------
#  BC2-style explicit 4-bit alpha  -> 16 values 0..255
# --------------------------------------------------------------------------
def bc2_alpha(block, off):
    bits = int.from_bytes(block[off:off + 8], "little")
    out = []
    for k in range(16):
        a = (bits >> (4 * k)) & 0xF
        out.append((a << 4) | a)
    return out


# --------------------------------------------------------------------------
#  Whole-surface decoders
# --------------------------------------------------------------------------
def decode_dxt1(surf, W, H):
    bw, bh = (W + 3) // 4, (H + 3) // 4
    img = np.zeros((H, W, 4), np.uint8)
    for bx, by in _grid_iter(bw, bh):
        o = (by * bw + bx) * 8
        _place(img, bx, by, bc1_colors(surf, o, True), W, H)
    return img


def decode_dxt3(surf, W, H):        # BC2
    bw, bh = (W + 3) // 4, (H + 3) // 4
    img = np.zeros((H, W, 4), np.uint8)
    for bx, by in _grid_iter(bw, bh):
        o = (by * bw + bx) * 16
        col = bc1_colors(surf, o + 8, False)
        alp = bc2_alpha(surf, o)
        px = [(c[0], c[1], c[2], a) for c, a in zip(col, alp)]
        _place(img, bx, by, px, W, H)
    return img


def decode_dxt5(surf, W, H):        # BC3
    bw, bh = (W + 3) // 4, (H + 3) // 4
    img = np.zeros((H, W, 4), np.uint8)
    for bx, by in _grid_iter(bw, bh):
        o = (by * bw + bx) * 16
        col = bc1_colors(surf, o + 8, False)
        alp = bc3_alpha(surf, o)
        px = [(c[0], c[1], c[2], a) for c, a in zip(col, alp)]
        _place(img, bx, by, px, W, H)
    return img


def decode_dxta(surf, W, H):
    """DXTA : single 8-byte BC1-colour block used as a luminance/alpha mask."""
    bw, bh = (W + 3) // 4, (H + 3) // 4
    img = np.zeros((H, W, 4), np.uint8)
    for bx, by in _grid_iter(bw, bh):
        o = (by * bw + bx) * 8
        col = bc1_colors(surf, o, False)
        px = [(c[0], c[0], c[0], 255) for c in col]   # grayscale from red
        _place(img, bx, by, px, W, H)
    return img


def decode_dxtl(surf, W, H):
    """DXTL : BC3 layout; the interpolated 'alpha' is a luminance multiplier."""
    bw, bh = (W + 3) // 4, (H + 3) // 4
    img = np.zeros((H, W, 4), np.uint8)
    for bx, by in _grid_iter(bw, bh):
        o = (by * bw + bx) * 16
        col = bc1_colors(surf, o + 8, False)
        lum = bc3_alpha(surf, o)
        px = []
        for c, l in zip(col, lum):
            px.append((c[0] * l // 255, c[1] * l // 255, c[2] * l // 255, 255))
        _place(img, bx, by, px, W, H)
    return img


def decode_bc5(surf, W, H, reconstruct_z=True, swap=False):
    """
    DXTN / 3DCX / BC5 : two interpolated 8-byte channels (X in first, Y in
    second).  Blue is reconstructed as a normal-map Z when reconstruct_z.
    """
    bw, bh = (W + 3) // 4, (H + 3) // 4
    img = np.zeros((H, W, 4), np.uint8)
    for bx, by in _grid_iter(bw, bh):
        o = (by * bw + bx) * 16
        ch0 = bc3_alpha(surf, o)          # first plane (0..8)
        ch1 = bc3_alpha(surf, o + 8)      # second plane (8..16)
        if swap:
            ch0, ch1 = ch1, ch0
        px = []
        for x, y in zip(ch0, ch1):
            if reconstruct_z:
                fx = x / 127.5 - 1.0
                fy = y / 127.5 - 1.0
                fz = 1.0 - fx * fx - fy * fy
                z = int((np.sqrt(fz) * 0.5 + 0.5) * 255) if fz > 0 else 128
                px.append((x, y, z, 255))
            else:
                px.append((x, y, 0, 255))
        _place(img, bx, by, px, W, H)
    return img


# --------------------------------------------------------------------------
#  BC7  (full mode 0-7 decoder)
# --------------------------------------------------------------------------
_BC7_MODES = [
    # ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2
    dict(ns=3, pb=4, rb=0, isb=0, cb=4, ab=0, epb=1, spb=0, ib=3, ib2=0),  # 0
    dict(ns=2, pb=6, rb=0, isb=0, cb=6, ab=0, epb=0, spb=1, ib=3, ib2=0),  # 1
    dict(ns=3, pb=6, rb=0, isb=0, cb=5, ab=0, epb=0, spb=0, ib=2, ib2=0),  # 2
    dict(ns=2, pb=6, rb=0, isb=0, cb=7, ab=0, epb=1, spb=0, ib=2, ib2=0),  # 3
    dict(ns=1, pb=0, rb=2, isb=1, cb=5, ab=6, epb=0, spb=0, ib=2, ib2=3),  # 4
    dict(ns=1, pb=0, rb=2, isb=0, cb=7, ab=8, epb=0, spb=0, ib=2, ib2=2),  # 5
    dict(ns=1, pb=0, rb=0, isb=0, cb=7, ab=7, epb=1, spb=0, ib=4, ib2=0),  # 6
    dict(ns=2, pb=6, rb=0, isb=0, cb=5, ab=5, epb=1, spb=0, ib=2, ib2=0),  # 7
]

_BC7_P2 = [
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80,
    0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
    0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE,
    0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
    0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A,
    0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660,
    0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C,
    0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22,
]

_BC7_P3 = [
    0xAA685050, 0x6A5A5040, 0x5A5A4200, 0x5450A0A8, 0xA5A50000, 0xA0A05050,
    0x5555A0A0, 0x5A5A5050, 0xAA550000, 0xAA555500, 0xAAAA5500, 0x90909090,
    0x94949494, 0xA4A4A4A4, 0xA9A59450, 0x2A0A4250, 0xA5945040, 0x0A425054,
    0xA5A5A500, 0x55A0A0A0, 0xA8A85454, 0x6A6A4040, 0xA4A45000, 0x1A1A0500,
    0x0050A4A4, 0xAAA59090, 0x14696914, 0x69691400, 0xA08585A0, 0xAA821414,
    0x50A4A450, 0x6A5A0200, 0xA9A58000, 0x5090A0A8, 0xA8A09050, 0x24242424,
    0x00AA5500, 0x24924924, 0x24499224, 0x50A50A50, 0x500AA550, 0xAAAA4444,
    0x66660000, 0xA5A0A5A0, 0x50A050A0, 0x69286928, 0x44AAAA44, 0x66666600,
    0xAA444444, 0x54A854A8, 0x95809580, 0x96969600, 0xA85454A8, 0x80959580,
    0xAA141414, 0x96960000, 0xAAAA1414, 0xA05050A0, 0xA0A5A5A0, 0x96000000,
    0x40804080, 0xA9A8A9A8, 0xAAAAAA44, 0x2A4A5254,
]

_BC7_A2 = [
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6,
    6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15,
]
_BC7_A3a = [
    3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3,
    3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
    8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15,
    3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3,
]
_BC7_A3b = [
    15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8,
    15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
    15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8,
    15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8,
]

_BC7_WEIGHTS = {
    2: [0, 21, 43, 64],
    3: [0, 9, 18, 27, 37, 46, 55, 64],
    4: [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64],
}


class _BitStream:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def get(self, n):
        v = 0
        for i in range(n):
            byte = self.data[self.pos >> 3]
            bit = (byte >> (self.pos & 7)) & 1
            v |= bit << i
            self.pos += 1
        return v


def _bc7_interp(e0, e1, idx, bits):
    w = _BC7_WEIGHTS[bits][idx]
    return (e0 * (64 - w) + e1 * w + 32) >> 6


def _decode_bc7_block(block):
    bs = _BitStream(block)
    mode = 0
    while mode < 8 and bs.get(1) == 0:
        mode += 1
    if mode == 8:
        return [(0, 0, 0, 255)] * 16
    m = _BC7_MODES[mode]
    ns = m["ns"]

    part = bs.get(m["pb"]) if m["pb"] else 0
    rot = bs.get(m["rb"]) if m["rb"] else 0
    isb = bs.get(m["isb"]) if m["isb"] else 0

    ne = ns * 2
    cb, ab = m["cb"], m["ab"]
    r = [0] * ne
    g = [0] * ne
    b = [0] * ne
    a = [255] * ne

    for i in range(ne):
        r[i] = bs.get(cb)
    for i in range(ne):
        g[i] = bs.get(cb)
    for i in range(ne):
        b[i] = bs.get(cb)
    if ab:
        for i in range(ne):
            a[i] = bs.get(ab)

    # p-bits
    if m["epb"]:
        pb = [bs.get(1) for _ in range(ne)]
        for i in range(ne):
            r[i] = (r[i] << 1) | pb[i]
            g[i] = (g[i] << 1) | pb[i]
            b[i] = (b[i] << 1) | pb[i]
            if ab:
                a[i] = (a[i] << 1) | pb[i]
        cb += 1
        ab += 1 if ab else 0
    elif m["spb"]:
        sp = [bs.get(1) for _ in range(2)]
        for i in range(ne):
            s = sp[i // (ne // 2)]
            r[i] = (r[i] << 1) | s
            g[i] = (g[i] << 1) | s
            b[i] = (b[i] << 1) | s
            if ab:
                a[i] = (a[i] << 1) | s
        cb += 1
        ab += 1 if ab else 0

    # scale endpoints to 8 bits
    def _sc(v, bits):
        v <<= (8 - bits)
        return v | (v >> bits)
    for i in range(ne):
        r[i] = _sc(r[i], cb)
        g[i] = _sc(g[i], cb)
        b[i] = _sc(b[i], cb)
        a[i] = _sc(a[i], ab) if ab else 255

    ib, ib2 = m["ib"], m["ib2"]

    # partition table + anchor indices
    if ns == 1:
        parts = [0] * 16
        anchors = [0]
    elif ns == 2:
        pt = _BC7_P2[part]
        parts = [(pt >> k) & 1 for k in range(16)]
        anchors = [0, _BC7_A2[part]]
    else:
        pt = _BC7_P3[part]
        parts = [(pt >> (2 * k)) & 3 for k in range(16)]
        anchors = [0, _BC7_A3a[part], _BC7_A3b[part]]

    # index bits
    idx1 = [0] * 16
    for k in range(16):
        bits_here = ib - 1 if k in anchors else ib
        idx1[k] = bs.get(bits_here)
    idx2 = None
    if ib2:
        idx2 = [0] * 16
        for k in range(16):
            bits_here = ib2 - 1 if k == 0 else ib2
            idx2[k] = bs.get(bits_here)

    out = []
    for k in range(16):
        s = parts[k]
        e0 = s * 2
        e1 = s * 2 + 1
        if idx2 is None:
            ci = idx1[k]
            ai = idx1[k]
            cbits = ib
            abits = ib
        else:
            if isb == 0:
                ci, cbits = idx1[k], ib
                ai, abits = idx2[k], ib2
            else:
                ci, cbits = idx2[k], ib2
                ai, abits = idx1[k], ib
        cr = _bc7_interp(r[e0], r[e1], ci, cbits)
        cg = _bc7_interp(g[e0], g[e1], ci, cbits)
        cbl = _bc7_interp(b[e0], b[e1], ci, cbits)
        ca = _bc7_interp(a[e0], a[e1], ai, abits) if m["ab"] else 255
        if rot == 1:
            ca, cr = cr, ca
        elif rot == 2:
            ca, cg = cg, ca
        elif rot == 3:
            ca, cbl = cbl, ca
        out.append((cr, cg, cbl, ca))
    return out


def decode_bc7(surf, W, H):
    bw, bh = (W + 3) // 4, (H + 3) // 4
    img = np.zeros((H, W, 4), np.uint8)
    for bx, by in _grid_iter(bw, bh):
        o = (by * bw + bx) * 16
        px = _decode_bc7_block(surf[o:o + 16])
        _place(img, bx, by, px, W, H)
    return img


# --------------------------------------------------------------------------
#  Dispatch
# --------------------------------------------------------------------------
def decode_surface(fmt_enum, surf, W, H):
    if fmt_enum in (22,):                      # DXT1
        return decode_dxt1(surf, W, H)
    if fmt_enum in (23, 24):                   # DXT2 / DXT3
        return decode_dxt3(surf, W, H)
    if fmt_enum in (25, 26):                   # DXT4 / DXT5
        return decode_dxt5(surf, W, H)
    if fmt_enum == 27:                         # DXTA
        return decode_dxta(surf, W, H)
    if fmt_enum == 28:                         # DXTL
        return decode_dxtl(surf, W, H)
    if fmt_enum in (29, 30, 31):               # DXTN / 3DCX / BC5
        return decode_bc5(surf, W, H, reconstruct_z=True)
    if fmt_enum == 32:                         # BC7
        return decode_bc7(surf, W, H)
    raise ValueError("no decoder for format enum %d" % fmt_enum)
