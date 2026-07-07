# =====================================================================
# dump_gw2_structs.py  -- akurat, 64-bit
# Dump semua definisi struct ArenaNet "Packfile" (def/typeinfo) dari IDB
# Gw2-64 ke output.txt, untuk dipakai template 010 Editor (.bt).
#
# Jalankan lewat IDA (File > Script file) ATAU via ida-pro-mcp py_exec_file.
#
# Format yang diparse (TERVERIFIKASI di biner):
#   chunk_info (32B) : char name[4]; u32 nVersions; u64 strucTab; u64 secondary; u64 pad
#   strucTab         : array entri 24B; entri[i] -> { u64 descPtr; u64 refFunc; u64 pad }
#   descriptor entry : 32B { u16 type@0x00; u64 namePtr@0x08; u64 childPtr@0x10; u64 count@0x18 }
#                      entri terminator: type==0, namePtr menunjuk NAMA struct.
#
# Relokasi pointer di runtime GW2 memakai offset-table yang dibaking per-chunk
# (bukan type-walking) -> ukuran skalar = konstanta format di bawah ini.
# =====================================================================
import ida_bytes, ida_segment, idc, os

OUT_PATH   = r"C:\Users\Ridwan Hidayatullah\Documents\gw2struct\output.txt"
STATE_PATH = OUT_PATH + ".state"   # penanda resume (hapus utk mulai dari nol)
DONE_PATH  = OUT_PATH + ".complete"

def u16(a): return ida_bytes.get_word(a)
def u32(a): return ida_bytes.get_dword(a)
def u64(a): return ida_bytes.get_qword(a)

def seg(name):
    s = ida_segment.get_segm_by_name(name)
    return (s.start_ea, s.end_ea) if s else (0, 0)

RD0, RD1 = seg(".rdata")
if RD0 == 0:
    RD0, RD1 = seg(".text")

def is_ascii(b):
    return (48 <= b <= 57) or (65 <= b <= 90) or (97 <= b <= 122)

def cstr(a, maxlen=128):
    if not a:
        return ""
    out = []
    for _ in range(maxlen):
        b = ida_bytes.get_byte(a)
        if b == 0:
            break
        out.append(chr(b))
        a += 1
    return "".join(out)

# ---------------------------------------------------------------------
# Peta type-ID -> nama tipe .bt.  Sumber: parser packfile + validasi scan.
# 'child' = ID yang memiliki sub-descriptor (harus di-recurse).
# UKURAN (untuk .bt) dicantumkan di legenda; dumper hanya butuh NAMA.
# ---------------------------------------------------------------------
SIMPLE = {
    0x04: "Unknown0x04",   # tak muncul di build ini
    0x05: "byte",          # 1
    0x06: "byte4",         # 4
    0x07: "double",        # 8  (tak muncul)
    0x08: "Unknown0x08",   # tak muncul
    0x09: "Unknown0x09",   # tak muncul
    0x0A: "dword",         # 4
    0x0B: "filename",      # ptr(8)  -> fileref terbungkus TPTR
    0x0C: "float",         # 4
    0x0D: "float2",        # 8
    0x0E: "float3",        # 12
    0x0F: "float4",        # 16
    0x11: "qword",         # 8
    0x12: "wchar_ptr",     # ptr(8)
    0x13: "char_ptr",      # ptr(8)
    0x15: "word",          # 2
    0x16: "byte16",        # 16
    0x17: "byte3",         # 3
    0x18: "dword2",        # 8
    0x19: "dword4",        # 16
    0x1A: "word3",         # 6  (tak muncul)
    0x1B: "fileref",       # 4
    0x1C: "variant0x1C",   # ? (2x di 'mach/actionData') -- lihat legenda, default 4
    0x24: "token32",       # 4  TERVERIFIKASI: type-info table ArenaNet id 0x24 = "token32"
    0x25: "token64",       # 8  TERVERIFIKASI: type-info table ArenaNet id 0x25 = "token64"
}
CHILD = {0x01, 0x02, 0x03, 0x10, 0x14, 0x1D}

LEGEND = """/* =====================================================================
 * TIPE DATA (untuk .bt) -- ukuran byte:
 *   byte=1  word=2  byte3=3  dword=4  byte4=4  fileref=4  float=4  token32=4
 *   float2=8  double=8  qword=8  token64=8  dword2=8  word3=6
 *   float3=12  byte16=16  dword4=16  float4=16
 *   wchar_ptr/char_ptr/filename = pointer(8, terbungkus TPTR)
 *   token32(0x24)=4 : TERVERIFIKASI dari type-info table ArenaNet (nama "token32")
 *   token64(0x25)=8 : TERVERIFIKASI dari type-info table ArenaNet (nama "token64")
 *   variant0x1C     : BELUM terbukti; default 4B; hanya 'mach/actionData'
 * TIPE TAK DIPAKAI di build ini: 0x04,0x07(double),0x08,0x09,0x1A(word3)
 * ===================================================================== */

"""

# ---------------------------------------------------------------------
# Validasi (sama logika dgn scan yg sudah dicocokkan)
# ---------------------------------------------------------------------
def is_anstruct(a):
    guard, cur = 50, a
    while u16(cur) != 0 and guard > 0:
        if u16(cur) > 0x1D:
            return False
        cur += 32
        guard -= 1
    if guard == 0:
        return False
    namep = u64(cur + 8)
    return namep != idc.BADADDR and namep != 0 and is_ascii(ida_bytes.get_byte(namep))

def is_anstructtab(a, n):
    cur = a
    for _ in range(n):
        p = u64(cur)
        if p != 0 and not is_anstruct(p):
            return False
        cur += 24
    return True

# ---------------------------------------------------------------------
# Emit typedef (children dulu, lalu parent) -- kompatibel dgn output IDC
# ---------------------------------------------------------------------
def struct_name(desc):
    cur = desc
    while u16(cur) != 0:
        cur += 32
    return cstr(u64(cur + 8))

def is_simple_wrapper(desc):
    # descriptor yang hanya membungkus 1 tipe skalar: nama entri-0 kosong
    np = u64(desc + 8)
    return np == 0 or ida_bytes.get_byte(np) == 0

def parse_member(entry, emitted, buf):
    tid = u16(entry)
    mname = cstr(u64(entry + 8))
    child = u64(entry + 16)
    count = u64(entry + 24)
    if tid == 0x01:
        return "%s %s[%d]" % (parse_struct(child, emitted, buf), mname, count)
    if tid == 0x02:
        return "TSTRUCT_ARRAY_PTR_START %s %s TSTRUCT_ARRAY_PTR_END" % (parse_struct(child, emitted, buf), mname)
    if tid == 0x03:
        return "TSTRUCT_PTR_ARRAY_PTR_START %s %s TSTRUCT_PTR_ARRAY_PTR_END" % (parse_struct(child, emitted, buf), mname)
    if tid == 0x10:
        return "TPTR_START %s %s TPTR_END" % (parse_struct(child, emitted, buf), mname)
    if tid in (0x14, 0x1D):
        return "%s %s" % (parse_struct(child, emitted, buf), mname)
    name = SIMPLE.get(tid)
    if name is None:
        print("  [WARN] typeId tak dikenal 0x%X pada '%s'" % (tid, mname))
        name = "ERROR0x%X" % tid
    return "%s %s" % (name, mname)

def parse_struct(desc, emitted, buf):
    if desc == 0:
        return "void"
    if is_simple_wrapper(desc):
        return SIMPLE.get(u16(desc), "ERROR0x%X" % u16(desc))
    name = struct_name(desc)
    if desc in emitted:
        return name
    emitted.add(desc)
    members, cur = [], desc
    while u16(cur) != 0:
        members.append(parse_member(cur, emitted, buf))
        cur += 32
    body = "".join("    %s;\n" % m for m in members)
    buf.append("typedef struct {\n%s} %s;\n\n" % (body, name))
    return name

# ---------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------
def find_chunks():
    seen, chunks, a = set(), [], RD0
    while a < RD1 - 16:
        b0, b1, b2, b3 = (ida_bytes.get_byte(a + i) for i in range(4))
        if is_ascii(b0) and is_ascii(b1) and is_ascii(b2) and (b3 == 0 or is_ascii(b3)):
            name = chr(b0) + chr(b1) + chr(b2) + (chr(b3) if b3 else "")
            nver = u32(a + 4)
            if 0 < nver < 100:
                tab = u64(a + 8)
                if RD0 < tab < RD1 and tab not in seen and is_anstructtab(tab, nver):
                    seen.add(tab)
                    chunks.append((name, nver, tab))
        a += 4
    return chunks

def load_state():
    hdr, ver = set(), set()
    if os.path.exists(STATE_PATH):
        for ln in open(STATE_PATH):
            p = ln.split()
            if len(p) == 2 and p[0] == "H":
                hdr.add(int(p[1], 16))
            elif len(p) == 3 and p[0] == "V":
                ver.add((int(p[1], 16), int(p[2])))
    return hdr, ver

def main():
    chunks = find_chunks()
    fresh = not os.path.exists(STATE_PATH)
    hdr_done, ver_done = load_state()
    fout = open(OUT_PATH, "w" if fresh else "a")
    fstate = open(STATE_PATH, "w" if fresh else "a")
    if fresh:
        fout.write(LEGEND)
        fout.flush()
    try:
        for name, nver, tab in chunks:
            if tab not in hdr_done:
                fout.write("/* ===============================================\n")
                fout.write(" * Chunk: %s, versions: %d, strucTab: 0x%X\n" % (name, nver, tab))
                fout.write(" * ===============================================\n */\n\n")
                fout.flush()
                fstate.write("H %X\n" % tab); fstate.flush()
                hdr_done.add(tab)
            for i in range(nver - 1, -1, -1):
                if (tab, i) in ver_done:
                    continue
                desc = u64(tab + 24 * i)
                if desc:
                    reff = u64(tab + 24 * i + 8)   # FIX: +8 (bukan +4 warisan 32-bit)
                    if reff:
                        fout.write("/* Version: %d, ReferencedFunction: 0x%X */\n" % (i, reff))
                    else:
                        fout.write("/* Version: %d */\n" % i)
                    buf = []
                    parse_struct(desc, set(), buf)
                    fout.write("".join(buf))
                    fout.flush()
                fstate.write("V %X %d\n" % (tab, i)); fstate.flush()
                ver_done.add((tab, i))
            fout.write("\n"); fout.flush()
    finally:
        fout.close(); fstate.close()
    open(DONE_PATH, "w").write("ok")
    print("COMPLETE: %d chunk -> %s" % (len(chunks), OUT_PATH))

main()
