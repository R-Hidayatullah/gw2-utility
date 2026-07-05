"""
Python reimplementation of ArenaNet's CmpDecompress Method 0 (Huffman + LZ77),
reverse-engineered from Gw2-64.exe (Services/Compress/CmpApi.cpp + CmpHuff.cpp).

Function addresses (for cross-reference in IDA):
    CmpDecompress                 0x140D921C0
    CmpDecompress_Method0_Inflate 0x140D96FC0   <- this is what we implement here
    CmpDecompress_Method1_Delta   0x140D94E20   <- NOT implemented (delta/patch method)
    Huffman table builder         0x140D9DF00   (CmpHuff.cpp)

Pipeline (Gw2.dat -> plaintext):
    1. Read the MFT entry's raw [offset, offset+size) bytes from Gw2.dat.
    2. Strip the 4-byte CRC32 inserted every 0x10000 bytes (+ trailing CRC).
       See remove_crc32_data().
    3. Parse an 8-byte header from the stripped buffer:
         offset 0..3 : flag dword (low 16 bits observed = compression method
                       id 8 == "ANet compress"; meaning only partially understood)
         offset 4..7 : uncompressedSize (u32 LE) -- verified byte-exact against
                       THIRDPARTYSOFTWAREREADME.txt (540537)
    4. The remaining bytes are the CmpDecompress bitstream:
         - first 4 bits : Method (0 = plain, 1 = delta -- not implemented here)
         - next 4 bits  : minMatchAdd-1 (Method 0 only)
         - then repeat: two freshly-rebuilt canonical Huffman tables
           (literal/length, then distance) followed by up to (nibble+1)<<12
           symbols coded against those tables, LZ77-style.
"""

import struct
import sys


# ---------------------------------------------------------------------------
# Bit reader: bytes are grouped into sequential little-endian 32-bit words,
# and bits are consumed MSB-first within each word, in word order. This is
# behaviourally identical to CmpIo's split 32+32 "rack" bit reader, just
# implemented with a Python bigint accumulator instead of two fixed 32-bit
# registers (much simpler, no risk of shift-amount edge cases).
# ---------------------------------------------------------------------------
class BitReader:
    def __init__(self, data: bytes):
        self.data = data
        self.end_byte = (len(data) // 4) * 4  # trailing <4 bytes are never read, matches CmpDecompress
        self.pos = 0
        self.acc = 0
        self.bits = 0

    def _refill(self, n):
        while self.bits < n:
            if self.pos + 4 <= self.end_byte:
                word = int.from_bytes(self.data[self.pos:self.pos + 4], "little")
                self.pos += 4
            else:
                word = 0
            self.acc = (self.acc << 32) | word
            self.bits += 32

    def read_bits(self, n):
        if n == 0:
            return 0
        self._refill(n)
        self.bits -= n
        val = (self.acc >> self.bits) & ((1 << n) - 1)
        self.acc &= (1 << self.bits) - 1
        return val

    def peek_bits(self, n):
        self._refill(n)
        return (self.acc >> (self.bits - n)) & ((1 << n) - 1)


# ---------------------------------------------------------------------------
# Static tables, extracted verbatim from the binary via IDA (get_bytes).
# ---------------------------------------------------------------------------

# byte_142061180 (32 entries) -- extra bits for length codes 256..284
LEN_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0,
             1, 1, 1, 1,
             2, 2, 2, 2,
             3, 3, 3, 3,
             4, 4, 4, 4,
             5, 5, 5, 5,
             0, 0, 0, 0]

# byte_142060FA0 (32 entries) -- base length value (before adding minMatchAdd)
LEN_BASE = [0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7,
            0x8, 0xa, 0xc, 0xe,
            0x10, 0x14, 0x18, 0x1c,
            0x20, 0x28, 0x30, 0x38,
            0x40, 0x50, 0x60, 0x70,
            0x80, 0xa0, 0xc0, 0xe0,
            0xff, 0x0, 0x0, 0x0]

# byte_1420610E0 (32 entries) -- extra bits for distance codes
DIST_EXTRA = [0, 0, 0, 0,
              1, 1, 2, 2,
              3, 3, 4, 4,
              5, 5, 6, 6,
              7, 7, 8, 8,
              9, 9, 10, 10,
              11, 11, 12, 12,
              13, 13, 14, 14]

# word_142060F60 (32 entries, 16-bit) -- base distance value
DIST_BASE = [0x0, 0x1, 0x2, 0x3, 0x4, 0x6, 0x8, 0xc,
             0x10, 0x18, 0x20, 0x30,
             0x40, 0x60, 0x80, 0xc0,
             0x100, 0x180, 0x200, 0x300,
             0x400, 0x600, 0x800, 0xc00,
             0x1000, 0x1800, 0x2000, 0x3000,
             0x4000, 0x6000, 0x0, 0x0]

# Fixed "meta Huffman" used to decode the RLE-packed code-length alphabet
# (unk_142061620: 14 (mask, offset, bitlen) records, mask descending).
META_TABLE = [
    (0xa0000000, 2, 3),
    (0x60000000, 6, 4),
    (0x40000000, 10, 5),
    (0x20000000, 18, 6),
    (0x12000000, 25, 7),
    (0x0c000000, 31, 8),
    (0x07000000, 41, 9),
    (0x03000000, 57, 10),
    (0x01600000, 70, 11),
    (0x00f00000, 77, 12),
    (0x00c00000, 83, 13),
    (0x00b00000, 87, 14),
    (0x00a00000, 95, 15),
    (0x00000000, 255, 16),
]

# byte_142061690 (256 entries) -- value table indexed by the meta-huffman lookup
META_VALUES = [
    0x8, 0x9, 0xa, 0x0, 0x7, 0xb, 0xc, 0x6, 0x29, 0x2a, 0xe0, 0x4, 0x5, 0x20, 0x28, 0x2b,
    0x2c, 0x40, 0x4a, 0x3, 0xd, 0x25, 0x26, 0x27, 0x48, 0x49, 0x24, 0x47, 0x4b, 0x4c, 0x69, 0x6a,
    0x23, 0x46, 0x60, 0x63, 0x67, 0x68, 0x88, 0x89, 0xa0, 0xe8, 0x1, 0x2, 0x2d, 0x43, 0x44, 0x45,
    0x65, 0x66, 0x80, 0x87, 0x8a, 0xa8, 0xa9, 0xc0, 0xc9, 0xe9, 0xe, 0x4d, 0x64, 0x6b, 0x6c, 0x84,
    0x85, 0x8b, 0xa4, 0xa5, 0xaa, 0xc8, 0xe5, 0x83, 0x86, 0xa6, 0xa7, 0xc7, 0xca, 0xe7, 0x22, 0x2e,
    0x8c, 0xc4, 0xe4, 0xe6, 0x4e, 0x6d, 0xc6, 0xec, 0xf, 0x10, 0x11, 0x8d, 0xab, 0xac, 0xcc, 0xea,
    0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x21, 0x2f,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    0x41, 0x42, 0x4f, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c,
    0x5d, 0x5e, 0x5f, 0x61, 0x62, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x81, 0x82, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94,
    0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0xa1, 0xa2, 0xa3, 0xad, 0xae,
    0xaf, 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe,
    0xbf, 0xc1, 0xc2, 0xc3, 0xc5, 0xcb, 0xcd, 0xce, 0xcf, 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6,
    0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, 0xe1, 0xe2, 0xe3, 0xeb, 0xed, 0xee, 0xef,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
]
assert len(META_VALUES) == 256


def decode_meta(br: BitReader) -> int:
    """Decode one RLE symbol (repeatCount<<5 | codeLength) via the fixed meta-Huffman table."""
    v = br.peek_bits(32)
    for mask, offset, bitlen in META_TABLE:
        if v >= mask:
            rel = (v - mask) >> (32 - bitlen)
            idx = offset - rel
            br.read_bits(bitlen)
            return META_VALUES[idx]
    raise ValueError("meta huffman decode failed (no matching mask)")


def build_huffman_table(br: BitReader):
    """
    Mirrors sub_140D9DF00: reads a 16-bit symbol count, then an RLE-encoded
    array of code lengths (via the fixed meta table), then builds a standard
    canonical Huffman decode table (ascending symbol index => ascending code
    within each length group, which is what ArenaNet's linked-list-based
    table builder produces).
    Returns dict {(bitlength, code): symbol}.
    """
    total_symbols = br.read_bits(16)
    code_lengths = [0] * total_symbols
    idx = total_symbols - 1
    while idx >= 0:
        rle = decode_meta(br)
        repeat = (rle >> 5) + 1
        length = rle & 0x1F
        if length != 0 or total_symbols < 2:
            for _ in range(repeat):
                if idx < 0:
                    raise ValueError("code length RLE underflow")
                code_lengths[idx] = length
                idx -= 1
        else:
            idx -= repeat

    # Per-length linked lists (head = v66[length], next = v63[symbol]), built
    # by prepending while idx descends -- walking from the head therefore
    # visits symbols in ascending index order, matching the C code exactly.
    max_len = max(code_lengths) if code_lengths else 0
    U32 = 0xFFFFFFFF
    v66 = [U32] * (max_len + 1)
    v63 = [0] * total_symbols
    idx = total_symbols - 1
    while idx >= 0:
        l = code_lengths[idx]
        if l != 0:
            v63[idx] = v66[l]
            v66[l] = idx
        idx -= 1

    # ArenaNet assigns codes in DESCENDING order per length group (ascending
    # symbol index -> descending code value), with the running counter (v35)
    # kept as an UNSIGNED 32-bit value that WRAPS AROUND once a length group
    # fully consumes its available code space. That wrapped value is exactly
    # what feeds "2*v35+1" for the next length -- using plain (never
    # wrapping) Python ints here silently produces wrong codes for every
    # length past the point where the first wrap happens.
    table = {}
    v35 = 0
    length = 0
    while length <= max_len:
        sym = v66[length]
        while sym != U32 and v35 < (1 << length) and sym < total_symbols:
            table[(length, v35)] = sym
            v35 = (v35 - 1) & U32
            sym = v63[sym]
        length += 1
        v35 = (2 * v35 + 1) & U32
    return table


def decode_symbol(br: BitReader, table: dict) -> int:
    code = 0
    length = 0
    while length <= 24:
        code = (code << 1) | br.read_bits(1)
        length += 1
        sym = table.get((length, code))
        if sym is not None:
            return sym
    raise ValueError("huffman decode failed (no matching code)")


def cmp_decompress_method0(comp: bytes, output_size: int) -> bytes:
    br = BitReader(comp)
    method = br.read_bits(4)
    if method != 0:
        raise ValueError(f"expected Method 0, got Method {method} (Method 1 = delta, not implemented)")
    min_match_add = br.read_bits(4) + 1

    out = bytearray()
    while len(out) < output_size:
        lit_table = build_huffman_table(br)
        dist_table = build_huffman_table(br)
        block_symbols = (br.read_bits(4) + 1) << 12

        for _ in range(block_symbols):
            if len(out) >= output_size:
                break
            sym = decode_symbol(br, lit_table)
            if sym < 0x100:
                out.append(sym)
            else:
                li = sym - 256
                extra = LEN_EXTRA[li]
                length = LEN_BASE[li] + (br.read_bits(extra) if extra else 0) + min_match_add

                dsym = decode_symbol(br, dist_table)
                dextra = DIST_EXTRA[dsym]
                dist = DIST_BASE[dsym] + (br.read_bits(dextra) if dextra else 0)

                start = len(out) - dist - 1
                if start < 0:
                    raise ValueError("back-reference distance out of range (corrupt stream or decoder bug)")
                for k in range(length):
                    out.append(out[start + k])

    return bytes(out[:output_size])


CHUNK_SIZE = 0x10000
START_INDEX = CHUNK_SIZE - 4
END_INDEX = CHUNK_SIZE


def remove_crc32_data(raw: bytes) -> bytes:
    """
    Gw2.dat inserts a 4-byte CRC32 checksum every 0x10000 bytes of the raw
    on-disk entry data (plus one trailing CRC at EOF). These must be
    stripped before the {flag,uncompressedSize} header and CmpDecompress
    bitstream can be parsed. Mirrors the user-provided remove_crc32c_data().
    """
    data = bytearray(raw)
    entry_size = len(raw)
    if entry_size > CHUNK_SIZE:
        position = 0
        while position + CHUNK_SIZE <= len(data):
            del data[position + START_INDEX: position + END_INDEX]
            position += CHUNK_SIZE - 4
        if len(data) > 4:
            del data[-4:]
    elif entry_size == CHUNK_SIZE:
        del data[START_INDEX:END_INDEX]
    else:
        if len(data) > 4:
            del data[-4:]
    return bytes(data)


def decompress_gw2_entry(raw_entry_bytes: bytes) -> bytes:
    """raw_entry_bytes = exactly what was read from Gw2.dat at [mftEntry.offset, +mftEntry.size)."""
    stripped = remove_crc32_data(raw_entry_bytes)
    flag, uncompressed_size = struct.unpack_from("<II", stripped, 0)
    comp = stripped[8:]
    return cmp_decompress_method0(comp, uncompressed_size)


if __name__ == "__main__":
    in_path = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\Ridwan Hidayatullah\Documents\decompressgw2\compressed_16.bin"
    out_path = sys.argv[2] if len(sys.argv) > 2 else r"C:\Users\Ridwan Hidayatullah\Documents\decompressgw2\decompressed_16.txt"

    with open(in_path, "rb") as f:
        raw = f.read()

    print(f"input: {in_path} ({len(raw)} bytes)")
    result = decompress_gw2_entry(raw)
    print(f"decompressed: {len(result)} bytes")

    with open(out_path, "wb") as f:
        f.write(result)
    print(f"written to: {out_path}")
