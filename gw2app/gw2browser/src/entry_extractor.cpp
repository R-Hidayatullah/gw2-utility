#include "entry_extractor.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <dxgiformat.h>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <thread>
#include <unordered_map>

#include <windows.h>

#include "cmp_decompress_method0.hpp"
#include "dds.h"
#include "gw2_atex.hpp"
#include "gw2_audio.hpp"
#include "gw2model.hpp"
#include "strs_view.h"
#include "struct_template.h"
#include "wic_image.h"

namespace {

// Runs fn(0..n-1) across a small worker pool (a dynamic queue so uneven work
// balances itself). Used to load a map's many prop models in parallel.
template <class F>
void parallel_for(size_t n, F&& fn) {
    if (n == 0) return;
    unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    unsigned nt = static_cast<unsigned>(std::min<size_t>(hw, n));
    if (nt <= 1) {
        for (size_t i = 0; i < n; ++i) fn(i);
        return;
    }
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    pool.reserve(nt);
    for (unsigned t = 0; t < nt; ++t) {
        pool.emplace_back([&] {
            for (size_t i = next.fetch_add(1); i < n; i = next.fetch_add(1)) fn(i);
        });
    }
    for (auto& th : pool) th.join();
}

bool starts_with_magic(const std::vector<uint8_t>& data, const char (&magic)[5]) {
    return data.size() >= 4 && std::memcmp(data.data(), magic, 4) == 0;
}

bool is_atex_family(const std::vector<uint8_t>& data) {
    // Every ATE*/CTE* container gw2atex::parse() understands. The C-prefixed
    // variants (CTEX/CTEU/CTEP/CTEC/CTET/CTTX) are the same format with a 'C'
    // magic instead of 'A' -- fill_preview_from_atex() aliases C->A before
    // parsing (matching gw2viewer.cpp's load_texture_rgba).
    if (data.size() < 4) {
        return false;
    }
    char c0 = static_cast<char>(data[0]);
    if (c0 != 'A' && c0 != 'C') {
        return false;
    }
    static const char* suffixes[] = {"TEX", "TTX", "TEC", "TEP", "TEU", "TET"};
    for (const char* s : suffixes) {
        if (data[1] == static_cast<uint8_t>(s[0]) && data[2] == static_cast<uint8_t>(s[1]) &&
            data[3] == static_cast<uint8_t>(s[2])) {
            return true;
        }
    }
    return false;
}

bool is_png(const std::vector<uint8_t>& data) {
    static constexpr uint8_t kSig[4] = {0x89, 'P', 'N', 'G'};
    return data.size() >= 4 && std::memcmp(data.data(), kSig, 4) == 0;
}

bool is_jpeg(const std::vector<uint8_t>& data) {
    static constexpr uint8_t kSig[3] = {0xFF, 0xD8, 0xFF};
    return data.size() >= 3 && std::memcmp(data.data(), kSig, 3) == 0;
}

bool is_bmp(const std::vector<uint8_t>& data) { return data.size() >= 2 && data[0] == 'B' && data[1] == 'M'; }

uint32_t read_u32_le(const std::vector<uint8_t>& data, size_t offset) {
    return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

const char* dxgi_short_name(uint32_t format) {
    switch (format) {
    case DXGI_FORMAT_BC1_UNORM: return "BC1";
    case DXGI_FORMAT_BC2_UNORM: return "BC2";
    case DXGI_FORMAT_BC3_UNORM: return "BC3";
    case DXGI_FORMAT_BC4_UNORM: return "BC4";
    case DXGI_FORMAT_BC5_UNORM: return "BC5";
    case DXGI_FORMAT_BC6H_UF16: return "BC6H";
    case DXGI_FORMAT_BC7_UNORM: return "BC7";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "BGRA8 sRGB";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8 sRGB";
    case DXGI_FORMAT_B8G8R8X8_UNORM: return "BGRX8";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2";
    case DXGI_FORMAT_B5G6R5_UNORM: return "B5G6R5";
    case DXGI_FORMAT_B5G5R5A1_UNORM: return "B5G5R5A1";
    case DXGI_FORMAT_B4G4R4A4_UNORM: return "B4G4R4A4";
    case DXGI_FORMAT_R8_UNORM: return "R8";
    default: return "DDS";
    }
}

// Decodes a real standalone "DDS " file into the preview_* fields, keeping its
// native BCn blocks for the GPU. Returns false (leaving is_image untouched) if
// the DDS isn't a recognized BC format.
bool fill_preview_from_dds(ExtractedEntry& result) {
    auto info = gw2dds::parse_dds(result.decompressed.data(), result.decompressed.size());
    if (!info) {
        return false;
    }
    result.preview_dxgi_format = info->dxgi_format;
    result.preview_width = info->width;
    result.preview_height = info->height;
    result.preview_pitch = info->sys_mem_pitch;
    result.preview_pixels.assign(result.decompressed.begin() + static_cast<ptrdiff_t>(info->data_offset),
                                  result.decompressed.end());
    result.preview_format_label = std::string("DDS ") + dxgi_short_name(info->dxgi_format);
    return true;
}

// Decodes an ATEX-family container to RGBA8888 via the accurate gw2_atex
// decoder. Returns false on any parse/decode failure so the entry falls back to
// a plain binary with no preview.
bool fill_preview_from_atex(ExtractedEntry& result) {
    if (result.decompressed.empty()) {
        return false;
    }
    // CTEX/CTEU/CTEP/... share the ATEX layout but start with 'C'; alias to 'A'
    // just for the parse, then restore so "Export Decompressed" stays byte-exact.
    uint8_t saved0 = result.decompressed[0];
    bool aliased = (saved0 == 'C');
    if (aliased) {
        result.decompressed[0] = 'A';
    }

    bool ok = false;
    try {
        gw2atex::Texture tex = gw2atex::parse(result.decompressed.data(), result.decompressed.size());
        gw2atex::Image img = gw2atex::decode(tex, 0);
        if (img.width > 0 && img.height > 0 && !img.rgba.empty()) {
            result.preview_dxgi_format = DXGI_FORMAT_R8G8B8A8_UNORM;
            result.preview_width = static_cast<uint32_t>(img.width);
            result.preview_height = static_cast<uint32_t>(img.height);
            result.preview_pitch = static_cast<uint32_t>(img.width) * 4;
            result.preview_pixels = std::move(img.rgba);
            result.preview_format_label = tex.fmt_name;
            ok = true;
        }
    } catch (const std::exception&) {
        ok = false;
    }

    if (aliased) {
        result.decompressed[0] = saved0;
    }
    return ok;
}

// ---- model support -------------------------------------------------------

// A "PF" packfile carrying a GEOM chunk is a renderable model. Walk the chunk
// table (mirrors gw2model::Extractor::findChunk) so we can classify the entry
// even when no struct template is loaded yet.
bool pf_has_chunk(const std::vector<uint8_t>& d, const char* want) {
    if (d.size() < 12 || d[0] != 'P' || d[1] != 'F') {
        return false;
    }
    auto rd16 = [&](size_t p) -> uint32_t { return (p + 2 <= d.size()) ? (d[p] | (d[p + 1] << 8)) : 0; };
    auto rd32 = [&](size_t p) -> uint32_t {
        return (p + 4 <= d.size()) ? (d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24)) : 0;
    };
    size_t pos = rd16(6); // headerSize
    while (pos + 16 <= d.size()) {
        char fourcc[5] = {0};
        std::memcpy(fourcc, d.data() + pos, 4);
        uint32_t chunk_size = rd32(pos + 4);
        if (std::strcmp(fourcc, want) == 0) {
            return true;
        }
        size_t next = pos + 8 + chunk_size;
        if (next <= pos) {
            break;
        }
        pos = next;
    }
    return false;
}

bool is_normal_format(const std::string& fmt) {
    return fmt.find("3DC") != std::string::npos || fmt.find("BC5") != std::string::npos ||
           fmt.find("ATI2") != std::string::npos;
}

// Grayscale texture with large near-black regions -> used as a glow/alpha mask
// (an "effect" material). Ported from gw2viewer.cpp::looks_like_effect.
bool looks_like_effect(const std::vector<uint8_t>& px) {
    long n = 0, colored = 0, dark = 0;
    for (size_t i = 0; i + 3 < px.size(); i += 4 * 97) {
        int r = px[i], g = px[i + 1], b = px[i + 2];
        int mx = std::max({r, g, b}), mn = std::min({r, g, b});
        int lum = (r * 77 + g * 150 + b * 29) >> 8;
        if (mx - mn > 18) colored++;
        if (lum < 32) dark++;
        n++;
    }
    if (n == 0) return false;
    bool grayscale = colored < n / 15;
    bool masky = dark > n / 8;
    return grayscale && masky;
}

// Texture resolution preference. GW2 stores many textures as a full/reduced pair
// at consecutive MFT indices (baseId B = full resolution, B-1 = the exact-half
// version, same format). A material may reference EITHER member of the pair, so the
// "full" texture is sometimes at the resolved index and sometimes one entry above
// it. When true (default) we load the full-resolution member; when false the reduced
// one (faster / less VRAM). Read on the bg thread; set from the UI.
static std::atomic<bool> g_tex_full_res{true};

// Decompress one MFT entry (by 0-based array index) into its raw ATEX/CTEX bytes.
static bool read_mft_atex_bytes(Gw2Dat& dat, size_t i0, std::vector<uint8_t>& bytes) {
    if (i0 >= dat.mft_data_list.size()) return false;
    const MftData& e = dat.mft_data_list[i0];
    try {
        std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
        std::vector<uint8_t> stripped = gw2cmp::strip_crc32(std::span<const uint8_t>(raw));
        if (e.compression_flag == 0) {
            bytes = std::move(stripped);
        } else {
            if (stripped.size() < 8) return false;
            uint32_t usz = stripped[4] | (stripped[5] << 8) | (stripped[6] << 16) | ((uint32_t)stripped[7] << 24);
            bytes = gw2cmp::decompress_method0(std::span<const uint8_t>(stripped).subspan(8), usz);
        }
        if (bytes.size() >= 4 && bytes[0] == 'C') bytes[0] = 'A'; // CTEX -> ATEX alias
        return bytes.size() >= 12;
    } catch (const std::exception&) {
        return false;
    }
}

// Peek an entry's ATEX dimensions + format from the header only (no pixel decode).
static bool peek_mft_atex(Gw2Dat& dat, size_t i0, int& w, int& h, std::string& fmt) {
    std::vector<uint8_t> bytes;
    if (!read_mft_atex_bytes(dat, i0, bytes)) return false;
    try {
        gw2atex::Texture t = gw2atex::parse(bytes.data(), bytes.size());
        w = t.width; h = t.height; fmt = t.fmt_name;
        return w > 0 && h > 0;
    } catch (const std::exception&) {
        return false;
    }
}

// Given the entry a fileId resolved to (i0), find the full/reduced member of its
// pair. The pair is a same-format neighbor with exactly double/half dimensions; the
// full member is always the higher index (baseId B vs B-1). Returns i0 unchanged if
// there is no paired sibling.
static size_t resolve_res_index(Gw2Dat& dat, size_t i0, bool wantFull) {
    int w0, h0; std::string f0;
    if (!peek_mft_atex(dat, i0, w0, h0, f0)) return i0;
    int w1, h1; std::string f1;
    // A full sibling one entry above => i0 is the reduced member.
    if (i0 + 1 < dat.mft_data_list.size() && peek_mft_atex(dat, i0 + 1, w1, h1, f1)
        && f1 == f0 && w1 == 2 * w0 && h1 == 2 * h0)
        return wantFull ? i0 + 1 : i0;
    // A reduced sibling one entry below => i0 is the full member.
    if (i0 >= 1 && peek_mft_atex(dat, i0 - 1, w1, h1, f1)
        && f1 == f0 && w0 == 2 * w1 && h0 == 2 * h1)
        return wantFull ? i0 : i0 - 1;
    return i0;
}

// Decode the ATEX at a 0-based MFT index to RGBA.
static bool decode_mft_atex(Gw2Dat& dat, size_t i0, ModelTextureCPU& out) {
    std::vector<uint8_t> bytes;
    if (!read_mft_atex_bytes(dat, i0, bytes)) return false;
    try {
        gw2atex::Texture t = gw2atex::parse(bytes.data(), bytes.size());
        gw2atex::Image im = gw2atex::decode(t, 0);
        if (im.width <= 0 || im.height <= 0 || im.rgba.empty()) return false;
        out.width = im.width;
        out.height = im.height;
        out.fmt = t.fmt_name;
        out.rgba = std::move(im.rgba);
        out.isNormal = is_normal_format(t.fmt_name);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// Decompress one MFT entry addressed by fileId, then decode its ATEX to RGBA,
// choosing the full- or reduced-resolution member of its pair per g_tex_full_res.
// Returns false if the fileId isn't found or isn't a decodable texture.
bool decode_texture_by_fileid(Gw2Dat& dat, uint32_t fileId, ModelTextureCPU& out) {
    uint32_t base = get_by_base_id(dat, fileId);
    if (base == 0 || base - 1 >= dat.mft_data_list.size()) {
        return false;
    }
    size_t i0 = resolve_res_index(dat, base - 1, g_tex_full_res.load());
    if (!decode_mft_atex(dat, i0, out)) return false;
    out.fileId = fileId;
    return true;
}

// ---- game shaders (real bgfx DXBC) --------------------------------------

// Decompress one MFT entry addressed by physical index (== baseId). Mirrors the
// `decomp` helper in gw2gsviewer.cpp; used to pull the AMAT package that sits at
// (texture baseId - 1) for a material.
std::vector<uint8_t> decompress_by_index(Gw2Dat& dat, uint32_t idx) {
    if (idx >= dat.mft_data_list.size()) return {};
    const MftData& e = dat.mft_data_list[idx];
    std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
    std::vector<uint8_t> s = gw2cmp::strip_crc32(std::span<const uint8_t>(raw));
    if (e.compression_flag == 0) return s;
    if (s.size() < 8) return {};
    uint32_t u = s[4] | (s[5] << 8) | (s[6] << 16) | ((uint32_t)s[7] << 24);
    return gw2cmp::decompress_method0(std::span<const uint8_t>(s).subspan(8), u);
}

// Parse a bgfx shader blob (v11) -> raw DXBC + uniform table. Byte-for-byte the
// same layout gw2gsviewer.cpp::parse_bgfx reads: 'V'/'F'/'C' + ver, hashes,
// uniform count, per-uniform {nameLen,name,type,_,reg,regCnt,(texInfo)(texFmt)},
// codeSize, DXBC, attrCount, attrs, constBufSize.
void parse_bgfx(const std::vector<uint8_t>& d, std::vector<uint8_t>& dxbc,
                std::vector<GameShaderUniform>& uniforms, uint32_t& constBuf) {
    dxbc.clear(); uniforms.clear(); constBuf = 0;
    if (d.size() < 8 || (d[0] != 'V' && d[0] != 'F' && d[0] != 'C')) return;
    size_t p = 3; int ver = d[p++]; p += 4; if (ver >= 6) p += 4;
    if (p + 2 > d.size()) return;
    uint16_t cnt = d[p] | (d[p + 1] << 8); p += 2;
    for (int i = 0; i < cnt; i++) {
        if (p >= d.size()) return;
        int nl = d[p++];
        if (p + nl > d.size()) return;
        std::string nm((const char*)&d[p], nl); p += nl;
        if (p + 6 > d.size()) return;
        int type = d[p++]; p++;
        int reg = d[p] | (d[p + 1] << 8); p += 2;
        int rc = d[p] | (d[p + 1] << 8); p += 2;
        if (ver >= 8) p += 2; if (ver >= 10) p += 2;
        int stage = (type & 0x20) ? 2 : (type & 0x10) ? 1 : 0;
        uniforms.push_back({nm, type & 0x0F, stage, reg, rc});
    }
    if (p + 4 > d.size()) return;
    uint32_t cs = d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24); p += 4;
    if (p + cs > d.size()) return;
    dxbc.assign(d.begin() + p, d.begin() + p + cs); p += cs;
    if (p >= d.size()) return;
    int ac = d[p++]; p += ac * 2;
    if (p + 2 <= d.size()) constBuf = d[p] | (d[p + 1] << 8);
}

// GW2's fixed engine-global env-cubemap sampler slot (reflection/irradiance);
// bgfx strips the DXBC RDEF chunk so we can't detect it via reflection.
constexpr int kEnvCubeSlot = 13;
// AMAT sampler `textureIndex` values >= this are engine-GLOBAL texture ROLES
// (not indices into the material's own texture list): observed 33=env cube (slot
// 13), 34=light buffer (slot 14), 35=slot 12, 37=shadow map (slot 15). Below this
// the value is a direct index into the material's textures.
constexpr uint32_t kGlobalRoleBase = 33;

// Traces a material's AMAT shader package (at baseId-1 of its filename texture),
// picks a matched VS+PS via extractAmat, parses both bgfx blobs and resolves the
// PS sampler bindings to either a decoded material texture (via `get_tex`, which
// dedups into ModelPreview::textures) or a global stand-in. All CPU-side; the
// renderer creates the GPU objects. Returns a GameMaterial (ok=false on failure).
// Engine-global shader uniforms: filled by the engine/renderer (camera, lighting,
// fog, transforms), NOT per-material MatConstants. Everything else in a shader's
// uniform table is a material constant that pairs with the AMAT constants[] tokens.
static const std::set<std::string> kEngineGlobalUniforms = {
    "CameraPosition", "FogColorNearMinusFar", "FogColorFar", "FogColorHeight", "FogDepthCue", "FogParam0",
    "ViewProjection", "World", "WorldView", "WorldViewProjection", "WorldToShadowA", "WorldToShadowB",
    "WorldToShadowC", "TexTransform0A", "TexTransform0B", "TexTransform1A", "TexTransform1B", "TexTransform2A",
    "TexTransform2B", "TexTransform3A", "TexTransform3B", "LightBuffer", "StencilId", "ScreenDims", "fxclr",
    "shSunColor", "shSunData", "shSun", "shRed", "shGreen", "shBlue", "TexelOffset", "StippleDensity", "Time",
    "SunColor", "SunDirection", "AmbientColor", "AlphaRef", "fadedif"};

GameMaterial extract_game_material(Gw2Dat& dat, const nlohmann::json& tpl, const gw2model::Material& m,
                                   const std::function<int(uint32_t)>& get_tex, bool effectHint) {
    GameMaterial out;
    out.index = m.index;
    uint32_t fnBase = get_by_base_id(dat, m.materialFile);
    if (!fnBase) return out;
    std::vector<uint8_t> amat;
    try { amat = decompress_by_index(dat, fnBase - 1); } catch (const std::exception&) { return out; }
    if (amat.empty()) return out;
    gw2model::AmatSet set;
    try { set = gw2model::Extractor(amat, tpl).extractAmat(); } catch (const std::exception&) { return out; }

    // Pick the AMAT's transparent effect for translucent submeshes. GW2's sortLayer
    // is the usual signal but is left 0 on some models' translucent materials (e.g.
    // jade-tech lamp 291977). The reliable per-material signal is materialFlags bit 0
    // (0x1) = alpha-blend/translucent: verified on 291977 -- mats 0 & 3 (the jade
    // lenses) carry it (flags 0x0a09) while the opaque wood/brick/blades don't
    // (0x0a08). Without this the jade renders as an OPAQUE grey block that occludes
    // the flower crest behind it (the opaque G-buffer variant, unlit offline).
    // Use the AMAT's OPAQUE effect variant unless the MODL marks the submesh for the
    // sorted/transparent pass (sortLayer>0). NOTE: materialFlags bit0 is NOT a
    // "render alpha-blended" flag -- forcing the transparent variant on the jade
    // rods (291977 mats 0 & 3) made them wrongly see-through; they are solid/opaque
    // in game. Correct color comes from the material constants (bound below), not
    // from switching effect variants.
    bool useTrans = set.hasTrans && m.sortLayer > 0;
    int vsIdx = useTrans ? set.transVsIndex : set.vsIndex;
    int psIdx = useTrans ? set.transPsIndex : set.psIndex;
    if (std::getenv("GW2_MATDBG")) {
        std::fprintf(stderr, "[mat %u] matId=%u flags=0x%08x sortLayer=%d useTrans=%d ps=%d transPs=%d nConst=%zu\n",
                     m.index, m.materialId, m.materialFlags, (int)m.sortLayer, (int)useTrans, set.psIndex,
                     set.transPsIndex, m.constants.size());
        // Decode each MatConstant token32 by reversing the engine's 5-bit name pack
        // (sub_140E3B5E0: a-z/A-Z -> 1..26, 5 bits each; top nibble = 2-digit suffix).
        for (const auto& c : m.constants) {
            char nm[16]; int k = 0; uint32_t t = c.name;
            for (int b = 0; b < 6 && k < 15; ++b) { uint32_t v = (t >> (b * 5)) & 0x1f; if (v >= 1 && v <= 26) nm[k++] = char('a' + v - 1); }
            uint32_t suf = (t >> 28) & 0xf; // top nibble
            nm[k] = 0;
            std::fprintf(stderr, "    const token=0x%08x name='%s' suf=%u val=[%.3f %.3f %.3f %.3f]\n",
                         c.name, nm, suf, c.value[0], c.value[1], c.value[2], c.value[3]);
        }
    }
    if (vsIdx < 0 || psIdx < 0 || vsIdx >= (int)set.shaders.size() || psIdx >= (int)set.shaders.size()) return out;

    parse_bgfx(set.shaders[vsIdx].dxbc, out.vsDXBC, out.vsUniforms, out.vsConstBuf);
    parse_bgfx(set.shaders[psIdx].dxbc, out.psDXBC, out.psUniforms, out.psConstBuf);
    if (out.vsDXBC.empty() || out.psDXBC.empty()) return out;

    // Bind MODL MatConstants -> shader cbuffer offsets. The AMAT's ordered
    // constants[] token list pairs one-to-one (in order) with the shader's
    // material-constant uniforms (the non-engine-global ones), so:
    //   token = constantTokens[i]  <->  uniform[i].byteOff
    // and a MODL MatConstant whose token matches writes its value there. Verified
    // on 291977 (token 0x894bbfd5 -> 'intmodx'@320 = 6.0, etc). This feeds glow /
    // spec / metalness / env / scroll / tint params that were previously zeroed.
    auto bind_consts = [&](const std::vector<uint32_t>& ctoks, const std::vector<GameShaderUniform>& unis,
                           std::vector<GameConstOverride>& outc) {
        std::vector<std::pair<uint32_t, int>> tokOff; // constantTokens[i] -> uniform[i].byteOff
        size_t mi = 0;
        for (const auto& u : unis) {
            if (u.type == 0 || kEngineGlobalUniforms.count(u.name)) continue; // engine-filled, not a material const
            if (mi < ctoks.size()) tokOff.emplace_back(ctoks[mi], u.byteOff);
            ++mi;
        }
        if (std::getenv("GW2_CONSTDBG"))
            std::fprintf(stderr, "[mat %u] bind: material-uniforms=%zu constantTokens=%zu %s\n",
                         m.index, mi, ctoks.size(), mi == ctoks.size() ? "ALIGNED" : "*** MISALIGNED ***");
        // SAFETY GATE: the positional pairing (constantTokens[i] <-> uniform[i]) is
        // only valid when every token has a live uniform. The DXBC compiler strips
        // unused constants, so counts often differ -- then the pairing drifts and a
        // MatConstant lands in the WRONG uniform (e.g. 291977 jade -> red). Without
        // the name->token hash (unsolved: tokens hash a longer internal name than the
        // truncated bgfx uniform name) we can't realign, so bind only when aligned.
        if (mi != ctoks.size()) return;
        for (const auto& c : m.constants) {
            for (const auto& to : tokOff) {
                if (to.first == c.name) {
                    GameConstOverride ov; ov.byteOff = to.second;
                    for (int k = 0; k < 4; ++k) ov.value[k] = c.value[k];
                    outc.push_back(ov);
                    break;
                }
            }
        }
    };
    bind_consts(set.shaders[vsIdx].constantTokens, out.vsUniforms, out.vsConsts);
    bind_consts(set.shaders[psIdx].constantTokens, out.psUniforms, out.psConsts);

    for (const auto& b : set.shaders[psIdx].samplers) {
        if (b.textureSlot >= 16) continue;
        GameSamplerCPU s;
        s.slot = static_cast<int>(b.textureSlot);
        if (b.textureIndex >= kGlobalRoleBase) {
            // Engine-global role we don't have offline. Match the known roles so
            // each gets a sane stand-in (env cube grey / shadow map WHITE = fully
            // lit / other globals mid-grey) instead of a flat-white blob.
            if (b.textureIndex == 33 || b.textureSlot == kEnvCubeSlot) s.global = 2;      // env cubemap
            else if (b.textureIndex == 37 || b.textureSlot == 15) s.global = 3;           // shadow map -> white
            else s.global = 1;                                                            // light buffer / other -> grey
        } else if (b.textureIndex < m.textures.size()) {
            uint32_t fid = m.textures[b.textureIndex].fileId;
            s.gameTex = fid ? get_tex(fid) : -1;
            if (s.gameTex < 0) s.global = 1; // texture failed to decode -> grey, not white
        } else {
            // A material-texture index that's out of range (material has fewer
            // textures than the shader samples): grey stand-in, never white.
            s.global = 1;
        }
        out.samplers.push_back(s);
    }
    out.renderState = useTrans ? set.transRenderState : set.renderState;
    out.ok = true;
    return out;
}

// Extracts geometry + materials (via gw2model, template-driven) and decodes
// every referenced texture into a self-contained, ready-to-upload ModelPreview.
// Returns null if the packfile isn't a renderable model.
//
// Takes the archive as an already-parsed Gw2Dat& (header + MFT tables only --
// the file handle inside it is already closed by load_dat_file, so this is
// pure read-only lookups: safe to call concurrently from many worker threads
// against the *same* Gw2Dat, same as read_entry_bytes/decode_texture_by_fileid
// already assume). Callers that loop over many unique models (map props, zone
// models) MUST load the Gw2Dat once and pass it in here -- re-parsing the
// whole MFT table (header + up to ~1M entries, each sorted twice) per model
// was previously the actual cost of "loading peta 3d lambat": for a map with
// 600 unique prop models, that meant re-doing a multi-hundred-thousand-entry
// parse + two full sorts 600 times over, on top of running that redundant
// work across several threads at once (see the string-path overload below,
// kept only for the single-model preview path which has no shared Gw2Dat).
std::shared_ptr<ModelPreview> build_model_preview(const std::vector<uint8_t>& modl_bytes, Gw2Dat& dat,
                                                   const nlohmann::json& tpl, bool want_game = false) {
    gw2model::Model model;
    try {
        model = gw2model::Extractor(modl_bytes, tpl).extract();
    } catch (const std::exception&) {
        return nullptr;
    }
    if (model.meshes.empty()) {
        return nullptr;
    }

    auto out = std::make_shared<ModelPreview>();

    std::map<uint32_t, int> tex_cache; // fileId -> index into out->textures (-1 = tried, failed)
    auto get_texture = [&](uint32_t fileId) -> int {
        if (fileId == 0) return -1;
        auto it = tex_cache.find(fileId);
        if (it != tex_cache.end()) return it->second;
        ModelTextureCPU tex;
        int idx = -1;
        if (decode_texture_by_fileid(dat, fileId, tex)) {
            idx = static_cast<int>(out->textures.size());
            out->textures.push_back(std::move(tex));
        }
        tex_cache[fileId] = idx;
        return idx;
    };

    // Materials: decode textures, classify diffuse vs normal, detect effects.
    for (const auto& m : model.materials) {
        ModelMaterialCPU mat;
        mat.index = m.index;
        mat.textureFileIds = m.textureFileIds();
        long bestDiffuseArea = -1, bestNormalArea = -1;
        for (uint32_t fid : mat.textureFileIds) {
            int ti = get_texture(fid);
            if (ti < 0) continue;
            const ModelTextureCPU& t = out->textures[ti];
            long area = static_cast<long>(t.width) * t.height;
            if (t.isNormal) {
                if (area > bestNormalArea) { bestNormalArea = area; mat.normalTex = ti; }
            } else {
                if (area > bestDiffuseArea) { bestDiffuseArea = area; mat.diffuseTex = ti; }
            }
        }
        if (mat.diffuseTex >= 0) {
            mat.isEffect = looks_like_effect(out->textures[mat.diffuseTex].rgba);
        }
        // A single plausible [0,1] color constant becomes the material tint.
        if (m.constants.size() == 1) {
            const float* v = m.constants[0].value;
            if (v[0] >= 0 && v[0] <= 1 && v[1] >= 0 && v[1] <= 1 && v[2] >= 0 && v[2] <= 1) {
                for (int k = 0; k < 4; ++k) mat.tint[k] = v[k];
            }
        }
        bool matIsEffect = mat.isEffect; // captured before the move below
        out->materials.push_back(std::move(mat));

        // Real game shaders (bgfx DXBC from the material's AMAT), for the
        // "Shader" render mode. Only for the single-model preview path -- the
        // map scene loads many props and skips this to stay fast. Pass the effect
        // flag so translucent materials pick the AMAT's transparent variant.
        if (want_game) {
            out->gameMaterials.push_back(extract_game_material(dat, tpl, m, get_texture, matIsEffect));
        }
    }

    // Meshes: convert to GVertex + compute overall bounds.
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const auto& src : model.meshes) {
        ModelMeshCPU mesh;
        mesh.materialIndex = src.materialIndex;
        mesh.fvf = src.fvf;
        mesh.vertexCount = src.vertexCount;
        mesh.hasTangents = src.hasTangents;
        // Smooth-skin only if the mesh has per-vertex blend data AND its bindings
        // resolved to bones.
        int resolved = 0;
        for (int bi : src.boneBindingSkelIndex) if (bi >= 0) ++resolved;
        const auto& bmap = src.boneBindingSkelIndex;
        bool smoothSkin = src.hasSkin && !bmap.empty() && resolved > 0;
        // Rigid single-bone attach: weapon blades / rigid props carry no per-vertex
        // weights (hasSkin=false), but the whole sub-mesh binds to ONE bone via its
        // single boneBinding. Without this they get pinned to bone 0 (root) and drift
        // off / separate from the hand as the rig animates. Bind every vertex to that
        // bone so the piece follows its attach bone (assembling into the pose).
        int rigidBone = -1;
        if (!smoothSkin && resolved > 0)
            for (int bi : bmap) if (bi >= 0) { rigidBone = bi; break; }
        mesh.hasSkin = smoothSkin || (rigidBone >= 0);
        gw2model::describeFvf(src.fvf, &mesh.stride);
        mesh.vertices.reserve(src.vertices.size());
        for (const auto& v : src.vertices) {
            GVertex g{v.px, v.py, v.pz, v.nx, v.ny, v.nz, v.tx, v.ty, v.tz, v.bx, v.by, v.bz, v.u, v.v};
            for (int c = 0; c < 7; ++c) { g.uv1[c][0] = v.uv[c][0]; g.uv1[c][1] = v.uv[c][1]; }
            if (smoothSkin) {
                for (int c = 0; c < 4; ++c) {
                    int raw = v.boneIdx[c];
                    int sk = (raw >= 0 && raw < static_cast<int>(bmap.size())) ? bmap[raw] : -1;
                    g.bidx[c] = static_cast<uint32_t>(sk >= 0 ? sk : 0);
                    g.bwt[c] = (sk >= 0) ? v.boneWt[c] : 0.0f;
                }
                float s = g.bwt[0] + g.bwt[1] + g.bwt[2] + g.bwt[3];
                if (s > 1e-6f) { for (int c = 0; c < 4; ++c) g.bwt[c] /= s; }
                else { g.bwt[0] = 1; g.bwt[1] = g.bwt[2] = g.bwt[3] = 0; }
            } else if (rigidBone >= 0) {
                g.bidx[0] = static_cast<uint32_t>(rigidBone);
                g.bidx[1] = g.bidx[2] = g.bidx[3] = 0;
                g.bwt[0] = 1.0f; g.bwt[1] = g.bwt[2] = g.bwt[3] = 0.0f;
            }
            mesh.vertices.push_back(g);
            lo[0] = std::min(lo[0], v.px); lo[1] = std::min(lo[1], v.py); lo[2] = std::min(lo[2], v.pz);
            hi[0] = std::max(hi[0], v.px); hi[1] = std::max(hi[1], v.py); hi[2] = std::max(hi[2], v.pz);
        }
        mesh.indices = src.indices;
        out->totalVerts += static_cast<uint32_t>(mesh.vertices.size());
        out->totalTris += static_cast<uint32_t>(mesh.indices.size() / 3);
        out->meshes.push_back(std::move(mesh));
    }
    if (out->totalVerts == 0) {
        return nullptr;
    }

    // Skeleton (bind pose) + embedded-animation metadata, straight from gw2model.
    out->skeletonVersion = model.skeleton.fileVersion;
    out->skeletonType = model.skeleton.skelDataType;
    out->externalSkeletonRef = model.skeleton.externalRef;
    out->joints.reserve(model.skeleton.bones.size());
    for (const auto& b : model.skeleton.bones) {
        ModelJoint j;
        j.pos[0] = b.worldPos[0]; j.pos[1] = b.worldPos[1]; j.pos[2] = b.worldPos[2];
        j.parent = b.parent;
        j.name = b.name;
        for (int k = 0; k < 3; ++k) j.localPos[k] = b.localPos[k];
        for (int k = 0; k < 4; ++k) j.localQuat[k] = b.localQuat[k];
        for (int k = 0; k < 9; ++k) j.localScale[k] = b.scaleShear[k];
        for (int k = 0; k < 16; ++k) j.invWorld[k] = b.invWorld[k];
        out->joints.push_back(std::move(j));
    }
    out->hasAnimation = model.anim.present;
    out->animationVersion = model.anim.present ? static_cast<int>(model.anim.chunkVersion) : -1;
    out->animationType = model.anim.typeKey;
    for (const auto& c : model.anim.clips) {
        out->animationTokens.push_back(c.token);
        if (!c.rawGranny.empty()) {
            granny::Anim clip = granny::parse(c.rawGranny.data(), c.rawGranny.size());
            if (clip.valid) out->animClips.push_back(std::move(clip));
        }
    }

    for (int i = 0; i < 3; ++i) out->center[i] = (lo[i] + hi[i]) * 0.5f;
    float ext[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
    out->radius = 0.5f * std::sqrt(ext[0] * ext[0] + ext[1] * ext[1] + ext[2] * ext[2]);
    if (out->radius < 1e-3f) out->radius = 1.0f;
    return out;
}

// Convenience overload for the single-model preview path, which (by design,
// see decompress_raw_entry) has no shared Gw2Dat to reuse -- it opens its own
// short-lived one. Fine when called once; NEVER call this in a loop over many
// models (that's exactly the per-model MFT re-parse this file used to do).
std::shared_ptr<ModelPreview> build_model_preview(const std::vector<uint8_t>& modl_bytes, const std::string& dat_path,
                                                   const nlohmann::json& tpl, bool want_game = false) {
    Gw2Dat dat;
    try {
        load_dat_file(dat, dat_path);
    } catch (const std::exception&) {
        // Leave `dat` at its empty default state: get_by_base_id/decode_texture_by_fileid
        // simply fail to resolve any texture against empty tables, so the model still
        // builds (untextured) instead of the whole preview failing.
    }
    return build_model_preview(modl_bytes, dat, tpl, want_game);
}

// Decompress a MODL entry addressed by fileId (mirrors decode_texture_by_fileid
// but returns the raw decompressed packfile bytes).
std::vector<uint8_t> load_modl_bytes_by_fileid(Gw2Dat& dat, uint32_t fileId) {
    uint32_t base = get_by_base_id(dat, fileId);
    if (base == 0 || base - 1 >= dat.mft_data_list.size()) return {};
    const MftData& e = dat.mft_data_list[base - 1];
    try {
        std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
        std::vector<uint8_t> stripped = gw2cmp::strip_crc32(std::span<const uint8_t>(raw));
        if (e.compression_flag == 0) return stripped;
        if (stripped.size() < 8) return {};
        uint32_t usz = stripped[4] | (stripped[5] << 8) | (stripped[6] << 16) | ((uint32_t)stripped[7] << 24);
        return gw2cmp::decompress_method0(std::span<const uint8_t>(stripped).subspan(8), usz);
    } catch (const std::exception&) {
        return {};
    }
}

// Builds a lit grey ground surface from the terrain height-map grid. Vertices
// are placed in the same world space as the props (GW2 is Z-up: X/Y horizontal,
// Z = height), so the terrain lines up under the placed models. Normals come
// from height central-differences for shading. No terrain textures yet.
// hasWaterZ/waterZ: the real water plane, parsed from PackMapCollideV16::
// waterSurfaceZ in the `havk` chunk (see parseMapCollision) -- NOT guessed.
std::shared_ptr<ModelPreview> build_terrain_model(const gw2model::Extractor::MapTerrain& t, bool hasWaterZ, float waterZ) {
    if (!t.present || t.heights.size() < 4) return nullptr;
    // Grid dimension: the sample count is a perfect square on the maps seen.
    int G = static_cast<int>(std::lround(std::sqrt(static_cast<double>(t.heights.size()))));
    if (G < 2 || static_cast<size_t>(G) * G > t.heights.size()) return nullptr;

    float x0 = t.rect[0], y0 = t.rect[1], x1 = t.rect[2], y1 = t.rect[3];
    if (!t.hasRect || x1 <= x0 || y1 <= y0) { x0 = y0 = -3072; x1 = y1 = 3072; }
    float dx = (x1 - x0) / (G - 1), dy = (y1 - y0) / (G - 1);
    auto H = [&](int i, int j) {
        i = std::clamp(i, 0, G - 1); j = std::clamp(j, 0, G - 1);
        return t.heights[static_cast<size_t>(j) * G + i];
    };
    // Median height, to clamp sentinel "no-data" spikes to something sane.
    float med;
    {
        std::vector<float> s = t.heights;
        std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
        med = s[s.size() / 2];
    }
    const float kClamp = 4000.0f; // reject heights absurdly far from the median

    auto model = std::make_shared<ModelPreview>();
    model->meshes.emplace_back();
    ModelMeshCPU& mesh = model->meshes[0];
    mesh.materialIndex = 0;
    mesh.vertices.reserve(static_cast<size_t>(G) * G);
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (int j = 0; j < G; ++j) {
        for (int i = 0; i < G; ++i) {
            float h = H(i, j);
            if (std::abs(h - med) > kClamp) h = med;
            float wx = x0 + i * dx, wy = y0 + j * dy;
            float nzx = (H(i - 1, j) - H(i + 1, j)) / (2 * dx);
            float nzy = (H(i, j - 1) - H(i, j + 1)) / (2 * dy);
            float nl = std::sqrt(nzx * nzx + nzy * nzy + 1.0f);
            GVertex v{wx, wy, h, nzx / nl, nzy / nl, 1.0f / nl, 0, 0, 0, 0, 0, 0,
                      static_cast<float>(i) / (G - 1), static_cast<float>(j) / (G - 1)};
            mesh.vertices.push_back(v);
            lo[0] = std::min(lo[0], wx); lo[1] = std::min(lo[1], wy); lo[2] = std::min(lo[2], h);
            hi[0] = std::max(hi[0], wx); hi[1] = std::max(hi[1], wy); hi[2] = std::max(hi[2], h);
        }
    }
    for (int j = 0; j < G - 1; ++j) {
        for (int i = 0; i < G - 1; ++i) {
            uint32_t a = j * G + i, b = j * G + i + 1, c = (j + 1) * G + i, d = (j + 1) * G + i + 1;
            mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
        }
    }
    ModelMaterialCPU mat; mat.index = 0; mat.kind = 1; // terrain (grass/rock, procedural)
    model->materials.push_back(mat);

    // Water: emit a translucent quad ONLY over terrain cells that sit below the
    // *real* water plane (parsed from the havk chunk), so land is always what
    // you see and water only fills in where the ground actually dips under it.
    // Previously this guessed a Z from the 20th-percentile of terrain heights,
    // which has no relationship to the map's actual water level -- on a lot of
    // maps that guess sat far too high and flooded huge stretches of normal,
    // dry land, making the whole map look "squeezed" into the water's footprint.
    // With no real water data, we skip water entirely rather than guess wrong.
    if (hasWaterZ) {
        ModelMeshCPU wm;
        wm.materialIndex = 1;
        for (int j = 0; j < G - 1; ++j)
            for (int i = 0; i < G - 1; ++i) {
                float h00 = H(i, j), h10 = H(i + 1, j), h01 = H(i, j + 1), h11 = H(i + 1, j + 1);
                if (std::max(std::max(h00, h10), std::max(h01, h11)) >= waterZ) continue; // any dry corner -> land wins
                uint32_t base = static_cast<uint32_t>(wm.vertices.size());
                float xa = x0 + i * dx, xb = x0 + (i + 1) * dx, ya = y0 + j * dy, yb = y0 + (j + 1) * dy;
                wm.vertices.push_back(GVertex{xa, ya, waterZ, 0,0,1, 0,0,0, 0,0,0, 0,0});
                wm.vertices.push_back(GVertex{xb, ya, waterZ, 0,0,1, 0,0,0, 0,0,0, 0,0});
                wm.vertices.push_back(GVertex{xa, yb, waterZ, 0,0,1, 0,0,0, 0,0,0, 0,0});
                wm.vertices.push_back(GVertex{xb, yb, waterZ, 0,0,1, 0,0,0, 0,0,0, 0,0});
                wm.indices.insert(wm.indices.end(), {base+0, base+2, base+1, base+1, base+2, base+3});
            }
        if (!wm.vertices.empty()) {
            model->meshes.push_back(std::move(wm));
            ModelMaterialCPU wmat; wmat.index = 1; wmat.kind = 2; // water (translucent blue)
            model->materials.push_back(wmat);
        }
    }

    model->totalVerts = 0; model->totalTris = 0;
    for (const auto& m : model->meshes) { model->totalVerts += (uint32_t)m.vertices.size(); model->totalTris += (uint32_t)m.indices.size() / 3; }
    for (int k = 0; k < 3; ++k) model->center[k] = (lo[k] + hi[k]) * 0.5f;
    float ext[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
    model->radius = 0.5f * std::sqrt(ext[0] * ext[0] + ext[1] * ext[1] + ext[2] * ext[2]);
    return model;
}

// Builds a renderable model from the Havok collision mesh (walls/floors/ramps).
// Rendered as a translucent orange overlay (material kind 3) so the collidable
// geometry is clearly distinguishable from props/terrain/water.
std::shared_ptr<ModelPreview> build_collision_model(const gw2model::Extractor::MapCollision& c) {
    if (!c.present) return nullptr;
    size_t nv = c.verts.size() / 3;
    if (nv < 3 || c.indices.size() < 3) return nullptr;

    std::vector<float> nrm(nv * 3, 0.0f);
    for (size_t t = 0; t + 2 < c.indices.size(); t += 3) {
        uint32_t a = c.indices[t], b = c.indices[t + 1], d = c.indices[t + 2];
        if (a >= nv || b >= nv || d >= nv) continue;
        const float* pa = &c.verts[a * 3]; const float* pb = &c.verts[b * 3]; const float* pd = &c.verts[d * 3];
        float u[3] = {pb[0]-pa[0], pb[1]-pa[1], pb[2]-pa[2]}, v[3] = {pd[0]-pa[0], pd[1]-pa[1], pd[2]-pa[2]};
        float fn[3] = {u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0]};
        for (uint32_t idx : {a, b, d}) for (int k = 0; k < 3; ++k) nrm[idx * 3 + k] += fn[k];
    }

    auto model = std::make_shared<ModelPreview>();
    model->meshes.emplace_back();
    ModelMeshCPU& mesh = model->meshes[0];
    mesh.materialIndex = 0;
    mesh.vertices.reserve(nv);
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < nv; ++i) {
        const float* p = &c.verts[i * 3];
        float n[3] = {nrm[i*3], nrm[i*3+1], nrm[i*3+2]};
        float nl = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]); if (nl < 1e-6f) { n[2] = 1; nl = 1; }
        mesh.vertices.push_back(GVertex{p[0], p[1], p[2], n[0]/nl, n[1]/nl, n[2]/nl, 0,0,0, 0,0,0, 0,0});
        for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); }
    }
    mesh.indices = c.indices;
    ModelMaterialCPU mat; mat.index = 0; mat.kind = 3; // collision (translucent orange)
    model->materials.push_back(mat);
    model->totalVerts = static_cast<uint32_t>(nv);
    model->totalTris = static_cast<uint32_t>(c.indices.size() / 3);
    for (int k = 0; k < 3; ++k) model->center[k] = (lo[k] + hi[k]) * 0.5f;
    float ext[3] = {hi[0]-lo[0], hi[1]-lo[1], hi[2]-lo[2]};
    model->radius = 0.5f * std::sqrt(ext[0]*ext[0] + ext[1]*ext[1] + ext[2]*ext[2]);
    return model;
}

// Builds a coordinated map scene from a mapc/area packfile: parse the prop
// placement (prp2), load each unique prop model (deduped by fileId, capped), and
// record one instance per placement with its world transform.
std::shared_ptr<MapScene> build_map_scene(const std::vector<uint8_t>& map_bytes, const std::string& dat_path,
                                          const nlohmann::json& tpl) {
    std::vector<gw2model::Extractor::MapProp> props;
    gw2model::Extractor::MapTerrain terr;
    gw2model::Extractor::MapCollision coll;
    try {
        gw2model::Extractor ex(map_bytes, tpl);
        props = ex.parseMapProps();
        terr = ex.parseTerrain();
        coll = ex.parseMapCollision();
    } catch (const std::exception&) {
        return nullptr;
    }
    if (props.empty() && !terr.present) return nullptr;

    Gw2Dat dat;
    try {
        load_dat_file(dat, dat_path);
    } catch (const std::exception&) {
        return nullptr;
    }

    auto scene = std::make_shared<MapScene>();
    scene->totalProps = static_cast<uint32_t>(props.size());

    // 1) De-duplicate the prop models -> a list of unique fileIds to load (capped).
    constexpr size_t kMaxModels = 600;
    std::vector<uint32_t> uniqueIds;
    std::unordered_map<uint32_t, int> idSlot; // fileId -> index into uniqueIds (-1 = over cap)
    for (const auto& p : props) {
        if (idSlot.count(p.fileId)) continue;
        if (uniqueIds.size() < kMaxModels) { idSlot[p.fileId] = static_cast<int>(uniqueIds.size()); uniqueIds.push_back(p.fileId); }
        else idSlot[p.fileId] = -1;
    }

    // 2) Load the unique models IN PARALLEL. Each task reads its MODL bytes off a
    //    shared read-only archive (read_entry_bytes uses its own file handle) and
    //    decodes geometry+textures via build_model_preview, reusing the single
    //    already-parsed `dat` (its file handle is already closed by load_dat_file,
    //    so concurrent read-only lookups against it are safe) instead of each
    //    model re-parsing the whole MFT table from scratch -- that redundant
    //    reparse (times up to kMaxModels) used to be the actual "loading lambat"
    //    cost, not a lack of threads.
    std::vector<std::shared_ptr<ModelPreview>> loaded(uniqueIds.size());
    parallel_for(uniqueIds.size(), [&](size_t i) {
        std::vector<uint8_t> modl = load_modl_bytes_by_fileid(dat, uniqueIds[i]);
        if (!modl.empty() && pf_has_chunk(modl, "GEOM")) {
            auto mp = build_model_preview(modl, dat, tpl);
            if (mp) loaded[i] = std::move(mp);
        }
    });

    // 3) Compact the successful loads into scene->models and map slot -> index.
    std::vector<int> slotToModel(uniqueIds.size(), -1);
    for (size_t i = 0; i < uniqueIds.size(); ++i) {
        if (!loaded[i]) continue;
        slotToModel[i] = static_cast<int>(scene->models.size());
        scene->models.push_back(std::move(*loaded[i]));
    }
    scene->loadedModels = static_cast<uint32_t>(scene->models.size());

    // 4) One instance per placed prop that resolved to a loaded model.
    for (const auto& p : props) {
        auto it = idSlot.find(p.fileId);
        if (it == idSlot.end() || it->second < 0) continue;
        int mi = slotToModel[it->second];
        if (mi < 0) continue;
        MapInstance in;
        in.model = mi;
        for (int k = 0; k < 3; ++k) { in.pos[k] = p.pos[k]; in.rot[k] = p.rot[k]; }
        in.scale = p.scale;
        scene->instances.push_back(in);
    }

    // 5) Terrain ground surface (layer 1) + Havok collision mesh (layer 2), both
    //    world-space with identity instances. Collision is hidden by default.
    if (auto tm = build_terrain_model(terr, coll.hasWater, coll.waterZ)) {
        MapInstance in;
        in.model = static_cast<int>(scene->models.size());
        in.scale = 1.0f; in.layer = 1;
        scene->models.push_back(std::move(*tm));
        scene->instances.push_back(in);
    }
    if (auto cm = build_collision_model(coll)) {
        MapInstance in;
        in.model = static_cast<int>(scene->models.size());
        in.scale = 1.0f; in.layer = 2;
        scene->models.push_back(std::move(*cm));
        scene->instances.push_back(in);
    }
    scene->loadedModels = static_cast<uint32_t>(scene->models.size());

    if (scene->instances.empty()) return nullptr;
    return scene;
}

// Heuristic: does the buffer look like human-readable text (html/css/js/xml/...)?
bool looks_like_text(const std::vector<uint8_t>& d) {
    if (d.empty()) return false;
    size_t sample = std::min<size_t>(d.size(), 4096);
    size_t printable = 0;
    for (size_t i = 0; i < sample; ++i) {
        uint8_t c = d[i];
        if (c == 0x09 || c == 0x0A || c == 0x0D || (c >= 0x20 && c <= 0x7E) || c >= 0x80) {
            ++printable;
        } else {
            // control byte other than tab/newline -> almost certainly binary
        }
    }
    return printable * 100 >= sample * 95;
}

std::wstring bytes_to_wide(const std::vector<uint8_t>& d) {
    if (d.empty()) return {};
    int len = static_cast<int>(std::min<size_t>(d.size(), 1u << 22)); // cap at ~4MB for the edit control
    const char* p = reinterpret_cast<const char*>(d.data());
    int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, len, nullptr, 0);
    UINT cp = CP_UTF8;
    if (need <= 0) {
        cp = CP_ACP;
        need = MultiByteToWideChar(CP_ACP, 0, p, len, nullptr, 0);
    }
    if (need <= 0) return {};
    std::wstring w(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, p, len, w.data(), need);
    return w;
}

// ---- PIMG (paged image atlas) --------------------------------------------
// A PIMG packfile (chunk PGTB) is not pixels: it's a grid of tiles, each an
// EXTERNAL texture referenced by fileId at a (cx,cy) coordinate. We composite the
// referenced tiles into a single downscaled preview image.
struct PimgAtlas {
    int tileW = 0, tileH = 0;
    uint32_t format = 0;                 // fourcc, e.g. 'DXT5'
    int minX = 0, minY = 0, maxX = 0, maxY = 0;
    struct Page { int cx, cy; uint32_t fileId; };
    std::vector<Page> pages;
};

// Decode a GW2 "filename" field at position p -> fileId. The field is a self-relative
// pointer to a {uint16 lo, uint16 hi, ...} record; fileId = 0xFF00*hi + lo - 0xFF00FF
// (mirrors gw2model::decodeFilenameAt).
uint32_t decode_pimg_fileref(const std::vector<uint8_t>& d, size_t p) {
    auto rd32 = [&](size_t q) -> uint32_t {
        return (q + 4 <= d.size()) ? (d[q] | (d[q + 1] << 8) | (d[q + 2] << 16) | ((uint32_t)d[q + 3] << 24)) : 0;
    };
    auto rd16 = [&](size_t q) -> uint32_t { return (q + 2 <= d.size()) ? (uint32_t)(d[q] | (d[q + 1] << 8)) : 0; };
    uint32_t v = rd32(p);
    if (v == 0) return 0;
    size_t t = p + v;
    uint32_t lo = rd16(t), hi = rd16(t + 2);
    if (lo < 0x100 || hi < 0x100) return 0;
    return (uint32_t)(0xFF00L * hi + lo - 0xFF00FF);
}

// Parse the PGTB v3 chunk (layers[0] gives tile size/format; strippedPages gives the
// tile list). Returns false for other versions / malformed data.
bool parse_pimg(const std::vector<uint8_t>& d, PimgAtlas& out) {
    if (d.size() < 12 || d[0] != 'P' || d[1] != 'F' || std::memcmp(d.data() + 8, "PIMG", 4) != 0) return false;
    auto rd16 = [&](size_t p) -> uint32_t { return (p + 2 <= d.size()) ? (d[p] | (d[p + 1] << 8)) : 0; };
    auto rd32 = [&](size_t p) -> uint32_t {
        return (p + 4 <= d.size()) ? (d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24)) : 0;
    };
    // Locate the PGTB chunk.
    size_t pos = rd16(6);
    while (pos + 16 <= d.size() && std::memcmp(d.data() + pos, "PGTB", 4) != 0) {
        uint32_t csz = rd32(pos + 4); size_t next = pos + 8 + csz;
        if (next <= pos) return false;
        pos = next;
    }
    if (pos + 16 > d.size() || std::memcmp(d.data() + pos, "PGTB", 4) != 0) return false;
    if (rd16(pos + 8) != 3) return false;            // only the V3 layout is implemented
    size_t fb = pos + rd16(pos + 10);                // field base (chunk header size)
    // layers[0]: strippedDims @+8 (tile w,h), strippedFormat @+20.
    uint32_t layerCount = rd32(fb);
    size_t layer0 = (fb + 4) + rd32(fb + 4);         // self-relative array_ptr
    if (layerCount >= 1 && layer0 + 24 <= d.size()) {
        out.tileW = (int)rd32(layer0 + 8);
        out.tileH = (int)rd32(layer0 + 12);
        out.format = rd32(layer0 + 20);
    }
    // strippedPages: array of PagedImagePageDataV3 (24 bytes: layer, coord(cx,cy), fileId, flags, solidColor).
    uint32_t pageCount = rd32(fb + 16);
    size_t pages0 = (fb + 20) + rd32(fb + 20);
    if (pageCount == 0 || pageCount > 500000) return false;
    out.minX = out.minY = 1 << 30; out.maxX = out.maxY = -(1 << 30);
    for (uint32_t i = 0; i < pageCount; ++i) {
        size_t pp = pages0 + (size_t)i * 24;
        if (pp + 24 > d.size()) break;
        PimgAtlas::Page pg{ (int)rd32(pp + 4), (int)rd32(pp + 8), decode_pimg_fileref(d, pp + 12) };
        if (pg.fileId == 0) continue;
        out.pages.push_back(pg);
        out.minX = std::min(out.minX, pg.cx); out.maxX = std::max(out.maxX, pg.cx);
        out.minY = std::min(out.minY, pg.cy); out.maxY = std::max(out.maxY, pg.cy);
    }
    return !out.pages.empty() && out.tileW > 0 && out.tileH > 0;
}

// Composite an atlas into a single RGBA preview (downscaled to fit kMaxDim, tiles
// capped so huge map atlases still preview quickly). Fills result.preview_* + returns
// a summary line. `dat` must be loaded.
std::string composite_pimg(const PimgAtlas& a, Gw2Dat& dat, ExtractedEntry& result) {
    const int kMaxDim = 2048;   // max preview dimension
    const int kMaxTiles = 300;  // cap on tiles actually decoded (subsample beyond) -- keeps
                                // giant map atlases (1000+ tiles) previewing in reasonable time

    long long gridW = (long long)a.maxX - a.minX + 1, gridH = (long long)a.maxY - a.minY + 1;
    long long fullW = gridW * a.tileW, fullH = gridH * a.tileH;
    double scale = std::min(1.0, (double)kMaxDim / std::max(fullW, fullH));
    int outW = std::max(1, (int)(fullW * scale)), outH = std::max(1, (int)(fullH * scale));
    result.preview_pixels.assign((size_t)outW * outH * 4, 0);
    uint8_t* dst = result.preview_pixels.data();

    int stride = 1 + (int)(a.pages.size() / (size_t)kMaxTiles); // subsample if too many
    int loaded = 0;
    for (size_t pi = 0; pi < a.pages.size(); pi += stride) {
        const auto& pg = a.pages[pi];
        ModelTextureCPU tex;
        if (!decode_texture_by_fileid(dat, pg.fileId, tex) || tex.rgba.empty()) continue;
        ++loaded;
        // Destination rectangle for this tile in the downscaled canvas.
        int dx0 = (int)(((long long)(pg.cx - a.minX) * a.tileW) * scale);
        int dy0 = (int)(((long long)(pg.cy - a.minY) * a.tileH) * scale);
        int dw = std::max(1, (int)(a.tileW * scale)), dh = std::max(1, (int)(a.tileH * scale));
        for (int y = 0; y < dh; ++y) {
            int oy = dy0 + y; if (oy < 0 || oy >= outH) continue;
            int sy = (int)((long long)y * tex.height / dh); if (sy >= tex.height) sy = tex.height - 1;
            for (int x = 0; x < dw; ++x) {
                int ox = dx0 + x; if (ox < 0 || ox >= outW) continue;
                int sx = (int)((long long)x * tex.width / dw); if (sx >= tex.width) sx = tex.width - 1;
                const uint8_t* s = &tex.rgba[((size_t)sy * tex.width + sx) * 4];
                uint8_t* o = &dst[((size_t)oy * outW + ox) * 4];
                o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = s[3];
            }
        }
    }
    result.preview_dxgi_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    result.preview_width = outW;
    result.preview_height = outH;
    result.preview_pitch = outW * 4;
    char fourcc[5] = {0}; std::memcpy(fourcc, &a.format, 4);
    for (int i = 0; i < 4; ++i) if (fourcc[i] < 32) fourcc[i] = '?';
    result.preview_format_label = std::string("PIMG atlas ") + fourcc;
    char buf[256];
    std::snprintf(buf, sizeof buf, "PIMG paged-image atlas: %zu tiles (%lldx%lld grid, %dx%d px each, %s), %d tiles loaded into a %dx%d preview.",
                  a.pages.size(), gridW, gridH, a.tileW, a.tileH, fourcc, loaded, outW, outH);
    return buf;
}

// ---- AMSP (sound bank) ----------------------------------------------------
// An AMSP packfile is a sound SCRIPT/bank (PF v5, 64-bit pointers): its metaSound
// array references the actual audio in EXTERNAL ASND entries by fileId. We collect
// those fileIds, load each ASND, and present them as a playable multi-sound bank.
// Layout (chunk AMSP v33): metaSound array_ptr @chunk+60 {u32 count, i64 selfRelPtr};
// MetaSoundDataV31 stride 326, fileName array_ptr @elem+60; FileNameDataV31 stride 36,
// fileName field @fe+24 = a 64-bit self-relative ptr to a {u16 lo, u16 hi} fileref
// (fileId = 0xFF00*hi + lo - 0xFF00FF, same decode as PIMG/model filenames).
std::vector<uint32_t> parse_amsp_sound_ids(const std::vector<uint8_t>& d) {
    std::vector<uint32_t> ids;
    if (d.size() < 12 || d[0] != 'P' || d[1] != 'F' || std::memcmp(d.data() + 8, "AMSP", 4) != 0) return ids;
    auto rd16 = [&](size_t p) -> uint32_t { return (p + 2 <= d.size()) ? (uint32_t)(d[p] | (d[p + 1] << 8)) : 0; };
    auto rd32 = [&](size_t p) -> uint32_t {
        return (p + 4 <= d.size()) ? (d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24)) : 0;
    };
    auto rd64 = [&](size_t p) -> int64_t {
        if (p + 8 > d.size()) return 0;
        int64_t v = 0; std::memcpy(&v, d.data() + p, 8); return v;
    };
    size_t pos = rd16(6);
    while (pos + 16 <= d.size() && std::memcmp(d.data() + pos, "AMSP", 4) != 0) {
        uint32_t csz = rd32(pos + 4); size_t next = pos + 8 + csz; if (next <= pos) return ids; pos = next;
    }
    if (pos + 72 > d.size() || std::memcmp(d.data() + pos, "AMSP", 4) != 0) return ids;

    uint32_t msCount = rd32(pos + 60);
    int64_t  msRel   = rd64(pos + 64);
    if (msRel == 0 || msCount == 0 || msCount > 500000) return ids;
    size_t ms0 = (pos + 64) + (size_t)msRel;

    const size_t MS_STRIDE = 326, FN_STRIDE = 36;
    const size_t kMaxSounds = 512;
    std::set<uint32_t> seen;
    for (uint32_t i = 0; i < msCount && ids.size() < kMaxSounds; ++i) {
        size_t elem = ms0 + (size_t)i * MS_STRIDE;
        if (elem + 72 > d.size()) break;
        uint32_t fnCount = rd32(elem + 60);
        int64_t  fnRel   = rd64(elem + 64);
        if (fnRel == 0 || fnCount == 0 || fnCount > 4096) continue;
        size_t fn0 = (elem + 64) + (size_t)fnRel;
        for (uint32_t j = 0; j < fnCount && ids.size() < kMaxSounds; ++j) {
            size_t fe = fn0 + (size_t)j * FN_STRIDE;
            if (fe + 32 > d.size()) break;
            int64_t rel = rd64(fe + 24);
            if (rel == 0) continue;
            size_t rec = (fe + 24) + (size_t)rel;
            uint32_t lo = rd16(rec), hi = rd16(rec + 2);
            if (lo < 0x100 || hi < 0x100) continue;
            uint32_t fid = (uint32_t)(0xFF00L * hi + lo - 0xFF00FF);
            if (fid && seen.insert(fid).second) ids.push_back(fid);
        }
    }
    return ids;
}

// Load the AMSP's referenced ASND sounds into result.audio_clips. Returns true if any
// playable sound was found. Builds a fileId->baseId map once (O(1) lookups).
bool build_amsp_audio(const std::vector<uint8_t>& amsp, const std::string& dat_path, ExtractedEntry& result) {
    std::vector<uint32_t> ids = parse_amsp_sound_ids(amsp);
    if (ids.empty() || dat_path.empty()) return false;
    Gw2Dat dat;
    try { load_dat_file(dat, dat_path); } catch (const std::exception&) { return false; }
    std::unordered_map<uint32_t, uint32_t> fid2base;
    fid2base.reserve(dat.mft_file_id_data_list.size() * 2);
    for (const auto& e : dat.mft_file_id_data_list) fid2base[e.file_id] = e.base_id;

    for (uint32_t fid : ids) {
        auto it = fid2base.find(fid);
        if (it == fid2base.end()) continue;
        uint32_t base = it->second;
        if (base == 0 || base - 1 >= dat.mft_data_list.size()) continue;
        std::vector<uint8_t> bytes;
        try {
            const MftData& e = dat.mft_data_list[base - 1];
            std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
            std::vector<uint8_t> stripped = gw2cmp::strip_crc32(std::span<const uint8_t>(raw));
            if (e.compression_flag == 0) bytes = std::move(stripped);
            else if (stripped.size() >= 8) {
                uint32_t usz = stripped[4] | (stripped[5] << 8) | (stripped[6] << 16) | ((uint32_t)stripped[7] << 24);
                bytes = gw2cmp::decompress_method0(std::span<const uint8_t>(stripped).subspan(8), usz);
            }
        } catch (const std::exception&) { continue; }
        if (bytes.empty()) continue;
        for (auto& c : gw2audio::extract(bytes.data(), bytes.size())) {
            AudioClipCPU cc; cc.codec = gw2audio::codecName(c.codec); cc.data = std::move(c.data);
            result.audio_clips.push_back(std::move(cc));
        }
    }
    return !result.audio_clips.empty();
}

// ---- cntc (content datastore) ---------------------------------------------
// A "cntc" packfile (PF v5, 64-bit ptrs, single chunk "Main" = PackContent) is GW2's
// content database: content type/namespace definitions, an index, fixup tables, a
// string table of content codenames, and a big content-data blob. It's not a viewable
// asset, so we parse the header into a readable summary + a sample of the codenames.
std::wstring parse_cntc_summary(const std::vector<uint8_t>& d) {
    if (d.size() < 12 || d[0] != 'P' || d[1] != 'F' || std::memcmp(d.data() + 8, "cntc", 4) != 0) return {};
    auto rd16 = [&](size_t p) -> uint32_t { return (p + 2 <= d.size()) ? (uint32_t)(d[p] | (d[p + 1] << 8)) : 0; };
    auto rd32 = [&](size_t p) -> uint32_t {
        return (p + 4 <= d.size()) ? (d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24)) : 0;
    };
    auto rd64 = [&](size_t p) -> int64_t { if (p + 8 > d.size()) return 0; int64_t v = 0; std::memcpy(&v, d.data() + p, 8); return v; };
    // Locate the Main chunk.
    size_t pos = rd16(6);
    while (pos + 16 <= d.size() && std::memcmp(d.data() + pos, "Main", 4) != 0) {
        uint32_t csz = rd32(pos + 4); size_t next = pos + 8 + csz; if (next <= pos) return {}; pos = next;
    }
    if (pos + 16 > d.size() || std::memcmp(d.data() + pos, "Main", 4) != 0) return {};
    size_t base = pos + 16;                       // PackContent fields (flags @base, arrays follow)
    // Each array_ptr = {u32 count, i64 self-relative ptr} (12 bytes); ptr from its own position.
    auto arr = [&](int idx, size_t& dataOff) -> uint32_t {
        size_t p = base + 4 + (size_t)idx * 12;   // skip flags(4)
        uint32_t c = rd32(p);
        int64_t rel = rd64(p + 4);
        dataOff = (size_t)((p + 4) + rel);
        return c;
    };
    static const wchar_t* kNames[11] = {
        L"content types", L"namespaces", L"file references", L"index entries", L"local fixups",
        L"external fixups", L"file-index fixups", L"string-index fixups", L"tracked references",
        L"strings (codenames)", L"content data bytes" };
    size_t off[11]; uint32_t cnt[11];
    for (int i = 0; i < 11; ++i) cnt[i] = arr(i, off[i]);

    std::wstring s = L"GW2 content datastore (cntc / PackContent)\r\n";
    s += L"An internal content database -- not a viewable/playable asset.\r\n\r\n";
    wchar_t line[128];
    for (int i = 0; i < 11; ++i) { swprintf(line, 128, L"  %-22ls %u\r\n", kNames[i], cnt[i]); s += line; }

    // Namespace names (present in master-manifest cntc files): wchar_ptr + domain + parentIndex (16 B).
    if (cnt[1] > 0) {
        s += L"\r\nNamespaces:\r\n";
        uint32_t nn = cnt[1] < 60 ? cnt[1] : 60;
        for (uint32_t i = 0; i < nn; ++i) {
            size_t ep = off[1] + (size_t)i * 16;
            int64_t rel = rd64(ep);
            size_t sp = (size_t)(ep + rel);
            std::wstring name;
            for (size_t q = sp; q + 2 <= d.size() && name.size() < 120; q += 2) {
                wchar_t ch = (wchar_t)(d[q] | (d[q + 1] << 8)); if (!ch) break; name += ch;
            }
            if (rel == 0) name = L"(root)";
            s += L"  " + name + L"\r\n";
        }
    }

    // Sample of the content codename string table.
    if (cnt[9] > 0) {
        s += L"\r\nContent codenames (first 60 of "; s += std::to_wstring(cnt[9]); s += L"):\r\n";
        uint32_t ns = cnt[9] < 60 ? cnt[9] : 60;
        for (uint32_t i = 0; i < ns; ++i) {
            size_t ep = off[9] + (size_t)i * 8;
            int64_t rel = rd64(ep);
            if (rel == 0) continue;
            size_t sp = (size_t)(ep + rel);
            std::wstring name;
            for (size_t q = sp; q + 2 <= d.size() && name.size() < 120; q += 2) {
                wchar_t ch = (wchar_t)(d[q] | (d[q + 1] << 8)); if (!ch) break; name += ch;
            }
            s += L"  " + name + L"\r\n";
        }
    }
    return s;
}

// Collect the unique external asset fileIds a cntc content blob references. Each
// `fileIndices` fixup {u32 relocOffset} marks a spot in the content byte-array that
// holds a raw fileId (verified: they resolve to textures/models). Returns up to `cap`
// unique ids.
std::vector<uint32_t> cntc_referenced_asset_ids(const std::vector<uint8_t>& d, size_t cap) {
    std::vector<uint32_t> out;
    if (d.size() < 12 || std::memcmp(d.data() + 8, "cntc", 4) != 0) return out;
    auto rd16 = [&](size_t p) -> uint32_t { return (p + 2 <= d.size()) ? (uint32_t)(d[p] | (d[p + 1] << 8)) : 0; };
    auto rd32 = [&](size_t p) -> uint32_t {
        return (p + 4 <= d.size()) ? (d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24)) : 0;
    };
    auto rd64 = [&](size_t p) -> int64_t { if (p + 8 > d.size()) return 0; int64_t v = 0; std::memcpy(&v, d.data() + p, 8); return v; };
    size_t pos = rd16(6);
    while (pos + 16 <= d.size() && std::memcmp(d.data() + pos, "Main", 4) != 0) {
        uint32_t csz = rd32(pos + 4); size_t next = pos + 8 + csz; if (next <= pos) return out; pos = next;
    }
    if (pos + 16 > d.size()) return out;
    size_t base = pos + 16;
    auto arr = [&](int i, size_t& off) { size_t p = base + 4 + (size_t)i * 12; off = (size_t)((p + 4) + rd64(p + 4)); return rd32(p); };
    size_t fidxOff, contentOff;
    uint32_t fidxCount = arr(6, fidxOff);  // fileIndices
    arr(10, contentOff);                   // content byte-array start
    std::set<uint32_t> seen;
    for (uint32_t i = 0; i < fidxCount && out.size() < cap; ++i) {
        uint32_t reloc = rd32(fidxOff + (size_t)i * 4);
        uint32_t fid = rd32(contentOff + reloc);
        if (fid > 40000 && fid < 0xFFFFFF && seen.insert(fid).second) out.push_back(fid);
    }
    return out;
}

// Lightweight type peek for an entry addressed by fileId: decompress its header and
// classify by magic/container. Used to characterize cntc asset references.
std::string classify_fileid(Gw2Dat& dat, uint32_t fileId) {
    std::vector<uint8_t> b = load_modl_bytes_by_fileid(dat, fileId); // generic decompress
    if (b.size() < 12) return "";
    if (b[0] == 'P' && b[1] == 'F') {
        char c[5] = {0}; std::memcpy(c, b.data() + 8, 4);
        std::string cs(c);
        if (pf_has_chunk(b, "GEOM")) return "model";
        if (cs == "ASND" || cs == "ABNK" || cs == "AMSP") return "audio";
        if (cs == "PIMG") return "image atlas";
        return "packfile " + cs;
    }
    static const char* atex[] = {"ATEX", "ATTX", "ATEC", "ATEP", "ATEU", "ATET", "CTEX"};
    for (const char* m : atex) if (std::memcmp(b.data(), m, 4) == 0) return "texture";
    if (b[0] == 'D' && b[1] == 'D' && b[2] == 'S') return "texture";
    if (std::memcmp(b.data(), "strs", 4) == 0) return "strings";
    return "other";
}

// All the CPU-bound decompression/format-detection work, independent of how
// `raw_bytes` was read off disk -- this is what makes it safe to run on a
// background thread. `dat_path` is used to resolve model textures.
ExtractedEntry decompress_raw_entry(std::vector<uint8_t> raw_bytes, uint16_t compression_flag,
                                    const std::string& dat_path) {
    ExtractedEntry result;
    result.compressed = raw_bytes;

    std::vector<uint8_t> stripped = gw2cmp::strip_crc32(raw_bytes);

    std::vector<uint8_t> file_bytes;
    if (compression_flag != 0) {
        if (stripped.size() < 8) {
            throw std::runtime_error("Entry too small to contain a Method0 header.");
        }
        uint32_t uncompressed_size = read_u32_le(stripped, 4);
        file_bytes = gw2cmp::decompress_method0(std::span<const uint8_t>(stripped).subspan(8), uncompressed_size);
    } else {
        file_bytes = std::move(stripped);
    }

    result.decompressed = std::move(file_bytes);

    if (is_atex_family(result.decompressed)) {
        result.is_image = fill_preview_from_atex(result);
    } else if (starts_with_magic(result.decompressed, "DDS ")) {
        result.is_image = fill_preview_from_dds(result);
    } else if (is_png(result.decompressed) || is_jpeg(result.decompressed) || is_bmp(result.decompressed)) {
        auto wic_image = gw2wic::decode(result.decompressed.data(), result.decompressed.size());
        if (wic_image) {
            result.preview_dxgi_format = DXGI_FORMAT_R8G8B8A8_UNORM;
            result.preview_width = wic_image->width;
            result.preview_height = wic_image->height;
            result.preview_pitch = wic_image->width * 4;
            result.preview_pixels = std::move(wic_image->rgba);
            result.preview_format_label =
                is_png(result.decompressed) ? "PNG" : (is_jpeg(result.decompressed) ? "JPEG" : "BMP");
            result.is_image = true;
        }
    }

    if (result.is_image) {
        result.kind = PreviewKind::Image;
        return result;
    }

    // strs string table -> text listing.
    if (gw2strs::is_strs(result.decompressed.data(), result.decompressed.size())) {
        result.kind = PreviewKind::Strs;
        result.text_preview = gw2strs::decode(result.decompressed.data(), result.decompressed.size());
        return result;
    }

    // ASND audio packfile (or raw audio) with an embedded MP3/Ogg -> playable audio.
    // AMSP sound-pool metadata has no embedded samples, so extract() returns empty
    // and we fall through to the generic handling.
    if (gw2audio::isAudio(result.decompressed.data(), result.decompressed.size())) {
        bool isAmsp = result.decompressed.size() >= 12 &&
                      std::memcmp(result.decompressed.data() + 8, "AMSP", 4) == 0;
        if (isAmsp) {
            // AMSP sound bank: the audio lives in external ASND entries it references.
            if (build_amsp_audio(result.decompressed, dat_path, result)) {
                result.kind = PreviewKind::Audio;
                return result;
            }
        } else {
            // ASND / ABNK bank / raw asnd: the audio is embedded.
            auto clips = gw2audio::extract(result.decompressed.data(), result.decompressed.size());
            if (!clips.empty()) {
                result.kind = PreviewKind::Audio;
                for (auto& c : clips) {
                    AudioClipCPU cc;
                    cc.codec = gw2audio::codecName(c.codec);
                    cc.data = std::move(c.data);
                    result.audio_clips.push_back(std::move(cc));
                }
                return result;
            }
        }
    }

    // PIMG paged-image atlas -> composite its referenced tiles into a preview image.
    if (result.decompressed.size() >= 12 && result.decompressed[0] == 'P' && result.decompressed[1] == 'F' &&
        std::memcmp(result.decompressed.data() + 8, "PIMG", 4) == 0) {
        PimgAtlas atlas;
        if (parse_pimg(result.decompressed, atlas) && !dat_path.empty()) {
            Gw2Dat dat;
            try { load_dat_file(dat, dat_path); } catch (const std::exception&) {}
            composite_pimg(atlas, dat, result);
            result.kind = PreviewKind::Image;
            result.is_image = true;
            return result;
        }
    }

    // cntc content datastore -> a readable summary (counts + content codenames).
    if (result.decompressed.size() >= 12 && result.decompressed[0] == 'P' && result.decompressed[1] == 'F' &&
        std::memcmp(result.decompressed.data() + 8, "cntc", 4) == 0) {
        std::wstring summary = parse_cntc_summary(result.decompressed);
        if (!summary.empty()) {
            // Every external asset fileId the content references -> an interactive list.
            result.content_asset_ids = cntc_referenced_asset_ids(result.decompressed, 100000);
            summary += L"\r\nReferences ";
            summary += std::to_wstring(result.content_asset_ids.size());
            summary += L" external asset fileIds (textures / models / audio / materials).\r\n"
                       L"Select one on the left to preview it here.";
            result.kind = result.content_asset_ids.empty() ? PreviewKind::Text : PreviewKind::Content;
            result.text_preview = std::move(summary);
            return result;
        }
    }

    // PF packfile with a prop-placement chunk -> map scene (mapc/area). Checked
    // before GEOM since a map has prp2 but no GEOM of its own.
    if (pf_has_chunk(result.decompressed, "prp2")) {
        result.kind = PreviewKind::Map;
        auto tpl = gw2tpl::get();
        if (tpl && !dat_path.empty()) {
            result.map = build_map_scene(result.decompressed, dat_path, *tpl);
        }
        return result;
    }

    // PF packfile with geometry -> model (parsed only if a template is loaded).
    if (pf_has_chunk(result.decompressed, "GEOM")) {
        result.kind = PreviewKind::Model;
        auto tpl = gw2tpl::get();
        if (tpl && !dat_path.empty()) {
            // want_game=true: also extract the real game (bgfx DXBC) shaders per
            // material for the "Shader" render mode (single-model path only).
            result.model = build_model_preview(result.decompressed, dat_path, *tpl, /*want_game=*/true);
        }
        return result;
    }

    // Fallback: printable text (html/css/js/xml/...).
    if (looks_like_text(result.decompressed)) {
        result.kind = PreviewKind::Text;
        result.text_preview = bytes_to_wide(result.decompressed);
        return result;
    }

    return result;
}

} // namespace

// Public texture-resolution API (g_tex_full_res lives in the anonymous namespace
// above but is reachable throughout this translation unit).
void set_texture_full_res(bool full) { g_tex_full_res.store(full); }
bool texture_full_res() { return g_tex_full_res.load(); }

ExtractedEntry extract_entry(Gw2Dat& data_gw2, uint32_t mft_index) {
    std::vector<uint8_t> raw = extract_compressed_data(data_gw2, mft_index);
    return decompress_raw_entry(std::move(raw), data_gw2.mft_data_list[mft_index].compression_flag,
                                data_gw2.file_info.file_path);
}

ExtractedEntry extract_entry(const std::string& file_path, const MftData& entry) {
    std::vector<uint8_t> raw = read_entry_bytes(file_path, entry);
    return decompress_raw_entry(std::move(raw), entry.compression_flag, file_path);
}

std::shared_ptr<MapScene> build_map_zone_layer(const std::vector<uint8_t>& map_bytes, const std::string& dat_path) {
    auto tplp = gw2tpl::get();
    if (!tplp || dat_path.empty()) return nullptr;
    const nlohmann::json& tpl = *tplp;
    std::vector<gw2model::Extractor::MapZoneInst> zones;
    try {
        zones = gw2model::Extractor(map_bytes, tpl).parseMapZones();
    } catch (const std::exception&) {
        return nullptr;
    }
    if (zones.empty()) return nullptr;

    Gw2Dat dat;
    try { load_dat_file(dat, dat_path); } catch (const std::exception&) { return nullptr; }

    auto scene = std::make_shared<MapScene>();
    constexpr size_t kMaxModels = 600;
    std::vector<uint32_t> uniqueIds;
    std::unordered_map<uint32_t, int> idSlot;
    for (const auto& z : zones) {
        if (idSlot.count(z.fileId)) continue;
        if (uniqueIds.size() < kMaxModels) { idSlot[z.fileId] = static_cast<int>(uniqueIds.size()); uniqueIds.push_back(z.fileId); }
        else idSlot[z.fileId] = -1;
    }
    std::vector<std::shared_ptr<ModelPreview>> loaded(uniqueIds.size());
    parallel_for(uniqueIds.size(), [&](size_t i) {
        std::vector<uint8_t> modl = load_modl_bytes_by_fileid(dat, uniqueIds[i]);
        if (!modl.empty() && pf_has_chunk(modl, "GEOM")) {
            auto mp = build_model_preview(modl, dat, tpl);
            if (mp) loaded[i] = std::move(mp);
        }
    });
    std::vector<int> slotToModel(uniqueIds.size(), -1);
    for (size_t i = 0; i < uniqueIds.size(); ++i)
        if (loaded[i]) { slotToModel[i] = static_cast<int>(scene->models.size()); scene->models.push_back(std::move(*loaded[i])); }
    for (const auto& z : zones) {
        auto it = idSlot.find(z.fileId);
        if (it == idSlot.end() || it->second < 0) continue;
        int mi = slotToModel[it->second];
        if (mi < 0) continue;
        MapInstance in;
        in.model = mi;
        for (int k = 0; k < 3; ++k) in.pos[k] = z.pos[k];
        in.scale = z.scale;
        in.layer = 3;
        scene->instances.push_back(in);
    }
    scene->loadedModels = static_cast<uint32_t>(scene->models.size());
    if (scene->instances.empty()) return nullptr;
    return scene;
}
