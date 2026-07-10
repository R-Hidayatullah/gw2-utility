# gw2viewer — Direct3D 11 GW2 model viewer

Loads a `.modl` straight out of `Gw2.dat` and renders it textured:

```
Gw2.dat → Method0 decompress → parse MODL (GEOM geometry + FVF, MODL materials)
        → decode ATEX diffuse → D3D11 vertex/index/texture → orbit-cam render
```

Reuses `../native/gw2model.hpp`, `gw2_atex.hpp`, `gw2dat.cpp`, and the
decompressor. Shaders are our own HLSL (compiled at runtime via d3dcompiler)
that **reconstruct the material behaviour** per `GW2_Vertex_FVF_Notes.md` §6 —
they are **not** the game's bgfx DXBC blobs (running those standalone would mean
re-implementing the whole bgfx+Amat runtime: the 58 AMATGRMT reflection blobs in
the .exe, the separate DXBC shader table, cbuffer packing and sampler binding —
a multi-week RE effort no community viewer attempts).

### Material-driven rendering (§6)

The extractor reads the full material (`materialId`, `materialFlags`,
`sortLayer`, per-texture `token`/`flags`/`uvIndex`, and float4 `constants`).
Because these legacy materials leave `materialId`/tokens at 0, texture **roles**
are resolved by the format heuristic the notes recommend (§6.3):

- **3DC/BC5/ATI2 → normal map** → tangent-space normal mapping (TBN built from
  the FVF tangent-frame; X,Y sampled, Z reconstructed).
- **largest DXT/BC7 → diffuse** → tinted by the first material constant.
- **grayscale diffuse → effect** → treated as an unlit mask: luminance drives
  alpha, tinted, alpha-blended with depth-write off (the ethereal look).

Surface models get normal-mapped lambert + hemispheric ambient; effect models
(e.g. base-id 291978) render as blended glows instead of flat grey.

## Build (MinGW-w64 g++)

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```
Links `-ld3d11 -ldxgi -ld3dcompiler -ldxguid`. Uses `CINTERFACE + COBJMACROS`
so the C-style COM macros work under g++.

## Run

```powershell
# interactive window (LMB-drag = orbit, wheel = zoom)
gw2viewer.exe --dat <Gw2.dat> --template ..\templates\gw2_packfile.json --base-id 291830

# selectors: --base-id N (MFT index) | --file-id N | --index N
# headless: render one frame to PNG and exit
gw2viewer.exe --dat <Gw2.dat> --template ..\templates\gw2_packfile.json --base-id 291830 --shot out.png
```

Good sample base-ids: `291830` (rock cliff), `291982` (painted billboard).

## Known limits

- Roles come from the **format heuristic**, not the actual Amat shader, so a
  material whose slots don't follow the usual format convention can mis-assign
  (e.g. a spec map read as diffuse). Faithful per-slot roles need the AMATGRMT
  sampler-token binding (§6.1–6.4), which these legacy materials don't carry.
- One diffuse + one normal per mesh; no specular/emissive maps, UV animation,
  skinning, or LOD selection yet.
- FVF coverage: bits 0–23 (position/normal/tangent-frame/UV F32+F16). The rare
  compressed-position variants (bits 24–29) aren't unpacked.
- Effect blending is single-pass alpha (no per-object sort), so overlapping
  transparent triangles can show ordering artifacts.
