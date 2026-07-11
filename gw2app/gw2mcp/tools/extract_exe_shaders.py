#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
extract_exe_shaders.py  --  dump every bgfx shader blob embedded in the GW2 client .exe.

The GW2 client (Gw2-64.exe) bakes ~2275 bgfx shader blobs into .rdata: the 58 built-in
MATERIAL shaders plus a large ENGINE set (camera / depth / deferred-lighting / post) and
11 COMPUTE shaders. Each blob is a self-delimiting bgfx v6..v11 shader (magic 'VSH'/'FSH'/
'CSH' + version), so we can enumerate them by scanning for the magic and parsing the exact
header layout used by ArenaNet's ShaderD3D11::create (renderer_d3d11.cpp, sub_140B5AD60).

Output (default ./exe_shaders/):
    manifest.json  -- one record per blob: index, va, fileoff, type, version, hashIn/Out,
                      codeSize, numAttrs, constBufSize, region, role_guess, uniforms[]
    cso/NNNN_<T>_<hash>.cso  -- the raw DXBC bytecode (loadable by D3DReflect / Create*Shader)

Blob layout (per ShaderD3D11::create):
    magic[3], version u8
    hashIn  u32
    hashOut u32                       (version >= 6)
    count   u16                       (# of uniform/predefined records)
    count x { nameLen u8, name[nameLen], type u8, num u8, regIndex u16, regCount u16,
              texInfo u16 (v>=8), texFmt u16 (v>=10) }
    codeSize u32
    code[codeSize]                    (DXBC)
    numAttrs u8
    attrs u16[numAttrs]
    constBufSize u16
"""
import sys, os, json, struct, argparse

MAGICS = {b'VSH': 'V', b'FSH': 'F', b'CSH': 'C'}
# bgfx UniformType low nibble (type & 0x0f); high bits: 0x10=fragment, 0x20=sampler
UNIFORM_TYPE = {0: 'Sampler', 1: 'End', 2: 'Vec4', 3: 'Mat3', 4: 'Mat4'}


def parse_pe(data):
    """Return (imagebase, [ (name, va, vsize, rawptr, rawsize) ])."""
    e_lfanew = struct.unpack_from('<I', data, 0x3C)[0]
    assert data[e_lfanew:e_lfanew+4] == b'PE\x00\x00', 'not a PE'
    coff = e_lfanew + 4
    num_sections = struct.unpack_from('<H', data, coff + 2)[0]
    size_opt = struct.unpack_from('<H', data, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from('<H', data, opt)[0]
    assert magic == 0x20B, 'expected PE32+ (0x20B), got 0x%X' % magic
    imagebase = struct.unpack_from('<Q', data, opt + 24)[0]
    sec = opt + size_opt
    sections = []
    for i in range(num_sections):
        off = sec + i * 40
        name = data[off:off+8].rstrip(b'\x00').decode('latin1')
        vsize, va, rawsize, rawptr = struct.unpack_from('<IIII', data, off + 8)
        sections.append((name, va, vsize, rawptr, rawsize))
    return imagebase, sections


def va_to_off(va, imagebase, sections):
    rva = va - imagebase
    for name, sva, vsize, rawptr, rawsize in sections:
        if sva <= rva < sva + max(vsize, rawsize):
            return rawptr + (rva - sva)
    return None


def try_parse_blob(data, pos, end):
    """Parse a bgfx blob starting at file offset `pos`. Return dict or None."""
    magic = data[pos:pos+3]
    t = MAGICS.get(magic)
    if t is None:
        return None
    version = data[pos+3]
    if version < 5 or version > 12:
        return None
    o = pos + 4
    try:
        hash_in = struct.unpack_from('<I', data, o)[0]; o += 4
        hash_out = hash_in
        if version >= 6:
            hash_out = struct.unpack_from('<I', data, o)[0]; o += 4
        count = struct.unpack_from('<H', data, o)[0]; o += 2
        if count > 512:
            return None
        uniforms = []
        for _ in range(count):
            nlen = data[o]; o += 1
            if nlen == 0 or nlen > 128 or o + nlen > end:
                return None
            name = data[o:o+nlen].decode('latin1'); o += nlen
            utype = data[o]; o += 1
            num = data[o]; o += 1
            reg_idx = struct.unpack_from('<H', data, o)[0]; o += 2
            reg_cnt = struct.unpack_from('<H', data, o)[0]; o += 2
            tex_info = tex_fmt = 0
            if version >= 8:
                tex_info = struct.unpack_from('<H', data, o)[0]; o += 2
            if version >= 10:
                tex_fmt = struct.unpack_from('<H', data, o)[0]; o += 2
            uniforms.append({
                'name': name,
                'type': UNIFORM_TYPE.get(utype & 0x0f, str(utype & 0x0f)),
                'frag': bool(utype & 0x10), 'sampler': bool(utype & 0x20),
                'num': num, 'regIndex': reg_idx, 'regCount': reg_cnt,
                'texInfo': tex_info, 'texFmt': tex_fmt,
            })
        code_size = struct.unpack_from('<I', data, o)[0]; o += 4
        if code_size < 4 or o + code_size > end:
            return None
        code = data[o:o+code_size]
        # validate: compute shaders may be raw; VS/PS must be DXBC
        if t in ('V', 'F') and code[:4] != b'DXBC':
            return None
        o += code_size
        num_attrs = data[o]; o += 1
        if num_attrs > 32 or o + num_attrs * 2 + 2 > end:
            return None
        attrs = list(struct.unpack_from('<%dH' % num_attrs, data, o)) if num_attrs else []
        o += num_attrs * 2
        const_buf_size = struct.unpack_from('<H', data, o)[0]; o += 2
        return {
            'type': t, 'version': version, 'hashIn': hash_in, 'hashOut': hash_out,
            'count': count, 'uniforms': uniforms, 'codeSize': code_size,
            'numAttrs': num_attrs, 'attrs': attrs, 'constBufSize': const_buf_size,
            'code': code, 'blobLen': o - pos,
        }
    except (struct.error, IndexError):
        return None


def role_guess(rec, va):
    """Heuristic role from the uniform signature + region."""
    names = {u['name'] for u in rec['uniforms']}
    samplers = [u for u in rec['uniforms'] if u['sampler']]
    if rec['type'] == 'C':
        return 'compute'
    if rec['type'] == 'V':
        if names <= {'ViewProjection'}:
            return 'vs-depth/shadow (camera-only)'
        if 'World' in names and 'ViewProjection' in names:
            return 'vs-world'
        return 'vs'
    # pixel/fragment
    lit = names & {'LightBuffer', 'shSunColor', 'shSun', 'shSunData', 'shRed', 'shGreen', 'shBlue'}
    if lit:
        return 'ps-lit (%s)' % ','.join(sorted(lit))
    if 'StencilId' in names or 'StippleDensity' in names:
        return 'ps-deferred/gbuffer'
    if len(samplers) == 0:
        return 'ps-fullscreen/post'
    return 'ps'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exe', nargs='?',
                    default=r'C:\Program Files (x86)\Steam\steamapps\common\Guild Wars 2\Gw2-64-disable-aslr.exe')
    ap.add_argument('-o', '--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'exe_shaders'))
    ap.add_argument('--no-cso', action='store_true', help='manifest only, do not dump .cso files')
    # material region boundary (VA) -- blobs below this VA are the 58 built-in material AMATs
    ap.add_argument('--material-end', type=lambda x: int(x, 0), default=0x141AB7C00)
    ap.add_argument('--compute-start', type=lambda x: int(x, 0), default=0x142039000)
    args = ap.parse_args()

    with open(args.exe, 'rb') as f:
        data = f.read()
    imagebase, sections = parse_pe(data)
    print('imagebase=0x%X  sections=%d' % (imagebase, len(sections)))

    # scan .rdata only (that's where the shader blobs live)
    scan_ranges = []
    for name, va, vsize, rawptr, rawsize in sections:
        if name in ('.rdata', '.data'):
            scan_ranges.append((rawptr, rawptr + rawsize, imagebase + va - rawptr))
            print('scan %-8s file[0x%X..0x%X] va_base=0x%X' % (name, rawptr, rawptr + rawsize, imagebase + va))

    os.makedirs(args.out, exist_ok=True)
    cso_dir = os.path.join(args.out, 'cso')
    if not args.no_cso:
        os.makedirs(cso_dir, exist_ok=True)

    records = []
    for rawptr, rawend, off2va in scan_ranges:
        pos = rawptr
        while pos < rawend - 4:
            if data[pos:pos+3] in MAGICS and 5 <= data[pos+3] <= 12:
                rec = try_parse_blob(data, pos, rawend)
                if rec:
                    va = pos + off2va
                    rec['va'] = '0x%X' % va
                    rec['fileoff'] = '0x%X' % pos
                    if va < args.material_end:
                        rec['region'] = 'material'
                    elif va >= args.compute_start or rec['type'] == 'C':
                        rec['region'] = 'compute'
                    else:
                        rec['region'] = 'engine'
                    rec['role'] = role_guess(rec, va)
                    records.append(rec)
                    pos += rec['blobLen']
                    continue
            pos += 1

    # assign indices, dump cso, strip raw code out of manifest
    manifest = []
    for i, rec in enumerate(records):
        code = rec.pop('code')
        h = rec['hashOut'] & 0xffffffff
        fn = '%04d_%s_%08x.cso' % (i, rec['type'], h)
        rec['index'] = i
        rec['cso'] = fn
        if not args.no_cso and code[:4] == b'DXBC':
            with open(os.path.join(cso_dir, fn), 'wb') as f:
                f.write(code)
        manifest.append(rec)

    with open(os.path.join(args.out, 'manifest.json'), 'w') as f:
        json.dump(manifest, f, indent=1)

    # summary
    from collections import Counter
    by_region = Counter(r['region'] for r in manifest)
    by_type = Counter(r['type'] for r in manifest)
    by_role = Counter(r['role'] for r in manifest)
    print('\n== %d shader blobs ==' % len(manifest))
    print('region:', dict(by_region))
    print('type  :', dict(by_type))
    print('\nrole histogram:')
    for role, n in by_role.most_common():
        print('  %5d  %s' % (n, role))
    print('\nmanifest -> %s' % os.path.join(args.out, 'manifest.json'))


if __name__ == '__main__':
    main()
