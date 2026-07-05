"""
Decoder for Guild Wars 2's "strs" (TEXT_STRINGS_SIGNATURE) string-table
container -- the format produced by decompressing a Gw2.dat MFT entry
through cmp_decompress_method0.py (Method 0 / CmpDecompress).

Reverse-engineered from Gw2-64-disable-aslr.exe via IDA Pro / Hex-Rays.
Source paths recovered from embedded assert strings:
    Engine\\Text\\TextCache.cpp        sub_1410CCA80  (file parser / loader)
    Engine\\Text\\TextSync.cpp         sub_1410CE830  (network sync path)
    Engine\\Text\\TextDecode.cpp       sub_1410CFF60  (bracket/tag substitution)
    Arena\\Services\\Crypt\\CptRc4.cpp  sub_140D9F4E0  (RC4 key schedule)

Container layout (all little-endian):
    offset 0..3   : magic "strs"   (TEXT_STRINGS_SIGNATURE, dword 0x73727473)
    offset 4..    : records back-to-back, no count field, no padding --
                    keep reading while >= 6 bytes remain.

Each record ("StringHeader", 6-byte header + payload):
    u16 bytes       total record length INCLUDING this 6-byte header
                     (next record starts at record_start + bytes)
    u16 baseChar    "base" character value used by the bit-packed decoder
    u8  rangeBits   bits per symbol in the bit-packed decoder (asserted <= 16)
    u8  _pad        unused/reserved
    ...payload (bytes - 6) bytes...

Two decode paths, selected by the runtime (sub_1410CFC50) from the header:

  1. RAW UTF-16 fast path -- CONFIRMED, verified byte-exact against real
     game text (Spanish skill/trait tooltips, Simplified Chinese item and
     achievement names all decoded perfectly from the samples in this repo):
         baseChar == 0 and rangeBits == 16 and len(payload) is even
     -> payload IS the string, verbatim UTF-16LE, nothing more to do.

  2. Bit-packed / "compact" path -- taken whenever baseChar != 0 (and for
     rangeBits != 16 with baseChar == 0). Each output character is a
     `rangeBits`-wide, LSB-first, byte-packed symbol:
         symbol == 0         -> NUL (end of string)
         1 <= symbol < 0x20   -> look up in a fixed 31-entry table embedded
                                 in the .exe at 0x1420F3100:
                                 "0123456strnum()[]<>%#/:-'\" ,.!\n"
         symbol >= 0x20       -> char = symbol + baseChar - 32

     NOT SOLVED YET: sub_1410CFC50 additionally requires an RC4-derived
     key ("a5" in the decompiled call) whenever baseChar != 0 -- without
     it the real function returns an empty string rather than decoding
     anything (`if (header->baseChar && !a5) return "";`, verified in
     disasm at 0x1410cfc7d-0x1410cfc8e). That key is only ever supplied
     by sub_1410CFF60 (TextDecode.cpp's bracket-substitution / cross-
     reference resolver), sourced from a per-context override table or
     an inline reference parsed out of an *already-decoded parent
     string* -- i.e. runtime state that doesn't live inside a standalone
     compressed_*.bin sample. This script still parses the record
     framing correctly (byte accounting checks out exactly on every
     sample file) and runs the bit-unpack math structurally, but the
     characters it prints for baseChar != 0 records are NOT the real
     text -- the RC4 keystream XOR that must happen before bit-unpacking
     is missing. Next step would be dynamic analysis (breakpoint on
     sub_1410CFC50 in a live client) to capture real (decodeId -> key)
     pairs, or locating where the key table is populated for ordinary
     (non cross-referenced) strings.

Usage:
    python strs_decode.py <decompressed_strs_file.bin>
"""
import struct
import sys

SPECIAL_TABLE = "0123456strnum()[]<>%#/:-'\" ,.!\n"  # symbol 1..31 -> table[symbol-1]


class StringRecord:
    __slots__ = ("offset", "bytes_len", "base_char", "range_bits", "text", "mode")

    def __init__(self, offset, bytes_len, base_char, range_bits, text, mode):
        self.offset = offset
        self.bytes_len = bytes_len
        self.base_char = base_char
        self.range_bits = range_bits
        self.text = text
        self.mode = mode


def _decode_packed(payload, base_char, range_bits):
    """Structural (framing-correct, NOT byte-exact) decode of the
    bit-packed path -- see module docstring: missing the RC4 keystream
    XOR that real game data requires. Kept for record bookkeeping and
    as a starting point for finishing this once the key is known.
    """
    n_symbols = (8 * len(payload)) // range_bits + 1
    bitbuf = 0
    bitcount = 0
    pi = 0
    out = []
    for _ in range(n_symbols):
        while bitcount <= 24 and pi < len(payload):
            bitbuf |= payload[pi] << bitcount
            bitcount += 8
            pi += 1
        symbol = bitbuf & ((1 << range_bits) - 1)
        bitcount -= range_bits
        bitbuf >>= range_bits
        if symbol == 0:
            out.append(0)
        elif symbol < 0x20:
            out.append(ord(SPECIAL_TABLE[symbol - 1]))
        else:
            out.append(symbol + base_char - 32)
    if 0 in out:
        out = out[: out.index(0)]
    return "".join(chr(c) if c < 0x110000 else "�" for c in out)


def decode_record(data, off):
    bytes_len, base_char = struct.unpack_from("<HH", data, off)
    range_bits = data[off + 4]
    payload = data[off + 6 : off + bytes_len]

    if base_char == 0 and range_bits == 16 and len(payload) % 2 == 0:
        text = payload.decode("utf-16le", errors="replace")
        return StringRecord(off, bytes_len, base_char, range_bits, text, "raw-utf16")

    text = _decode_packed(payload, base_char, range_bits)
    return StringRecord(off, bytes_len, base_char, range_bits, text, "packed*")


def parse_strs(data):
    if data[0:4] != b"strs":
        raise ValueError(f"not a strs container (magic={data[0:4]!r})")
    off = 4
    records = []
    while off + 6 <= len(data):
        rec_len = struct.unpack_from("<H", data, off)[0]
        if rec_len < 6 or off + rec_len > len(data):
            break
        rec = decode_record(data, off)
        records.append(rec)
        off += rec_len
    return records, off


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "out_2926.bin"
    with open(path, "rb") as f:
        data = f.read()
    records, consumed = parse_strs(data)

    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    print(f"{path}: {len(records)} records, {consumed}/{len(data)} bytes consumed")
    for r in records:
        note = "" if r.mode == "raw-utf16" else "  [* structural only, not byte-exact -- see docstring]"
        print(
            f"  off={r.offset:#06x} len={r.bytes_len:3d} base={r.base_char:#04x} "
            f"rangeBits={r.range_bits:2d} [{r.mode}]{note}  {r.text!r}"
        )


if __name__ == "__main__":
    main()
