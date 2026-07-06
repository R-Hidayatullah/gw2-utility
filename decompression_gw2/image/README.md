# ArenaNet ATEX texture decoder

Python decoder for Guild Wars 2's `ATEX` / `ATEP` / `ATEU` / `ATEC` / `ATET` /
`ATTX` textures, reverse-engineered from `Gw2-64.exe`
(`sub_140B83040`, source path `Arena\Engine\Gr\Img\ImgAtex.cpp`).

It reproduces the game's decoder **exactly** — every one of the 18 sample
`decompressed_*.bin` files inflates byte-for-byte, and every block decoder
matches an independent reference (Pillow's DDS decoder) **pixel-for-pixel**.

```
python decode_atex.py FILE.bin ...          # -> FILE.png (mip 0)
python decode_atex.py --all-mips FILE.bin   # every mip level
python decode_atex.py --info FILE.bin        # header + mip table only
python decode_atex.py --outdir out FILE ...
```

Requires `numpy` and `Pillow`.

## Files

| file | role |
|------|------|
| `gw2_atex.py`      | header parsing + the custom **inflate** (RLE constant passes, plane de-interleave, bit reader) |
| `block_decoders.py`| block → RGBA8888 decoders: DXT1/2/3/4/5, DXTA, DXTL, DXTN, 3DCX, BC5, **BC7** |
| `decode_atex.py`   | command-line front-end (writes PNGs) |

## The format

```
off 0   char[4] magic     ATEX | ATTX | ATEC | ATEP | ATEU | ATET
off 4   char[4] fourCC     DXT1 DXT2 DXT3 DXT4 DXT5 DXTA DXTL DXTN 3DCX BC5X BC7X
off 8   u16     width
off 10  u16     height
off 12  <mip levels...>

per mip level:
  u32 dataSize     total bytes of the level, incl. these 8 (chains the levels)
  u32 flags        which constant-fill passes were applied
  <inflated block stream>
```

The `fourCC` selects the *pixel data format*; it maps to an internal enum
(`DXT1`=22 … `BC7`=32) whose flag word (`s_flags[]`, extracted from the binary)
drives the decoder state:

* `flags & 0x001` – block-compressed (vs. uncompressed packed pixels)
* `flags & 0x210`, `flags & 0x280` – which **planes** a block has, and thus its
  size: `bytesPerBlock = 4 * ((0x280?2)+(0x210?2)+(DXTL?2))`
  → DXT1/DXTA = 8, everything else = 16.

## The compression

A block surface is stored **de-interleaved by plane**. A 16-byte DXT5 block is
`[8 alpha][4 colour-endpoints][4 colour-indices]`; the stream stores *all* alpha
chunks, then *all* endpoint chunks, then *all* index chunks. Re-interleaving
them yields a standard DXT/BC surface (there is **no** swizzle/tiling — verified:
`sub_140B14C00` returns the offset unchanged).

Before the planes, up to five passes fill runs of identical blocks so they are
never stored. They are gated by `flags` and always run in this order:

| bit | pass | applies to | notes |
|-----|------|-----------|-------|
| 0x01 | white colour block | DXT1 | writes `FF*8`; no selector bit |
| 0x02 | constant 4-bit alpha | DXT2/DXT3 | **selector bit**: zero-block vs constant |
| 0x04 | constant 8-bit alpha | DXT4/DXT5/DXTA/DXTL | **selector bit**: zero-block vs constant |
| 0x08 | constant RGB colour | any colour format | encoded as a BC1 block (`encode_bc1_color`, port of `sub_140BF7470`) |
| 0x10 | 256×256 terrain-atlas border mirroring | DXT3/DXT4 | port of `sub_140B82AF0` |

Each pass is a run-length code over the *not-yet-filled* blocks: a fixed Huffman
symbol (`byte_141C86390`, 6-bit lookup) gives a run length, one flag bit says
whether the run is filled, and the two alpha passes read one extra bit selecting
between an all-zero block and the constant value.

The run/constant passes read an **MSB-first bit stream fed by 32-bit
little-endian words**; the plane data that follows is byte-aligned literal
bytes. Switching between the two realigns the same way the game does: if ≥32
read-ahead bits remain, one word is kept, otherwise the accumulator is dropped.

## Formats & how each is turned into RGBA

| fourCC | decode |
|--------|--------|
| DXT1 (BC1) | 565 endpoints + 2-bit indices, 1-bit punch-through alpha |
| DXT2/DXT3 (BC2) | explicit 4-bit alpha + BC1 colour |
| DXT4/DXT5 (BC3) | interpolated 8-bit alpha + BC1 colour |
| DXTA | 8-byte BC1-style block used as a grayscale/mask channel |
| DXTL | BC3 layout; the interpolated channel modulates colour as luminance |
| DXTN / 3DCX / BC5 | two interpolated channels (X,Y); Z reconstructed as a normal map |
| BC7 | full modes 0–7 decoder |

## Validation

* **Inflate**: all mips of all 18 samples consume their stream with 0-byte
  slack.
* **Block decode**: DXT1, DXT5, BC5/3DCX and BC7 outputs are identical
  (`max|diff| == 0`) to Pillow's DDS decoder on the raw surfaces.
