// gw2viewer.cpp -- minimal Direct3D 11 viewer for GW2 .modl models.
//
// Loads a model straight out of Gw2.dat: decompress (Method0) -> extract
// geometry + material texture fileIds (gw2model) -> decode the diffuse ATEX to
// RGBA (gw2_atex) -> upload to D3D11 and render with an orbit camera.
//
// The shaders here are our own HLSL (compiled at runtime via d3dcompiler);
// they approximate the game look (albedo * lambert + hemispheric ambient),
// they are NOT the game's bgfx DXBC blobs.
//
// Build (MinGW): see build.ps1.  Usage:
//   gw2viewer.exe --dat <Gw2.dat> --base-id <N> --template <gw2_packfile.json>
//   gw2viewer.exe --dat <Gw2.dat> --file-id <N> --template <...>

#define WIN32_LEAN_AND_MEAN
#define CINTERFACE // expose C-style COM (lpVtbl) so COBJMACROS work under g++
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "gw2dat.h"
#include "cmp_decompress_method0.hpp"
#include "gw2_atex.hpp"
#include "gw2model.hpp"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// ---------------------------------------------------------------------------
//  tiny math (row-major 4x4)
// ---------------------------------------------------------------------------
struct Vec3
{
    float x, y, z;
};
static Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 norm(Vec3 a)
{
    float l = std::sqrt(dot(a, a));
    return l > 0 ? Vec3{a.x / l, a.y / l, a.z / l} : a;
}

struct Mat4
{
    float m[16];
};
static Mat4 identity()
{
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
    return r;
}
static Mat4 mul(const Mat4 &a, const Mat4 &b)
{
    Mat4 r{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
        {
            float s = 0;
            for (int k = 0; k < 4; k++)
                s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}
static Mat4 perspective(float fovy, float aspect, float zn, float zf)
{
    float f = 1.0f / std::tan(fovy * 0.5f);
    Mat4 r{};
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = zf / (zf - zn);
    r.m[11] = 1.0f;
    r.m[14] = -zn * zf / (zf - zn);
    return r;
}
static Mat4 lookAt(Vec3 eye, Vec3 at, Vec3 up)
{
    Vec3 z = norm(sub(at, eye));
    Vec3 x = norm(cross(up, z));
    Vec3 y = cross(z, x);
    Mat4 r = identity();
    r.m[0] = x.x;
    r.m[1] = y.x;
    r.m[2] = z.x;
    r.m[4] = x.y;
    r.m[5] = y.y;
    r.m[6] = z.y;
    r.m[8] = x.z;
    r.m[9] = y.z;
    r.m[10] = z.z;
    r.m[12] = -dot(x, eye);
    r.m[13] = -dot(y, eye);
    r.m[14] = -dot(z, eye);
    return r;
}

// ---------------------------------------------------------------------------
//  HLSL
// ---------------------------------------------------------------------------
static const char *kHLSL = R"(
cbuffer CB : register(b0) {
    row_major float4x4 uMVP;
    row_major float4x4 uModel;
    float3 uLightDir;  float uHasTex;
    float4 uTint;
    float  uHasNormal; float uIsEffect; float2 uPad;
};
Texture2D    gDiffuse : register(t0);   // albedo / color map
Texture2D    gNormal  : register(t1);   // 3DCX/BC5 normal (xy), z reconstructed
SamplerState gSamp    : register(s0);

struct VSIn  { float3 pos:POSITION; float3 nrm:NORMAL; float3 tan:TANGENT; float3 bit:BITANGENT; float2 uv:TEXCOORD0; };
struct VSOut { float4 pos:SV_POSITION; float3 nrm:NORMAL; float3 tan:TANGENT; float3 bit:BITANGENT; float2 uv:TEXCOORD0; };

VSOut VSMain(VSIn i){
    VSOut o;
    o.pos = mul(float4(i.pos,1.0), uMVP);
    o.nrm = normalize(mul(float4(i.nrm,0.0), uModel).xyz);
    o.tan = normalize(mul(float4(i.tan,0.0), uModel).xyz);
    o.bit = normalize(mul(float4(i.bit,0.0), uModel).xyz);
    o.uv  = i.uv;
    return o;
}

float4 PSMain(VSOut i):SV_TARGET{
    float4 tex = uHasTex > 0.5 ? gDiffuse.Sample(gSamp, i.uv) : float4(0.75,0.75,0.78,1.0);

    // --- effect materials: grayscale mask used as glow/alpha, tinted ---
    if (uIsEffect > 0.5){
        float lum = dot(tex.rgb, float3(0.299,0.587,0.114));
        float a = saturate(max(lum, tex.a)) * uTint.a;
        float3 col = tex.rgb * uTint.rgb;          // tint the emissive color
        col = pow(saturate(col), 1.0/2.2);
        return float4(col, a);                      // alpha-blended, unlit
    }

    // --- surface materials: normal-mapped lambert + hemispheric ambient ---
    float3 N = normalize(i.nrm);
    if (uHasNormal > 0.5){
        float2 nxy = gNormal.Sample(gSamp, i.uv).xy * 2.0 - 1.0;   // 3DCX stores X,Y
        float  nz  = sqrt(saturate(1.0 - dot(nxy,nxy)));
        float3x3 TBN = float3x3(normalize(i.tan), normalize(i.bit), N);
        N = normalize(mul(float3(nxy, nz), TBN));
    }
    float ndl = saturate(dot(N, -uLightDir));
    float3 albedo = tex.rgb * uTint.rgb;
    float3 amb = lerp(float3(0.18,0.20,0.24), float3(0.42,0.42,0.40), N.y*0.5+0.5);
    float3 col = albedo * (amb + ndl*0.9);
    col = pow(saturate(col), 1.0/2.2);
    return float4(col, 1.0);
}
)";

// ---------------------------------------------------------------------------
//  globals
// ---------------------------------------------------------------------------
struct GVertex
{
    float px, py, pz, nx, ny, nz, tx, ty, tz, bx, by, bz, u, v;
};
struct CB
{
    Mat4 mvp;
    Mat4 model;
    float lx, ly, lz, hasTex;         // light dir + diffuse present
    float tintR, tintG, tintB, tintA; // material tint (float4 constant)
    float hasNormal, isEffect, pad0, pad1;
};

static ID3D11Device *g_dev = nullptr;
static ID3D11DeviceContext *g_ctx = nullptr;
static IDXGISwapChain *g_swap = nullptr;
static ID3D11RenderTargetView *g_rtv = nullptr;
static ID3D11DepthStencilView *g_dsv = nullptr;
static ID3D11Buffer *g_vb = nullptr;
static ID3D11Buffer *g_ib = nullptr;
static ID3D11Buffer *g_cb = nullptr;
static ID3D11VertexShader *g_vs = nullptr;
static ID3D11PixelShader *g_ps = nullptr;
static ID3D11InputLayout *g_il = nullptr;
static ID3D11SamplerState *g_samp = nullptr;
static ID3D11BlendState *g_blendAlpha = nullptr;
static ID3D11BlendState *g_blendOpaque = nullptr;

// Per-material GPU resources + per-mesh draw ranges, for multi-material models.
struct MaterialGPU
{
    ID3D11ShaderResourceView *srv = nullptr;       // diffuse
    ID3D11ShaderResourceView *srvNormal = nullptr; // normal map
    float tint[4] = {1, 1, 1, 1};
    bool isEffect = false;
    bool hasDiffuse = false;
};
struct SubMesh
{
    UINT indexStart = 0, indexCount = 0;
    uint32_t matIndex = 0;
};
static std::vector<MaterialGPU> g_mats; // indexed by material.index
static std::vector<SubMesh> g_subs;
static ID3D11RasterizerState *g_rs = nullptr;
static ID3D11DepthStencilState *g_dss = nullptr;
static ID3D11DepthStencilState *g_dssNoWrite = nullptr;
static UINT g_indexCount = 0;
static Vec3 g_center{0, 0, 0};
static float g_radius = 100.0f;
static int g_w = 1280, g_h = 800;
static float g_yaw = 0.6f, g_pitch = 0.35f, g_dist = 1.7f;
static bool g_dragging = false;
static POINT g_lastMouse{};

static void die(const char *msg)
{
    MessageBoxA(nullptr, msg, "gw2viewer", MB_ICONERROR);
    std::exit(1);
}

// ---------------------------------------------------------------------------
//  model + texture loading
// ---------------------------------------------------------------------------
static std::vector<uint8_t> decompress_modl(const std::string &dat, const char *selKind, uint32_t sel)
{
    Gw2Dat d;
    load_dat_file(d, dat);
    uint32_t idx = sel;
    if (std::strcmp(selKind, "file") == 0)
    {
        uint32_t b = get_by_base_id(d, sel);
        if (!b)
            die("file-id not found file id" + sel);
        idx = b - 1;
    }
    if (idx >= d.mft_data_list.size())
        die("index out of range");
    const MftData &e = d.mft_data_list[idx];
    std::vector<uint8_t> raw = read_entry_bytes(d.file_info.file_path, e);
    std::vector<uint8_t> stripped = gw2cmp::strip_crc32(std::span<const uint8_t>(raw));
    if (e.compression_flag == 0)
        return stripped;
    uint32_t usz = stripped[4] | (stripped[5] << 8) | (stripped[6] << 16) | ((uint32_t)stripped[7] << 24);
    return gw2cmp::decompress_method0(std::span<const uint8_t>(stripped).subspan(8), usz);
}

// decode a texture fileId -> RGBA; returns false if not a decodable ATEX
static bool load_texture_rgba(const std::string &dat, uint32_t fileId, std::vector<uint8_t> &rgba, int &w, int &h, std::string &fmt)
{
    try
    {
        std::vector<uint8_t> bytes = decompress_modl(dat, "file", fileId);
        if (bytes.size() >= 4 && bytes[0] == 'C')
            bytes[0] = 'A'; // CTEX->ATEX alias
        gw2atex::Texture t = gw2atex::parse(bytes.data(), bytes.size());
        gw2atex::Image im = gw2atex::decode(t, 0);
        if (im.width <= 0 || im.height <= 0 || im.rgba.empty())
            return false;
        rgba = std::move(im.rgba);
        w = im.width;
        h = im.height;
        fmt = t.fmt_name;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

static void make_srv(std::vector<uint8_t> &px, int w, int h, ID3D11ShaderResourceView **out)
{
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA tsd{};
    tsd.pSysMem = px.data();
    tsd.SysMemPitch = (UINT)w * 4;
    ID3D11Texture2D *tex = nullptr;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &td, &tsd, &tex)))
    {
        ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)tex, nullptr, out);
        ID3D11Texture2D_Release(tex);
    }
}
// An effect/mask texture is grayscale AND has large near-black regions (used as
// a dissolve/alpha map). A plain grayscale *surface* (stone, parchment, metal)
// is mostly mid-tone with few black pixels -> not an effect.
static bool looks_like_effect(const std::vector<uint8_t> &px)
{
    long n = 0, colored = 0, dark = 0;
    for (size_t i = 0; i + 3 < px.size(); i += 4 * 97)
    {
        int r = px[i], g = px[i + 1], b = px[i + 2];
        int mx = std::max({r, g, b}), mn = std::min({r, g, b});
        int lum = (r * 77 + g * 150 + b * 29) >> 8;
        if (mx - mn > 18)
            colored++;
        if (lum < 32)
            dark++;
        n++;
    }
    if (n == 0)
        return false;
    bool grayscale = colored < n / 15; // < ~7% of samples carry chroma
    bool masky = dark > n / 8;         // > 12.5% near-black (transparent regions)
    return grayscale && masky;
}

// Load one material's textures into a GPU resource set (diffuse + normal,
// role by format per GW2_Vertex_FVF_Notes sec.6.3; grayscale diffuse -> effect).
static MaterialGPU load_material(const gw2model::Material &mat, const std::string &dat)
{
    MaterialGPU g;
    std::vector<uint8_t> diff, nrm;
    int dw = 0, dh = 0, nw = 0, nh = 0;
    long bestArea = -1;
    for (uint32_t fid : mat.textureFileIds())
    {
        std::string f;
        std::vector<uint8_t> tmp;
        int a = 0, b = 0;
        if (!load_texture_rgba(dat, fid, tmp, a, b, f))
            continue;
        bool isNormal = f.find("3DC") != std::string::npos || f.find("BC5") != std::string::npos || f.find("ATI2") != std::string::npos;
        if (isNormal)
        {
            if ((long)a * b > (long)nw * nh)
            {
                nrm = std::move(tmp);
                nw = a;
                nh = b;
            }
        }
        else
        {
            long area = (long)a * b;
            if (area > bestArea)
            {
                bestArea = area;
                diff = std::move(tmp);
                dw = a;
                dh = b;
            }
        }
    }
    g.hasDiffuse = !diff.empty();
    g.isEffect = g.hasDiffuse && looks_like_effect(diff);
    if (g.hasDiffuse)
        make_srv(diff, dw, dh, &g.srv);
    if (!nrm.empty() && !g.isEffect)
        make_srv(nrm, nw, nh, &g.srvNormal);
    // tint only when the material carries a single plausible color constant
    if (mat.constants.size() == 1)
    {
        const float *v = mat.constants[0].value;
        if (v[0] >= 0 && v[0] <= 1 && v[1] >= 0 && v[1] <= 1 && v[2] >= 0 && v[2] <= 1)
            for (int k = 0; k < 4; k++)
                g.tint[k] = v[k];
    }
    return g;
}

static void build_mesh(const gw2model::Model &model, const std::string &dat)
{
    std::vector<GVertex> verts;
    std::vector<uint32_t> indices;
    Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    // one combined VB/IB, one SubMesh (draw range) per model mesh
    for (const auto &m : model.meshes)
    {
        uint32_t voff = (uint32_t)verts.size();
        SubMesh s;
        s.indexStart = (UINT)indices.size();
        s.matIndex = m.materialIndex;
        for (const auto &v : m.vertices)
        {
            verts.push_back({v.px, v.py, v.pz, v.nx, v.ny, v.nz, v.tx, v.ty, v.tz, v.bx, v.by, v.bz, v.u, v.v});
            lo = {std::min(lo.x, v.px), std::min(lo.y, v.py), std::min(lo.z, v.pz)};
            hi = {std::max(hi.x, v.px), std::max(hi.y, v.py), std::max(hi.z, v.pz)};
        }
        for (uint32_t i : m.indices)
            indices.push_back(voff + i);
        s.indexCount = (UINT)indices.size() - s.indexStart;
        if (s.indexCount)
            g_subs.push_back(s);
    }
    if (verts.empty())
        die("model has no vertices");
    g_center = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
    Vec3 ext = sub(hi, lo);
    g_radius = 0.5f * std::sqrt(dot(ext, ext));
    if (g_radius < 1e-3f)
        g_radius = 1.0f;

    D3D11_BUFFER_DESC bd{};
    D3D11_SUBRESOURCE_DATA sd{};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.ByteWidth = (UINT)(verts.size() * sizeof(GVertex));
    sd.pSysMem = verts.data();
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, &sd, &g_vb)))
        die("CreateBuffer VB");
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.ByteWidth = (UINT)(indices.size() * sizeof(uint32_t));
    sd.pSysMem = indices.data();
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, &sd, &g_ib)))
        die("CreateBuffer IB");

    // load every material (indexed by material.index)
    uint32_t maxIdx = 0;
    for (const auto &m : model.materials)
        maxIdx = std::max(maxIdx, m.index);
    g_mats.resize(maxIdx + 1);
    for (const auto &m : model.materials)
        g_mats[m.index] = load_material(m, dat);

    int effects = 0;
    for (auto &m : g_mats)
        if (m.isEffect)
            effects++;
    char title[256];
    std::snprintf(title, sizeof title,
                  "gw2viewer | %u verts %u tris | %zu meshes %zu materials (%d effect)",
                  (unsigned)verts.size(), (UINT)(indices.size() / 3), g_subs.size(), model.materials.size(), effects);
    SetWindowTextA(FindWindowA("gw2viewerwnd", nullptr), title);
}

// ---------------------------------------------------------------------------
//  D3D setup
// ---------------------------------------------------------------------------
static void create_targets()
{
    ID3D11Texture2D *back = nullptr;
    IDXGISwapChain_GetBuffer(g_swap, 0, IID_ID3D11Texture2D, (void **)&back);
    ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)back, nullptr, &g_rtv);
    ID3D11Texture2D_Release(back);
    D3D11_TEXTURE2D_DESC dd{};
    dd.Width = g_w;
    dd.Height = g_h;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D *ds = nullptr;
    ID3D11Device_CreateTexture2D(g_dev, &dd, nullptr, &ds);
    ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)ds, nullptr, &g_dsv);
    ID3D11Texture2D_Release(ds);
}

static void init_pipeline()
{
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kHLSL, std::strlen(kHLSL), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsb, &err)))
        die(err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "VS compile");
    if (FAILED(D3DCompile(kHLSL, std::strlen(kHLSL), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psb, &err)))
        die(err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "PS compile");
    ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), nullptr, &g_vs);
    ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), nullptr, &g_ps);
    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BITANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11Device_CreateInputLayout(g_dev, il, 5, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &g_il);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);

    D3D11_BUFFER_DESC cbd{};
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cbd.ByteWidth = (sizeof(CB) + 15) & ~15;
    ID3D11Device_CreateBuffer(g_dev, &cbd, nullptr, &g_cb);

    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(g_dev, &smp, &g_samp);

    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    ID3D11Device_CreateRasterizerState(g_dev, &rs, &g_rs);
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS;
    ID3D11Device_CreateDepthStencilState(g_dev, &ds, &g_dss);
    D3D11_DEPTH_STENCIL_DESC dsn = ds;
    dsn.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; // test but don't write (transparent)
    ID3D11Device_CreateDepthStencilState(g_dev, &dsn, &g_dssNoWrite);

    // opaque (default) + alpha blend (for effect materials)
    D3D11_BLEND_DESC bo{};
    bo.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(g_dev, &bo, &g_blendOpaque);
    D3D11_BLEND_DESC ba{};
    auto &rt = ba.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(g_dev, &ba, &g_blendAlpha);
}

static void render()
{
    g_dist = std::max(g_dist, 0.2f);
    Vec3 eye{g_center.x + g_radius * g_dist * std::cos(g_pitch) * std::cos(g_yaw),
             g_center.y + g_radius * g_dist * std::sin(g_pitch),
             g_center.z + g_radius * g_dist * std::cos(g_pitch) * std::sin(g_yaw)};
    Mat4 view = lookAt(eye, g_center, {0, 1, 0});
    Mat4 proj = perspective(0.9f, (float)g_w / g_h, g_radius * 0.02f, g_radius * 40.0f);
    Mat4 model = identity();
    Mat4 mvp = mul(model, mul(view, proj));
    Vec3 L = norm({-0.4f, -0.8f, -0.4f});

    float clear[4] = {0.10f, 0.11f, 0.13f, 1.0f};
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_rtv, clear);
    ID3D11DeviceContext_ClearDepthStencilView(g_ctx, g_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, g_dsv);
    D3D11_VIEWPORT vp{0, 0, (float)g_w, (float)g_h, 0, 1};
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(g_ctx, g_rs);

    UINT stride = sizeof(GVertex), off = 0;
    ID3D11DeviceContext_IASetInputLayout(g_ctx, g_il);
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &g_vb, &stride, &off);
    ID3D11DeviceContext_IASetIndexBuffer(g_ctx, g_ib, DXGI_FORMAT_R32_UINT, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, nullptr, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_ps, nullptr, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 1, &g_cb);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_cb);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &g_samp);
    float bf[4] = {0, 0, 0, 0};

    // draw opaque submeshes first, then effect (blended) ones
    for (int pass = 0; pass < 2; ++pass)
    {
        bool effectPass = (pass == 1);
        ID3D11DeviceContext_OMSetBlendState(g_ctx, effectPass ? g_blendAlpha : g_blendOpaque, bf, 0xffffffff);
        ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, effectPass ? g_dssNoWrite : g_dss, 0);
        for (const auto &s : g_subs)
        {
            const MaterialGPU &m = (s.matIndex < g_mats.size()) ? g_mats[s.matIndex] : MaterialGPU{};
            if (m.isEffect != effectPass)
                continue;
            CB cb{};
            cb.model = model;
            cb.mvp = mvp;
            cb.lx = L.x;
            cb.ly = L.y;
            cb.lz = L.z;
            cb.hasTex = m.srv ? 1.0f : 0.0f;
            cb.tintR = m.tint[0];
            cb.tintG = m.tint[1];
            cb.tintB = m.tint[2];
            cb.tintA = m.tint[3];
            cb.hasNormal = m.srvNormal ? 1.0f : 0.0f;
            cb.isEffect = m.isEffect ? 1.0f : 0.0f;
            D3D11_MAPPED_SUBRESOURCE ms;
            ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
            std::memcpy(ms.pData, &cb, sizeof cb);
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_cb, 0);
            ID3D11ShaderResourceView *srvs[2] = {m.srv, m.srvNormal};
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 2, srvs);
            ID3D11DeviceContext_DrawIndexed(g_ctx, s.indexCount, s.indexStart, 0);
        }
    }
    IDXGISwapChain_Present(g_swap, 1, 0);
}

// Copy the current back buffer to a CPU-readable staging texture and write PNG.
static void save_screenshot(const char *path)
{
    ID3D11Texture2D *back = nullptr;
    if (FAILED(IDXGISwapChain_GetBuffer(g_swap, 0, IID_ID3D11Texture2D, (void **)&back)))
        return;
    D3D11_TEXTURE2D_DESC td{};
    ID3D11Texture2D_GetDesc(back, &td);
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D *stage = nullptr;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &sd, nullptr, &stage)))
    {
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)stage, (ID3D11Resource *)back);
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)stage, 0, D3D11_MAP_READ, 0, &ms)))
        {
            std::vector<uint8_t> px((size_t)td.Width * td.Height * 4);
            for (UINT y = 0; y < td.Height; y++)
                std::memcpy(px.data() + (size_t)y * td.Width * 4, (uint8_t *)ms.pData + (size_t)y * ms.RowPitch, (size_t)td.Width * 4);
            stbi_write_png(path, td.Width, td.Height, 4, px.data(), td.Width * 4);
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stage, 0);
        }
        ID3D11Texture2D_Release(stage);
    }
    ID3D11Texture2D_Release(back);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_LBUTTONDOWN:
        g_dragging = true;
        g_lastMouse = {(short)LOWORD(lp), (short)HIWORD(lp)};
        SetCapture(h);
        return 0;
    case WM_LBUTTONUP:
        g_dragging = false;
        ReleaseCapture();
        return 0;
    case WM_MOUSEMOVE:
        if (g_dragging)
        {
            int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
            g_yaw += (mx - g_lastMouse.x) * 0.01f;
            g_pitch += (my - g_lastMouse.y) * 0.01f;
            if (g_pitch > 1.5f)
                g_pitch = 1.5f;
            if (g_pitch < -1.5f)
                g_pitch = -1.5f;
            g_lastMouse = {mx, my};
        }
        return 0;
    case WM_MOUSEWHEEL:
        g_dist *= (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 0.9f : 1.1f);
        return 0;
    case WM_SIZE:
        if (g_swap && wp != SIZE_MINIMIZED)
        {
            g_w = LOWORD(lp);
            g_h = HIWORD(lp);
            if (g_rtv)
            {
                ID3D11RenderTargetView_Release(g_rtv);
                g_rtv = nullptr;
            }
            if (g_dsv)
            {
                ID3D11DepthStencilView_Release(g_dsv);
                g_dsv = nullptr;
            }
            IDXGISwapChain_ResizeBuffers(g_swap, 0, g_w, g_h, DXGI_FORMAT_UNKNOWN, 0);
            create_targets();
        }
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

static std::string argval(int argc, char **argv, const char *key, const char *def = "")
{
    for (int i = 1; i < argc - 1; i++)
        if (std::strcmp(argv[i], key) == 0)
            return argv[i + 1];
    return def;
}

int main(int argc, char **argv)
{
    std::string dat = argval(argc, argv, "--dat");
    std::string tpl = argval(argc, argv, "--template");
    if (dat.empty() || tpl.empty())
    {
        std::puts("usage: gw2viewer --dat <Gw2.dat> --template <json> (--base-id N | --file-id N | --index N)");
        return 1;
    }
    const char *selKind = "base";
    uint32_t sel = 0;
    if (!argval(argc, argv, "--file-id").empty())
    {
        selKind = "file";
        sel = (uint32_t)std::stoul(argval(argc, argv, "--file-id"));
    }
    else if (!argval(argc, argv, "--index").empty())
    {
        selKind = "base";
        sel = (uint32_t)std::stoul(argval(argc, argv, "--index"));
    }
    else if (!argval(argc, argv, "--base-id").empty())
    {
        selKind = "base";
        sel = (uint32_t)std::stoul(argval(argc, argv, "--base-id"));
    }
    else
    {
        std::puts("need --base-id / --file-id / --index");
        return 1;
    }

    nlohmann::json tj;
    {
        std::ifstream in(tpl, std::ios::binary);
        if (!in)
            die("cannot open template");
        in >> tj;
    }
    std::vector<uint8_t> modl = decompress_modl(dat, selKind, sel);
    gw2model::Model model = gw2model::Extractor(modl, tj).extract();

    // window
    WNDCLASSA wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "gw2viewerwnd";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    RECT rc{0, 0, g_w, g_h};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("gw2viewerwnd", "gw2viewer", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    DXGI_SWAP_CHAIN_DESC sc{};
    sc.BufferCount = 2;
    sc.BufferDesc.Width = g_w;
    sc.BufferDesc.Height = g_h;
    sc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.OutputWindow = hwnd;
    sc.SampleDesc.Count = 1;
    sc.Windowed = TRUE;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
                                             D3D11_SDK_VERSION, &sc, &g_swap, &g_dev, nullptr, &g_ctx)))
    {
        // retry with BITBLT swap effect (older setups)
        sc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        sc.BufferCount = 1;
        if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
                                                 D3D11_SDK_VERSION, &sc, &g_swap, &g_dev, nullptr, &g_ctx)))
            die("D3D11CreateDeviceAndSwapChain");
    }
    create_targets();
    init_pipeline();
    build_mesh(model, dat);

    // Headless one-frame capture mode.
    std::string shot = argval(argc, argv, "--shot");
    if (!shot.empty())
    {
        for (int i = 0; i < 2; i++)
            render(); // warm up + draw a frame
        save_screenshot(shot.c_str());
        std::printf("wrote %s\n", shot.c_str());
        return 0;
    }

    MSG m{};
    while (m.message != WM_QUIT)
    {
        if (PeekMessage(&m, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
        else
        {
            g_yaw += 0.003f;
            render();
        }
    }
    return 0;
}
