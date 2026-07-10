// gw2gsviewer.cpp -- headless renderer that draws a GW2 model using the GAME's
// OWN bgfx DXBC shaders pulled from gw2.dat, one per submesh/material.
//
// Per material: AMAT at (baseId-1 of material.filename texture) -> extractAmat()
// picks a matched VS+PS -> parse bgfx blob -> DXBC -> Create*Shader -> input
// layout from VS reflection -> cbuffers filled by uniform name at byte offset
// (uniform `reg` = byte offset; matrices transposed for bgfx column-major) ->
// textures bound per PS sampler slot. Each submesh drawn with its own material.
//
// Build: build_gs.ps1.  Usage:
//   interactive window (LMB-drag = orbit, wheel = zoom):
//     gw2gsviewer --dat <Gw2.dat> --template <json> --index <modelBaseId>
//   headless one-frame PNG (add --shot):
//     gw2gsviewer --dat <Gw2.dat> --template <json> --index <modelBaseId> --shot out.png
//   env: BLOOM=1 (emissive bloom, off by default), GW2_ENVCUBE=<0-255> (env-cube grey)

#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
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
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "gw2dat.h"
#include "cmp_decompress_method0.hpp"
#include "gw2_atex.hpp"
#include "gw2model.hpp"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static void die(const char* m){ std::fprintf(stderr,"ERROR: %s\n",m); std::exit(1); }

// ---- tiny math (row-major; transposed on upload for bgfx column-major) ----
struct M4{ float m[16]; };
static M4 ident(){ M4 r{}; r.m[0]=r.m[5]=r.m[10]=r.m[15]=1; return r; }
static M4 mul(const M4&a,const M4&b){ M4 r{}; for(int i=0;i<4;i++)for(int j=0;j<4;j++){float s=0;for(int k=0;k<4;k++)s+=a.m[i*4+k]*b.m[k*4+j];r.m[i*4+j]=s;}return r;}
static M4 transpose(const M4&a){ M4 r{}; for(int i=0;i<4;i++)for(int j=0;j<4;j++)r.m[i*4+j]=a.m[j*4+i]; return r; }
struct V3{float x,y,z;};
static V3 sub(V3 a,V3 b){return{a.x-b.x,a.y-b.y,a.z-b.z};}
static V3 cross(V3 a,V3 b){return{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
static float dot(V3 a,V3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static V3 nrm(V3 a){float l=std::sqrt(dot(a,a));return l>0?V3{a.x/l,a.y/l,a.z/l}:a;}
static M4 persp(float fovy,float asp,float zn,float zf){float f=1/std::tan(fovy/2);M4 r{};r.m[0]=f/asp;r.m[5]=f;r.m[10]=zf/(zf-zn);r.m[11]=1;r.m[14]=-zn*zf/(zf-zn);return r;}
static M4 lookat(V3 e,V3 a,V3 up){V3 z=nrm(sub(a,e)),x=nrm(cross(up,z)),y=cross(z,x);M4 r=ident();
    r.m[0]=x.x;r.m[1]=y.x;r.m[2]=z.x;r.m[4]=x.y;r.m[5]=y.y;r.m[6]=z.y;r.m[8]=x.z;r.m[9]=y.z;r.m[10]=z.z;
    r.m[12]=-dot(x,e);r.m[13]=-dot(y,e);r.m[14]=-dot(z,e);return r;}

// ---- bgfx blob parse (v11) -> raw DXBC + uniform table ----
struct Uni{ std::string name; int type; int stage; int byteOff; int vec4s; };
struct BgfxSh{ std::vector<uint8_t> dxbc; std::vector<Uni> uniforms; uint16_t constBuf=0; };
static BgfxSh parse_bgfx(const std::vector<uint8_t>& d){
    BgfxSh o; size_t p=0;
    if(d.size()<8||(d[0]!='V'&&d[0]!='F'&&d[0]!='C')) return o;
    p=3; int ver=d[p++]; p+=4; if(ver>=6)p+=4;
    uint16_t cnt=d[p]|(d[p+1]<<8); p+=2;
    for(int i=0;i<cnt;i++){
        int nl=d[p++]; std::string nm((const char*)&d[p],nl); p+=nl;
        int type=d[p++]; p++;
        int reg=d[p]|(d[p+1]<<8); p+=2;
        int rc=d[p]|(d[p+1]<<8); p+=2;
        if(ver>=8)p+=2; if(ver>=10)p+=2;
        int stage=(type&0x20)?2:(type&0x10)?1:0;
        o.uniforms.push_back({nm,type&0x0F,stage,reg,rc});
    }
    uint32_t cs=d[p]|(d[p+1]<<8)|(d[p+2]<<16)|((uint32_t)d[p+3]<<24); p+=4;
    o.dxbc.assign(d.begin()+p,d.begin()+p+cs); p+=cs;
    int ac=d[p++]; p+=ac*2; o.constBuf=d[p]|(d[p+1]<<8);
    return o;
}

static std::vector<uint8_t> decomp(Gw2Dat& dat,uint32_t idx){
    const MftData& e=dat.mft_data_list[idx];
    std::vector<uint8_t> raw=read_entry_bytes(dat.file_info.file_path,e);
    std::vector<uint8_t> s=gw2cmp::strip_crc32(std::span<const uint8_t>(raw));
    if(e.compression_flag==0) return s;
    uint32_t u=s[4]|(s[5]<<8)|(s[6]<<16)|((uint32_t)s[7]<<24);
    return gw2cmp::decompress_method0(std::span<const uint8_t>(s).subspan(8),u);
}

static ID3D11Device* g_dev; static ID3D11DeviceContext* g_ctx;
static ID3D11ShaderResourceView* make_tex(std::vector<uint8_t>& rgba,int w,int h){
    D3D11_TEXTURE2D_DESC td{}; td.Width=w;td.Height=h;td.MipLevels=1;td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem=rgba.data();sd.SysMemPitch=w*4;
    ID3D11Texture2D* t=nullptr; ID3D11ShaderResourceView* v=nullptr;
    if(SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev,&td,&sd,&t))){ ID3D11Device_CreateShaderResourceView(g_dev,(ID3D11Resource*)t,nullptr,&v); ID3D11Texture2D_Release(t);} return v;
}
// 1x1 constant-color SRV -- used to stand in for a GLOBAL engine texture we
// don't have offline (chiefly gSs14 = the screen-space light-accumulation
// buffer). Neutral white here means "fully lit", so albedo shows at face value.
static ID3D11ShaderResourceView* make_solid(uint8_t r,uint8_t g,uint8_t b,uint8_t a){ std::vector<uint8_t> px={r,g,b,a}; return make_tex(px,1,1); }
// 1x1x6 constant-color CUBE SRV -- stand-in for a GLOBAL environment cubemap
// (reflection/irradiance, slot 13) that we don't have offline. Must be an actual
// cube: the PS declares t13 as texturecube, and a mismatched 2D bound there
// samples as white -> mirror-white metals that bloom into a glow. A neutral grey
// gives subtle, believable reflections instead of a blown-out mirror.
static ID3D11ShaderResourceView* make_solid_cube(uint8_t r,uint8_t g,uint8_t b,uint8_t a){
    uint8_t px[4]={r,g,b,a};
    D3D11_TEXTURE2D_DESC td{}; td.Width=1;td.Height=1;td.MipLevels=1;td.ArraySize=6;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;td.MiscFlags=D3D11_RESOURCE_MISC_TEXTURECUBE;
    D3D11_SUBRESOURCE_DATA sd[6]; for(int i=0;i<6;i++){ sd[i].pSysMem=px; sd[i].SysMemPitch=4; sd[i].SysMemSlicePitch=4; }
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format=td.Format; vd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURECUBE; vd.TextureCube.MipLevels=1;
    ID3D11Texture2D* t=nullptr; ID3D11ShaderResourceView* v=nullptr;
    if(SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev,&td,sd,&t))){ ID3D11Device_CreateShaderResourceView(g_dev,(ID3D11Resource*)t,&vd,&v); ID3D11Texture2D_Release(t);} return v;
}
static std::string arg(int c,char**v,const char*k,const char*d=""){for(int i=1;i<c-1;i++)if(!strcmp(v[i],k))return v[i+1];return d;}

// Per-material GPU state built from its game shaders.
struct MatGPU {
    ID3D11VertexShader* vs=nullptr; ID3D11PixelShader* ps=nullptr; ID3D11InputLayout* layout=nullptr;
    ID3D11Buffer* vcb=nullptr; ID3D11Buffer* pcb=nullptr;
    ID3D11ShaderResourceView* srv[16]={};
    uint64_t rstate=0;                 // bgfx state word from the AMAT effect
    ID3D11BlendState* blend=nullptr;   // non-null => translucent (drawn in pass 2)
    bool depthWrite=true;
    bool ok=false;
    BgfxSh vsSh, psSh;                  // uniform tables, for per-frame cbuffer refill
};

// bgfx 64-bit state -> D3D11 blend. Blend nibbles at shifts 12/16/20/24
// (srcRGB/dstRGB/srcA/dstA); BGFX_STATE_WRITE_Z = 0x40'00000000.
static D3D11_BLEND bgfx_blend(uint32_t v){
    switch(v){
        case 0x1: return D3D11_BLEND_ZERO;        case 0x2: return D3D11_BLEND_ONE;
        case 0x3: return D3D11_BLEND_SRC_COLOR;   case 0x4: return D3D11_BLEND_INV_SRC_COLOR;
        case 0x5: return D3D11_BLEND_SRC_ALPHA;   case 0x6: return D3D11_BLEND_INV_SRC_ALPHA;
        case 0x7: return D3D11_BLEND_DEST_ALPHA;  case 0x8: return D3D11_BLEND_INV_DEST_ALPHA;
        case 0x9: return D3D11_BLEND_DEST_COLOR;  case 0xa: return D3D11_BLEND_INV_DEST_COLOR;
        case 0xb: return D3D11_BLEND_SRC_ALPHA_SAT;
        default:  return D3D11_BLEND_ONE;
    }
}

// Size the cbuffer to the largest register the shader actually reads (bgfx
// constBufSize can be smaller than the DXBC's CB0 declaration -> cutting off
// later uniforms like TexTransform; that starved UVs and blew the output white).
static UINT cb_size(const BgfxSh& sh){
    UINT need=sh.constBuf;
    for(auto&u:sh.uniforms){ if(u.type==0)continue; UINT end=(UINT)u.byteOff+(UINT)u.vec4s*16; if(end>need)need=end; }
    UINT sz=(need+15)&~15u; return sz<16?16:sz;
}
static void fill_cb(std::vector<uint8_t>& buf, const BgfxSh& sh, std::map<std::string,std::vector<float>>& vals){
    std::fill(buf.begin(),buf.end(),(uint8_t)0);
    for(auto&u:sh.uniforms){ if(u.type==0)continue; auto it=vals.find(u.name); if(it==vals.end())continue;
        size_t n=std::min((size_t)u.vec4s*16,it->second.size()*4);
        if(u.byteOff+n<=buf.size()) std::memcpy(buf.data()+u.byteOff,it->second.data(),n); }
}
// DYNAMIC constant buffer -- refilled each frame from `vals` so the orbit camera
// (ViewProjection/WorldView/CameraPosition) can change interactively.
static ID3D11Buffer* build_cb(const BgfxSh& sh, std::map<std::string,std::vector<float>>& vals){
    UINT sz=cb_size(sh); std::vector<uint8_t> buf(sz,0); fill_cb(buf,sh,vals);
    if(getenv("CBDUMP")){ std::fprintf(stderr,"  CB(size=%u):",sz);
        for(auto&u:sh.uniforms){ if(u.type==0)continue; const float* f=(const float*)(buf.data()+u.byteOff);
            std::fprintf(stderr,"\n    @%3d %-22s = %.3f %.3f %.3f %.3f",u.byteOff,u.name.c_str(),f[0],f[1],f[2],f[3]); }
        std::fprintf(stderr,"\n"); }
    D3D11_BUFFER_DESC bd{}; bd.Usage=D3D11_USAGE_DYNAMIC;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;bd.ByteWidth=sz;
    D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem=buf.data(); ID3D11Buffer* b=nullptr; ID3D11Device_CreateBuffer(g_dev,&bd,&sd,&b); return b;
}
static void update_cb(ID3D11Buffer* b, const BgfxSh& sh, std::map<std::string,std::vector<float>>& vals){
    if(!b) return; UINT sz=cb_size(sh); std::vector<uint8_t> buf(sz,0); fill_cb(buf,sh,vals);
    D3D11_MAPPED_SUBRESOURCE m;
    if(SUCCEEDED(ID3D11DeviceContext_Map(g_ctx,(ID3D11Resource*)b,0,D3D11_MAP_WRITE_DISCARD,0,&m))){
        std::memcpy(m.pData,buf.data(),sz); ID3D11DeviceContext_Unmap(g_ctx,(ID3D11Resource*)b,0); }
}
static UINT sem_off(const char* s,UINT idx){
    if(!strcmp(s,"POSITION"))return 0; if(!strcmp(s,"NORMAL"))return 12;
    if(!strcmp(s,"TANGENT"))return 24; if(!strcmp(s,"BINORMAL")||!strcmp(s,"BITANGENT"))return 36;
    // Vertex: UV0(u,v)@48, then uv[7][2]@56 -> TEXCOORD0..7 at 48,56,64,...,104
    if(!strcmp(s,"TEXCOORD"))return 48+8*(idx>7?7:idx);
    return 0;
}

static MatGPU build_material(Gw2Dat& dat, const nlohmann::json& tpl, const gw2model::Material& mat,
                             std::map<std::string,std::vector<float>>& vals){
    MatGPU g;
    uint32_t fnBase=get_by_base_id(dat, mat.materialFile);
    printf("Material\n");
    printf("File Id : %d\n", mat.materialFile);
    printf("Base Id : %d\n", fnBase);
    if(!fnBase) return g;
    std::vector<uint8_t> amat; try{ amat=decomp(dat, fnBase-1);}catch(...){ return g; }
    gw2model::AmatSet set=gw2model::Extractor(amat,tpl).extractAmat();
    // sortLayer>0 on the MODL material means this submesh is meant to draw in
    // the transparent/sorted pass -- if the AMAT actually defines one (passFlags
    // & AMAT_PASSFLAG_TRANSPARENT), use ITS effect/renderState instead of the
    // best-by-sampler-count opaque one (which previously won even for
    // genuinely transparent materials, e.g. water/glass/fx planes).
    bool useTrans = mat.sortLayer>0 && set.hasTrans;
    int vsIdx = useTrans? set.transVsIndex : set.vsIndex;
    int psIdx = useTrans? set.transPsIndex : set.psIndex;
    if(vsIdx<0||psIdx<0) return g;
    BgfxSh vs=parse_bgfx(set.shaders[vsIdx].dxbc), ps=parse_bgfx(set.shaders[psIdx].dxbc);
    if(vs.dxbc.empty()||ps.dxbc.empty()) return g;
    std::fprintf(stderr,"[mat %u] sortLayer=%u useTrans=%d vsIdx=%d psIdx=%d vsDXBC=%zu psDXBC=%zu\n",
                 mat.index,mat.sortLayer,useTrans,vsIdx,psIdx,vs.dxbc.size(),ps.dxbc.size());
    if(getenv("DUMPDXBC")){ char pth[256];
        std::snprintf(pth,sizeof pth,"%s\\mat%u_ps.cso",getenv("DUMPDXBC"),mat.index);
        std::ofstream(pth,std::ios::binary).write((const char*)ps.dxbc.data(),ps.dxbc.size());
        std::snprintf(pth,sizeof pth,"%s\\mat%u_vs.cso",getenv("DUMPDXBC"),mat.index);
        std::ofstream(pth,std::ios::binary).write((const char*)vs.dxbc.data(),vs.dxbc.size());
    }
    if(FAILED(ID3D11Device_CreateVertexShader(g_dev,vs.dxbc.data(),vs.dxbc.size(),nullptr,&g.vs))) return g;
    if(FAILED(ID3D11Device_CreatePixelShader(g_dev,ps.dxbc.data(),ps.dxbc.size(),nullptr,&g.ps))) return g;
    // Slot 13 is GW2's fixed engine-global ENV cubemap sampler (reflection +
    // irradiance), always declared texturecube. (Can't detect via D3DReflect --
    // bgfx strips the DXBC RDEF chunk, so boundResources=0.)
    const int kEnvCubeSlot=13;
    // input layout from VS reflection
    ID3D11ShaderReflection* refl=nullptr;
    if(FAILED(D3DReflect(vs.dxbc.data(),vs.dxbc.size(),IID_ID3D11ShaderReflection,(void**)&refl))) return g;
    D3D11_SHADER_DESC sd{}; refl->lpVtbl->GetDesc(refl,&sd);
    std::vector<std::string> sem(sd.InputParameters);
    std::vector<D3D11_INPUT_ELEMENT_DESC> il;
    for(UINT i=0;i<sd.InputParameters;i++){ D3D11_SIGNATURE_PARAMETER_DESC pd{}; refl->lpVtbl->GetInputParameterDesc(refl,i,&pd); sem[i]=pd.SemanticName; }
    for(UINT i=0;i<sd.InputParameters;i++){ D3D11_SIGNATURE_PARAMETER_DESC pd{}; refl->lpVtbl->GetInputParameterDesc(refl,i,&pd);
        D3D11_INPUT_ELEMENT_DESC e{}; e.SemanticName=sem[i].c_str(); e.SemanticIndex=pd.SemanticIndex;
        e.Format=(pd.Mask<=3)?DXGI_FORMAT_R32G32_FLOAT:DXGI_FORMAT_R32G32B32_FLOAT;
        e.AlignedByteOffset=sem_off(sem[i].c_str(),pd.SemanticIndex); e.InputSlotClass=D3D11_INPUT_PER_VERTEX_DATA; il.push_back(e); }
    if(FAILED(ID3D11Device_CreateInputLayout(g_dev,il.data(),(UINT)il.size(),vs.dxbc.data(),vs.dxbc.size(),&g.layout))) return g;
    g.vcb=build_cb(vs,vals); g.pcb=build_cb(ps,vals);
    // textures per PS sampler binding
    std::fprintf(stderr,"[mat %u] textures=%zu samplers:",mat.index,mat.textures.size());
    for(auto& b:set.shaders[psIdx].samplers) std::fprintf(stderr," idx%d->slot%d",b.textureIndex,b.textureSlot);
    std::fprintf(stderr,"\n");
    printf("Textures\n");

    for(auto& b:set.shaders[psIdx].samplers){
        if(b.textureSlot>=16) continue;
        // A sampler whose textureIndex is beyond the material's own texture list
        // is a GLOBAL engine texture (e.g. gSs14 = the deferred light buffer,
        // token textureIndex 34). At runtime the engine binds a screen-space
        // target there; offline we substitute a neutral-white constant so the
        // shader's `albedo * lightBuffer` term isn't multiplied by black.
        if(b.textureIndex>=mat.textures.size()){
            if(b.textureSlot==kEnvCubeSlot){
                // env reflection/irradiance cube: neutral grey, not white-mirror
                int lv = getenv("GW2_ENVCUBE")? atoi(getenv("GW2_ENVCUBE")) : 64;
                g.srv[b.textureSlot]=make_solid_cube((uint8_t)lv,(uint8_t)lv,(uint8_t)lv,255);
                std::fprintf(stderr,"  slot%d = GLOBAL(env-cube grey %d)\n",b.textureSlot,lv);
            } else {
                g.srv[b.textureSlot]=make_solid(255,255,255,255);
                std::fprintf(stderr,"  slot%d = GLOBAL(white)\n",b.textureSlot);
            }
            continue; }
        if(getenv("TESTALBEDO")&&b.textureSlot==0){ g.srv[0]=make_solid(255,0,0,255); std::fprintf(stderr,"  slot0 = TEST RED\n"); continue; }
        uint32_t fid=mat.textures[b.textureIndex].fileId; if(!fid){std::fprintf(stderr,"  slot%d idx%d fid=0\n",b.textureSlot,b.textureIndex);continue;}
        uint32_t tb=get_by_base_id(dat,fid);
        printf("File Id : %d\n", fid);
        printf("Base Id : %d\n", tb);
        if(!tb){std::fprintf(stderr,"  slot%d idx%d fid=%u NOBASE\n",b.textureSlot,b.textureIndex,fid);continue;}
        try{ std::vector<uint8_t> tx=decomp(dat,tb-1); if(tx.size()>=4&&tx[0]=='C')tx[0]='A';
             gw2atex::Texture t=gw2atex::parse(tx.data(),tx.size()); gw2atex::Image im=gw2atex::decode(t,0);
             if(im.width>0){ g.srv[b.textureSlot]=make_tex(im.rgba,im.width,im.height);
               std::fprintf(stderr,"  slot%d idx%d fid=%u base=%u %dx%d fmt=%s\n",b.textureSlot,b.textureIndex,fid,tb,im.width,im.height,t.fmt_name.c_str());
             } else std::fprintf(stderr,"  slot%d idx%d fid=%u DECODE0\n",b.textureSlot,b.textureIndex,fid);
        }catch(...){ std::fprintf(stderr,"  slot%d idx%d fid=%u EXC\n",b.textureSlot,b.textureIndex,fid); }
    }
    // ---- blend / depth from the AMAT effect's bgfx state word ----
    uint64_t st=useTrans? set.transRenderState : set.renderState;
    g.rstate=st;
    uint32_t srcRGB=(st>>12)&0xf, dstRGB=(st>>16)&0xf, srcA=(st>>20)&0xf, dstA=(st>>24)&0xf;
    bool blendOn=(st & 0x0ffff000ull)!=0;           // any blend nibble set
    g.depthWrite=(st & 0x0000004000000000ull)!=0;   // BGFX_STATE_WRITE_Z
    std::fprintf(stderr,"  rstate=0x%016llx blend=%d src/dst RGB=%x/%x A=%x/%x writeZ=%d\n",
                 (unsigned long long)st,blendOn,srcRGB,dstRGB,srcA,dstA,g.depthWrite);
    if(blendOn){
        D3D11_BLEND_DESC bd{}; bd.RenderTarget[0].BlendEnable=TRUE;
        bd.RenderTarget[0].SrcBlend=bgfx_blend(srcRGB); bd.RenderTarget[0].DestBlend=bgfx_blend(dstRGB);
        bd.RenderTarget[0].BlendOp=D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha=bgfx_blend(srcA?srcA:2); bd.RenderTarget[0].DestBlendAlpha=bgfx_blend(dstA?dstA:1);
        bd.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        ID3D11Device_CreateBlendState(g_dev,&bd,&g.blend);
    }
    g.vsSh=std::move(vs); g.psSh=std::move(ps);   // keep uniform tables for per-frame refill
    g.ok=true; return g;
}

// ---- interactive orbit-camera state (driven by WndProc) ----
static float g_yaw=0.7f, g_pitch=0.35f, g_dist=1.8f;
static bool g_drag=false; static POINT g_last{};
static LRESULT CALLBACK WndProc(HWND h,UINT m,WPARAM w,LPARAM l){
    switch(m){
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_LBUTTONDOWN: g_drag=true; g_last={(short)LOWORD(l),(short)HIWORD(l)}; SetCapture(h); return 0;
        case WM_LBUTTONUP: g_drag=false; ReleaseCapture(); return 0;
        case WM_MOUSEMOVE: if(g_drag){ int x=(short)LOWORD(l),y=(short)HIWORD(l);
            g_yaw+=(x-g_last.x)*0.01f; g_pitch+=(y-g_last.y)*0.01f;
            if(g_pitch>1.5f)g_pitch=1.5f; if(g_pitch<-1.5f)g_pitch=-1.5f; g_last={x,y}; } return 0;
        case WM_MOUSEWHEEL: g_dist*=(GET_WHEEL_DELTA_WPARAM(w)>0?0.9f:1.1f); if(g_dist<0.15f)g_dist=0.15f; return 0;
    }
    return DefWindowProc(h,m,w,l);
}

int main(int argc,char**argv){
    std::string datp=arg(argc,argv,"--dat"), tplp=arg(argc,argv,"--template"), shot=arg(argc,argv,"--shot","");
    bool headless=!shot.empty();   // --shot => render one frame to PNG and exit; else interactive window
    uint32_t idx=(uint32_t)std::stoul(arg(argc,argv,"--index","291982"));
    int W=1280,H=1024;
    nlohmann::json tpl; { std::ifstream f(tplp,std::ios::binary); if(!f)die("template"); f>>tpl; }
    Gw2Dat dat; load_dat_file(dat,datp);

    std::vector<uint8_t> modl=decomp(dat,idx);
    gw2model::Model model=gw2model::Extractor(modl,tpl).extract();
    if(model.meshes.empty()) die("no meshes");

    // combined VB/IB + submeshes
    std::vector<gw2model::Vertex> V; std::vector<uint32_t> I;
    struct Sub{UINT start,count;uint32_t mat; V3 ctr; float dist;}; std::vector<Sub> subs;
    V3 lo{1e9f,1e9f,1e9f},hi{-1e9f,-1e9f,-1e9f};
    for(const auto&m:model.meshes){ UINT vo=(UINT)V.size(); Sub s{(UINT)I.size(),0,m.materialIndex,{0,0,0},0};
        V3 acc{0,0,0};
        for(const auto&v:m.vertices){ V.push_back(v); acc={acc.x+v.px,acc.y+v.py,acc.z+v.pz}; lo={std::min(lo.x,v.px),std::min(lo.y,v.py),std::min(lo.z,v.pz)};hi={std::max(hi.x,v.px),std::max(hi.y,v.py),std::max(hi.z,v.pz)};}
        for(uint32_t i:m.indices) I.push_back(vo+i); s.count=(UINT)I.size()-s.start;
        if(!m.vertices.empty()) s.ctr={acc.x/m.vertices.size(),acc.y/m.vertices.size(),acc.z/m.vertices.size()};
        if(s.count)subs.push_back(s); }
    V3 c{(lo.x+hi.x)/2,(lo.y+hi.y)/2,(lo.z+hi.z)/2}; V3 ext=sub(hi,lo); float rad=0.5f*std::sqrt(dot(ext,ext)); if(rad<1e-3f)rad=1;
    std::fprintf(stderr,"[geo] V=%zu I=%zu subs=%zu bbox lo(%.1f %.1f %.1f) hi(%.1f %.1f %.1f) rad=%.1f\n",V.size(),I.size(),subs.size(),lo.x,lo.y,lo.z,hi.x,hi.y,hi.z,rad);

    // window (hidden in headless --shot mode) + swap chain. We always render to
    // an offscreen RT `rt` (needed as an SRV for bloom) and copy it to the
    // backbuffer for presentation; fixed size so no resize handling is needed.
    WNDCLASSA wc{}; wc.lpfnWndProc=WndProc; wc.hInstance=GetModuleHandle(nullptr); wc.lpszClassName="gw2gswnd"; wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    RegisterClassA(&wc);
    DWORD wstyle=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
    RECT wr{0,0,W,H}; AdjustWindowRect(&wr,wstyle,FALSE);
    HWND hwnd=CreateWindowA("gw2gswnd","gw2gsviewer (game shaders) -- LMB-drag: orbit  wheel: zoom",wstyle,
                            CW_USEDEFAULT,CW_USEDEFAULT,wr.right-wr.left,wr.bottom-wr.top,nullptr,nullptr,wc.hInstance,nullptr);
    ShowWindow(hwnd, headless?SW_HIDE:SW_SHOW);
    DXGI_SWAP_CHAIN_DESC scd{}; scd.BufferCount=1; scd.BufferDesc.Width=W; scd.BufferDesc.Height=H;
    scd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; scd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow=hwnd; scd.SampleDesc.Count=1; scd.Windowed=TRUE; scd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl=D3D_FEATURE_LEVEL_11_0; IDXGISwapChain* swap=nullptr;
    if(FAILED(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,&fl,1,D3D11_SDK_VERSION,&scd,&swap,&g_dev,nullptr,&g_ctx))) die("device+swapchain");
    ID3D11Texture2D* back=nullptr; IDXGISwapChain_GetBuffer(swap,0,IID_ID3D11Texture2D,(void**)&back);

    ID3D11Buffer *vb=nullptr,*ib=nullptr;
    { D3D11_BUFFER_DESC bd{}; D3D11_SUBRESOURCE_DATA sd{}; bd.Usage=D3D11_USAGE_IMMUTABLE;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;
      bd.ByteWidth=(UINT)(V.size()*sizeof(gw2model::Vertex)); sd.pSysMem=V.data(); ID3D11Device_CreateBuffer(g_dev,&bd,&sd,&vb);
      bd.BindFlags=D3D11_BIND_INDEX_BUFFER; bd.ByteWidth=(UINT)(I.size()*4); sd.pSysMem=I.data(); ID3D11Device_CreateBuffer(g_dev,&bd,&sd,&ib); }

    // shared uniform values -- camera (recomputed each frame from the orbit
    // state) + a best-effort lighting/fog environment (static).
    M4 world=ident(); V3 camEye{};
    std::map<std::string,std::vector<float>> vals;
    auto put=[&](const char*n,std::initializer_list<float> f){ vals[n]=std::vector<float>(f); };
    auto computeCam=[&](){
        camEye=V3{ c.x+rad*g_dist*std::cos(g_pitch)*std::cos(g_yaw),
                   c.y+rad*g_dist*std::sin(g_pitch),
                   c.z+rad*g_dist*std::cos(g_pitch)*std::sin(g_yaw) };
        M4 view=lookat(camEye,c,{0,1,0}), proj=persp(0.9f,(float)W/H,rad*0.02f,rad*40);
        M4 vp=mul(world,mul(view,proj)), wv=mul(world,view);
        auto pM=[&](const char*n,const M4&m){ M4 t=transpose(m); vals[n]=std::vector<float>(t.m,t.m+16); };
        pM("ViewProjection",vp); pM("WorldViewProjection",vp); pM("WorldView",wv); pM("World",world);
        vals["CameraPosition"]=std::vector<float>{camEye.x,camEye.y,camEye.z,1};
    };
    computeCam();
    put("TexTransform0A",{1,0,0,0}); put("TexTransform0B",{0,1,0,0});
    put("TexTransform1A",{1,0,0,0}); put("TexTransform1B",{0,1,0,0});
    // The light-prepass PS does `light = t14.rgba * LightBuffer.y` then
    // `color = albedo * light.rgb + spec`. LightBuffer.y is the light-buffer
    // decode scale -- if it's 0 (the old default) the whole lit term is zeroed
    // and the model renders black. Feed 1.0 so our neutral-white t14 = full light.
    put("LightBuffer",{1,1,1,1});
    // fxclr = the stipple/fade "visibility" uniform on deferred shaders. Zero =
    // fully faded out -> the stipple-dither `discard` rejects EVERY pixel (model
    // renders empty). Full = fully visible, no fade discard. StippleDensity 0 =
    // no dither. (Verified on 291976: fxclr 0 -> blank, fxclr 1 -> draws.)
    put("fxclr",{1,1,1,1}); put("StippleDensity",{0,0,0,0}); put("StencilId",{0,0,0,0});
    // ---- forward SH-lit shaders (e.g. world props) compute lighting IN the PS
    // from these uniforms; all-zero = black. Feed a plausible daylight rig:
    //   shChannel = (Lx,Ly,Lz, DC) : dp4(worldNormal4, shChannel) -> per-channel
    //   ambient (DC = flat term, xyz = directional sky). shSun = (sunDir,0);
    //   shSunColor = sun rgb; shSunData.xyz = sun dir for the spec half-vector.
    auto vlen=[&](float x,float y,float z){return std::sqrt(x*x+y*y+z*z);};
    float sdx=0.45f,sdy=0.80f,sdz=0.40f; { float l=vlen(sdx,sdy,sdz); sdx/=l;sdy/=l;sdz/=l; }
    put("shSun",{sdx,sdy,sdz,0});
    put("shSunColor",{1.25f,1.16f,1.00f,1});          // warm sun, slight overdrive
    put("shSunData",{sdx,sdy,sdz,0});
    put("shRed",  {0,0.10f,0,0.32f});                 // DC ambient + gentle sky-up term
    put("shGreen",{0,0.11f,0,0.36f});
    put("shBlue", {0,0.13f,0,0.46f});
    put("shSunColor",{1.25f,1.16f,1.00f,1});
    // legacy/other names kept near-zero (additive terms that would wash out)
    put("SunColor",{0,0,0,0}); put("SunDirection",{-0.4f,-0.8f,-0.4f,0});
    put("AmbientColor",{0,0,0,0}); put("AlphaRef",{0,0,0,0}); put("fadedif",{1,1,1,1});
    put("ScreenDims",{(float)W,(float)H,1.0f/W,1.0f/H});
    put("FogColorNearMinusFar",{0,0,0,0}); put("FogColorFar", getenv("FOGTEST")?std::initializer_list<float>{1,0,1,0}:std::initializer_list<float>{0,0,0,0}); put("FogColorHeight",{0,0,0,0});
    put("FogDepthCue",{0,0,0,0}); put("FogParam0",{0,0,0,0});
    put("WorldToShadowA",{0,0,0,0}); put("WorldToShadowB",{0,0,0,0}); put("WorldToShadowC",{0,0,0,0});

    // build per-material game shaders (cache by index)
    uint32_t maxi=0; for(auto&m:model.materials)maxi=std::max(maxi,m.index);
    std::vector<MatGPU> mats(maxi+1); int builtOk=0;
    for(auto&m:model.materials){ mats[m.index]=build_material(dat,tpl,m,vals); if(mats[m.index].ok)builtOk++; }
    std::fprintf(stderr,"model %u: %zu submeshes, %zu materials, %d built w/ game shaders\n",idx,subs.size(),model.materials.size(),builtOk);

    // render targets (scene RT also readable as SRV for the bloom post-pass)
    D3D11_TEXTURE2D_DESC rd{}; rd.Width=W;rd.Height=H;rd.MipLevels=1;rd.ArraySize=1;rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.SampleDesc.Count=1;rd.Usage=D3D11_USAGE_DEFAULT;rd.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* rt=nullptr; ID3D11Device_CreateTexture2D(g_dev,&rd,nullptr,&rt);
    ID3D11RenderTargetView* rtv=nullptr; ID3D11Device_CreateRenderTargetView(g_dev,(ID3D11Resource*)rt,nullptr,&rtv);
    ID3D11ShaderResourceView* rtSRV=nullptr; ID3D11Device_CreateShaderResourceView(g_dev,(ID3D11Resource*)rt,nullptr,&rtSRV);
    D3D11_TEXTURE2D_DESC dd=rd; dd.Format=DXGI_FORMAT_D24_UNORM_S8_UINT; dd.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D* dt=nullptr; ID3D11Device_CreateTexture2D(g_dev,&dd,nullptr,&dt);
    ID3D11DepthStencilView* dsv=nullptr; ID3D11Device_CreateDepthStencilView(g_dev,(ID3D11Resource*)dt,nullptr,&dsv);
    D3D11_RASTERIZER_DESC rsd{}; rsd.FillMode=D3D11_FILL_SOLID; rsd.CullMode=D3D11_CULL_NONE; rsd.DepthClipEnable=TRUE;
    ID3D11RasterizerState* rs=nullptr; ID3D11Device_CreateRasterizerState(g_dev,&rsd,&rs);
    D3D11_DEPTH_STENCIL_DESC dsd{}; dsd.DepthEnable=TRUE;dsd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dsd.DepthFunc=D3D11_COMPARISON_LESS;
    ID3D11DepthStencilState* dss=nullptr; ID3D11Device_CreateDepthStencilState(g_dev,&dsd,&dss);
    // transparent pass: depth test on, depth WRITE off
    D3D11_DEPTH_STENCIL_DESC dsdT=dsd; dsdT.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
    ID3D11DepthStencilState* dssT=nullptr; ID3D11Device_CreateDepthStencilState(g_dev,&dsdT,&dssT);
    D3D11_SAMPLER_DESC smp{}; smp.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR; smp.AddressU=smp.AddressV=smp.AddressW=D3D11_TEXTURE_ADDRESS_WRAP; smp.MaxLOD=D3D11_FLOAT32_MAX;
    ID3D11SamplerState* ss=nullptr; ID3D11Device_CreateSamplerState(g_dev,&smp,&ss); ID3D11SamplerState* samplers[16]; for(int i=0;i<16;i++)samplers[i]=ss;

    // ---- BLOOM (opt-in via env BLOOM): one-time setup, applied per-frame.
    // The brightness-only bright-pass haloes ordinary bright specular/white
    // surfaces (steel blades, saddle buckles, claw tips) into a fake glow, so
    // bloom is OFF by default; set BLOOM=1 for emissive showcases (jade-tech).
    bool useBloom=getenv("BLOOM")!=nullptr;
    int BW=W/2,BH=H/2;
    ID3D11VertexShader* fvs=nullptr; ID3D11PixelShader* pBright=nullptr,*pBlur=nullptr,*pComp=nullptr;
    ID3D11Texture2D *tA=nullptr,*tB=nullptr; ID3D11RenderTargetView *rvA=nullptr,*rvB=nullptr; ID3D11ShaderResourceView *svA=nullptr,*svB=nullptr;
    ID3D11SamplerState* ls=nullptr; ID3D11Buffer* bpcb=nullptr; ID3D11BlendState* addb=nullptr;
    float bthr=getenv("BLOOMTHR")?(float)atof(getenv("BLOOMTHR")):0.32f;
    float bint=getenv("BLOOMINT")?(float)atof(getenv("BLOOMINT")):1.5f;
    if(useBloom){
      auto compile=[&](const char* src,const char* entry,const char* tgt)->ID3DBlob*{
        ID3DBlob* b=nullptr; ID3DBlob* e=nullptr;
        if(FAILED(D3DCompile(src,strlen(src),nullptr,nullptr,nullptr,entry,tgt,0,0,&b,&e))){
          if(e) std::fprintf(stderr,"bloom compile err: %s\n",(char*)ID3D10Blob_GetBufferPointer(e)); return nullptr; }
        return b; };
      const char* fs=
        "struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};"
        "V VS(uint id:SV_VertexID){V o;o.uv=float2((id<<1)&2,id&2);o.p=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;}"
        "Texture2D t:register(t0);SamplerState s:register(s0);"
        "cbuffer P:register(b0){float4 pr;}"
        "float4 Bright(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{"
          "float3 c=t.Sample(s,uv).rgb;float l=max(max(c.r,c.g),c.b);"
          "float3 b=c*saturate((l-pr.x)/max(1e-3,1-pr.x));return float4(b*pr.y,1);}"
        "float4 Blur(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{"
          "float w[5]={0.204164,0.180174,0.123832,0.066282,0.027631};"
          "float3 c=t.Sample(s,uv).rgb*w[0];"
          "[unroll]for(int i=1;i<5;i++){c+=t.Sample(s,uv+pr.xy*i).rgb*w[i];c+=t.Sample(s,uv-pr.xy*i).rgb*w[i];}"
          "return float4(c,1);}"
        "float4 Comp(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{return float4(t.Sample(s,uv).rgb,1);}";
      ID3DBlob* vsb=compile(fs,"VS","vs_5_0"); ID3DBlob* bb=compile(fs,"Bright","ps_5_0");
      ID3DBlob* lbb=compile(fs,"Blur","ps_5_0"); ID3DBlob* cbb=compile(fs,"Comp","ps_5_0");
      if(vsb&&bb&&lbb&&cbb){
        ID3D11Device_CreateVertexShader(g_dev,ID3D10Blob_GetBufferPointer(vsb),ID3D10Blob_GetBufferSize(vsb),nullptr,&fvs);
        ID3D11Device_CreatePixelShader(g_dev,ID3D10Blob_GetBufferPointer(bb),ID3D10Blob_GetBufferSize(bb),nullptr,&pBright);
        ID3D11Device_CreatePixelShader(g_dev,ID3D10Blob_GetBufferPointer(lbb),ID3D10Blob_GetBufferSize(lbb),nullptr,&pBlur);
        ID3D11Device_CreatePixelShader(g_dev,ID3D10Blob_GetBufferPointer(cbb),ID3D10Blob_GetBufferSize(cbb),nullptr,&pComp);
        auto mkRT=[&](int w,int h,ID3D11Texture2D** tex,ID3D11RenderTargetView** rv,ID3D11ShaderResourceView** sv){
          D3D11_TEXTURE2D_DESC d{}; d.Width=w;d.Height=h;d.MipLevels=1;d.ArraySize=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
          ID3D11Device_CreateTexture2D(g_dev,&d,nullptr,tex); ID3D11Device_CreateRenderTargetView(g_dev,(ID3D11Resource*)*tex,nullptr,rv); ID3D11Device_CreateShaderResourceView(g_dev,(ID3D11Resource*)*tex,nullptr,sv); };
        mkRT(BW,BH,&tA,&rvA,&svA); mkRT(BW,BH,&tB,&rvB,&svB);
        D3D11_SAMPLER_DESC ld{}; ld.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR; ld.AddressU=ld.AddressV=ld.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; ld.MaxLOD=D3D11_FLOAT32_MAX;
        ID3D11Device_CreateSamplerState(g_dev,&ld,&ls);
        { D3D11_BUFFER_DESC bd{}; bd.Usage=D3D11_USAGE_DYNAMIC;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;bd.ByteWidth=16; ID3D11Device_CreateBuffer(g_dev,&bd,nullptr,&bpcb); }
        D3D11_BLEND_DESC ad{}; ad.RenderTarget[0].BlendEnable=TRUE; ad.RenderTarget[0].SrcBlend=D3D11_BLEND_ONE; ad.RenderTarget[0].DestBlend=D3D11_BLEND_ONE; ad.RenderTarget[0].BlendOp=D3D11_BLEND_OP_ADD; ad.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE; ad.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ONE; ad.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD; ad.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        ID3D11Device_CreateBlendState(g_dev,&ad,&addb);
        std::fprintf(stderr,"bloom: thr=%.2f intensity=%.2f\n",bthr,bint);
      } else useBloom=false;
    }
    ID3D11ShaderResourceView* nullSRV[1]={nullptr};
    float blendf[4]={0,0,0,0}; float clear[4]={0.10f,0.11f,0.13f,1};
    UINT vstride=sizeof(gw2model::Vertex),voff=0;
    auto setP=[&](float x,float y,float z,float w){ D3D11_MAPPED_SUBRESOURCE m; if(SUCCEEDED(ID3D11DeviceContext_Map(g_ctx,(ID3D11Resource*)bpcb,0,D3D11_MAP_WRITE_DISCARD,0,&m))){ float v[4]={x,y,z,w}; memcpy(m.pData,v,16); ID3D11DeviceContext_Unmap(g_ctx,(ID3D11Resource*)bpcb,0);} };
    auto fsPass=[&](ID3D11RenderTargetView* out,ID3D11ShaderResourceView* in,ID3D11PixelShader* ps,int w,int h){
      ID3D11DeviceContext_OMSetRenderTargets(g_ctx,1,&out,nullptr);
      D3D11_VIEWPORT v{0,0,(float)w,(float)h,0,1}; ID3D11DeviceContext_RSSetViewports(g_ctx,1,&v);
      ID3D11DeviceContext_IASetInputLayout(g_ctx,nullptr); ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ID3D11DeviceContext_VSSetShader(g_ctx,fvs,nullptr,0); ID3D11DeviceContext_PSSetShader(g_ctx,ps,nullptr,0);
      ID3D11DeviceContext_PSSetShaderResources(g_ctx,0,1,&in); ID3D11DeviceContext_PSSetSamplers(g_ctx,0,1,&ls); ID3D11DeviceContext_PSSetConstantBuffers(g_ctx,0,1,&bpcb);
      ID3D11DeviceContext_OMSetDepthStencilState(g_ctx,nullptr,0); ID3D11DeviceContext_Draw(g_ctx,3,0);
      ID3D11DeviceContext_PSSetShaderResources(g_ctx,0,1,nullSRV); };
    auto applyBloom=[&](){
      ID3D11DeviceContext_OMSetBlendState(g_ctx,nullptr,blendf,0xffffffff);
      setP(bthr,bint,0,0); fsPass(rvA,rtSRV,pBright,BW,BH);                       // bright-pass scene -> A
      for(int it=0; it<2; ++it){ setP(1.2f/BW,0,0,0); fsPass(rvB,svA,pBlur,BW,BH); setP(0,1.2f/BH,0,0); fsPass(rvA,svB,pBlur,BW,BH); }
      ID3D11DeviceContext_OMSetBlendState(g_ctx,addb,blendf,0xffffffff);
      fsPass(rtv,svA,pComp,W,H);                                                  // additive composite -> scene
      ID3D11DeviceContext_OMSetBlendState(g_ctx,nullptr,blendf,0xffffffff); };

    auto drawSub=[&](const Sub& s){ MatGPU& g=mats[s.mat];
        ID3D11DeviceContext_IASetInputLayout(g_ctx,g.layout);
        ID3D11DeviceContext_VSSetShader(g_ctx,g.vs,nullptr,0);
        ID3D11DeviceContext_PSSetShader(g_ctx,g.ps,nullptr,0);
        ID3D11DeviceContext_VSSetConstantBuffers(g_ctx,0,1,&g.vcb);
        ID3D11DeviceContext_PSSetConstantBuffers(g_ctx,0,1,&g.pcb);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx,0,16,g.srv);
        ID3D11DeviceContext_DrawIndexed(g_ctx,s.count,s.start,0); };

    auto renderFrame=[&](){
        computeCam();                                            // camera may have moved
        for(auto&g:mats){ if(!g.ok)continue; update_cb(g.vcb,g.vsSh,vals); update_cb(g.pcb,g.psSh,vals); }
        ID3D11DeviceContext_ClearRenderTargetView(g_ctx,rtv,clear);
        ID3D11DeviceContext_ClearDepthStencilView(g_ctx,dsv,D3D11_CLEAR_DEPTH,1,0);
        ID3D11DeviceContext_OMSetRenderTargets(g_ctx,1,&rtv,dsv);
        D3D11_VIEWPORT vpt{0,0,(float)W,(float)H,0,1}; ID3D11DeviceContext_RSSetViewports(g_ctx,1,&vpt);
        ID3D11DeviceContext_RSSetState(g_ctx,rs); ID3D11DeviceContext_OMSetDepthStencilState(g_ctx,dss,0);
        ID3D11DeviceContext_OMSetBlendState(g_ctx,nullptr,blendf,0xffffffff);
        ID3D11DeviceContext_IASetVertexBuffers(g_ctx,0,1,&vb,&vstride,&voff);
        ID3D11DeviceContext_IASetIndexBuffer(g_ctx,ib,DXGI_FORMAT_R32_UINT,0);
        ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_PSSetSamplers(g_ctx,0,16,samplers);
        // opaque first, then translucent back-to-front (blend from bgfx state)
        for(const auto& s:subs){ if(s.mat>=mats.size()||!mats[s.mat].ok||mats[s.mat].blend) continue; drawSub(s); }
        std::vector<Sub> tsubs;
        for(auto s:subs){ if(s.mat<mats.size()&&mats[s.mat].ok&&mats[s.mat].blend){ V3 d=sub(s.ctr,camEye); s.dist=dot(d,d); tsubs.push_back(s);} }
        std::sort(tsubs.begin(),tsubs.end(),[](const Sub&a,const Sub&b){return a.dist>b.dist;});
        if(!tsubs.empty()){
            ID3D11DeviceContext_OMSetDepthStencilState(g_ctx,dssT,0);
            for(const auto& s:tsubs){ ID3D11DeviceContext_OMSetBlendState(g_ctx,mats[s.mat].blend,blendf,0xffffffff); drawSub(s); }
            ID3D11DeviceContext_OMSetBlendState(g_ctx,nullptr,blendf,0xffffffff);
            ID3D11DeviceContext_OMSetDepthStencilState(g_ctx,dss,0);
        }
        if(useBloom) applyBloom();
        if(!headless){ ID3D11DeviceContext_CopyResource(g_ctx,(ID3D11Resource*)back,(ID3D11Resource*)rt); IDXGISwapChain_Present(swap,1,0); }
    };

    if(headless){   // one frame -> PNG, then exit
        renderFrame();
        D3D11_TEXTURE2D_DESC st=rd; st.Usage=D3D11_USAGE_STAGING; st.BindFlags=0; st.CPUAccessFlags=D3D11_CPU_ACCESS_READ; st.MiscFlags=0;
        ID3D11Texture2D* stage=nullptr; ID3D11Device_CreateTexture2D(g_dev,&st,nullptr,&stage);
        ID3D11DeviceContext_CopyResource(g_ctx,(ID3D11Resource*)stage,(ID3D11Resource*)rt);
        D3D11_MAPPED_SUBRESOURCE ms; ID3D11DeviceContext_Map(g_ctx,(ID3D11Resource*)stage,0,D3D11_MAP_READ,0,&ms);
        std::vector<uint8_t> px((size_t)W*H*4);
        for(int y=0;y<H;y++) std::memcpy(px.data()+(size_t)y*W*4,(uint8_t*)ms.pData+(size_t)y*ms.RowPitch,(size_t)W*4);
        for(size_t i=0;i<px.size();i+=4) px[i+3]=255;   // force opaque alpha for the PNG
        ID3D11DeviceContext_Unmap(g_ctx,(ID3D11Resource*)stage,0);
        stbi_write_png(shot.c_str(),W,H,4,px.data(),W*4);
        std::fprintf(stderr,"wrote %s\n",shot.c_str());
        return 0;
    }

    // interactive: pump messages, render every idle iteration
    std::fprintf(stderr,"interactive: LMB-drag = orbit, wheel = zoom, close window to exit\n");
    MSG msg{};
    while(msg.message!=WM_QUIT){
        if(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)){ TranslateMessage(&msg); DispatchMessage(&msg); }
        else renderFrame();
    }
    return 0;
}
