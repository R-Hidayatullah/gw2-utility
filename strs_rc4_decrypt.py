"""
RC4-keyed decode for GW2 "strs" records where StringHeader.baseChar != 0
(the "packed"/cross-reference path in strs_decode.py, previously
un-decryptable there). See STRS_TEXT_RESEARCH_NOTES.md "Session 3" for
how each piece below was recovered.

Pipeline (all reverse-engineered from Gw2-64-disable-aslr.exe):

  keySeed (64-bit int, the "a5" argument to sub_1410CFC50)
    -> 8 raw bytes (little-endian)                              [trivial]
    -> cycled to 20 bytes: buf[i] = seed_bytes[i % 8]            [sub_140D9F630]
    -> one round of a SHA-1-like mixer over those 20 bytes       [sub_140D9F630]
    -> that 20-byte result is the real RC4 key
  RC4 key (20 bytes)
    -> standard RC4 KSA (256-byte S-box)                         [sub_140D9F4E0]
  payload bytes
    -> standard RC4 PRGA, XORed with payload, same length out    [sub_140D9F8C0,
                                                                    the RC4 object's
                                                                    vtable slot 0]
  decrypted bytes
    -> bit-unpack same as strs_decode.py's _decode_packed()       [sub_1410CFC50]

VERIFICATION STATUS (see STRS_TEXT_RESEARCH_NOTES.md Session 3 for the
full writeup): live-captured from the running game via IDA's debugger --
- keySeed == 0 is confirmed correct: the header's early-exit fires
  (`if baseChar != 0 and keySeed == 0: return ""`), and the caller's
  fallback display ("[null]") matches exactly.
- One live sample with a genuinely non-zero keySeed did NOT reproduce the
  game's own decoded output when run through this pipeline. The KSA/PRGA
  steps come directly from decompiled code with no ambiguity, so the most
  likely explanation is that the *captured* keySeed for that sample was
  unreliable (repeat hits of the same decodeId sometimes returned a
  different keySeed value across calls -- one of them suspiciously equal
  to an unrelated context pointer -- pointing at an occasional bad stack
  read during capture, not a bug in the algorithm below).
- In short: treat this as the best-understood, best-effort implementation
  of the documented algorithm, not yet a byte-exact-verified decoder.
"""
import struct


def _rol32(x: int, n: int) -> int:
    x &= 0xFFFFFFFF
    return ((x << n) | (x >> (32 - n))) & 0xFFFFFFFF


def _mix20(buf20: bytes) -> bytes:
    """One round of sub_140D9F630's SHA-1-like compression over 5 dwords."""
    d0, d1, d2, d3, d4 = struct.unpack("<5I", buf20)

    t = (d0 - 1615554381) & 0xFFFFFFFF
    v15 = (_rol32(t, 5) + d1 + 1722862861) & 0xFFFFFFFF
    v16 = _rol32(t, 30)
    v17 = ((~(t & 0x22222222) & 0x7BF36AE2) - 214083945 + d2 + _rol32(v15, 5)) & 0xFFFFFFFF
    v18 = (v15 & (v16 ^ 0x59D148C0) ^ 0x59D148C0) & 0xFFFFFFFF
    v19 = _rol32(v15, 30)
    v20 = (v18 - 696916869 + d3 + _rol32(v17, 5)) & 0xFFFFFFFF

    d2_new = (d2 + _rol32(v17, 30)) & 0xFFFFFFFF
    d0_new = (d0 + _rol32(v20, 5) + d4 + (v16 ^ (v17 & (v19 ^ v16))) - 1269579175) & 0xFFFFFFFF
    d1_new = (d1 + v20) & 0xFFFFFFFF
    d3_new = (d3 + v19) & 0xFFFFFFFF
    d4_new = (d4 + v16) & 0xFFFFFFFF
    return struct.pack("<5I", d0_new, d1_new, d2_new, d3_new, d4_new)


def derive_rc4_key(key_seed: int) -> bytes:
    """keySeed (a5, a plain 64-bit int, not a pointer) -> 20-byte RC4 key."""
    seed8 = key_seed.to_bytes(8, "little")
    buf20 = bytes(seed8[i % 8] for i in range(20))
    return _mix20(buf20)


def rc4_ksa(key: bytes) -> list[int]:
    s = list(range(256))
    j = 0
    keylen = len(key)
    for i in range(256):
        j = (j + s[i] + key[i % keylen]) & 0xFF
        s[i], s[j] = s[j], s[i]
    return s


def rc4_crypt(s_box: list[int], data: bytes) -> bytes:
    """Standard RC4 PRGA + XOR (self-inverse: same call encrypts or decrypts)."""
    s = s_box[:]
    i = j = 0
    out = bytearray(len(data))
    for k, b in enumerate(data):
        i = (i + 1) & 0xFF
        j = (j + s[i]) & 0xFF
        s[i], s[j] = s[j], s[i]
        out[k] = b ^ s[(s[i] + s[j]) & 0xFF]
    return bytes(out)


SPECIAL_TABLE = "0123456strnum()[]<>%#/:-'\" ,.!\n"  # symbol 1..31 -> table[symbol-1]


def _bit_unpack(payload: bytes, base_char: int, range_bits: int) -> str:
    """Same algorithm as strs_decode.py's _decode_packed, factored out here."""
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


def decrypt_rc4_record(payload: bytes, base_char: int, range_bits: int, key_seed: int) -> str:
    """Full pipeline: keySeed -> RC4 key -> decrypt payload -> bit-unpack -> text.

    If key_seed == 0 (and base_char != 0), the real function returns an
    empty string without even reaching the RC4 step -- mirrored here.
    """
    if base_char != 0 and key_seed == 0:
        return ""
    key = derive_rc4_key(key_seed)
    s_box = rc4_ksa(key)
    decrypted = rc4_crypt(s_box, payload)
    return _bit_unpack(decrypted, base_char, range_bits)


if __name__ == "__main__":
    import sys
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")

    # Samples captured live via IDA debugger attach, see strs_rc4_key_log.jsonl
    # and STRS_TEXT_RESEARCH_NOTES.md "Session 3" for provenance/caveats.
    samples = [
        # (label, payload_hex, base_char, range_bits, key_seed, ground_truth_or_note)
        ("decodeId=43462 (keySeed=0)", "859c7a24ac82db8e727338fb734e", 66, 7, 0x0,
         'game returned "[null]" -- CONFIRMED matches (early-exit on keySeed=0)'),
        ("decodeId=44576 (keySeed=0xb830, seen 3x consistently)", "c7ffa52ee2c7", 86, 6, 0xb830,
         "no live ground truth captured for this one"),
        ("decodeId=257809 (keySeed=0x9ae7f6bc)", "7ab31e09cb9764b9bc35ee476c0f3b", 66, 7, 0x9ae7f6bc,
         'game returned "슓¥" -- does NOT match; see caveats in module docstring'),
    ]
    for label, payload_hex, base_char, range_bits, key_seed, note in samples:
        text = decrypt_rc4_record(bytes.fromhex(payload_hex), base_char, range_bits, key_seed)
        print(f"{label}")
        print(f"  decoded : {text!r}")
        print(f"  note    : {note}")
        print()
