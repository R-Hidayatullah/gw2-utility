// test_shader.cpp -- validate a GW2-extracted DXBC shader (.cso) is loadable in
// D3D11 and reflect its interface: input signature (vertex layout the shader
// expects), constant buffers, and bound textures/samplers.
//
// Proves the shaders pulled out of the dat AMAT/BGFX chunk are real, usable
// D3D11 bytecode -- the foundation for rendering with the game's own shaders.
//
// Build (MinGW): g++ -std=c++20 test_shader.cpp -o test_shader.exe \
//                    -ld3d11 -ld3dcompiler -ldxgi -ldxguid -lole32 -static ...
// Usage: test_shader.exe <shader.cso>

#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <cstdint>
#include <fstream>
#include <vector>

static std::vector<uint8_t> readfile(const char* p){
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv){
    if (argc < 2){ std::puts("usage: test_shader <shader.cso>"); return 1; }
    std::vector<uint8_t> dxbc = readfile(argv[1]);
    if (dxbc.size() < 4 || memcmp(dxbc.data(), "DXBC", 4) != 0){ std::puts("not a DXBC file"); return 1; }
    std::printf("%s : %zu bytes DXBC\n", argv[1], dxbc.size());

    // headless D3D11 device
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
                                 D3D11_SDK_VERSION, &dev, nullptr, &ctx))){
        std::puts("D3D11CreateDevice failed"); return 1;
    }

    // reflect
    ID3D11ShaderReflection* refl = nullptr;
    if (FAILED(D3DReflect(dxbc.data(), dxbc.size(), IID_ID3D11ShaderReflection, (void**)&refl))){
        std::puts("D3DReflect failed"); return 1;
    }
    D3D11_SHADER_DESC sd{}; refl->lpVtbl->GetDesc(refl, &sd);
    UINT progType = (sd.Version >> 16) & 0xFFFF; // 0=PS,1=VS,...
    const char* kind = progType==0?"PIXEL":progType==1?"VERTEX":progType==5?"COMPUTE":"OTHER";
    std::printf("shader type: %s  instrs=%u  inputs=%u  cbuffers=%u  boundResources=%u\n",
                kind, sd.InstructionCount, sd.InputParameters, sd.ConstantBuffers, sd.BoundResources);

    // input signature (only meaningful for VS -> needed for the input layout)
    if (progType == 1){
        std::puts("input signature (semantic -> reg):");
        for (UINT i=0;i<sd.InputParameters;i++){
            D3D11_SIGNATURE_PARAMETER_DESC pd{};
            refl->lpVtbl->GetInputParameterDesc(refl, i, &pd);
            std::printf("   %-14s%u  reg=%u  mask=0x%X\n", pd.SemanticName, pd.SemanticIndex, pd.Register, pd.Mask);
        }
    }

    // output signature (for a PS: how many SV_Target it writes = G-buffer size)
    if (sd.OutputParameters){
        std::puts("output signature (SV_Target etc.):");
        for (UINT i=0;i<sd.OutputParameters;i++){
            D3D11_SIGNATURE_PARAMETER_DESC pd{};
            refl->lpVtbl->GetOutputParameterDesc(refl, i, &pd);
            std::printf("   %-14s%u  reg=%u  mask=0x%X\n", pd.SemanticName, pd.SemanticIndex, pd.Register, pd.Mask);
        }
    }

    // constant buffers (uniform layout the shader expects)
    for (UINT c=0;c<sd.ConstantBuffers;c++){
        ID3D11ShaderReflectionConstantBuffer* cb = refl->lpVtbl->GetConstantBufferByIndex(refl, c);
        D3D11_SHADER_BUFFER_DESC bd{}; cb->lpVtbl->GetDesc(cb, &bd);
        std::printf("cbuffer '%s' size=%uB vars=%u:\n", bd.Name, bd.Size, bd.Variables);
        for (UINT v=0; v<bd.Variables && v<24; v++){
            ID3D11ShaderReflectionVariable* var = cb->lpVtbl->GetVariableByIndex(cb, v);
            D3D11_SHADER_VARIABLE_DESC vd{}; var->lpVtbl->GetDesc(var, &vd);
            std::printf("   +%-4u %-24s %uB\n", vd.StartOffset, vd.Name, vd.Size);
        }
    }

    // bound resources (textures t#, samplers s#)
    for (UINT r=0;r<sd.BoundResources;r++){
        D3D11_SHADER_INPUT_BIND_DESC bd{};
        refl->lpVtbl->GetResourceBindingDesc(refl, r, &bd);
        const char* ty = bd.Type==D3D_SIT_TEXTURE?"Texture":bd.Type==D3D_SIT_SAMPLER?"Sampler":
                         bd.Type==D3D_SIT_CBUFFER?"CBuffer":"Other";
        std::printf("bind %-8s %-20s slot=%u\n", ty, bd.Name, bd.BindPoint);
    }

    // disassemble (to understand what the shader computes)
    { ID3DBlob* asmb=nullptr;
      if(SUCCEEDED(D3DDisassemble(dxbc.data(),dxbc.size(),0,nullptr,&asmb))){
        std::puts("--- disassembly ---");
        std::fwrite(ID3D10Blob_GetBufferPointer(asmb),1,ID3D10Blob_GetBufferSize(asmb),stdout);
      } }

    // actually create the shader object -> proves it's loadable
    HRESULT hr;
    if (progType == 1){
        ID3D11VertexShader* vs=nullptr;
        hr = ID3D11Device_CreateVertexShader(dev, dxbc.data(), dxbc.size(), nullptr, &vs);
        std::printf("CreateVertexShader: %s\n", SUCCEEDED(hr)?"OK":"FAILED");
    } else if (progType == 0){
        ID3D11PixelShader* ps=nullptr;
        hr = ID3D11Device_CreatePixelShader(dev, dxbc.data(), dxbc.size(), nullptr, &ps);
        std::printf("CreatePixelShader: %s\n", SUCCEEDED(hr)?"OK":"FAILED");
    }
    return 0;
}
