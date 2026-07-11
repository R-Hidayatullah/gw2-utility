# GW2 client-executable shader library (extracted)

Every bgfx shader blob baked into `Gw2-64.exe` `.rdata`, dumped by
[`../extract_exe_shaders.py`](../extract_exe_shaders.py). This is the "complete" shader
set — the engine camera / depth / deferred-lighting / sky / post / compute shaders that
do **not** exist in the `.dat` AMAT material packages.

## Regenerate
```
python ../extract_exe_shaders.py            # default exe path = the disable-aslr build
python ../extract_exe_shaders.py <exe> -o <dir>
```
Reads the PE directly (no IDA needed); each bgfx blob is self-delimiting
(`ShaderD3D11::create` layout), so extraction needs no directory table.

## Contents
- `cso/NNNN_<T>_<hash>.cso` — raw DXBC bytecode per blob (loads in D3DReflect / Create*Shader;
  cbuffers=0 because bgfx strips the RDEF chunk — read uniforms from the manifest instead).
- `manifest.json` — one record per blob: `index, va, fileoff, type(V/F/C), version, hashIn/Out,
  codeSize, numAttrs, constBufSize, region(material|engine|compute), role, uniforms[]`.
- `stage_candidates.json` — engine shaders grouped by render-pipeline stage (see below).

## Inventory (2244 validated blobs)
| region | count | what |
|---|---|---|
| material | 1115 | the 58 built-in AMAT packages' internal VS/FS variants |
| engine   | 1094 | camera / depth / deferred-light / sky / post graphics shaders |
| compute  |   35 | incl. 11 real GRCS compute shaders (auto-exposure / tonemap / culling) |

## Directory by role (content-derived — engine blobs carry no ArenaNet names in release)
Pick shaders for a pipeline stage by their **uniform signature** (authoritative; the DXBC
hash is the runtime identity). Concrete candidates in `stage_candidates.json`:

- **Depth / shadow VS** (27): uniforms = `{ViewProjection}` only. e.g. `#1140`.
- **Forward SH-lit PS** (20): `shRed/shGreen/shBlue` (SH ambient) + `shSun/shSunColor/shSunData`
  (directional sun) + `WorldToShadowD` + shadow sampler `gSs15`. e.g. `#1428`, `#1437`.
  → the authentic replacement for the hand-written `kLightHLSL` in `gw2browser/model_renderer.cpp`.
- **Sky / atmosphere PS** (16): `FogColorFar, skypara, sunDir, sunClr` + `shRed/Green/Blue`. e.g. `#1611`.
- **LightBuffer deferred-sample PS** (87): `LightBuffer, ScreenDims, StippleDensity` + light-buffer
  sampler `gSs12`. e.g. `#1274`. → what the deferred materials sample; feed from a real light buffer.
- **Post fullscreen PS** (~20+): `ModelColor, fxclr`. e.g. `#1135`.
- **Compute — auto-exposure / tonemap** (11): `ExposureControl` (#2215/#2218), `Exposures` (#2217),
  `HistParams`/`HistParams2` (#2221/#2222 = luminance histogram), `FusionExponents` (#2223 =
  exposure fusion). The GRCS packfiles are also embedded as `PF/COMP/GRCS` at their VAs.

## How this maps back to the runtime (ordering / identity)
See memory `gw2-exe-shaders.md`: asset `shaderId` → per-context `graphicsShaderCache[shaderId]`;
each blob is realized by `ShaderD3D11::create` (sub_140B5AD60) which keys it by a **DXBC content
hash** (`hashOut`). Uniforms bind by `regIndex` as a **byte offset** into the cbuffer.

## Next (integration into gw2browser)
`GameShader` mode already uses the game's real per-material DXBC. The reconstructed stage is
lighting/post. To use these engine shaders: swap `kLightHLSL` for a **Forward SH-lit PS** (#1428),
feed its `sh*`/`shSun*` cbuffer (approx sun/SH rig, or real values from a live-frame capture), and
optionally run the **ExposureControl** compute for tonemap. Blocked from *pixel-exact* only by the
runtime scene DATA (real light buffer / SH / shadow maps), not by shader availability.
