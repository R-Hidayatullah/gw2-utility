import sys
sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
from strs_rc4_decrypt import derive_rc4_key, rc4_ksa, rc4_crypt, _bit_unpack

def score(text: str) -> float:
    if not text:
        return -1.0
    printable = sum(1 for c in text if 0x20 <= ord(c) <= 0x7e)
    return printable / len(text)

def brute_force(payload_hex, base_char, range_bits, max_key, top_n=10, label=""):
    payload = bytes.fromhex(payload_hex)
    results = []
    for key_seed in range(max_key):
        key = derive_rc4_key(key_seed)
        s_box = rc4_ksa(key)
        decrypted = rc4_crypt(s_box, payload)
        text = _bit_unpack(decrypted, base_char, range_bits)
        sc = score(text)
        if sc >= 0.9 and len(text) >= 4:
            results.append((sc, key_seed, text))
    results.sort(key=lambda x: (-x[0], x[1]))
    print(f"=== {label} (payload_len={len(payload)}, searched 0..{max_key-1}) ===")
    print(f"candidates with >=90% printable ascii, len>=4: {len(results)}")
    for sc, ks, text in results[:top_n]:
        print(f"  keySeed={ks:#x} ({ks}) score={sc:.2f}  text={text!r}")
    print()

# hit14: longest payload (67 bytes) -> strongest discriminating power
brute_force("aaf565cab6053c5af3c85c0fa1e331efb81fcbe17e043188cece8e9610dee90c2a768a15efcbeaba74639ece674f396d069678a58f27400c1215384385db4cf197bacf",
            68, 7, 2_000_000, label="hit14 decodeId=303972")

# hit13: shorter (15 bytes) - cross-check
brute_force("7ab31e09cb9764b9bc35ee476c0f3b", 66, 7, 2_000_000, label="hit13 decodeId=257809")
