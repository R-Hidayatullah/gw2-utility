# =====================================================================
# gen_gw2_json.py -- hasilkan registry JSON packfile GW2 dari IDB (64-bit)
# Output: minieditor/templates/gw2_packfile.json
#
# Skema:
#   { "format":"gw2packfile", "pointerSize":64,
#     "chunks": { "MODL": {"70":"<typeKey>", ...}, ... },
#     "types":  { "<typeKey>": { "fields":[ <field>... ] }, ... } }
#
#   <field> = { "name":str, "kind":str, ...extra }
#     kind primitif (ukuran tetap):
#        byte byte3 byte4 byte16 word word3 dword dword2 dword4 qword
#        float float2 float3 float4 double fileref token32 token64
#     kind pointer-ke-data (lebar = pointerSize): filename wchar_ptr char_ptr
#     kind "struct"        -> +"type": "<typeKey>"       (struct inline)
#     kind "array"         -> +"element":<elem>,"count":int   (array inline, 0x01)
#     kind "array_ptr"     -> +"element":<elem>               (count u32 + ptr, 0x02)
#     kind "ptr_array_ptr" -> +"element":<elem>               (0x03)
#     kind "ptr"           -> +"target":<elem>                (0x10)
#   <elem> = string primitif  ATAU  { "struct":"<typeKey>" }
# =====================================================================
import ida_bytes, ida_segment, idc, json, os

OUT_PATH = r"C:\Users\Ridwan Hidayatullah\Documents\gw2struct\minieditor\templates\gw2_packfile.json"

def u16(a): return ida_bytes.get_word(a)
def u32(a): return ida_bytes.get_dword(a)
def u64(a): return ida_bytes.get_qword(a)

def seg(n):
    s = ida_segment.get_segm_by_name(n)
    return (s.start_ea, s.end_ea) if s else (0, 0)
RD0, RD1 = seg(".rdata")
if RD0 == 0: RD0, RD1 = seg(".text")

def is_ascii(b): return (48<=b<=57) or (65<=b<=90) or (97<=b<=122)
def cstr(a, m=128):
    if not a: return ""
    o=[]
    for _ in range(m):
        b=ida_bytes.get_byte(a)
        if b==0: break
        o.append(chr(b)); a+=1
    return "".join(o)

SIMPLE = {
    0x05:"byte", 0x06:"byte4", 0x07:"double", 0x0A:"dword", 0x0B:"filename",
    0x0C:"float", 0x0D:"float2", 0x0E:"float3", 0x0F:"float4", 0x11:"qword",
    0x12:"wchar_ptr", 0x13:"char_ptr", 0x15:"word", 0x16:"byte16", 0x17:"byte3",
    0x18:"dword2", 0x19:"dword4", 0x1A:"word3", 0x1B:"fileref",
    0x1C:"dword",        # variant0x1C (belum terbukti; 4B seperti dword)
    0x24:"token32", 0x25:"token64",
}
CHILD = {0x01,0x02,0x03,0x10,0x14,0x1D}

def is_anstruct(a):
    g,c=50,a
    while u16(c)!=0 and g>0:
        if u16(c)>0x1D: return False
        c+=32; g-=1
    if g==0: return False
    p=u64(c+8)
    return p not in (0, idc.BADADDR) and is_ascii(ida_bytes.get_byte(p))
def is_anstructtab(a,n):
    c=a
    for _ in range(n):
        p=u64(c)
        if p!=0 and not is_anstruct(p): return False
        c+=24
    return True

types = {}          # key -> {"fields":[...]}
addr2key = {}       # descriptor addr -> key
name2addr = {}      # name -> addr (untuk deteksi tabrakan nama)

def struct_name(desc):
    c=desc
    while u16(c)!=0: c+=32
    return cstr(u64(c+8))

def is_simple_wrapper(desc):
    np=u64(desc+8)
    return np==0 or ida_bytes.get_byte(np)==0

def elem_of(child):
    # kembalikan <elem>: string primitif ATAU {"struct":key}
    if child==0: return "dword"
    if is_simple_wrapper(child):
        return SIMPLE.get(u16(child), "dword")
    return {"struct": build_struct(child)}

def member(entry):
    tid=u16(entry); nm=cstr(u64(entry+8)); child=u64(entry+16); cnt=u64(entry+24)
    if tid==0x01: return {"name":nm,"kind":"array","element":elem_of(child),"count":int(cnt)}
    if tid==0x02: return {"name":nm,"kind":"array_ptr","element":elem_of(child)}
    if tid==0x03: return {"name":nm,"kind":"ptr_array_ptr","element":elem_of(child)}
    if tid==0x10: return {"name":nm,"kind":"ptr","target":elem_of(child)}
    if tid in (0x14,0x1D):
        e=elem_of(child)
        if isinstance(e,dict): return {"name":nm,"kind":"struct","type":e["struct"]}
        return {"name":nm,"kind":e}
    return {"name":nm,"kind":SIMPLE.get(tid,"dword")}

def build_struct(desc):
    if desc in addr2key: return addr2key[desc]
    name = struct_name(desc) or ("Anon_%X"%desc)
    key = name
    if name in name2addr and name2addr[name]!=desc:
        key = "%s_%X"%(name, desc & 0xFFFFF)   # disambiguasi tabrakan nama
    else:
        name2addr[name]=desc
    addr2key[desc]=key
    types[key]={"fields":[]}          # daftarkan dulu (cegah rekursi tak henti)
    fields=[]
    c=desc
    while u16(c)!=0:
        fields.append(member(c)); c+=32
    types[key]["fields"]=fields
    return key

CHUNK_INFO_STRIDE = 16   # {char name[4]; u32 nVersions; u64 strucTab}

def build_vmap(nver, tab):
    vmap={}
    for i in range(nver):
        desc=u64(tab+24*i)
        if desc: vmap[str(i)]=build_struct(desc)
    return vmap

def main():
    # 1) kumpulkan semua chunk_info (TANPA dedup) beserta alamatnya
    infos=[]  # (addr,name,nver,tab)
    a=RD0
    while a<RD1-16:
        b0,b1,b2,b3=(ida_bytes.get_byte(a+i) for i in range(4))
        if is_ascii(b0) and is_ascii(b1) and is_ascii(b2) and (b3==0 or is_ascii(b3)):
            name=chr(b0)+chr(b1)+chr(b2)+(chr(b3) if b3 else "")
            nver=u32(a+4)
            if 0<nver<100:
                tab=u64(a+8)
                if RD0<tab<RD1 and is_anstructtab(tab,nver):
                    infos.append((a,name,nver,tab))
        a+=4
    infos.sort()

    # 2) kelompokkan jadi array kontigu (stride 16) = satu file-type per array
    groups=[]; cur=[]; prev=None
    for addr,name,nver,tab in infos:
        if prev is not None and addr-prev!=CHUNK_INFO_STRIDE:
            if cur: groups.append(cur)
            cur=[]
        cur.append((name,nver,tab)); prev=addr
    if cur: groups.append(cur)

    # 3) fileTypes[containerFourcc][chunkFourcc][ver] = typeKey  (merge array sejenis)
    fileTypes={}
    for g in groups:
        ftKey=g[0][0]                       # fourcc pertama array = tipe file (container)
        ft=fileTypes.setdefault(ftKey,{})
        for name,nver,tab in g:
            vmap=build_vmap(nver,tab)
            if vmap: ft.setdefault(name,{}).update(vmap)

    # 4) fallback global: hanya fourcc yang punya TEPAT SATU strucTab (tak ambigu)
    tabs_per_name={}
    for _,name,_,tab in infos:
        tabs_per_name.setdefault(name,set()).add(tab)
    chunks={}
    for name,nver,tab in {(n,v,t) for _,n,v,t in infos}:
        if len(tabs_per_name[name])==1:
            vmap=build_vmap(nver,tab)
            if vmap: chunks[name]=vmap

    # 5) strucTabs[fourcc] = daftar strucTab BERBEDA utk fourcc itu (utk dipilih manual di UI)
    #    tiap entri: {tab, usedBy:[fileType...], versions:{ver:typeKey}}
    usedby={}   # name -> tab -> set(fileType primary)
    for g in groups:
        ftKey=g[0][0]
        for name,nver,tab in g:
            usedby.setdefault(name,{}).setdefault(tab,set()).add(ftKey)
    nver_of={}  # (name,tab) -> nver
    for _,name,nver,tab in infos:
        nver_of[(name,tab)]=max(nver_of.get((name,tab),0),nver)
    strucTabs={}
    for name in usedby:
        lst=[]
        for tab in sorted(usedby[name]):
            nv=nver_of[(name,tab)]
            vmap=build_vmap(nv,tab)
            lst.append({"tab":"0x%X"%tab,"nver":nv,"usedBy":sorted(usedby[name][tab]),"versions":vmap})
        strucTabs[name]=lst

    doc={"format":"gw2packfile","pointerSize":64,
         "fileTypes":fileTypes,"chunks":chunks,"strucTabs":strucTabs,"types":types}
    with open(OUT_PATH,"w") as f:
        json.dump(doc,f,separators=(",",":"))
    amb=sorted(n for n,t in tabs_per_name.items() if len(t)>1)
    open(OUT_PATH+".done","w").write("fileTypes=%d globalChunks=%d types=%d ambiguous=%s"%(
        len(fileTypes),len(chunks),len(types),amb))
    print("OK fileTypes=%d chunks=%d types=%d ambiguous=%s"%(len(fileTypes),len(chunks),len(types),amb))

main()
