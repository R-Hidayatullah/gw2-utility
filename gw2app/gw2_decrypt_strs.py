#!/usr/bin/env python3
# gw2_decrypt_strs.py  -- decrypt GW2 password-protected (packed) strings.
#
# Terverifikasi: textId 1601 -> "The Cost of Victory", 611643 -> "Second Birthday Gift".
#
# Input:
#   textkeys.csv          : hasil capture debugger (textId,key8_hex)  [lihat gw2_capture_textkeys.py]
#   folder STRS_DIR       : file strs sudah di-extract, dinamai "<fileId>.bin"
#                           (extract via gw2mcp gw2_extract / tool dat Anda)
# Mapping (bahasa-0): fileId = STRS_BASE + textId//STRINGS_PER_FILE ; record = textId % STRINGS_PER_FILE
#
# Output: textId,plaintext ke stdout / decrypted.csv
#
# Cara dapat fileId yang perlu di-extract:  python gw2_decrypt_strs.py --list-files

import os, sys, csv, struct, argparse

STRINGS_PER_FILE = 1024
# Peta indeks->fileId untuk bahasa-0 (lang0) dari TextPackManifest (base 58570).
# lang0 TIDAK sepenuhnya berurutan (konten baru melompat ke range fileId lain),
# jadi kita pakai daftar sebenarnya, satu fileId per baris (baris i = indeks i).
LANG0_MAP_FILE   = "lang0_files.txt"

def load_lang0(path):
    return [int(x) for x in open(path).read().split()]
TABLE            = "0123456strnum()[]<>%#/:-'\" ,.!\n"   # simbol 1..31 (@0x1420f3100)
M = 0xFFFFFFFF

def rol(x, n): x &= M; return ((x << n) | (x >> (32 - n))) & M

def expand_key(key8):                       # port sub_140D9F630 (a1=8): 8B -> 20B
    kb = struct.pack("<Q", key8)
    A, B, C, D, E = struct.unpack("<5I", bytes(kb[i % 8] for i in range(20)))
    t   = (A - 1615554381) & M
    v15 = (rol(t, 5) + B + 1722862861) & M
    v16 = rol(t, 30)
    v17 = (((~(t & 0x22222222)) & 0x7BF36AE2) - 214083945 + C + rol(v15, 5)) & M
    v18 = ((v15 & (v16 ^ 0x59D148C0)) ^ 0x59D148C0) & M
    v19 = rol(v15, 30)
    v20 = (v18 - 696916869 + D + rol(v17, 5)) & M
    return struct.pack("<5I",
        (A + rol(v20, 5) + E + (v16 ^ (v17 & (v19 ^ v16))) - 1269579175) & M,
        (B + v20) & M, (C + rol(v17, 30)) & M, (D + v19) & M, (E + v16) & M)

def rc4(key, buf):                          # CptRc4 (sub_140D9F4E0 KSA + standard PRGA)
    S = list(range(256)); j = 0
    for i in range(256):
        j = (j + S[i] + key[i % len(key)]) & 0xFF; S[i], S[j] = S[j], S[i]
    out = bytearray(); i = j = 0
    for b in buf:
        i = (i + 1) & 0xFF; j = (j + S[i]) & 0xFF; S[i], S[j] = S[j], S[i]
        out.append(b ^ S[(S[i] + S[j]) & 0xFF])
    return bytes(out)

def bitunpack(pl, baseChar, rangeBits):     # port loop sub_1410CFC50
    acc = n = p = 0; out = []
    for _ in range(8 * len(pl) // rangeBits + 1):
        while n <= 24:
            if p < len(pl): acc |= pl[p] << n; p += 1
            n += 8
        s = acc & ((1 << rangeBits) - 1); acc >>= rangeBits; n -= rangeBits
        if s == 0: break
        out.append(TABLE[s - 1] if s < 0x20 else chr(s + baseChar - 32))
    return ''.join(out)

def parse_strs(path):                       # -> list[(baseChar, rangeBits, payload)]
    d = open(path, "rb").read()
    if d[:4] != b"strs": raise ValueError("not a strs file: " + path)
    off = 4; es = []
    while off + 6 <= len(d):
        L = int.from_bytes(d[off:off+2], "little")
        if L < 6: break
        es.append((int.from_bytes(d[off+2:off+4], "little"), d[off+4], d[off+6:off+L]))
        off += L
    return es

def decode_record(entry, key8):
    bc, rb, pl = entry
    if bc == 0 and rb == 16:                # raw-utf16, tak terenkripsi
        return pl.decode("utf-16-le", "replace").rstrip("\x00")
    if key8 is not None:
        pl = rc4(expand_key(key8), pl)
    return bitunpack(pl, bc, rb)

def load_keys(csv_path):
    keys = {}
    for row in csv.reader(open(csv_path, encoding="utf-8")):
        if not row or row[0].startswith("#") or row[0] == "textId": continue
        tid = int(row[0])
        if tid == 0xFFFFFFFF: continue      # sentinel
        keys[tid] = int(row[1], 16)
    return keys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keys", default="textkeys.csv")
    ap.add_argument("--strs-dir", default="strs")
    ap.add_argument("--out", default="decrypted.csv")
    ap.add_argument("--list-files", action="store_true",
                    help="cetak fileId unik yang perlu di-extract, lalu keluar")
    ap.add_argument("--lang0", default=LANG0_MAP_FILE)
    a = ap.parse_args()
    keys = load_keys(a.keys)
    lang0 = load_lang0(a.lang0)
    need = {}
    for tid in keys:
        fi = tid // STRINGS_PER_FILE
        if fi < len(lang0): need.setdefault(lang0[fi], []).append(tid)
    if a.list_files:
        for fid in sorted(need): print(fid)
        print(f"# {len(need)} file, {len(keys)} kunci", file=sys.stderr); return
    w = csv.writer(open(a.out, "w", encoding="utf-8", newline=""))
    w.writerow(["textId", "text"]); ok = miss = 0
    for fid in sorted(need):
        fp = os.path.join(a.strs_dir, f"{fid}.bin")
        if not os.path.exists(fp): miss += len(need[fid]); continue
        es = parse_strs(fp)
        for tid in need[fid]:
            r = tid % STRINGS_PER_FILE
            if r < len(es):
                txt = decode_record(es[r], keys[tid]); w.writerow([tid, txt]); ok += 1
                sys.stdout.buffer.write(f"{tid}\t{txt}\n".encode("utf-8", "replace"))
    print(f"\n[done] {ok} didekripsi, {miss} file hilang -> {a.out}", file=sys.stderr)

if __name__ == "__main__":
    main()
