"""
GW2 "strs" (TEXT_STRINGS_SIGNATURE) string-table decoder.

Ported from the repo's strs_decode.py (reverse-engineered from
Gw2-64.exe: Engine\\Text\\TextCache.cpp / TextDecode.cpp, and
Arena\\Services\\Crypt\\CptRc4.cpp). Operates on the *decompressed* bytes of a
Gw2.dat entry (Method0-decompressed by the native CLI upstream).

Container (little-endian): magic "strs", then back-to-back records, no count.
Each record: u16 total length (incl. 6-byte header), u16 baseChar,
u8 rangeBits, u8 pad, then payload.

Two decode paths:
  * raw-utf16  (baseChar==0, rangeBits==16, even payload) -- CONFIRMED byte-exact
    against real game text. `confirmed=True`.
  * packed     (baseChar!=0, or rangeBits!=16) -- bit-packed symbols. The real
    game path additionally XORs an RC4 keystream keyed by runtime cross-
    reference state that isn't present in a standalone entry, so the text here
    is STRUCTURAL ONLY (framing correct, characters not real). `confirmed=False`.
"""

from __future__ import annotations

import struct
from typing import Any

SPECIAL_TABLE = "0123456strnum()[]<>%#/:-'\" ,.!\n"  # symbol 1..31 -> table[symbol-1]


def _decode_packed(payload: bytes, base_char: int, range_bits: int) -> str:
    """Structural (framing-correct, NOT byte-exact) bit-unpack -- see module
    docstring: missing the RC4 keystream XOR real game data requires."""
    if range_bits <= 0:
        return ""
    n_symbols = (8 * len(payload)) // range_bits + 1
    bitbuf = 0
    bitcount = 0
    pi = 0
    out: list[int] = []
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


def decode_record(data: bytes, off: int) -> dict[str, Any]:
    bytes_len, base_char = struct.unpack_from("<HH", data, off)
    range_bits = data[off + 4]
    payload = data[off + 6 : off + bytes_len]

    if base_char == 0 and range_bits == 16 and len(payload) % 2 == 0:
        return {
            "offset": off,
            "length": bytes_len,
            "baseChar": base_char,
            "rangeBits": range_bits,
            "mode": "raw-utf16",
            "confirmed": True,
            "text": payload.decode("utf-16le", errors="replace"),
        }
    return {
        "offset": off,
        "length": bytes_len,
        "baseChar": base_char,
        "rangeBits": range_bits,
        "mode": "packed",
        "confirmed": False,
        "text": _decode_packed(payload, base_char, range_bits),
    }


def parse_strs(data: bytes) -> dict[str, Any]:
    """Parse a full 'strs' buffer. Returns a dict with metadata + records list."""
    if data[0:4] != b"strs":
        return {"ok": False, "error": f"not a strs container (magic={data[0:4]!r})"}
    off = 4
    records: list[dict[str, Any]] = []
    while off + 6 <= len(data):
        rec_len = struct.unpack_from("<H", data, off)[0]
        if rec_len < 6 or off + rec_len > len(data):
            break
        records.append(decode_record(data, off))
        off += rec_len
    confirmed = sum(1 for r in records if r["confirmed"])
    return {
        "ok": True,
        "recordCount": len(records),
        "confirmedCount": confirmed,
        "packedCount": len(records) - confirmed,
        "bytesConsumed": off,
        "bytesTotal": len(data),
        "records": records,
    }
