# gw2_capture_textkeys.py  (v2 - live-memory safe)
# Jalankan DI IDA setelah proses Gw2 berjalan di bawah debugger (base 0x140000000).
#
#  - BP sub_1410CBBB0(textId /*ECX*/, key8 /*RDX*/): tangkap MURNI dari register.
#  - BP sub_1410D6A10(_, chunk /*RDX*/): bulk txtp; dibaca via read_dbg_memory
#    dengan sanity-check (banyak pemanggil lain => di-skip kalau tak masuk akal).
#  - dump_map(): snapshot langsung hashmap ctx+512 (paling andal; panggil kapan saja
#    dari IDA console SETELAH login & sudah pindah/masuk map).
#
# Output CSV: textId,key8_hex

import ida_dbg, ida_idd, idaapi, struct, datetime

BASE      = 0x140000000
BP_INSERT = BASE + 0x10CBBB0
BP_BULK   = BASE + 0x10D6A10
TLSINDEX  = BASE + 0x27C9140     # dword TlsIndex
OUT = r"C:\Users\Ridwan Hidayatullah\Documents\gw2app\textkeys.csv"

_seen = set()
def _emit(tid, key8, tag):
    if not key8 or (tid, key8) in _seen:
        return
    _seen.add((tid, key8))
    with open(OUT, "a", encoding="utf-8") as fh:
        fh.write("%d,%016x\n" % (tid, key8))
    print("[%s] textId=%d key8=%016x" % (tag, tid, key8))

def _rd(ea, n):
    b = ida_dbg.read_dbg_memory(ea, n)
    return b if b and len(b) == n else None

def _reg(name):
    rv = ida_idd.regval_t(); ida_dbg.get_reg_val(name, rv); return rv.ival

def _u(b): return int.from_bytes(b, "little")

# ---- ctx via TLS of the current (stopped) thread ----
def get_ctx():
    # ctx = *(*(TEB.ThreadLocalStoragePointer[TlsIndex]) + 8)
    teb = _reg("FSBASE") if False else None
    # IDA exposes TEB base per-thread; use the debugger's thread TEB:
    import ida_dbg as D
    teb = D.get_thread_sreg_base(D.get_current_thread(), 0)  # gs base on x64
    if not teb:
        print("[dump] no TEB"); return None
    tls_arr = _u(_rd(teb + 0x58, 8))            # TEB.ThreadLocalStoragePointer
    idx = _u(_rd(TLSINDEX, 4))
    slot = _u(_rd(tls_arr + 8*idx, 8))
    ctx  = _u(_rd(slot + 8, 8))
    return ctx

def dump_map(ctx=None):
    if ctx is None: ctx = get_ctx()
    if not ctx:
        print("[dump] ctx unknown - run while stopped at a BP on the text thread"); return
    m = ctx + 512
    size    = _u(_rd(m + 0, 4))
    count   = _u(_rd(m + 4, 4))
    entries = _u(_rd(m + 8, 8))
    print("[dump] ctx=%#x map size=%d count=%d entries=%#x" % (ctx, size, count, entries))
    n = 0
    for i in range(size):
        e = entries + 24*i
        occ = _u(_rd(e + 16, 4))
        if not occ: continue
        tid = _u(_rd(e + 0, 4)); key8 = _u(_rd(e + 8, 8))
        _emit(tid, key8, "map"); n += 1
    print("[dump] wrote %d entries -> %s" % (n, OUT))

class Hook(ida_dbg.DBG_Hooks):
    def dbg_bpt(self, tid, ea):
        if ea == BP_INSERT:
            _emit(_reg("RCX") & 0xFFFFFFFF, _reg("RDX") & (2**64-1), "key")
        elif ea == BP_BULK:
            chunk = _reg("RDX")
            hdr = _rd(chunk + 2, 9)               # count(1) + ptr(8)
            if hdr:
                count = hdr[0]; arr = _u(hdr[1:9])
                if 0 < count <= 255 and arr > 0x10000:
                    for i in range(count):
                        r = _rd(arr + 12*i, 12)
                        if not r: break
                        _emit(_u(r[0:4]), _u(r[4:12]), "bulk")
        ida_dbg.continue_process()
        return 0

try: _hook.unhook()
except Exception: pass
_hook = Hook(); _hook.hook()
for bp in (BP_INSERT, BP_BULK):
    ida_dbg.add_bpt(bp, 0, idaapi.BPT_SOFT)
print("Armed. BP insert=%#x bulk=%#x. After login run: dump_map()" % (BP_INSERT, BP_BULK))
