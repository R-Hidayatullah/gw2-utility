#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gw2_atex.py  --  ArenaNet ATEX / ATEP / ATEU ... texture decoder

Reverse-engineered from Gw2-64.exe `sub_140B83040`
(D:\\Perforce\\Live\\NAEU\\v2\\Code\\Arena\\Engine\\Gr\\Img\\ImgAtex.cpp).

Pipeline (matches the game's decoder exactly):

    file bytes ->  [ATEX header parse]
                ->  [per-mip custom inflate  (this module: inflate_mip)]
                ->  raw block surface (DXT1/2/3/4/5, DXTA, DXTL, DXTN, 3DCX/BC5, BC7, ...)
                ->  [standard block decode  (block_decoders.py)]
                ->  RGBA8888 image

The header
---------
    off 0   char[4]  magic     one of: ATEX ATTX ATEC ATEP ATEU ATET
    off 4   char[4]  fourCC    texture data format ("DXT5", "3DCX", "BC7X", ...)
    off 8   u16      width
    off 10  u16      height
    off 12  <mip levels...>

Each mip level:
    off 0   u32      dataSize     total bytes of this level *including* these 8
    off 4   u32      flags        which constant-fill passes were applied
    off 8   <inflated block stream>

`dataSize` chains the mip levels, so every level is independently locatable
(this is what makes the decoder robust even if a bit/byte boundary is off).

The custom compression
----------------------
The block surface is stored *de-interleaved by plane*.  A 16-byte block such
as DXT5 is [8 alpha][4 color-endpoints][4 color-indices]; the stream stores
*all* alpha chunks, then *all* endpoint chunks, then *all* index chunks.

Before the planes, up to four RLE "constant" passes (selected by `flags`) fill
whole runs of identical blocks so they don't need to be stored:

    flag 0x01  white color blocks          (DXT1-style, color plane)
    flag 0x02  constant 4-bit alpha        (DXT2/DXT3, alpha plane)
    flag 0x04  constant 8-bit alpha        (DXT4/DXT5/DXTA/DXTL, alpha plane)
    flag 0x08  constant RGB color          (color plane, encoded via encode_bc1_color)
    flag 0x10  256x256 terrain-atlas border mirroring (DXT3/DXT4 only)

Every value/RLE pass reads from an MSB-first bit stream fed by 32-bit
little-endian words; the plane data that follows is byte-aligned literal bytes.
"""

import struct

# --------------------------------------------------------------------------
#  Format table   (extracted verbatim from the binary)
# --------------------------------------------------------------------------
#  enum -> fourCC name  (order of .rdata string table @ 141C73D88)
FORMAT_NAMES = [
    "ARGB32323232F", "ARGB16161616F", "ARGB2101010", "ARGB8888", "XRGB8888",
    "ARGB4444", "ARGB1555", "RGB888", "RGB565", "RGB555",
    "RG1616", "RG1616F", "RG3232F", "R16F", "R32F",
    "AL88", "AL44", "AL8", "L8", "A8",
    "P8", "VU88", "DXT1", "DXT2", "DXT3",
    "DXT4", "DXT5", "DXTA", "DXTL", "DXTN",
    "3DCX", "BC5", "BC7", "D24", "SHADOWMAP",
    "ABGR8888", "R32UINT", "UNKNOWN",
]

#  s_flags[]  @ dword_141C73CF0   (bit0=block-compressed, 0x210/0x280 select planes)
FORMAT_FLAGS = [
    0xB2, 0xB2, 0xB2, 0xB2, 0x12, 0xB2, 0x72, 0x12, 0x12, 0x12,
    0x12, 0x12, 0x12, 0x100, 0x100, 0x1A4, 0x1A4, 0x1A4, 0x104, 0xA2,
    0x78, 0x400, 0x71, 0xB1, 0xB1, 0xB1, 0xB1, 0xA1, 0x11, 0x201,
    0x201, 0x201, 0xB1, 0x00, 0x00, 0xB2, 0x12, 0x00,
]

#  bits-per-pixel  = first u32 of each 3-u32 record @ unk_141C73A90
FORMAT_BPP = [
    128, 64, 32, 32, 32, 16, 16, 24, 16, 16,
    32, 32, 64, 16, 32, 16, 8, 8, 8, 8,
    8, 16, 4, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 32, 32, 32, 32, 32,
]

#  fourCC (as it appears in the file, little-endian ASCII) -> format enum.
#  Built from the switch in sub_140B83040 (case '1TXD' -> 22, etc.).
FOURCC_TO_ENUM = {
    b"DXT1": 22, b"DXT2": 23, b"DXT3": 24, b"DXT4": 25, b"DXT5": 26,
    b"DXTA": 27, b"DXTL": 28, b"DXTN": 29, b"3DCX": 30,
    b"BC5X": 31, b"BC7X": 32,
}

MAGICS = {b"ATEX", b"ATTX", b"ATEC", b"ATEP", b"ATEU", b"ATET"}


class AtexError(Exception):
    pass


# --------------------------------------------------------------------------
#  MSB-first bit reader fed by 32-bit little-endian words
# --------------------------------------------------------------------------
class _BitReader:
    """
    Mirrors the game's reader:  bytes are pulled 4 at a time as a little-endian
    u32 and merged MSB-first into a 64-bit accumulator.  Bits are consumed from
    the top (bit 63 first).  `self.pos` is the byte offset of the next word to
    pull, and is always where byte-aligned plane reads resume.
    """
    MASK64 = (1 << 64) - 1

    def __init__(self, buf, start, end):
        self.buf = buf
        self.pos = start
        self.end = end
        self.head = 0
        self.bits = 0

    def _pull_word(self):
        p = self.pos
        if p + 4 <= self.end:
            word = self.buf[p] | (self.buf[p + 1] << 8) | \
                   (self.buf[p + 2] << 16) | (self.buf[p + 3] << 24)
        else:  # pad with zeros past the end
            word = 0
            for i in range(4):
                if p + i < self.end:
                    word |= self.buf[p + i] << (8 * i)
        self.pos = p + 4
        self.head |= (word << (32 - self.bits)) & self.MASK64
        self.bits += 32

    def need(self, n):
        while self.bits < n:
            self._pull_word()

    def read(self, n):
        if n == 0:
            return 0
        self.need(n)
        v = self.head >> (64 - n)
        self.head = (self.head << n) & self.MASK64
        self.bits -= n
        return v

    def read_run(self, huff_len, huff_val):
        """One RLE token: returns (count, filled).  count = symbol+1."""
        self.need(7)                       # 6-bit index + 1 flag bit worst case
        k = self.head >> 58                # top 6 bits
        clen = huff_len[k]
        sym = huff_val[k]
        self.head = (self.head << clen) & self.MASK64
        self.bits -= clen
        filled = self.read(1)
        return sym + 1, filled

    def align_to_word(self):
        """
        Switch to byte-aligned reads, matching the game's realign branch:
            if leftover_bits >= 32:  keep exactly one word (rewind 4 bytes)
            else:                    discard the whole accumulator (rewind 0)
        """
        if self.bits >= 32:
            self.pos -= 4
        self.head = 0
        self.bits = 0


# RLE Huffman table @ byte_141C86390 : 64 (len,val) pairs indexed by top 6 bits.
_HUFF_RAW = bytes([
    6, 0x10, 6, 0x0F, 6, 0x0E, 6, 0x0D, 6, 0x0C, 6, 0x0B, 6, 0x0A, 6, 0x09,
    6, 0x08, 6, 0x07, 6, 0x06, 6, 0x05, 6, 0x04, 6, 0x03, 6, 0x02, 6, 0x01,
    2, 0x11, 2, 0x11, 2, 0x11, 2, 0x11, 2, 0x11, 2, 0x11, 2, 0x11, 2, 0x11,
    2, 0x11, 2, 0x11, 2, 0x11, 2, 0x11, 2, 0x11, 2, 0x11, 2, 0x11, 2, 0x11,
    1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00,
    1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00,
    1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00,
    1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00,
])
HUFF_LEN = [_HUFF_RAW[2 * i] for i in range(64)]
HUFF_VAL = [_HUFF_RAW[2 * i + 1] for i in range(64)]


# --------------------------------------------------------------------------
#  encode_bc1_color  --  port of sub_140BF7470
#  Encodes a single RGB888 colour into an 8-byte BC1 colour block
#  (used by the flag-0x08 constant-colour pass).
# --------------------------------------------------------------------------
def encode_bc1_color(rgb, is_dxt1):
    v3 = rgb & 0xFF            # R
    v4 = (rgb >> 8) & 0xFF     # G
    v5 = (rgb >> 16) & 0xFF    # B

    v6 = v4 - (v4 >> 6)
    v7 = ((v3 - (v3 >> 5)) & 0xFFFFFFFF) >> 3
    v8 = v5 >> 5
    v9 = 8 * v7 + (((v3 - (v3 >> 5)) & 0xFFFFFFFF) >> 5)
    v31 = v6 >> 2
    v32 = ((v5 - v8) & 0xFFFFFFFF) >> 3
    v10 = v7 + 1
    v30 = v7
    v11 = 4 * (v6 >> 2)
    v12 = v11 + (v6 >> 6)
    den1 = (((v7 + 1) >> 2) - v9 + 8 * v7 + 8)
    v13 = (12 * (v3 - v9) // den1) if den1 else 0
    v14 = (v6 >> 2) + 1
    v15 = v32 + 1
    den2 = ((v14 >> 4) - v12 + 4 + v11)
    v16 = (12 * (v4 - v12) // den2) if den2 else 0
    den3 = (((v32 + 1) >> 2) - (((v5 - v8) & 0xFFFFFFFF) >> 5) + 8)
    v17 = (12 * (v5 - (8 * v32 + (((v5 - v8) & 0xFFFFFFFF) >> 5))) // den3) if den3 else 0

    # --- red channel endpoint selection
    if v13 < 2:
        v10 = v30
        v18 = v10
    elif v13 >= 6:
        if v13 >= 0xA:
            v18 = v10
        else:
            v18 = v10
            v10 = v30
    else:
        v18 = v30
    # --- green
    if v16 < 2:
        v14 = v31
        v19 = v14
    elif v16 >= 6:
        if v16 >= 0xA:
            v19 = v14
        else:
            v19 = v14
            v14 = v31
    else:
        v19 = v31
    # --- blue
    if v17 >= 2:
        if v17 < 6:
            v20 = v32
        elif v17 < 0xA:
            v20 = v32 + 1
            v15 = v32
        else:
            v20 = v15
    else:
        v15 = v32
        v20 = v15

    v22 = (v18 | (32 * (v19 | (v20 << 6)))) & 0xFFFF     # endpoint 0 (565)
    v23 = (v10 | (32 * (v14 | (v15 << 6)))) & 0xFFFF     # endpoint 1 (565)

    v24 = 0
    v25 = 0
    if v18 != v10:
        v24 = v13
        v25 = 1
        if v18 != v30:
            v24 = 12 - v13
    if v19 != v14:
        if v19 != v31:
            v16 = 12 - v16
        v24 += v16
        v25 += 1
    if v20 != v15:
        if v20 != v32:
            v17 = 12 - v17
        v24 += v17
        v25 += 1
    if v25:
        v24 = (v24 + (v25 >> 1)) // v25

    v26 = 1 if (is_dxt1 and (0 <= v24 - 5 <= 1 or v25 == 0)) else 0
    if v25 == 0 and not v26:
        if v23 == 0xFFFF:
            v24 = 12
            v22 = (v22 - 1) & 0xFFFF
        else:
            v24 = 0
            v23 = (v23 + 1) & 0xFFFF

    if v26 != (1 if v22 <= v23 else 0):
        v22, v23 = v23, v22
        v24 = 12 - v24

    v21 = 0
    if not v26:
        if v24 >= 2:
            if v24 < 6:
                v21 = 2
            else:
                v21 = 1
                if v24 < 0xA:
                    v21 = 3
    else:
        v21 = 2

    idx = (v21 | (4 * v21) | (16 * (v21 | (4 * v21))))
    idx = (idx | (idx << 8)) & 0xFFFF
    idx = (idx | (idx << 16)) & 0xFFFFFFFF
    return struct.pack("<HHI", v22, v23, idx)


# --------------------------------------------------------------------------
#  Terrain-atlas border mirroring  (port of sub_140B82AF0)
#  Only used for 256x256 DXT3/DXT4 levels with flag 0x10.
# --------------------------------------------------------------------------
TERRAIN_BORDERS = 0xC0000003


def _ror4(v, n):
    v &= 0xFFFFFFFF
    return ((v >> n) | (v << (32 - n))) & 0xFFFFFFFF


def terrain_mirror(surf, nbBlocks):
    """surf: bytearray of the 256x256 surface (64x64 blocks * 16 bytes)."""
    for i in range(nbBlocks):
        v5 = (1 << (i >> 6)) & TERRAIN_BORDERS
        v6 = (1 << i) & TERRAIN_BORDERS
        if not (v6 or v5):
            continue
        v7 = (i & 0x3F) ^ 3 if v6 else (i & 0x3F)
        v8 = (i >> 6) ^ 3 if v5 else (i >> 6)
        src = 16 * (v7 + (v8 << 6))
        v11, v12, v13, v14 = struct.unpack_from("<IIII", surf, src)
        if v6:
            v12 = v11
            v11 = ((16 * (v11 & 0xF000F0 | ((v11 & 0xFFFF000F) << 8))) |
                   ((v11 & 0xF000F00 | (v11 >> 8) & 0xF000F0) >> 4)) & 0xFFFFFFFF
            v14 = (((v14 & 0x30303030 | (v14 >> 4) & 0xC0C0C0C) >> 2) |
                   (4 * (v14 & 0xC0C0C0C | (16 * (v14 & 0xFF030303))))) & 0xFFFFFFFF
        if v5:
            v11, v12 = _ror4(v12, 16), _ror4(v11, 16)
            v14 = struct.unpack("<I", struct.pack(">I", v14))[0]
        struct.pack_into("<IIII", surf, 16 * i, v11 & 0xFFFFFFFF,
                         v12 & 0xFFFFFFFF, v13 & 0xFFFFFFFF, v14 & 0xFFFFFFFF)


# --------------------------------------------------------------------------
#  Bit helpers for the block bitmaps
# --------------------------------------------------------------------------
def _bt(bm, i):
    return (bm[i >> 5] >> (i & 31)) & 1


def _bs(bm, i):
    bm[i >> 5] |= (1 << (i & 31))


# --------------------------------------------------------------------------
#  Per-mip inflate
# --------------------------------------------------------------------------
def inflate_mip(buf, payload_start, payload_end, flags, fmt_enum,
                block_w, block_h):
    """
    Inflate one mip level into its raw block surface.

    Returns a bytearray of length nbBlocks * bytes_per_block.
    """
    ff = FORMAT_FLAGS[fmt_enum]
    a2 = 2 if (ff & 0x280) else 0        # plane-A present?  (0x280) -> 2 dwords
    dxtl = 2 if (fmt_enum == 28) else 0  # DXTL adds a plane-A too
    a210 = 2 if (ff & 0x210) else 0      # planes B & C present?
    units = a2 + a210 + dxtl             # dwords per block
    bpb = 4 * units                      # bytes per block
    nbBlocks = block_w * block_h
    surf = bytearray(nbBlocks * bpb)

    has_A = bool(ff & 0x280) or fmt_enum == 28
    has_BC = bool(ff & 0x210)

    # within-block byte offsets of each plane
    offA = 0
    offB = 4 * (dxtl + a2)
    offC = 4 * (dxtl + a2 + 1)

    bmA = [0] * ((nbBlocks + 31) >> 5)   # st+40 : "plane A constant"
    bmB = [0] * ((nbBlocks + 31) >> 5)   # st+48 : "plane B/C constant"

    is256 = (block_w == 64 and block_h == 64)   # 256x256

    # ---- terrain border pre-mark (state 7): skip border blocks, mirror later
    do_terrain = (flags & 0x10) and is256 and fmt_enum in (23, 24)
    if do_terrain:
        for i in range(nbBlocks):
            if ((1 << (i & 0x3F)) & TERRAIN_BORDERS) or \
               ((1 << (i >> 6)) & TERRAIN_BORDERS):
                _bs(bmA, i)
                _bs(bmB, i)

    br = _BitReader(buf, payload_start, payload_end)

    # ================= constant-fill (bit-stream) passes ==================
    # flag 0x01 : white colour blocks (DXT1-style; only when 0x210 & !0x280 & !DXTL)
    if (flags & 0x01) and (ff & 0x210) and not (ff & 0x280) and fmt_enum != 28:
        val = b"\xff\xff\xff\xff\xff\xff\xff\xff"
        _rle_fill(br, surf, nbBlocks, bpb, offB, val, bmB, (bmA, bmB))

    # flag 0x02 : constant 4-bit alpha (DXT2 / DXT3)
    #   two-value: selector bit picks zero-block vs constant-nibble block
    if (flags & 0x02) and fmt_enum in (23, 24):
        nib = br.read(4)
        b = (nib | (nib << 4)) & 0xFF
        val = bytes([b] * 8)
        _rle_fill(br, surf, nbBlocks, bpb, offA, val, bmB, (bmA,),
                  two_value=True)

    # flag 0x04 : constant 8-bit alpha (DXT4 / DXT5 / DXTA / DXTL)
    #   two-value: selector bit picks zero-block vs constant-alpha block
    if (flags & 0x04) and fmt_enum in (25, 26, 27, 28):
        a = br.read(8)
        val = bytes([a, a, 0, 0, 0, 0, 0, 0])
        _rle_fill(br, surf, nbBlocks, bpb, offA, val, bmB, (bmA,),
                  two_value=True)

    # flag 0x08 : constant RGB colour (encode via BC1 colour block)
    if (flags & 0x08) and has_BC:
        rgb = br.read(24)
        val = encode_bc1_color(rgb, fmt_enum == 22)
        _rle_fill(br, surf, nbBlocks, bpb, offB, val, bmB, (bmB,))

    # =================== byte-aligned plane reads =========================
    br.align_to_word()
    pos = br.pos

    end = payload_end

    def _copy(dst, n):
        """Copy n bytes from `pos`, zero-padding past end; never resizes surf."""
        nonlocal pos
        avail = max(0, min(n, end - pos))
        if avail:
            surf[dst:dst + avail] = buf[pos:pos + avail]
        pos += n

    # plane A : 8 bytes/block at offA, for blocks with bmA clear
    if has_A:
        for i in range(nbBlocks):
            if not _bt(bmA, i):
                _copy(i * bpb + offA, 8)
    # plane B : 4 bytes/block at offB, for blocks with bmB clear
    if has_BC:
        for i in range(nbBlocks):
            if not _bt(bmB, i):
                _copy(i * bpb + offB, 4)
        # plane C : 4 bytes/block at offC, for blocks with bmB clear
        for i in range(nbBlocks):
            if not _bt(bmB, i):
                _copy(i * bpb + offC, 4)

    # ---- terrain border mirror (state 19)
    if do_terrain:
        terrain_mirror(surf, nbBlocks)

    global _LAST_POS, _LAST_END
    _LAST_POS, _LAST_END = pos, payload_end
    return surf, bpb


_LAST_POS = _LAST_END = 0


def _rle_fill(br, surf, nbBlocks, bpb, within_off, val, read_bm, mark_bms,
              two_value=False):
    """
    Run-length constant fill.  Iterates blocks that are clear in `read_bm`;
    each RLE token covers `count` clear blocks, writing (and marking `mark_bms`)
    when the token's `filled` flag is set.

    When `two_value` (the alpha passes 0x02/0x04), a filled token reads one
    extra selector bit: 0 -> a zero block, 1 -> `val`.
    """
    n = len(val)
    zero = bytes(n)
    i = 0
    while i < nbBlocks:
        count, filled = br.read_run(HUFF_LEN, HUFF_VAL)
        cur = val
        if filled and two_value:
            cur = val if br.read(1) else zero
        while count > 0 and i < nbBlocks:
            if not _bt(read_bm, i):
                if filled:
                    dst = i * bpb + within_off
                    surf[dst:dst + n] = cur
                    for bm in mark_bms:
                        _bs(bm, i)
                count -= 1
            i += 1
        while i < nbBlocks and _bt(read_bm, i):
            i += 1


# --------------------------------------------------------------------------
#  Top-level parse
# --------------------------------------------------------------------------
def _block_dims(fmt_enum, w, h):
    """4x4 block grid for compressed formats, else 1x1 (per-pixel)."""
    if FORMAT_FLAGS[fmt_enum] & 1:            # block-compressed
        return (w + 3) >> 2, (h + 3) >> 2
    return w, h


def parse_atex(data):
    """
    Parse an ATEX blob.  Returns dict with:
        magic, fourcc, fmt_enum, fmt_name, width, height, mips
    where mips = [ {level, width, height, surface(bytearray), bpb, flags,
                    block_w, block_h, raw(bool)} ... ]
    """
    magic = bytes(data[:4])
    if len(data) < 12 or magic not in MAGICS:
        raise AtexError("not an ATEX file (bad magic %r)" % magic)
    fourcc = bytes(data[4:8])
    width, height = struct.unpack_from("<HH", data, 8)

    fmt_enum = FOURCC_TO_ENUM.get(fourcc)
    if fmt_enum is None:
        # uncompressed / packed format: fall back by bpp guess (rare)
        raise AtexError("unsupported/uncompressed fourCC %r" % fourcc)

    mips = []
    pos = 12
    w, h = width, height
    level = 0
    n = len(data)
    while pos + 8 <= n and w >= 1 and h >= 1:
        dataSize, flags = struct.unpack_from("<II", data, pos)
        if dataSize < 8:
            break
        payload_start = pos + 8
        payload_end = min(pos + dataSize, n)
        bw, bh = _block_dims(fmt_enum, w, h)
        surf, bpb = inflate_mip(data, payload_start, payload_end, flags,
                                fmt_enum, bw, bh)
        mips.append(dict(level=level, width=w, height=h, surface=surf, bpb=bpb,
                         flags=flags, block_w=bw, block_h=bh,
                         raw=(flags == 0)))
        pos += dataSize
        level += 1
        w = max(1, w >> 1)
        h = max(1, h >> 1)

    return dict(magic=magic, fourcc=fourcc, fmt_enum=fmt_enum,
                fmt_name=FORMAT_NAMES[fmt_enum], width=width, height=height,
                mips=mips)
