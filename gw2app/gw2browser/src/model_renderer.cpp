#include "model_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

namespace gw2m3d {

namespace {

using Microsoft::WRL::ComPtr;

// ---- tiny row-major math (matches gw2viewer.cpp) --------------------------
struct Vec3 {
    float x, y, z;
};
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 norm(Vec3 a) {
    float l = std::sqrt(dot(a, a));
    return l > 0 ? Vec3{a.x / l, a.y / l, a.z / l} : a;
}
struct Mat4 {
    float m[16];
};
Mat4 identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
    return r;
}
Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}
Mat4 perspective(float fovy, float aspect, float zn, float zf) {
    float f = 1.0f / std::tan(fovy * 0.5f);
    Mat4 r{};
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = zf / (zf - zn);
    r.m[11] = 1.0f;
    r.m[14] = -zn * zf / (zf - zn);
    return r;
}
Mat4 lookAt(Vec3 eye, Vec3 at, Vec3 up) {
    Vec3 z = norm(sub(at, eye));
    Vec3 x = norm(cross(up, z));
    Vec3 y = cross(z, x);
    Mat4 r = identity();
    r.m[0] = x.x; r.m[1] = y.x; r.m[2] = z.x;
    r.m[4] = x.y; r.m[5] = y.y; r.m[6] = z.y;
    r.m[8] = x.z; r.m[9] = y.z; r.m[10] = z.z;
    r.m[12] = -dot(x, eye); r.m[13] = -dot(y, eye); r.m[14] = -dot(z, eye);
    return r;
}
// Row-major (row-vector) translate/rotate helpers for the free-trackball model
// rotation -- translation lives in the last row (m[12..14]), matching lookAt.
Mat4 translate(Vec3 t) {
    Mat4 r = identity();
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}
Mat4 rotY(float a) {
    Mat4 r = identity();
    float c = std::cos(a), s = std::sin(a);
    r.m[0] = c; r.m[2] = -s; r.m[8] = s; r.m[10] = c;
    return r;
}
Mat4 rotX(float a) {
    Mat4 r = identity();
    float c = std::cos(a), s = std::sin(a);
    r.m[5] = c; r.m[6] = s; r.m[9] = -s; r.m[10] = c;
    return r;
}

// Same HLSL as gw2viewer.cpp (albedo * lambert + hemispheric ambient, optional
// 3DCX normal mapping, effect-material emissive path).
const char* kHLSL = R"(
cbuffer CB : register(b0) {
    row_major float4x4 uMVP;
    row_major float4x4 uModel;
    float3 uLightDir;  float uHasTex;
    float4 uTint;
    float  uHasNormal; float uIsEffect; float uHasSkin; float uMatKind; // matKind: 0 normal, 1 terrain, 2 water
};
// Bone matrix palette as an unbounded StructuredBuffer (4 float4 rows per bone),
// so rigs of ANY size work (no fixed cap). Rows are row-major SkinMat rows, so
// float4x4(r0,r1,r2,r3) rebuilds the row-major matrix and mul(rowvec, M) is exact.
StructuredBuffer<float4> gBoneRows : register(t2);
float4x4 boneMat(uint i){ uint b=i*4u; return float4x4(gBoneRows[b], gBoneRows[b+1u], gBoneRows[b+2u], gBoneRows[b+3u]); }
Texture2D    gDiffuse : register(t0);
Texture2D    gNormal  : register(t1);
SamplerState gSamp    : register(s0);

struct VSIn  { float3 pos:POSITION; float3 nrm:NORMAL; float3 tan:TANGENT; float3 bit:BITANGENT; float2 uv:TEXCOORD0;
               uint4 bidx:BLENDINDICES; float4 bwt:BLENDWEIGHT; };
struct VSOut { float4 pos:SV_POSITION; float3 nrm:NORMAL; float3 tan:TANGENT; float3 bit:BITANGENT; float2 uv:TEXCOORD0;
               float up:TEXCOORD1; float height:TEXCOORD2; };

VSOut VSMain(VSIn i){
    VSOut o;
    float3 p = i.pos, nn = i.nrm, tt = i.tan, bb = i.bit;
    if (uHasSkin > 0.5){
        // Linear-blend skinning: blend the per-bone skin matrices by weight, then
        // transform position + tangent frame into the animated (model) space.
        float4x4 sm = i.bwt.x*boneMat(i.bidx.x) + i.bwt.y*boneMat(i.bidx.y)
                    + i.bwt.z*boneMat(i.bidx.z) + i.bwt.w*boneMat(i.bidx.w);
        p  = mul(float4(i.pos,1.0), sm).xyz;
        nn = mul(float4(i.nrm,0.0), sm).xyz;
        tt = mul(float4(i.tan,0.0), sm).xyz;
        bb = mul(float4(i.bit,0.0), sm).xyz;
    }
    o.pos = mul(float4(p,1.0), uMVP);
    o.nrm = normalize(mul(float4(nn,0.0), uModel).xyz);
    o.tan = normalize(mul(float4(tt,0.0), uModel).xyz);
    o.bit = normalize(mul(float4(bb,0.0), uModel).xyz);
    o.uv  = i.uv;
    o.up  = i.nrm.z;   // geometric up-ness (Z-up terrain), before rotation -- for slope color
    o.height = p.z;    // world height (Z), for depth-tinting grass
    return o;
}

float4 PSMain(VSOut i):SV_TARGET{
    float3 N = normalize(i.nrm);
    float ndl = saturate(dot(N, -uLightDir));
    float3 amb = lerp(float3(0.18,0.20,0.24), float3(0.42,0.42,0.40), N.y*0.5+0.5);

    // Terrain: grass on flat ground, rock on steep slopes (slope from geometric
    // up-ness), with a little height variation. No real splat textures yet.
    if (uMatKind > 0.5 && uMatKind < 1.5){
        float slope = saturate(1.0 - i.up);                 // 0 flat .. 1 vertical
        float3 grass = float3(0.24, 0.42, 0.17);
        float3 grassHi = float3(0.34, 0.52, 0.24);
        float3 rock  = float3(0.34, 0.30, 0.26);
        float3 g = lerp(grass, grassHi, saturate(i.height*0.002 + 0.5));
        float3 albedo = lerp(g, rock, smoothstep(0.28, 0.62, slope));
        float3 col = albedo * (amb + ndl*0.95);
        return float4(pow(saturate(col), 1.0/2.2), 1.0);
    }
    // Water: translucent blue sheet (pools where terrain sits below it).
    if (uMatKind > 1.5 && uMatKind < 2.5){
        float3 water = float3(0.04, 0.22, 0.38);
        float fres = pow(1.0 - saturate(N.y), 4.0);          // subtle rim brighten
        float3 col = water * (0.7 + ndl*0.4) + float3(0.05,0.10,0.14)*fres;
        return float4(pow(saturate(col), 1.0/2.2), 0.78);
    }
    // Collision: translucent orange overlay (physics geometry), lit for shape.
    if (uMatKind > 2.5){
        float3 c = float3(1.00, 0.55, 0.08) * (0.45 + ndl*0.55);
        return float4(pow(saturate(c), 1.0/2.2), 0.55);
    }

    float4 tex = uHasTex > 0.5 ? gDiffuse.Sample(gSamp, i.uv) : float4(0.75,0.75,0.78,1.0);
    if (uIsEffect > 0.5){
        float lum = dot(tex.rgb, float3(0.299,0.587,0.114));
        float a = saturate(max(lum, tex.a)) * uTint.a;
        float3 col = tex.rgb * uTint.rgb;
        col = pow(saturate(col), 1.0/2.2);
        return float4(col, a);
    }
    if (uHasNormal > 0.5){
        float2 nxy = gNormal.Sample(gSamp, i.uv).xy * 2.0 - 1.0;
        float  nz  = sqrt(saturate(1.0 - dot(nxy,nxy)));
        float3x3 TBN = float3x3(normalize(i.tan), normalize(i.bit), N);
        N = normalize(mul(float3(nxy, nz), TBN));
        ndl = saturate(dot(N, -uLightDir));
    }
    float3 albedo = tex.rgb * uTint.rgb;
    float3 col = albedo * (amb + ndl*0.9);
    col = pow(saturate(col), 1.0/2.2);
    return float4(col, 1.0);
}
)";

struct CB {
    Mat4 mvp;
    Mat4 model;
    float lx, ly, lz, hasTex;
    float tintR, tintG, tintB, tintA;
    float hasNormal, isEffect, hasSkin, matKind;
};

// Position-only unlit shader for the skeleton overlay (bone lines + joint
// crosses). Reuses register(b0): only the leading uMVP of CB is read.
const char* kLineHLSL = R"(
cbuffer CB : register(b0) { row_major float4x4 uMVP; };
float4 VSLine(float3 pos : POSITION) : SV_POSITION { return mul(float4(pos, 1.0), uMVP); }
float4 PSLine() : SV_TARGET { return float4(0.20, 1.0, 0.35, 1.0); }
)";

struct LineVtx {
    float x, y, z;
};

// ---- pose math (matches native/main.cpp posemath, verified vs bind pose) ----
// Granny transform: v' = pos + R(quat) * (ScaleShear * v).
void quatToM3(const float q[4], float m[9]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float n = x*x + y*y + z*z + w*w; float s = n > 1e-12f ? 2.0f/n : 0.0f;
    float xs=x*s,ys=y*s,zs=z*s, wx=w*xs,wy=w*ys,wz=w*zs, xx=x*xs,xy=x*ys,xz=x*zs, yy=y*ys,yz=y*zs,zz=z*zs;
    m[0]=1-(yy+zz); m[1]=xy-wz;    m[2]=xz+wy;
    m[3]=xy+wz;     m[4]=1-(xx+zz);m[5]=yz-wx;
    m[6]=xz-wy;     m[7]=yz+wx;    m[8]=1-(xx+yy);
}
void m3mul(const float a[9], const float b[9], float o[9]) {
    for (int r=0;r<3;++r) for (int c=0;c<3;++c)
        o[r*3+c]=a[r*3+0]*b[0*3+c]+a[r*3+1]*b[1*3+c]+a[r*3+2]*b[2*3+c];
}
void m3vec(const float m[9], const float v[3], float o[3]) {
    for (int r=0;r<3;++r) o[r]=m[r*3+0]*v[0]+m[r*3+1]*v[1]+m[r*3+2]*v[2];
}
// Hamilton quaternion product (x,y,z,w).
void quatMul(const float a[4], const float b[4], float o[4]) {
    o[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    o[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    o[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    o[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
}

struct MaterialGPU {
    ComPtr<ID3D11ShaderResourceView> srv;       // diffuse
    ComPtr<ID3D11ShaderResourceView> srvNormal; // normal map
    float tint[4] = {1, 1, 1, 1};
    bool isEffect = false;
    int kind = 0; // 0 normal, 1 terrain, 2 water
};
struct SubMesh {
    UINT indexStart = 0, indexCount = 0;
    uint32_t matIndex = 0;
    bool hasSkin = false;
    float center[3] = {0, 0, 0}; // submesh centroid (model space), for game-shader transparent sort
};

// ---- game-shader (real bgfx DXBC) GPU material ----------------------------
// Per material, built from ModelPreview::gameMaterials at set_model: the game's
// own VS+PS created straight from the DXBC blobs, an input layout from the VS
// reflection, DYNAMIC cbuffers refilled from a shared uniform map each frame,
// and one SRV per PS sampler slot. Mirrors gw2gsviewer.cpp::MatGPU.
struct GameMatGPU {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vcb, pcb;
    ComPtr<ID3D11ShaderResourceView> srv[16];
    std::vector<GameShaderUniform> vsU, psU;
    uint32_t vsCB = 0, psCB = 0;
    uint64_t renderState = 0;
    ComPtr<ID3D11BlendState> blend; // non-null => translucent (drawn in pass 2)
    bool depthWrite = true;
    bool ok = false;
};

HWND g_hwnd = nullptr;
ComPtr<ID3D11Device> g_dev;
ComPtr<ID3D11DeviceContext> g_ctx;
ComPtr<IDXGISwapChain> g_swap;
ComPtr<ID3D11RenderTargetView> g_rtv;
ComPtr<ID3D11DepthStencilView> g_dsv;
ComPtr<ID3D11VertexShader> g_vs;
ComPtr<ID3D11PixelShader> g_ps;
ComPtr<ID3D11InputLayout> g_il;
ComPtr<ID3D11Buffer> g_cb;
ComPtr<ID3D11SamplerState> g_samp;
ComPtr<ID3D11RasterizerState> g_rsSolid;
ComPtr<ID3D11RasterizerState> g_rsWire;
ComPtr<ID3D11DepthStencilState> g_dss;
ComPtr<ID3D11DepthStencilState> g_dssNoWrite;
ComPtr<ID3D11BlendState> g_blendOpaque;
ComPtr<ID3D11BlendState> g_blendAlpha;

ComPtr<ID3D11Buffer> g_vb;
ComPtr<ID3D11Buffer> g_ib;
// GW2's material (game) vertex shaders take ALREADY-skinned vertices (they have
// no blend inputs / bone uniform -- the engine skins in a prior pass). So for the
// game-shader path we CPU-skin into this DYNAMIC buffer each posed frame. g_cpu_verts
// is the rest-pose source (kept only for skinned models).
std::vector<GVertex> g_cpu_verts;
ComPtr<ID3D11Buffer> g_vb_skinned;
LARGE_INTEGER g_qpc_start{}; // wall clock origin for the game shaders' Time uniform
std::vector<MaterialGPU> g_mats;
std::vector<SubMesh> g_subs;
bool g_has_model = false;

// Game-shader (real DXBC) materials, indexed by material.index, + a shared
// uniform environment (camera refilled each frame, lighting rig static).
// Reuses g_samp (wrap linear), g_dss (depth write on) and g_dssNoWrite
// (transparent, depth read-only) from the reconstruction pipeline.
std::vector<GameMatGPU> g_game_mats;
std::map<std::string, std::vector<float>> g_game_vals;
bool g_game_any_ok = false;      // at least one material built with game shaders
bool g_game_uniforms_ready = false;

// Bone matrix palette as a DYNAMIC StructuredBuffer (VS register t2), sized to
// the model's bone count -- rigs of any size, no fixed cap.
ComPtr<ID3D11Buffer> g_bonePalette;
ComPtr<ID3D11ShaderResourceView> g_bonePaletteSRV;
UINT g_bone_cap = 0;      // capacity (bones) of the current palette buffer
bool g_skin_ok = false;   // model has an inline rig (any size)

// Skeleton overlay resources.
ComPtr<ID3D11VertexShader> g_lineVs;
ComPtr<ID3D11PixelShader> g_linePs;
ComPtr<ID3D11InputLayout> g_lineIl;
ComPtr<ID3D11DepthStencilState> g_dssNoDepth; // draw skeleton over the mesh
ComPtr<ID3D11Buffer> g_skelVb;
UINT g_skel_vert_count = 0;
UINT g_skel_vert_cap = 0;      // capacity of the dynamic line buffer
bool g_has_skeleton = false;
bool g_show_skeleton = false;

// Skeleton + decoded animation clips (owned copies, for runtime posing).
std::vector<ModelJoint> g_joints;
std::vector<granny::Anim> g_clips;
int g_clip_index = -1;         // -1 = bind pose
float g_anim_time = 0.0f;
bool g_anim_playing = false;
LARGE_INTEGER g_qpc_freq{}, g_qpc_last{};

// --- map scene (multiple models placed at world transforms) ---
struct SceneModelGPU {
    ComPtr<ID3D11Buffer> vb, ib;
    std::vector<MaterialGPU> mats;
    std::vector<SubMesh> subs;
    bool ok = false;
};
struct SceneInstGPU {
    int model = 0;
    Mat4 world; // model->world (row-major, row-vector)
    int layer = 0;
};
std::vector<SceneModelGPU> g_scene_models;
std::vector<SceneInstGPU> g_scene_insts;
bool g_scene_mode = false;
Vec3 g_scene_center{0, 0, 0};
float g_scene_radius = 1.0f;
bool g_layer_visible[LAYER_COUNT] = {true, true, false, false}; // prop+terrain on; collision+zone off

int g_w = 1, g_h = 1;
Vec3 g_center{0, 0, 0};
float g_radius = 1.0f;
Mat4 g_rot = identity(); // accumulated free-trackball model rotation
float g_dist = 1.7f;     // camera distance in bounding-radius units (zoom)
RenderMode g_mode = RenderMode::Full;

void create_targets() {
    g_rtv.Reset();
    g_dsv.Reset();
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    g_dev->CreateRenderTargetView(back.Get(), nullptr, &g_rtv);

    D3D11_TEXTURE2D_DESC dd{};
    dd.Width = static_cast<UINT>(g_w);
    dd.Height = static_cast<UINT>(g_h);
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> ds;
    if (SUCCEEDED(g_dev->CreateTexture2D(&dd, nullptr, &ds))) {
        g_dev->CreateDepthStencilView(ds.Get(), nullptr, &g_dsv);
    }
}

bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sc{};
    sc.BufferCount = 2;
    sc.BufferDesc.Width = static_cast<UINT>(g_w);
    sc.BufferDesc.Height = static_cast<UINT>(g_h);
    sc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.OutputWindow = hwnd;
    sc.SampleDesc.Count = 1;
    sc.Windowed = TRUE;
    sc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, ARRAYSIZE(levels),
                                               D3D11_SDK_VERSION, &sc, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, ARRAYSIZE(levels),
                                           D3D11_SDK_VERSION, &sc, &g_swap, &g_dev, &got, &g_ctx);
    }
    return SUCCEEDED(hr);
}

bool init_pipeline() {
    ComPtr<ID3DBlob> vsb, psb, err;
    if (FAILED(D3DCompile(kHLSL, std::strlen(kHLSL), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsb, &err)))
        return false;
    if (FAILED(D3DCompile(kHLSL, std::strlen(kHLSL), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psb, &err)))
        return false;
    if (FAILED(g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs))) return false;
    if (FAILED(g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps))) return false;

    const D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BITANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 112, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 128, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(g_dev->CreateInputLayout(il, 7, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_il))) return false;

    D3D11_BUFFER_DESC cbd{};
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cbd.ByteWidth = (sizeof(CB) + 15) & ~15u;
    if (FAILED(g_dev->CreateBuffer(&cbd, nullptr, &g_cb))) return false;

    // Bone palette (StructuredBuffer) is created per-model in build_skeleton once
    // the bone count is known -- no fixed-size allocation here.

    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&smp, &g_samp);

    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    g_dev->CreateRasterizerState(&rs, &g_rsSolid);
    rs.FillMode = D3D11_FILL_WIREFRAME;
    g_dev->CreateRasterizerState(&rs, &g_rsWire);

    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS;
    g_dev->CreateDepthStencilState(&ds, &g_dss);
    D3D11_DEPTH_STENCIL_DESC dsn = ds;
    dsn.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    g_dev->CreateDepthStencilState(&dsn, &g_dssNoWrite);
    D3D11_DEPTH_STENCIL_DESC dsk{};
    dsk.DepthEnable = FALSE; // skeleton always visible, drawn over the mesh
    g_dev->CreateDepthStencilState(&dsk, &g_dssNoDepth);

    // Skeleton overlay pipeline (position-only line list).
    ComPtr<ID3DBlob> lvsb, lpsb, lerr;
    if (SUCCEEDED(D3DCompile(kLineHLSL, std::strlen(kLineHLSL), nullptr, nullptr, nullptr, "VSLine", "vs_5_0", 0, 0,
                             &lvsb, &lerr)) &&
        SUCCEEDED(D3DCompile(kLineHLSL, std::strlen(kLineHLSL), nullptr, nullptr, nullptr, "PSLine", "ps_5_0", 0, 0,
                             &lpsb, &lerr))) {
        g_dev->CreateVertexShader(lvsb->GetBufferPointer(), lvsb->GetBufferSize(), nullptr, &g_lineVs);
        g_dev->CreatePixelShader(lpsb->GetBufferPointer(), lpsb->GetBufferSize(), nullptr, &g_linePs);
        const D3D11_INPUT_ELEMENT_DESC lil[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        g_dev->CreateInputLayout(lil, 1, lvsb->GetBufferPointer(), lvsb->GetBufferSize(), &g_lineIl);
    }

    D3D11_BLEND_DESC bo{};
    bo.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_dev->CreateBlendState(&bo, &g_blendOpaque);
    D3D11_BLEND_DESC ba{};
    auto& rt = ba.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_dev->CreateBlendState(&ba, &g_blendAlpha);
    return true;
}

ComPtr<ID3D11ShaderResourceView> make_srv(const std::vector<uint8_t>& px, int w, int h) {
    ComPtr<ID3D11ShaderResourceView> out;
    if (px.empty() || w <= 0 || h <= 0) return out;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = px.data();
    sd.SysMemPitch = static_cast<UINT>(w) * 4;
    ComPtr<ID3D11Texture2D> tex;
    if (SUCCEEDED(g_dev->CreateTexture2D(&td, &sd, &tex))) {
        g_dev->CreateShaderResourceView(tex.Get(), nullptr, &out);
    }
    return out;
}

// ==== game-shader (real bgfx DXBC) helpers, ported from gw2gsviewer.cpp ======

// 1x1 constant-color SRV -- stand-in for a GLOBAL engine texture we don't have
// offline (e.g. the deferred light buffer). White = "fully lit".
ComPtr<ID3D11ShaderResourceView> make_solid(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    std::vector<uint8_t> px = {r, g, b, a};
    return make_srv(px, 1, 1);
}
// 1x1x6 constant-color CUBE SRV -- stand-in for the GLOBAL env cubemap (slot 13).
// Must be an actual cube (the PS declares texturecube); a mismatched 2D samples
// white -> mirror-white metals. Neutral grey gives believable reflections.
ComPtr<ID3D11ShaderResourceView> make_solid_cube(uint8_t lv) {
    uint8_t px[4] = {lv, lv, lv, 255};
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 1; td.Height = 1; td.MipLevels = 1; td.ArraySize = 6;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    D3D11_SUBRESOURCE_DATA sd[6];
    for (int i = 0; i < 6; i++) { sd[i].pSysMem = px; sd[i].SysMemPitch = 4; sd[i].SysMemSlicePitch = 4; }
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = td.Format; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE; vd.TextureCube.MipLevels = 1;
    ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> out;
    if (SUCCEEDED(g_dev->CreateTexture2D(&td, sd, &tex))) g_dev->CreateShaderResourceView(tex.Get(), &vd, &out);
    return out;
}

// bgfx 64-bit state blend nibble -> D3D11_BLEND (shifts 12/16/20/24).
// Exact bgfx blend-factor table (renderer_d3d11.cpp s_blendFactor). Column 0 is the
// RGB channel, column 1 the ALPHA channel -- D3D11 forbids *_COLOR factors on the
// alpha channel, so bgfx remaps them to the *_ALPHA equivalents there.
static const D3D11_BLEND kBgfxBlendFactor[14][2] = {
    { D3D11_BLEND(0),               D3D11_BLEND(0)               }, // 0 ignored
    { D3D11_BLEND_ZERO,             D3D11_BLEND_ZERO             }, // 1 ZERO
    { D3D11_BLEND_ONE,              D3D11_BLEND_ONE              }, // 2 ONE
    { D3D11_BLEND_SRC_COLOR,        D3D11_BLEND_SRC_ALPHA        }, // 3 SRC_COLOR
    { D3D11_BLEND_INV_SRC_COLOR,    D3D11_BLEND_INV_SRC_ALPHA    }, // 4 INV_SRC_COLOR
    { D3D11_BLEND_SRC_ALPHA,        D3D11_BLEND_SRC_ALPHA        }, // 5 SRC_ALPHA
    { D3D11_BLEND_INV_SRC_ALPHA,    D3D11_BLEND_INV_SRC_ALPHA    }, // 6 INV_SRC_ALPHA
    { D3D11_BLEND_DEST_ALPHA,       D3D11_BLEND_DEST_ALPHA       }, // 7 DST_ALPHA
    { D3D11_BLEND_INV_DEST_ALPHA,   D3D11_BLEND_INV_DEST_ALPHA   }, // 8 INV_DST_ALPHA
    { D3D11_BLEND_DEST_COLOR,       D3D11_BLEND_DEST_ALPHA       }, // 9 DST_COLOR
    { D3D11_BLEND_INV_DEST_COLOR,   D3D11_BLEND_INV_DEST_ALPHA   }, // a INV_DST_COLOR
    { D3D11_BLEND_SRC_ALPHA_SAT,    D3D11_BLEND_ONE              }, // b SRC_ALPHA_SAT
    { D3D11_BLEND_BLEND_FACTOR,     D3D11_BLEND_BLEND_FACTOR     }, // c FACTOR
    { D3D11_BLEND_INV_BLEND_FACTOR, D3D11_BLEND_INV_BLEND_FACTOR }, // d INV_FACTOR
};
// bgfx blend equation table (s_blendEquation).
static const D3D11_BLEND_OP kBgfxBlendEquation[5] = {
    D3D11_BLEND_OP_ADD, D3D11_BLEND_OP_SUBTRACT, D3D11_BLEND_OP_REV_SUBTRACT,
    D3D11_BLEND_OP_MIN, D3D11_BLEND_OP_MAX,
};
D3D11_BLEND bgfx_blend(uint32_t v, int col = 0) {
    return (v < 14) ? kBgfxBlendFactor[v][col & 1] : D3D11_BLEND_ONE;
}

// Size a cbuffer to the largest register the shader actually reads (bgfx's
// constBufSize can be smaller than the DXBC's CB0 declaration).
UINT game_cb_size(const std::vector<GameShaderUniform>& u, uint32_t constBuf) {
    UINT need = constBuf;
    for (const auto& x : u) {
        if (x.type == 0) continue;
        UINT end = (UINT)x.byteOff + (UINT)x.vec4s * 16;
        if (end > need) need = end;
    }
    UINT sz = (need + 15) & ~15u;
    return sz < 16 ? 16 : sz;
}
void game_fill_cb(std::vector<uint8_t>& buf, const std::vector<GameShaderUniform>& u,
                  const std::map<std::string, std::vector<float>>& vals) {
    std::fill(buf.begin(), buf.end(), (uint8_t)0);
    for (const auto& x : u) {
        if (x.type == 0) continue;
        auto it = vals.find(x.name);
        if (it == vals.end()) continue;
        size_t n = std::min((size_t)x.vec4s * 16, it->second.size() * 4);
        if ((size_t)x.byteOff + n <= buf.size()) std::memcpy(buf.data() + x.byteOff, it->second.data(), n);
    }
}
ComPtr<ID3D11Buffer> game_build_cb(const std::vector<GameShaderUniform>& u, uint32_t constBuf,
                                   const std::map<std::string, std::vector<float>>& vals) {
    UINT sz = game_cb_size(u, constBuf);
    std::vector<uint8_t> buf(sz, 0);
    game_fill_cb(buf, u, vals);
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; bd.ByteWidth = sz;
    D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = buf.data();
    ComPtr<ID3D11Buffer> b;
    g_dev->CreateBuffer(&bd, &sd, &b);
    return b;
}
void game_update_cb(ID3D11Buffer* b, const std::vector<GameShaderUniform>& u, uint32_t constBuf,
                    const std::map<std::string, std::vector<float>>& vals) {
    if (!b) return;
    UINT sz = game_cb_size(u, constBuf);
    std::vector<uint8_t> buf(sz, 0);
    game_fill_cb(buf, u, vals);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, buf.data(), sz);
        g_ctx->Unmap(b, 0);
    }
}
// GVertex byte offset for a VS input semantic. GVertex now carries all 8 UV sets
// (UV0 at 48, UV1..7 at 56 in 8-byte steps) so multi-UV game materials sample the
// correct detail/decal channels (matches gw2gsviewer's gw2model::Vertex layout).
UINT game_sem_off(const char* s, UINT idx) {
    if (!std::strcmp(s, "POSITION")) return 0;
    if (!std::strcmp(s, "NORMAL")) return 12;
    if (!std::strcmp(s, "TANGENT")) return 24;
    if (!std::strcmp(s, "BINORMAL") || !std::strcmp(s, "BITANGENT")) return 36;
    if (!std::strcmp(s, "TEXCOORD")) return 48 + 8 * (idx > 7 ? 7 : idx);
    if (!std::strcmp(s, "BLENDINDICES")) return 112;
    if (!std::strcmp(s, "BLENDWEIGHT")) return 128;
    return 0;
}

} // namespace

bool initialize(HWND target_window) {
    g_hwnd = target_window;
    RECT rc;
    GetClientRect(target_window, &rc);
    g_w = std::max<int>(1, rc.right - rc.left);
    g_h = std::max<int>(1, rc.bottom - rc.top);
    if (!create_device(target_window)) return false;
    create_targets();
    QueryPerformanceFrequency(&g_qpc_freq);
    QueryPerformanceCounter(&g_qpc_last);
    QueryPerformanceCounter(&g_qpc_start);
    return init_pipeline();
}

void shutdown() {
    clear_model();
    g_bonePaletteSRV.Reset();
    g_bonePalette.Reset();
    g_bone_cap = 0;
    g_lineIl.Reset(); g_linePs.Reset(); g_lineVs.Reset(); g_dssNoDepth.Reset();
    g_blendAlpha.Reset(); g_blendOpaque.Reset();
    g_dssNoWrite.Reset(); g_dss.Reset();
    g_rsWire.Reset(); g_rsSolid.Reset();
    g_samp.Reset(); g_cb.Reset(); g_il.Reset(); g_ps.Reset(); g_vs.Reset();
    g_dsv.Reset(); g_rtv.Reset(); g_swap.Reset(); g_ctx.Reset(); g_dev.Reset();
    g_hwnd = nullptr;
}

void on_resize(int width, int height) {
    if (!g_swap || width <= 0 || height <= 0) return;
    g_w = width;
    g_h = height;
    g_rtv.Reset();
    g_dsv.Reset();
    g_swap->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN, 0);
    create_targets();
}

void clear_model() {
    g_vb.Reset();
    g_ib.Reset();
    g_vb_skinned.Reset();
    g_cpu_verts.clear();
    g_mats.clear();
    g_subs.clear();
    g_game_mats.clear();
    g_game_any_ok = false;
    g_has_model = false;
    g_skelVb.Reset();
    g_skel_vert_count = 0;
    g_skel_vert_cap = 0;
    g_has_skeleton = false;
    g_joints.clear();
    g_clips.clear();
    g_clip_index = -1;
    g_anim_time = 0.0f;
    g_anim_playing = false;
    g_skin_ok = false;
    g_scene_models.clear();
    g_scene_insts.clear();
    g_scene_mode = false;
}

// Animated world transform of a bone: p_model = lin * p_local + pos (column-vec).
struct BoneXform {
    float lin[9] = {1,0,0, 0,1,0, 0,0,1};
    float pos[3] = {0, 0, 0};
};

// Poses the skeleton at the current clip + time into per-bone world transforms.
// clip -1 = bind pose: reuse the InverseWorld-derived positions with identity
// linear part (the skin palette then resolves to identity). Otherwise compose
// each bone's local transform (Granny track by name, else bind local) down the
// hierarchy.
void compute_world(std::vector<BoneXform>& out) {
    const auto& J = g_joints;
    out.assign(J.size(), BoneXform{});
    bool posed = (g_clip_index >= 0 && g_clip_index < static_cast<int>(g_clips.size()));
    if (!posed) {
        for (size_t i = 0; i < J.size(); ++i) {
            out[i].pos[0] = J[i].pos[0]; out[i].pos[1] = J[i].pos[1]; out[i].pos[2] = J[i].pos[2];
        }
        return;
    }
    const granny::Anim* anim = &g_clips[g_clip_index];
    std::unordered_map<std::string, int> byName;
    for (size_t i = 0; i < anim->tracks.size(); ++i) byName[anim->tracks[i].name] = static_cast<int>(i);

    for (size_t i = 0; i < J.size(); ++i) {
        float pos[3] = {J[i].localPos[0], J[i].localPos[1], J[i].localPos[2]};
        float quat[4] = {J[i].localQuat[0], J[i].localQuat[1], J[i].localQuat[2], J[i].localQuat[3]};
        float ss[9]; std::memcpy(ss, J[i].localScale, sizeof ss);
        auto it = byName.find(J[i].name);
        if (it != byName.end()) {
            const granny::Track& tr = anim->tracks[it->second];
            granny::sample(tr.pos, g_anim_time, pos, 3);
            granny::sample(tr.ori, g_anim_time, quat, 4);
            granny::sample(tr.sca, g_anim_time, ss, 9);
        }
        float R[9]; quatToM3(quat, R);
        float L[9]; m3mul(R, ss, L);
        int p = J[i].parent;
        if (p >= 0 && p < static_cast<int>(i)) {
            m3mul(out[p].lin, L, out[i].lin);
            float rp[3]; m3vec(out[p].lin, pos, rp);
            for (int k = 0; k < 3; ++k) out[i].pos[k] = out[p].pos[k] + rp[k];
        } else {
            std::memcpy(out[i].lin, L, sizeof L);
            for (int k = 0; k < 3; ++k) out[i].pos[k] = pos[k];
        }
    }
}

// Uploads the bone matrix palette (register b1). Each entry = InverseWorld(bind,
// model->bone) * AnimatedWorld(bone->model), so a bind-pose bone resolves to
// identity and the vertex is left where it started. Row-major to match the HLSL.
void update_bone_palette() {
    if (!g_bonePalette || g_bone_cap == 0) return;
    std::vector<BoneXform> W;
    compute_world(W);
    int n = std::min<int>(static_cast<int>(g_joints.size()), static_cast<int>(g_bone_cap));
    bool posed = (g_clip_index >= 0 && g_clip_index < static_cast<int>(g_clips.size()));
    std::vector<float> pal(static_cast<size_t>(g_bone_cap) * 16, 0.0f);
    for (int b = 0; b < static_cast<int>(g_bone_cap); ++b) {
        float* m = pal.data() + b * 16;
        if (!posed || b >= n) { // identity
            m[0] = m[5] = m[10] = m[15] = 1.0f;
            continue;
        }
        // Wrow (row-vector, row-major): linear = lin^T, translation row = pos.
        const BoneXform& w = W[b];
        float Wr[16] = {
            w.lin[0], w.lin[3], w.lin[6], 0,
            w.lin[1], w.lin[4], w.lin[7], 0,
            w.lin[2], w.lin[5], w.lin[8], 0,
            w.pos[0], w.pos[1], w.pos[2], 1,
        };
        const float* IB = g_joints[b].invWorld; // row-major, row-vector (model->bone)
        // SkinMat = IB * Wr (row-major 4x4 multiply)
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                m[r*4+c] = IB[r*4+0]*Wr[0*4+c] + IB[r*4+1]*Wr[1*4+c] + IB[r*4+2]*Wr[2*4+c] + IB[r*4+3]*Wr[3*4+c];
    }
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_ctx->Map(g_bonePalette.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, pal.data(), pal.size() * sizeof(float));
        g_ctx->Unmap(g_bonePalette.Get(), 0);
    }
}

// (Re)creates the bone palette StructuredBuffer sized to the current rig. 4 rows
// (float4) per bone; DYNAMIC so update_bone_palette refills it each frame.
void ensure_bone_palette(size_t bones) {
    if (bones == 0) { g_bonePalette.Reset(); g_bonePaletteSRV.Reset(); g_bone_cap = 0; return; }
    if (g_bone_cap >= bones && g_bonePalette) return; // existing buffer is big enough
    g_bonePaletteSRV.Reset();
    g_bonePalette.Reset();
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = 16; // one float4 row
    bd.ByteWidth = static_cast<UINT>(bones * 4 * 16); // 4 rows * 16 bytes per bone
    if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_bonePalette))) { g_bone_cap = 0; return; }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_UNKNOWN;
    sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sv.Buffer.FirstElement = 0;
    sv.Buffer.NumElements = static_cast<UINT>(bones * 4);
    if (FAILED(g_dev->CreateShaderResourceView(g_bonePalette.Get(), &sv, &g_bonePaletteSRV))) {
        g_bonePalette.Reset(); g_bone_cap = 0; return;
    }
    g_bone_cap = static_cast<UINT>(bones);
}

// Fills the dynamic line buffer from the current pose (bone segments + joint
// crosses). Cheap enough to run every frame during playback.
void rebuild_skel_lines() {
    if (!g_has_skeleton || !g_skelVb || g_joints.empty()) return;
    std::vector<BoneXform> W;
    compute_world(W);
    const float cross = (g_radius > 1e-3f ? g_radius : 1.0f) * 0.012f;
    std::vector<LineVtx> verts;
    verts.reserve(W.size() * 8);
    for (size_t i = 0; i < W.size(); ++i) {
        const float* p = W[i].pos;
        int par = g_joints[i].parent;
        if (par >= 0 && par < static_cast<int>(W.size())) {
            const float* pp = W[par].pos;
            verts.push_back({pp[0], pp[1], pp[2]});
            verts.push_back({p[0], p[1], p[2]});
        }
        verts.push_back({p[0] - cross, p[1], p[2]}); verts.push_back({p[0] + cross, p[1], p[2]});
        verts.push_back({p[0], p[1] - cross, p[2]}); verts.push_back({p[0], p[1] + cross, p[2]});
        verts.push_back({p[0], p[1], p[2] - cross}); verts.push_back({p[0], p[1], p[2] + cross});
    }
    g_skel_vert_count = std::min<UINT>(static_cast<UINT>(verts.size()), g_skel_vert_cap);
    if (!g_skel_vert_count) return;
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_ctx->Map(g_skelVb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, verts.data(), g_skel_vert_count * sizeof(LineVtx));
        g_ctx->Unmap(g_skelVb.Get(), 0);
    }
}

// Stores the skeleton + decoded clips and allocates the dynamic line buffer.
void build_skeleton(const ModelPreview& model) {
    g_skelVb.Reset();
    g_skel_vert_count = 0;
    g_skel_vert_cap = 0;
    g_has_skeleton = false;
    g_joints = model.joints;
    g_clips = model.animClips;
    g_clip_index = -1;
    g_anim_time = 0.0f;
    g_anim_playing = false;
    if (g_dev) ensure_bone_palette(g_joints.size()); // dynamic, any rig size
    if (!g_dev || g_joints.empty()) return;

    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = static_cast<UINT>(g_joints.size() * 8 * sizeof(LineVtx));
    if (SUCCEEDED(g_dev->CreateBuffer(&bd, nullptr, &g_skelVb))) {
        g_skel_vert_cap = static_cast<UINT>(g_joints.size() * 8);
        g_has_skeleton = true;
        rebuild_skel_lines();
    }
}

// Static lighting/fog/texture-transform uniform environment for the game
// shaders (ported from gw2gsviewer.cpp main()). Camera uniforms are refilled
// per frame in render_game(); these constants stay put. A daylight SH rig +
// full-visibility fxclr so forward-lit and stipple-discard shaders both draw.
void setup_game_uniforms() {
    if (g_game_uniforms_ready) return;
    auto put = [&](const char* n, std::initializer_list<float> f) { g_game_vals[n] = std::vector<float>(f); };
    // Overall brightness knob (env GW2_LIGHT, default 1.0). gw2gsviewer used a
    // deliberately over-driven 1.25 "showcase" sun that blows a dark boss (Deimos)
    // out to a pale, self-lit "lamp" look; a neutral ~0.9 sun + lower ambient
    // reads as the intended moody material. Turn GW2_LIGHT up for bright props.
    float lit = 1.0f;
    if (const char* e = std::getenv("GW2_LIGHT")) { double v = std::atof(e); if (v > 0.01) lit = static_cast<float>(v); }
    put("TexTransform0A", {1, 0, 0, 0}); put("TexTransform0B", {0, 1, 0, 0});
    put("TexTransform1A", {1, 0, 0, 0}); put("TexTransform1B", {0, 1, 0, 0});
    // light-prepass decode scale: albedo * lightBuffer * this. 1.0 with a white
    // buffer stand-in = flat full-bright ("lamp"); 0.8 tames it a little.
    put("LightBuffer", {0.8f * lit, 0.8f * lit, 0.8f * lit, 0.8f * lit});
    put("fxclr", {1, 1, 1, 1});                        // stipple-fade visibility (0 = discards all)
    put("StippleDensity", {0, 0, 0, 0}); put("StencilId", {0, 0, 0, 0});
    float sdx = 0.45f, sdy = 0.80f, sdz = 0.40f;
    { float l = std::sqrt(sdx * sdx + sdy * sdy + sdz * sdz); sdx /= l; sdy /= l; sdz /= l; }
    put("shSun", {sdx, sdy, sdz, 0});
    // Neutral daylight sun (no >1 overdrive) so lit surfaces don't clip to white.
    float s = 0.92f * lit;
    put("shSunColor", {s, s * 0.94f, s * 0.86f, 1});
    put("shSunData", {sdx, sdy, sdz, 0});
    // Dimmer SH ambient (DC in .w) so shadowed sides stay dark and moody.
    float a = lit;
    put("shRed", {0, 0.06f * a, 0, 0.16f * a}); put("shGreen", {0, 0.07f * a, 0, 0.18f * a});
    put("shBlue", {0, 0.09f * a, 0, 0.24f * a});
    put("SunColor", {0, 0, 0, 0}); put("SunDirection", {-0.4f, -0.8f, -0.4f, 0});
    put("AmbientColor", {0, 0, 0, 0}); put("AlphaRef", {0, 0, 0, 0}); put("fadedif", {1, 1, 1, 1});
    put("FogColorNearMinusFar", {0, 0, 0, 0}); put("FogColorFar", {0, 0, 0, 0}); put("FogColorHeight", {0, 0, 0, 0});
    put("FogDepthCue", {0, 0, 0, 0}); put("FogParam0", {0, 0, 0, 0});
    put("WorldToShadowA", {0, 0, 0, 0}); put("WorldToShadowB", {0, 0, 0, 0}); put("WorldToShadowC", {0, 0, 0, 0});
    g_game_uniforms_ready = true;
}

// Builds a GameMatGPU per material from ModelPreview::gameMaterials: creates the
// game's VS+PS from the DXBC blobs, an input layout from the VS reflection, the
// DYNAMIC cbuffers, and one SRV per PS sampler slot. Mirrors gw2gsviewer's
// build_material (minus the debug prints). Reuses the model's g_vb/g_ib.
void build_game_materials(const ModelPreview& model) {
    g_game_mats.clear();
    g_game_any_ok = false;
    if (!g_dev || model.gameMaterials.empty()) return;
    setup_game_uniforms();

    uint32_t maxIdx = 0;
    for (const auto& m : model.materials) maxIdx = std::max(maxIdx, m.index);
    for (const auto& gm : model.gameMaterials) maxIdx = std::max(maxIdx, gm.index);
    g_game_mats.resize(maxIdx + 1);

    for (const auto& c : model.gameMaterials) {
        if (!c.ok || c.vsDXBC.empty() || c.psDXBC.empty()) continue;
        GameMatGPU g;
        if (FAILED(g_dev->CreateVertexShader(c.vsDXBC.data(), c.vsDXBC.size(), nullptr, &g.vs))) continue;
        if (FAILED(g_dev->CreatePixelShader(c.psDXBC.data(), c.psDXBC.size(), nullptr, &g.ps))) continue;

        // Input layout from the VS input signature (bgfx keeps the ISGN chunk).
        ComPtr<ID3D11ShaderReflection> refl;
        if (FAILED(D3DReflect(c.vsDXBC.data(), c.vsDXBC.size(), IID_ID3D11ShaderReflection,
                              reinterpret_cast<void**>(refl.GetAddressOf()))))
            continue;
        D3D11_SHADER_DESC sdesc{};
        refl->GetDesc(&sdesc);
        std::vector<std::string> sem(sdesc.InputParameters);
        std::vector<D3D11_INPUT_ELEMENT_DESC> il;
        for (UINT i = 0; i < sdesc.InputParameters; i++) {
            D3D11_SIGNATURE_PARAMETER_DESC pd{};
            refl->GetInputParameterDesc(i, &pd);
            sem[i] = pd.SemanticName ? pd.SemanticName : "";
        }
        for (UINT i = 0; i < sdesc.InputParameters; i++) {
            D3D11_SIGNATURE_PARAMETER_DESC pd{};
            refl->GetInputParameterDesc(i, &pd);
            D3D11_INPUT_ELEMENT_DESC e{};
            e.SemanticName = sem[i].c_str();
            e.SemanticIndex = pd.SemanticIndex;
            e.Format = (pd.Mask <= 3) ? DXGI_FORMAT_R32G32_FLOAT : DXGI_FORMAT_R32G32B32_FLOAT;
            e.AlignedByteOffset = game_sem_off(sem[i].c_str(), pd.SemanticIndex);
            e.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            il.push_back(e);
        }
        if (FAILED(g_dev->CreateInputLayout(il.data(), static_cast<UINT>(il.size()), c.vsDXBC.data(), c.vsDXBC.size(),
                                            &g.layout)))
            continue;

        g.vsU = c.vsUniforms; g.psU = c.psUniforms; g.vsCB = c.vsConstBuf; g.psCB = c.psConstBuf;
        g.vcb = game_build_cb(g.vsU, g.vsCB, g_game_vals);
        g.pcb = game_build_cb(g.psU, g.psCB, g_game_vals);

        for (const auto& s : c.samplers) {
            if (s.slot < 0 || s.slot >= 16) continue;
            if (s.global == 2) g.srv[s.slot] = make_solid_cube(64);
            else if (s.global == 3) g.srv[s.slot] = make_solid(255, 255, 255, 255); // shadow map: unshadowed
            // Light-buffer / other global engine textures: a dark-grey stand-in
            // (not white) so deferred albedo*lightBuffer terms don't blow out to a
            // flat "lamp". Tunable via GW2_GLOBALLIT (0..255, default 110).
            else if (s.global == 1) {
                int lv = 110;
                if (const char* e = std::getenv("GW2_GLOBALLIT")) { int v = std::atoi(e); if (v >= 0 && v <= 255) lv = v; }
                g.srv[s.slot] = make_solid(static_cast<uint8_t>(lv), static_cast<uint8_t>(lv), static_cast<uint8_t>(lv), 255);
            }
            else if (s.gameTex >= 0 && s.gameTex < static_cast<int>(model.textures.size())) {
                const auto& t = model.textures[s.gameTex];
                g.srv[s.slot] = make_srv(t.rgba, t.width, t.height);
            }
        }

        // Decode the bgfx 64-bit state word exactly as bgfx's D3D11 renderer does.
        // GW2's AMAT renderState is a PARTIAL (blend-focused) word -- the opaque pass
        // is literally 0 and even transparent passes leave WRITE/DEPTH/CULL bits clear
        // (the engine ORs a base state at submit) -- so we only trust the blend fields
        // and keep full color write. BGFX_STATE_BLEND_SHIFT=12, EQUATION_SHIFT=28,
        // ALPHA_TO_COVERAGE=bit35.
        uint64_t st = c.renderState;
        g.renderState = st;
        uint32_t blend = (uint32_t)((st >> 12) & 0xffff);
        uint32_t equ   = (uint32_t)((st >> 28) & 0x3f);
        uint32_t srcRGB = blend & 0xf, dstRGB = (blend >> 4) & 0xf, srcA = (blend >> 8) & 0xf, dstA = (blend >> 12) & 0xf;
        uint32_t equRGB = equ & 0x7, equA = (equ >> 3) & 0x7;
        bool blendOn = blend != 0;
        g.depthWrite = (st & 0x0000004000000000ull) != 0;
        if (blendOn) {
            D3D11_BLEND_DESC bd{};
            bd.AlphaToCoverageEnable = (st & 0x0000000800000000ull) != 0; // BGFX_STATE_BLEND_ALPHA_TO_COVERAGE
            bd.RenderTarget[0].BlendEnable = TRUE;
            bd.RenderTarget[0].SrcBlend = bgfx_blend(srcRGB, 0);
            bd.RenderTarget[0].DestBlend = bgfx_blend(dstRGB, 0);
            bd.RenderTarget[0].BlendOp = kBgfxBlendEquation[equRGB < 5 ? equRGB : 0];
            bd.RenderTarget[0].SrcBlendAlpha = bgfx_blend(srcA ? srcA : 2, 1);
            bd.RenderTarget[0].DestBlendAlpha = bgfx_blend(dstA ? dstA : 1, 1);
            bd.RenderTarget[0].BlendOpAlpha = kBgfxBlendEquation[equA < 5 ? equA : 0];
            bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            g_dev->CreateBlendState(&bd, &g.blend);
        }
        g.ok = true;
        if (c.index < g_game_mats.size()) {
            g_game_mats[c.index] = std::move(g);
            g_game_any_ok = true;
        }
    }
}

void set_model(const ModelPreview& model) {
    clear_model();
    if (!g_dev || model.meshes.empty()) return;

    std::vector<GVertex> verts;
    std::vector<uint32_t> indices;
    for (const auto& m : model.meshes) {
        uint32_t voff = static_cast<uint32_t>(verts.size());
        SubMesh s;
        s.indexStart = static_cast<UINT>(indices.size());
        s.matIndex = m.materialIndex;
        s.hasSkin = m.hasSkin;
        // Submesh centroid (model space), for the game-shader transparent sort.
        double cx = 0, cy = 0, cz = 0;
        for (const auto& v : m.vertices) { cx += v.px; cy += v.py; cz += v.pz; }
        if (!m.vertices.empty()) {
            s.center[0] = static_cast<float>(cx / m.vertices.size());
            s.center[1] = static_cast<float>(cy / m.vertices.size());
            s.center[2] = static_cast<float>(cz / m.vertices.size());
        }
        verts.insert(verts.end(), m.vertices.begin(), m.vertices.end());
        for (uint32_t i : m.indices) indices.push_back(voff + i);
        s.indexCount = static_cast<UINT>(indices.size()) - s.indexStart;
        if (s.indexCount) g_subs.push_back(s);
    }
    if (verts.empty() || indices.empty()) {
        g_subs.clear();
        return;
    }

    D3D11_BUFFER_DESC bd{};
    D3D11_SUBRESOURCE_DATA sd{};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.ByteWidth = static_cast<UINT>(verts.size() * sizeof(GVertex));
    sd.pSysMem = verts.data();
    if (FAILED(g_dev->CreateBuffer(&bd, &sd, &g_vb))) { clear_model(); return; }
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint32_t));
    sd.pSysMem = indices.data();
    if (FAILED(g_dev->CreateBuffer(&bd, &sd, &g_ib))) { clear_model(); return; }

    // Materials (indexed by material.index).
    uint32_t maxIdx = 0;
    for (const auto& m : model.materials) maxIdx = std::max(maxIdx, m.index);
    g_mats.resize(model.materials.empty() ? 0 : maxIdx + 1);
    for (const auto& m : model.materials) {
        MaterialGPU g;
        for (int k = 0; k < 4; ++k) g.tint[k] = m.tint[k];
        g.isEffect = m.isEffect;
        g.kind = m.kind;
        if (m.diffuseTex >= 0 && m.diffuseTex < static_cast<int>(model.textures.size())) {
            const auto& t = model.textures[m.diffuseTex];
            g.srv = make_srv(t.rgba, t.width, t.height);
        }
        if (!m.isEffect && m.normalTex >= 0 && m.normalTex < static_cast<int>(model.textures.size())) {
            const auto& t = model.textures[m.normalTex];
            g.srvNormal = make_srv(t.rgba, t.width, t.height);
        }
        if (m.index < g_mats.size()) g_mats[m.index] = std::move(g);
    }

    g_center = {model.center[0], model.center[1], model.center[2]};
    g_radius = model.radius > 1e-3f ? model.radius : 1.0f;
    g_has_model = true;
    build_game_materials(model); // real bgfx DXBC shaders for the "Shader" mode
    build_skeleton(model);
    // Skinning is possible whenever there's an inline rig (palette is dynamic).
    g_skin_ok = !g_joints.empty() && g_bonePaletteSRV;
    // For the game-shader path we CPU-skin, so keep a rest-pose vertex copy + a
    // DYNAMIC buffer to skin into (only for skinned models).
    if (g_skin_ok) {
        g_cpu_verts = verts;
        D3D11_BUFFER_DESC sbd{};
        sbd.Usage = D3D11_USAGE_DYNAMIC;
        sbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        sbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        sbd.ByteWidth = static_cast<UINT>(verts.size() * sizeof(GVertex));
        if (FAILED(g_dev->CreateBuffer(&sbd, nullptr, &g_vb_skinned))) g_vb_skinned.Reset();
    }
    reset_view();
}

// ---- map scene ----
Mat4 rotZ(float a) {
    Mat4 r = identity();
    float c = std::cos(a), s = std::sin(a);
    r.m[0] = c; r.m[1] = s; r.m[4] = -s; r.m[5] = c;
    return r;
}
Mat4 scaleMat(float s) {
    Mat4 r = identity();
    r.m[0] = r.m[5] = r.m[10] = s;
    return r;
}
// world (row-vector: p_world = p_model * world) = Scale * Rot(XYZ) * Translate.
Mat4 sceneWorld(const float pos[3], const float rot[3], float scale) {
    Mat4 R = mul(mul(rotX(rot[0]), rotY(rot[1])), rotZ(rot[2]));
    return mul(mul(scaleMat(scale), R), translate({pos[0], pos[1], pos[2]}));
}

bool upload_scene_model(const ModelPreview& model, SceneModelGPU& out) {
    if (!g_dev || model.meshes.empty()) return false;
    std::vector<GVertex> verts;
    std::vector<uint32_t> indices;
    for (const auto& m : model.meshes) {
        uint32_t voff = static_cast<uint32_t>(verts.size());
        SubMesh s;
        s.indexStart = static_cast<UINT>(indices.size());
        s.matIndex = m.materialIndex;
        s.hasSkin = false; // props render static (no per-scene skinning)
        verts.insert(verts.end(), m.vertices.begin(), m.vertices.end());
        for (uint32_t i : m.indices) indices.push_back(voff + i);
        s.indexCount = static_cast<UINT>(indices.size()) - s.indexStart;
        if (s.indexCount) out.subs.push_back(s);
    }
    if (verts.empty() || indices.empty()) return false;

    D3D11_BUFFER_DESC bd{}; D3D11_SUBRESOURCE_DATA sd{};
    bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.ByteWidth = static_cast<UINT>(verts.size() * sizeof(GVertex));
    sd.pSysMem = verts.data();
    if (FAILED(g_dev->CreateBuffer(&bd, &sd, &out.vb))) return false;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER; bd.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint32_t));
    sd.pSysMem = indices.data();
    if (FAILED(g_dev->CreateBuffer(&bd, &sd, &out.ib))) return false;

    uint32_t maxIdx = 0;
    for (const auto& m : model.materials) maxIdx = std::max(maxIdx, m.index);
    out.mats.resize(model.materials.empty() ? 0 : maxIdx + 1);
    for (const auto& m : model.materials) {
        MaterialGPU g;
        for (int k = 0; k < 4; ++k) g.tint[k] = m.tint[k];
        g.isEffect = m.isEffect;
        g.kind = m.kind;
        if (m.diffuseTex >= 0 && m.diffuseTex < static_cast<int>(model.textures.size())) {
            const auto& t = model.textures[m.diffuseTex];
            g.srv = make_srv(t.rgba, t.width, t.height);
        }
        if (!m.isEffect && m.normalTex >= 0 && m.normalTex < static_cast<int>(model.textures.size())) {
            const auto& t = model.textures[m.normalTex];
            g.srvNormal = make_srv(t.rgba, t.width, t.height);
        }
        if (m.index < out.mats.size()) out.mats[m.index] = std::move(g);
    }
    out.ok = true;
    return true;
}

void clear_scene() {
    g_scene_models.clear();
    g_scene_insts.clear();
    g_scene_mode = false;
}

void set_scene(const std::vector<ModelPreview>& models, const std::vector<SceneInstance>& instances) {
    clear_model();
    clear_scene();
    if (!g_dev || models.empty() || instances.empty()) return;

    g_scene_models.resize(models.size());
    for (size_t i = 0; i < models.size(); ++i) upload_scene_model(models[i], g_scene_models[i]);

    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const auto& in : instances) {
        if (in.model < 0 || in.model >= static_cast<int>(g_scene_models.size())) continue;
        if (!g_scene_models[in.model].ok) continue;
        SceneInstGPU g;
        g.model = in.model;
        g.world = sceneWorld(in.pos, in.rot, in.scale);
        g.layer = in.layer;
        g_scene_insts.push_back(g);
        // Expand the scene AABB by each instance's model extent (so a big model
        // like the terrain frames the whole map), approximating rotation away.
        // COLLISION (and ZONE, appended later via add_scene_models) are hidden
        // overlays by default and can have very different extents than the
        // prop/terrain cluster -- a stray far-away collision volume, or a single
        // mis-parsed/garbage vertex from the raw havk chunk, is enough to blow up
        // lo/hi and misframe the *entire* scene (props end up clumped tiny in a
        // corner -- exactly the "posisi jadi berantakan" symptom). Don't let
        // overlays that aren't even visible drive the camera framing.
        if (in.layer == LAYER_COLLISION || in.layer == LAYER_ZONE) continue;
        const ModelPreview& mp = models[in.model];
        float r = mp.radius * in.scale;
        // Extra guard: ignore non-finite / absurd extents outright instead of
        // letting one bad instance poison the whole bounding box.
        if (!std::isfinite(r) || r > 200000.0f) continue;
        for (int k = 0; k < 3; ++k) {
            float c = in.pos[k] + mp.center[k] * in.scale;
            if (!std::isfinite(c)) continue;
            lo[k] = std::min(lo[k], c - r);
            hi[k] = std::max(hi[k], c + r);
        }
    }
    if (g_scene_insts.empty()) return;

    g_scene_center = {(lo[0]+hi[0])*0.5f, (lo[1]+hi[1])*0.5f, (lo[2]+hi[2])*0.5f};
    float ext[3] = {hi[0]-lo[0], hi[1]-lo[1], hi[2]-lo[2]};
    g_scene_radius = 0.5f * std::sqrt(ext[0]*ext[0] + ext[1]*ext[1] + ext[2]*ext[2]);
    if (g_scene_radius < 1e-2f) g_scene_radius = 1.0f;
    g_scene_mode = true;
    reset_view();
}

// Appends more models + instances (e.g. a lazily-loaded zone layer) to the live
// scene without disturbing the camera. instance.model is 0-based over `models`.
void add_scene_models(const std::vector<ModelPreview>& models, const std::vector<SceneInstance>& instances) {
    if (!g_dev || !g_scene_mode || models.empty() || instances.empty()) return;
    int base = static_cast<int>(g_scene_models.size());
    g_scene_models.resize(g_scene_models.size() + models.size());
    for (size_t i = 0; i < models.size(); ++i) upload_scene_model(models[i], g_scene_models[base + i]);
    for (const auto& in : instances) {
        if (in.model < 0 || in.model >= static_cast<int>(models.size())) continue;
        int gm = base + in.model;
        if (!g_scene_models[gm].ok) continue;
        SceneInstGPU g;
        g.model = gm;
        g.world = sceneWorld(in.pos, in.rot, in.scale);
        g.layer = in.layer;
        g_scene_insts.push_back(g);
    }
}

bool scene_active() { return g_scene_mode; }
void set_layer_visible(int layer, bool visible) { if (layer >= 0 && layer < LAYER_COUNT) g_layer_visible[layer] = visible; }
bool layer_visible(int layer) { return (layer >= 0 && layer < LAYER_COUNT) ? g_layer_visible[layer] : false; }

void set_mode(RenderMode mode) { g_mode = mode; }
void set_show_skeleton(bool show) { g_show_skeleton = show; }
bool has_skeleton() { return g_has_skeleton; }

int animation_count() { return static_cast<int>(g_clips.size()); }
const char* animation_name(int i) {
    return (i >= 0 && i < static_cast<int>(g_clips.size())) ? g_clips[i].name.c_str() : "";
}
float animation_duration(int i) {
    return (i >= 0 && i < static_cast<int>(g_clips.size())) ? g_clips[i].duration : 0.0f;
}
// True if a curve is keyframed AND its control values actually vary across keys.
// Note: a static "zeropose" can still encode a few multi-knot spline curves whose
// control values are all identical (no real motion), so knots.size()>1 alone is not
// enough -- we must check that the samples actually change.
static bool curve_has_motion(const granny::Curve& c) {
    if (c.knots.size() <= 1 || c.dim <= 0) return false;
    int nk = static_cast<int>(c.controls.size()) / c.dim;
    if (nk <= 1) return false;
    for (int d = 0; d < c.dim; ++d) {
        float mn = c.controls[d], mx = mn;
        for (int k = 1; k < nk; ++k) {
            float v = c.controls[(size_t)k * c.dim + d];
            if (v < mn) mn = v; if (v > mx) mx = v;
        }
        if (mx - mn > 1e-4f) return true;
    }
    return false;
}
// A clip has real motion if any transform curve is keyframed with varying values.
// Mount/prop models embed only a static "zeropose"; character/boss/weapon models
// embed many real keyframed clips that actually move the rig.
bool animation_has_motion(int i) {
    if (i < 0 || i >= static_cast<int>(g_clips.size())) return false;
    for (const granny::Track& tr : g_clips[i].tracks)
        if (curve_has_motion(tr.ori) || curve_has_motion(tr.pos) || curve_has_motion(tr.sca))
            return true;
    return false;
}
int first_motion_animation() {
    // Prefer a substantial clip: the static "zeropose" can carry a couple of
    // barely-varying curves over its ~1-frame (0.033s) duration, so require both
    // real motion and a non-trivial length to land on an actual animation.
    for (int i = 0; i < static_cast<int>(g_clips.size()); ++i)
        if (g_clips[i].duration > 0.15f && animation_has_motion(i)) return i;
    // Fallback: any clip with motion regardless of length.
    for (int i = 0; i < static_cast<int>(g_clips.size()); ++i)
        if (animation_has_motion(i)) return i;
    return -1;
}
int current_animation() { return g_clip_index; }
void set_animation(int clip_index) {
    if (clip_index < -1 || clip_index >= static_cast<int>(g_clips.size()))
        clip_index = -1;
    g_clip_index = clip_index;
    g_anim_time = 0.0f;
    QueryPerformanceCounter(&g_qpc_last);
    rebuild_skel_lines();
}
void set_anim_time(float seconds) {
    g_anim_time = seconds;
    rebuild_skel_lines();
}
float anim_time() { return g_anim_time; }
void set_playing(bool playing) {
    g_anim_playing = playing;
    if (playing) QueryPerformanceCounter(&g_qpc_last);
}
bool is_playing() { return g_anim_playing; }

void orbit(float dyaw, float dpitch) {
    // Free trackball: compose the drag as world-space rotations (yaw about Y,
    // pitch about X) applied *after* the current orientation. No clamped axis
    // and no gimbal lock, so the model tumbles freely in every direction.
    Mat4 inc = mul(rotY(dyaw), rotX(dpitch));
    g_rot = mul(g_rot, inc);
}

void zoom(float factor) { g_dist = std::clamp(g_dist * factor, 0.2f, 20.0f); }

void reset_view() {
    g_rot = identity();
    g_dist = 1.7f;
}

// Draws every scene instance at its world transform. Orbit camera around the
// scene center (reusing g_rot/g_dist so the existing drag/zoom controls work).
void render_scene() {
    bool wire = (g_mode == RenderMode::Wireframe);
    bool textured = (g_mode == RenderMode::Full);
    g_ctx->RSSetState(wire ? g_rsWire.Get() : g_rsSolid.Get());

    float camDist = g_scene_radius * g_dist;
    Vec3 eye{g_scene_center.x, g_scene_center.y, g_scene_center.z - camDist};
    Mat4 view = lookAt(eye, g_scene_center, {0, 1, 0});
    Mat4 proj = perspective(0.9f, static_cast<float>(g_w) / std::max(1, g_h),
                            g_scene_radius * 0.005f, g_scene_radius * 80.0f);
    Mat4 sceneRot = mul(translate({-g_scene_center.x, -g_scene_center.y, -g_scene_center.z}),
                        mul(g_rot, translate(g_scene_center)));
    Mat4 VP = mul(sceneRot, mul(view, proj));
    Vec3 L = norm({-0.4f, -0.8f, -0.4f});

    g_ctx->IASetInputLayout(g_il.Get());
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_ps.Get(), nullptr, 0);
    ID3D11Buffer* cbs[] = {g_cb.Get()};
    g_ctx->VSSetConstantBuffers(0, 1, cbs);
    g_ctx->PSSetConstantBuffers(0, 1, cbs);
    // Scene props render unskinned (uHasSkin=0), so the bone palette (t2) is unused.
    ID3D11SamplerState* samps[] = {g_samp.Get()};
    g_ctx->PSSetSamplers(0, 1, samps);
    const float bf[4] = {0, 0, 0, 0};
    UINT stride = sizeof(GVertex), off = 0;

    int passes = textured ? 2 : 1;
    for (int pass = 0; pass < passes; ++pass) {
        bool effectPass = textured && (pass == 1);
        g_ctx->OMSetBlendState(effectPass ? g_blendAlpha.Get() : g_blendOpaque.Get(), bf, 0xffffffff);
        g_ctx->OMSetDepthStencilState(effectPass ? g_dssNoWrite.Get() : g_dss.Get(), 0);
        for (const auto& in : g_scene_insts) {
            if (!g_layer_visible[in.layer]) continue;
            const SceneModelGPU& sm = g_scene_models[in.model];
            if (!sm.ok) continue;
            Mat4 modelMat = mul(in.world, sceneRot);
            Mat4 mvp = mul(in.world, VP);
            ID3D11Buffer* vbs[] = {sm.vb.Get()};
            g_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &off);
            g_ctx->IASetIndexBuffer(sm.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
            for (const auto& s : sm.subs) {
                const MaterialGPU* mat = (s.matIndex < sm.mats.size()) ? &sm.mats[s.matIndex] : nullptr;
                bool isBlend = mat && (mat->kind == 2 || mat->kind == 3); // water + collision blend
                bool isEffect = mat && (mat->isEffect || isBlend);        // rendered in the effect pass
                if (textured && isEffect != effectPass) continue;
                CB cb{};
                cb.model = modelMat;
                cb.mvp = mvp;
                cb.lx = L.x; cb.ly = L.y; cb.lz = L.z;
                cb.hasSkin = 0.0f;
                cb.matKind = mat ? static_cast<float>(mat->kind) : 0.0f; // terrain/water shading
                if (textured) {
                    cb.hasTex = (mat && mat->srv) ? 1.0f : 0.0f;
                    cb.hasNormal = (mat && mat->srvNormal) ? 1.0f : 0.0f;
                    cb.isEffect = (mat && mat->isEffect) ? 1.0f : 0.0f;
                    cb.tintR = mat ? mat->tint[0] : 1.0f; cb.tintG = mat ? mat->tint[1] : 1.0f;
                    cb.tintB = mat ? mat->tint[2] : 1.0f; cb.tintA = mat ? mat->tint[3] : 1.0f;
                } else {
                    cb.hasTex = cb.hasNormal = cb.isEffect = 0.0f;
                    cb.tintR = cb.tintG = cb.tintB = cb.tintA = 1.0f;
                }
                D3D11_MAPPED_SUBRESOURCE ms;
                if (SUCCEEDED(g_ctx->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                    std::memcpy(ms.pData, &cb, sizeof cb);
                    g_ctx->Unmap(g_cb.Get(), 0);
                }
                ID3D11ShaderResourceView* srvs[2] = {textured && mat ? mat->srv.Get() : nullptr,
                                                     textured && mat ? mat->srvNormal.Get() : nullptr};
                g_ctx->PSSetShaderResources(0, 2, srvs);
                g_ctx->DrawIndexed(s.indexCount, s.indexStart, 0);
            }
        }
    }
}

LONGLONG now_qpc() {
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return n.QuadPart;
}

// CPU linear-blend skinning of the rest mesh (g_cpu_verts) into `dst` using the
// current pose. GW2's material VS take pre-skinned vertices, so this is how the
// game-shader path animates a rig. Positions + tangent frame are skinned; UVs
// and bone data are copied through. Returns false if there's nothing to skin.
bool cpu_skin_into(ID3D11Buffer* dst) {
    if (!dst || g_cpu_verts.empty() || g_joints.empty()) return false;
    std::vector<BoneXform> W;
    compute_world(W);
    const int nb = static_cast<int>(g_joints.size());
    std::vector<std::array<float, 16>> S(nb); // per-bone SkinMat (row-major, row-vector)
    for (int b = 0; b < nb; ++b) {
        const BoneXform& w = W[b];
        float Wr[16] = {w.lin[0], w.lin[3], w.lin[6], 0, w.lin[1], w.lin[4], w.lin[7], 0,
                        w.lin[2], w.lin[5], w.lin[8], 0, w.pos[0], w.pos[1], w.pos[2], 1};
        const float* IB = g_joints[b].invWorld;
        float* m = S[b].data();
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                m[r*4+c] = IB[r*4+0]*Wr[0*4+c] + IB[r*4+1]*Wr[1*4+c] + IB[r*4+2]*Wr[2*4+c] + IB[r*4+3]*Wr[3*4+c];
    }
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g_ctx->Map(dst, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return false;
    GVertex* out = static_cast<GVertex*>(ms.pData);
    for (size_t i = 0; i < g_cpu_verts.size(); ++i) {
        const GVertex& s = g_cpu_verts[i];
        GVertex o = s;
        float p[3] = {0, 0, 0}, n[3] = {0, 0, 0}, t[3] = {0, 0, 0}, bt[3] = {0, 0, 0}, wsum = 0;
        for (int c = 0; c < 4; ++c) {
            float w = s.bwt[c];
            if (w <= 0.0f) continue;
            int bi = static_cast<int>(s.bidx[c]);
            if (bi < 0 || bi >= nb) continue;
            const float* M = S[bi].data();
            p[0] += w*(s.px*M[0]+s.py*M[4]+s.pz*M[8]+M[12]);
            p[1] += w*(s.px*M[1]+s.py*M[5]+s.pz*M[9]+M[13]);
            p[2] += w*(s.px*M[2]+s.py*M[6]+s.pz*M[10]+M[14]);
            n[0] += w*(s.nx*M[0]+s.ny*M[4]+s.nz*M[8]);
            n[1] += w*(s.nx*M[1]+s.ny*M[5]+s.nz*M[9]);
            n[2] += w*(s.nx*M[2]+s.ny*M[6]+s.nz*M[10]);
            t[0] += w*(s.tx*M[0]+s.ty*M[4]+s.tz*M[8]);
            t[1] += w*(s.tx*M[1]+s.ty*M[5]+s.tz*M[9]);
            t[2] += w*(s.tx*M[2]+s.ty*M[6]+s.tz*M[10]);
            bt[0] += w*(s.bx*M[0]+s.by*M[4]+s.bz*M[8]);
            bt[1] += w*(s.bx*M[1]+s.by*M[5]+s.bz*M[9]);
            bt[2] += w*(s.bx*M[2]+s.by*M[6]+s.bz*M[10]);
            wsum += w;
        }
        if (wsum > 1e-6f) {
            o.px = p[0]; o.py = p[1]; o.pz = p[2];
            o.nx = n[0]; o.ny = n[1]; o.nz = n[2];
            o.tx = t[0]; o.ty = t[1]; o.tz = t[2];
            o.bx = bt[0]; o.by = bt[1]; o.bz = bt[2];
        }
        out[i] = o;
    }
    g_ctx->Unmap(dst, 0);
    return true;
}

// Draws the single model with each submesh's own GAME bgfx VS+PS (opaque pass
// then translucent back-to-front). Reuses g_vb/g_ib (GVertex). Camera matches
// the reconstruction path so the two modes frame identically. Assumes the RTV /
// depth / viewport are already bound by render().
void render_game() {
    g_ctx->RSSetState(g_rsSolid.Get());

    float camDist = g_radius * g_dist;
    Vec3 eye{g_center.x, g_center.y, g_center.z - camDist};
    Mat4 view = lookAt(eye, g_center, {0, 1, 0});
    Mat4 proj = perspective(0.9f, static_cast<float>(g_w) / std::max(1, g_h), g_radius * 0.02f, g_radius * 40.0f);
    Mat4 model = mul(translate({-g_center.x, -g_center.y, -g_center.z}), mul(g_rot, translate(g_center)));
    Mat4 vp = mul(view, proj);
    Mat4 mvp = mul(model, vp);
    Mat4 wv = mul(model, view);

    // bgfx uniforms are column-major -> store the transpose of each row-major matrix.
    auto pM = [&](const char* n, const Mat4& m) {
        Mat4 t{};
        for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) t.m[i * 4 + j] = m.m[j * 4 + i];
        g_game_vals[n] = std::vector<float>(t.m, t.m + 16);
    };
    pM("World", model); pM("ViewProjection", vp); pM("WorldViewProjection", mvp); pM("WorldView", wv);
    g_game_vals["CameraPosition"] = std::vector<float>{eye.x, eye.y, eye.z, 1};
    g_game_vals["ScreenDims"] =
        std::vector<float>{(float)g_w, (float)g_h, 1.0f / std::max(1, g_w), 1.0f / std::max(1, g_h)};
    // Wall-clock time for animated material effects (scrolling lava/energy, etc.).
    double tsec = g_qpc_freq.QuadPart ? double(now_qpc() - g_qpc_start.QuadPart) / g_qpc_freq.QuadPart : 0.0;
    float tf = static_cast<float>(tsec);
    g_game_vals["Time"] = std::vector<float>{tf, tf, tf, tf};

    for (auto& g : g_game_mats) {
        if (!g.ok) continue;
        game_update_cb(g.vcb.Get(), g.vsU, g.vsCB, g_game_vals);
        game_update_cb(g.pcb.Get(), g.psU, g.psCB, g_game_vals);
    }

    // The game VS wants pre-skinned vertices (it has no bone inputs). When a rig
    // is posed, CPU-skin the rest mesh into g_vb_skinned and draw that.
    bool posed = (g_clip_index >= 0 && g_clip_index < static_cast<int>(g_clips.size()));
    ID3D11Buffer* vbUse = g_vb.Get();
    if (g_skin_ok && g_vb_skinned && posed && cpu_skin_into(g_vb_skinned.Get())) vbUse = g_vb_skinned.Get();

    UINT stride = sizeof(GVertex), off = 0;
    ID3D11Buffer* vbs[] = {vbUse};
    g_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &off);
    g_ctx->IASetIndexBuffer(g_ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11SamplerState* samps[16];
    for (int i = 0; i < 16; i++) samps[i] = g_samp.Get();
    g_ctx->PSSetSamplers(0, 16, samps);
    const float bf[4] = {0, 0, 0, 0};

    auto drawSub = [&](const SubMesh& s) {
        if (s.matIndex >= g_game_mats.size()) return;
        GameMatGPU& g = g_game_mats[s.matIndex];
        if (!g.ok) return;
        g_ctx->IASetInputLayout(g.layout.Get());
        g_ctx->VSSetShader(g.vs.Get(), nullptr, 0);
        g_ctx->PSSetShader(g.ps.Get(), nullptr, 0);
        ID3D11Buffer* vcb[] = {g.vcb.Get()};
        ID3D11Buffer* pcb[] = {g.pcb.Get()};
        g_ctx->VSSetConstantBuffers(0, 1, vcb);
        g_ctx->PSSetConstantBuffers(0, 1, pcb);
        ID3D11ShaderResourceView* srvs[16];
        for (int i = 0; i < 16; i++) srvs[i] = g.srv[i].Get();
        g_ctx->PSSetShaderResources(0, 16, srvs);
        g_ctx->DrawIndexed(s.indexCount, s.indexStart, 0);
    };

    // Opaque first.
    g_ctx->OMSetBlendState(g_blendOpaque.Get(), bf, 0xffffffff);
    g_ctx->OMSetDepthStencilState(g_dss.Get(), 0);
    for (const auto& s : g_subs)
        if (s.matIndex < g_game_mats.size() && g_game_mats[s.matIndex].ok && !g_game_mats[s.matIndex].blend)
            drawSub(s);

    // Translucent back-to-front (blend from the AMAT bgfx state), depth read-only.
    std::vector<const SubMesh*> tsubs;
    for (const auto& s : g_subs)
        if (s.matIndex < g_game_mats.size() && g_game_mats[s.matIndex].ok && g_game_mats[s.matIndex].blend)
            tsubs.push_back(&s);
    if (!tsubs.empty()) {
        auto worldZ = [&](const SubMesh* s) {
            const float* p = s->center;
            float wx = p[0] * model.m[0] + p[1] * model.m[4] + p[2] * model.m[8] + model.m[12];
            float wy = p[0] * model.m[1] + p[1] * model.m[5] + p[2] * model.m[9] + model.m[13];
            float wz = p[0] * model.m[2] + p[1] * model.m[6] + p[2] * model.m[10] + model.m[14];
            float dx = wx - eye.x, dy = wy - eye.y, dz = wz - eye.z;
            return dx * dx + dy * dy + dz * dz;
        };
        std::sort(tsubs.begin(), tsubs.end(),
                  [&](const SubMesh* a, const SubMesh* b) { return worldZ(a) > worldZ(b); });
        g_ctx->OMSetDepthStencilState(g_dssNoWrite.Get(), 0);
        for (const SubMesh* s : tsubs) {
            g_ctx->OMSetBlendState(g_game_mats[s->matIndex].blend.Get(), bf, 0xffffffff);
            drawSub(*s);
        }
        g_ctx->OMSetBlendState(g_blendOpaque.Get(), bf, 0xffffffff);
        g_ctx->OMSetDepthStencilState(g_dss.Get(), 0);
    }
}

void render() {
    if (!g_ctx || !g_rtv) return;

    // Advance playback (real time via QPC, so it's frame-rate independent) and
    // re-pose the rig. A WM_TIMER in the host keeps invalidating us while playing.
    bool clipActive = (g_clip_index >= 0 && g_clip_index < static_cast<int>(g_clips.size()));
    if (g_anim_playing && clipActive) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double dt = g_qpc_freq.QuadPart ? double(now.QuadPart - g_qpc_last.QuadPart) / g_qpc_freq.QuadPart : 0.0;
        g_qpc_last = now;
        // Real clips wrap at their duration.
        float dur = g_clips[g_clip_index].duration;
        if (dur > 1e-4f) {
            g_anim_time += static_cast<float>(dt);
            if (g_anim_time > dur) g_anim_time = std::fmod(g_anim_time, dur);
        }
        rebuild_skel_lines();
    }

    const float clear[4] = {0.10f, 0.11f, 0.13f, 1.0f};
    g_ctx->ClearRenderTargetView(g_rtv.Get(), clear);
    if (g_dsv) g_ctx->ClearDepthStencilView(g_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    ID3D11RenderTargetView* rtvs[] = {g_rtv.Get()};
    g_ctx->OMSetRenderTargets(1, rtvs, g_dsv.Get());
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(g_w), static_cast<float>(g_h), 0, 1};
    g_ctx->RSSetViewports(1, &vp);

    if (g_scene_mode) {
        render_scene();
        g_swap->Present(1, 0);
        return;
    }

    if (!g_has_model || !g_vb || !g_ib) {
        g_swap->Present(1, 0);
        return;
    }

    // GameShader mode draws each submesh with its own game bgfx VS+PS; the
    // reconstruction (Full/Plain/Wireframe) path is skipped. Falls back to the
    // reconstruction draw if no material had usable game shaders.
    bool useGame = (g_mode == RenderMode::GameShader) && g_game_any_ok;
    bool wire = (g_mode == RenderMode::Wireframe);
    bool textured = (g_mode == RenderMode::Full);
    if (!useGame) g_ctx->RSSetState(wire ? g_rsWire.Get() : g_rsSolid.Get());

    // Fixed camera looking at the model center; all rotation lives in the model
    // matrix (free trackball) so there is no locked axis or pole to stall at.
    float camDist = g_radius * g_dist;
    Vec3 eye{g_center.x, g_center.y, g_center.z - camDist};
    Mat4 view = lookAt(eye, g_center, {0, 1, 0});
    Mat4 proj = perspective(0.9f, static_cast<float>(g_w) / std::max(1, g_h), g_radius * 0.02f, g_radius * 40.0f);
    Mat4 model = mul(translate({-g_center.x, -g_center.y, -g_center.z}), mul(g_rot, translate(g_center)));
    Mat4 mvp = mul(model, mul(view, proj));
    Vec3 L = norm({-0.4f, -0.8f, -0.4f});
    const float bf[4] = {0, 0, 0, 0};

    if (useGame) {
        render_game();
    } else {
    UINT stride = sizeof(GVertex), off = 0;
    ID3D11Buffer* vbs[] = {g_vb.Get()};
    g_ctx->IASetInputLayout(g_il.Get());
    g_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &off);
    g_ctx->IASetIndexBuffer(g_ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_ps.Get(), nullptr, 0);
    update_bone_palette();
    ID3D11Buffer* cbs[] = {g_cb.Get()};
    g_ctx->VSSetConstantBuffers(0, 1, cbs);
    g_ctx->PSSetConstantBuffers(0, 1, cbs);
    ID3D11ShaderResourceView* bsrv[] = {g_bonePaletteSRV.Get()};
    g_ctx->VSSetShaderResources(2, 1, bsrv); // bone palette StructuredBuffer (t2)
    ID3D11SamplerState* samps[] = {g_samp.Get()};
    g_ctx->PSSetSamplers(0, 1, samps);

    // Full mode does the two-pass opaque/effect split; Plain/Wireframe draw
    // everything once, opaque and untextured.
    int passes = textured ? 2 : 1;
    for (int pass = 0; pass < passes; ++pass) {
        bool effectPass = textured && (pass == 1);
        g_ctx->OMSetBlendState(effectPass ? g_blendAlpha.Get() : g_blendOpaque.Get(), bf, 0xffffffff);
        g_ctx->OMSetDepthStencilState(effectPass ? g_dssNoWrite.Get() : g_dss.Get(), 0);
        for (const auto& s : g_subs) {
            const MaterialGPU* mat = (s.matIndex < g_mats.size()) ? &g_mats[s.matIndex] : nullptr;
            bool isEffect = mat && mat->isEffect;
            if (textured && isEffect != effectPass) continue;

            CB cb{};
            cb.model = model;
            cb.mvp = mvp;
            cb.lx = L.x; cb.ly = L.y; cb.lz = L.z;
            cb.hasSkin = (s.hasSkin && g_skin_ok) ? 1.0f : 0.0f;
            if (textured) {
                cb.hasTex = (mat && mat->srv) ? 1.0f : 0.0f;
                cb.hasNormal = (mat && mat->srvNormal) ? 1.0f : 0.0f;
                cb.isEffect = isEffect ? 1.0f : 0.0f;
                cb.tintR = mat ? mat->tint[0] : 1.0f;
                cb.tintG = mat ? mat->tint[1] : 1.0f;
                cb.tintB = mat ? mat->tint[2] : 1.0f;
                cb.tintA = mat ? mat->tint[3] : 1.0f;
            } else {
                cb.hasTex = 0.0f; cb.hasNormal = 0.0f; cb.isEffect = 0.0f;
                cb.tintR = cb.tintG = cb.tintB = cb.tintA = 1.0f;
            }
            D3D11_MAPPED_SUBRESOURCE ms;
            if (SUCCEEDED(g_ctx->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                std::memcpy(ms.pData, &cb, sizeof cb);
                g_ctx->Unmap(g_cb.Get(), 0);
            }
            ID3D11ShaderResourceView* srvs[2] = {textured && mat ? mat->srv.Get() : nullptr,
                                                 textured && mat ? mat->srvNormal.Get() : nullptr};
            g_ctx->PSSetShaderResources(0, 2, srvs);
            g_ctx->DrawIndexed(s.indexCount, s.indexStart, 0);
        }
    }
    } // end reconstruction path (!useGame)

    // Skeleton overlay: bind-pose bones as green lines, drawn over the mesh with
    // depth test off so the rig stays visible regardless of orientation. Reuses
    // g_cb (uMVP is its leading field, already holding the current mvp).
    if (g_show_skeleton && g_has_skeleton && g_skelVb && g_lineVs && g_linePs && g_lineIl) {
        g_ctx->OMSetBlendState(g_blendOpaque.Get(), bf, 0xffffffff);
        g_ctx->OMSetDepthStencilState(g_dssNoDepth.Get(), 0);
        g_ctx->RSSetState(g_rsSolid.Get());
        UINT lstride = sizeof(LineVtx), loff = 0;
        ID3D11Buffer* lvbs[] = {g_skelVb.Get()};
        g_ctx->IASetInputLayout(g_lineIl.Get());
        g_ctx->IASetVertexBuffers(0, 1, lvbs, &lstride, &loff);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        g_ctx->VSSetShader(g_lineVs.Get(), nullptr, 0);
        g_ctx->PSSetShader(g_linePs.Get(), nullptr, 0);
        ID3D11Buffer* lcbs[] = {g_cb.Get()};
        g_ctx->VSSetConstantBuffers(0, 1, lcbs);
        // g_cb still holds the last submesh's CB; its mvp is the shared model mvp.
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(g_ctx->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            CB cb{};
            cb.mvp = mvp;
            std::memcpy(ms.pData, &cb, sizeof cb);
            g_ctx->Unmap(g_cb.Get(), 0);
        }
        g_ctx->Draw(g_skel_vert_count, 0);
    }

    g_swap->Present(1, 0);
}

void save_screenshot(const char* path) {
    if (!g_swap || !g_dev || !g_ctx) return;
    render(); // ensure the latest frame is drawn
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    D3D11_TEXTURE2D_DESC td; back->GetDesc(&td);
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> stg;
    if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &stg))) return;
    g_ctx->CopyResource(stg.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g_ctx->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &ms))) return;
    int w = (int)td.Width, h = (int)td.Height;
    int rowBytes = w * 3, pad = (4 - (rowBytes & 3)) & 3, stride = rowBytes + pad;
    uint32_t dataSize = stride * h, fileSize = 54 + dataSize;
    std::vector<uint8_t> bmp(fileSize, 0);
    uint8_t hdr[54] = {'B','M'};
    std::memcpy(hdr + 2, &fileSize, 4); uint32_t off = 54; std::memcpy(hdr + 10, &off, 4);
    uint32_t ih = 40; std::memcpy(hdr + 14, &ih, 4); std::memcpy(hdr + 18, &w, 4); std::memcpy(hdr + 22, &h, 4);
    uint16_t planes = 1, bpp = 24; std::memcpy(hdr + 26, &planes, 2); std::memcpy(hdr + 28, &bpp, 2);
    std::memcpy(bmp.data(), hdr, 54);
    const uint8_t* src = static_cast<const uint8_t*>(ms.pData);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = src + (size_t)(h - 1 - y) * ms.RowPitch; // BMP is bottom-up
        uint8_t* dst = bmp.data() + 54 + (size_t)y * stride;
        for (int x = 0; x < w; ++x) { dst[x*3+0] = row[x*4+2]; dst[x*3+1] = row[x*4+1]; dst[x*3+2] = row[x*4+0]; }
    }
    g_ctx->Unmap(stg.Get(), 0);
    FILE* f = std::fopen(path, "wb");
    if (f) { std::fwrite(bmp.data(), 1, bmp.size(), f); std::fclose(f); }
}

} // namespace gw2m3d
