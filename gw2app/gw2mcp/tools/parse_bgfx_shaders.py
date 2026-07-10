"""
Parse the bgfx shader blobs embedded in a GW2 AMAT packfile (chunk BGFX) and
extract their DXBC bytecode + uniform/attribute tables.

The AMAT file is the decompressed dat entry found at baseId-1 of a model
material's `filename` texture (see the trace: material.filename -> texture
baseId B -> baseId B-1 = AMAT packfile with GRMT + BGFX chunks).

bgfx ShaderD3D11 blob layout (version 11), confirmed against the bytes in the
.exe and matching bgfx/src/renderer_d3d11.cpp ShaderD3D11::create:

  magic     'VSH'/'FSH'/'CSH' + version(u8)
  hashIn    u32
  hashOut   u32                         (version >= 6)
  count     u16                         (uniform count)
  per uniform:
    nameLen u8; name[nameLen]
    type    u8   (bit4=fragment, bit5=sampler; low nibble = UniformType)
    num     u8
    regIdx  u16
    regCnt  u16
    texInfo u16                         (version >= 8)
    texFmt  u16                         (version >= 10)
  codeSize  u32
  code      byte[codeSize]              (DXBC container)
  attrCnt   u8
  attrs     u16 * attrCnt              (bgfx Attrib ids)
  constSize u16                         (constant buffer size)

Usage: python parse_bgfx_shaders.py <amat.bin> [out_dir]
"""
import struct, sys, os

UNIFORM_TYPE = {0: "Sampler", 1: "End", 2: "Vec4", 3: "Mat3", 4: "Mat4"}
# bgfx Attrib enum (subset) -> name
ATTRIB = {0:"Position",1:"Normal",2:"Tangent",3:"Bitangent",4:"Color0",5:"Color1",
          6:"Color2",7:"Color3",8:"Indices",9:"Weight",
          10:"TexCoord0",11:"TexCoord1",12:"TexCoord2",13:"TexCoord3",
          14:"TexCoord4",15:"TexCoord5",16:"TexCoord6",17:"TexCoord7"}


def parse_blob(d, off):
    """Parse one VSH/FSH/CSH blob at offset `off`. Returns dict or None."""
    magic = d[off:off+3]
    if magic not in (b"VSH", b"FSH", b"CSH"):
        return None
    p = off + 3
    ver = d[p]; p += 1
    hash_in = struct.unpack_from("<I", d, p)[0]; p += 4
    hash_out = 0
    if ver >= 6:
        hash_out = struct.unpack_from("<I", d, p)[0]; p += 4
    count = struct.unpack_from("<H", d, p)[0]; p += 2
    uniforms = []
    for _ in range(count):
        nlen = d[p]; p += 1
        name = d[p:p+nlen].decode("ascii", "replace"); p += nlen
        typ = d[p]; p += 1
        num = d[p]; p += 1
        reg_idx = struct.unpack_from("<H", d, p)[0]; p += 2
        reg_cnt = struct.unpack_from("<H", d, p)[0]; p += 2
        if ver >= 8:
            p += 2  # texInfo
        if ver >= 10:
            p += 2  # texFormat
        base = typ & 0x0F
        kind = "sampler" if (typ & 0x20) else ("frag" if (typ & 0x10) else "vert")
        uniforms.append({
            "name": name, "type": UNIFORM_TYPE.get(base, f"?{base}"),
            "stage": kind, "num": num, "regIndex": reg_idx, "regCount": reg_cnt,
        })
    code_size = struct.unpack_from("<I", d, p)[0]; p += 4
    code = d[p:p+code_size]; p += code_size
    attr_cnt = d[p]; p += 1
    attrs = []
    for _ in range(attr_cnt):
        a = struct.unpack_from("<H", d, p)[0]; p += 2
        attrs.append(ATTRIB.get(a, f"id{a}"))
    const_size = struct.unpack_from("<H", d, p)[0]; p += 2
    return {
        "magic": magic.decode(), "version": ver, "hashIn": hash_in, "hashOut": hash_out,
        "uniforms": uniforms, "codeSize": code_size, "code": code,
        "attribs": attrs, "constBufSize": const_size, "end": p,
    }


def valid_dxbc(code):
    if code[:4] != b"DXBC" or len(code) < 32:
        return False
    total = struct.unpack_from("<I", code, 24)[0]
    return total == len(code)


def main():
    path = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(path) or "."
    os.makedirs(out_dir, exist_ok=True)
    d = open(path, "rb").read()

    # locate every VSH/FSH/CSH blob by scanning for its magic
    offs = []
    for tag in (b"VSH", b"FSH", b"CSH"):
        i = d.find(tag)
        while i != -1:
            # magic must be followed by a plausible version byte
            if d[i+3] in range(1, 20):
                offs.append(i)
            i = d.find(tag, i+1)
    offs.sort()

    print(f"{os.path.basename(path)}: {len(d)} bytes, {len(offs)} shader blobs")
    stats = {"VSH": 0, "FSH": 0, "CSH": 0, "validDXBC": 0}
    first_detail = {"VSH": None, "FSH": None}
    for k, off in enumerate(offs):
        b = parse_blob(d, off)
        if not b:
            continue
        stats[b["magic"]] += 1
        ok = valid_dxbc(b["code"])
        if ok:
            stats["validDXBC"] += 1
            fn = os.path.join(out_dir, f"shader_{k:03d}_{b['magic']}_{off:08x}.cso")
            open(fn, "wb").write(b["code"])
        if b["magic"] in first_detail and first_detail[b["magic"]] is None:
            first_detail[b["magic"]] = b

    print(f"  VSH={stats['VSH']} FSH={stats['FSH']} CSH={stats['CSH']} | valid DXBC extracted={stats['validDXBC']}")
    for mg, b in first_detail.items():
        if not b:
            continue
        print(f"\n=== first {mg} (v{b['version']}) codeSize={b['codeSize']} DXBC={valid_dxbc(b['code'])} constBuf={b['constBufSize']}B ===")
        print(f"   attribs: {b['attribs']}")
        print(f"   uniforms ({len(b['uniforms'])}):")
        for u in b["uniforms"]:
            print(f"     {u['name']:22} {u['type']:8} {u['stage']:8} reg={u['regIndex']}(+{u['regCount']}) num={u['num']}")


if __name__ == "__main__":
    main()
