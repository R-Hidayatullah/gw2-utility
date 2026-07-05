import time
from strs_rc4_decrypt import derive_rc4_key, rc4_ksa, rc4_crypt

payload = bytes.fromhex("aaf565cab6053c5af3c85c0fa1e331efb81fcbe17e043188cece8e9610dee90c2a768a15efcbeaba74639ece674f396d069678a58f27400c1215384385db4cf197bacf")
N = 5000
t0 = time.time()
for key_seed in range(N):
    key = derive_rc4_key(key_seed)
    s_box = rc4_ksa(key)
    decrypted = rc4_crypt(s_box, payload)
dt = time.time() - t0
print(f"{N} iterations took {dt:.2f}s -> {N/dt:.0f} keys/sec")
