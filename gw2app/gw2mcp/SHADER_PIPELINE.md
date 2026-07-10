# GW2 game-shader (DXBC) pipeline — status & handoff

Goal: render GW2 models with the **game's own bgfx DXBC shaders** (not our
reconstructed HLSL). Everything below is proven working; only the final viewer
wiring remains.

## The full traced chain (all verified)

```
GEOM.submesh.materialIndex ──> MODL.permutations[].materials[idx]
  material.filename (fileId) ──> texture at baseId B
        baseId B − 1 = AMAT packfile (container AMAT, chunks GRMT v6 + BGFX v1)
          BGFX chunk = AmatMaterialV1
            ├─ shaders[]  (AmatShaderV1: isPixelShader, dx11Shader=AmatShaderBinaryV1)
            │     AmatShaderBinaryV1 { data[byte]=bgfx VSH/FSH blob (DXBC),
            │                          constants[], samplers[]=AmatSamplerConstant }
            └─ techniques[0].passes[0].effects[0]
                  ├─ pixelShaderIndex                 -> shaders[psIndex]
                  └─ vertexShaderVariants[0].vertexShaderIndex -> shaders[vsIndex]
```

Example (model 143917, material 0): filename fileId 14165 → texture baseId
19897 → **AMAT at baseId 19896** (414 KB, 141 shaders). technique[0] selects
**VS index 7, PS index 3**.

## What works (tools in `gw2mcp/`)

- `native/gw2dat_cli.exe amat --dat <Gw2.dat> --index <amatBaseId> --template templates/gw2_packfile.json --out-dir <dir>`
  → prints shaderCount, selected vsIndex/psIndex, PS **sampler bindings**
  (`textureIndex → textureSlot` = the texture ROLE map), and dumps the selected
  VS/PS bgfx blobs to `<dir>/vs_N.bgfxsh`, `ps_N.bgfxsh`.
- `tools/parse_bgfx_shaders.py <blob> <outdir>` → parses the bgfx VSH/FSH wrapper
  (magic, uniform table with names+regIndex, codeSize) and extracts raw **DXBC**
  to `.cso`.
- `tools/test_shader.exe <cso>` → D3DReflect + `CreateVertex/PixelShader`.
  **Confirmed:** the game VS+PS load OK; VS input signature =
  POSITION,NORMAL,TANGENT,BITANGENT,TEXCOORD0,TEXCOORD1 = **our GVertex exactly**.

Uniforms the VS needs (fill cbuffer at `regIndex*16` bytes): `ViewProjection`
(Mat4), `World` (Mat4), `WorldView` (Mat4), `CameraPosition`, `TexTransform0A/0B`
(identity), plus `Fog*` / `WorldToShadow*` (can be 0 for a static preview).

## DONE: game-shader renderer runs (`viewer/gw2gsviewer.cpp`)

`gw2gsviewer --dat <Gw2.dat> --template <json> --index 291982 --shot out.png`
loads the model, finds its AMAT (baseId-1 of material.filename texture), picks
VS+PS via technique, parses the bgfx blobs → DXBC, `Create*Shader`, builds the
input layout from the VS ISGN reflection, fills VS/PS cbuffers by uniform name
at their **byte offset** (uniform `reg` field IS the byte offset, not reg*16;
matrices transposed for bgfx column-major), binds material textures per PS
sampler slot, renders offscreen → PNG.

Result on 291982: **the game VS+PS run and produce the correct silhouette** (VS +
cbuffer + layout all correct). Output is solid **white** because the selected PS
is a deferred/lit shader (uniforms LightBuffer/shSunColor/AlphaRef) with no light
environment fed — expected. To get final color: supply a proper light setup (or
pick a forward/unlit technique variant, or set up the G-buffer MRT + lighting
pass). Geometry/shader-execution is proven; only the lighting context remains.

## DONE: multi-submesh game shaders + key finding

`gw2gsviewer` now renders **every submesh with its own material's game VS+PS**
(model 291977: 9 submeshes, 6/6 materials built from their AMAT). Geometry all
correct.

**Key finding on the white output:** the PS writes only **one** render target
(`SV_TARGET 0`, RGBA) — it is NOT a deferred G-buffer MRT writer (good, no MRT
needed). Yet output is white even with fog disabled + sun/ambient set. Two
likely causes, both solvable with more RE:
1. **Wrong pass**: `extractAmat` picks `technique[0].pass[0].effect[0]`, which
   may be a **depth/shadow/pre-pass** (constant output). Need to enumerate all
   passes and pick the **color** pass (heuristic: pass whose PS has the texture
   samplers / a color render state). AmatTechnique.passes[] / AmatPass.effects[]
   are already in the template; extend extractAmat to return them all.
2. **Lighting constants**: uniform NAMES are known (LightBuffer, shSunColor,
   SunDirection, ...) but the game's per-frame VALUES (the light buffer contents)
   must be reconstructed/approximated for correct color.
So "build the game shader environment" IS feasible (single-target forward PS),
just needs pass-selection + light-buffer reconstruction — not a deferred engine.

## Deep diagnosis of the white output (via D3DDisassemble)

PS writes 1 target. Decoded logic: `o0.xyz = texSample.xyz * factor + v2` where
**v2 is a fog/ambient term the VS outputs** (`o2 = fogColor*(1-fogFactor) + base`,
CB0[25]). Fixes applied: (1) size the cbuffer to the max uniform extent — bgfx's
`constBufSize` (260) is smaller than the DXBC `CB0[25]` (400B), which was cutting
off TexTransform/WorldView; (2) pick the color pass (PS with most samplers) over
all techniques/passes; (3) zero fog/ambient uniforms. Geometry projects correctly
(World/ViewProjection land), but output is STILL white — so a remaining
uniform/texture value is off. Root cause needs the game's **actual runtime
constant values** (the fog/light/exposure constants, the CB0 contents), which
realistically means a **RenderDoc frame capture on the live game** to read the
real cbuffer + confirm register mapping, or a full instruction-level trace of
every DXBC register back to its uniform/sampler. This is the honest next step for
correct color; the shaders themselves run correctly on all submeshes.

For a correct-color image NOW, use the reconstruction renderer `gw2viewer`.

## Remaining work (viewer integration)

1. In `gw2viewer`, for each material: find the AMAT (baseId of `material.filename`
   texture − 1), run the `AmatSet` extraction (reuse `gw2model::Extractor::extractAmat`),
   get the selected VS+PS blob, extract the DXBC (port `parse_bgfx_shaders.py`
   logic to C++), `CreateVertexShader`/`CreatePixelShader`.
2. Input layout: build from the VS DXBC input signature (D3DReflect) — already
   matches our vertex buffer, so the existing GVertex layout works.
3. Constant buffer: one `float4`-indexed buffer; write each uniform at
   `regIndex*16` by name (from the bgfx uniform table). Fill ViewProjection,
   World, WorldView, CameraPosition, TexTransform*=identity, rest = 0.
4. Textures: for each PS sampler bind `{textureIndex, textureSlot}`, load
   `material.textures[textureIndex]` (decode ATEX→RGBA) and bind its SRV to
   register `textureSlot`. Sampler states from `AmatMaterial.samplers[]`.
5. **Deferred-rendering caveat:** the selected PS is a *deferred* shader (uniforms
   `LightBuffer`, `StencilId`; writes a G-buffer via MRT), so a single-RT draw
   yields G-buffer albedo/normal, not a final lit image. Options: (a) pick a
   forward/unlit technique variant if present; (b) set up MRT + a simple lighting
   pass; (c) accept the albedo target as proof-of-pipeline first.

## Recommended lightweight test case (for the render PoC)

**Model baseId 291982** (1 mesh, 86 verts) — smallest good candidate. Its
`material.filename` fileId 835971 → texture baseId 135432 → **AMAT baseId 135431**
(132 KB, 47 shaders). technique[0] → **VS idx 6, PS idx 0** (blobs 2091/1251 B),
both `Create*Shader OK`.
- VS input sig: POSITION, NORMAL, TEXCOORD0, TEXCOORD1 (NO tangent/bitangent —
  simpler than 143917; build the input layout from this 4-element ISGN, mapping
  to our GVertex offsets pos@0, normal@12, uv@48; only UV0 available, feed it
  for both TEXCOORD0/1).
- PS samplers: ss0→0, ss1→1, ss2→2, gSs14→14; uniforms include shSunColor +
  AlphaRef (light-based; likely closer to final color than the 143917 deferred PS).
Start the viewer integration with THIS model — fewer shaders, no tangent, 4 samplers.

## Known minor issues

- `amat` command's `isPixelShader` flag reads all-true (count wrong) — cosmetic;
  the VS/PS **selection uses technique indices** and is correct (verified: vs_7
  blob magic = `VSH`, ps_3 = `FSH`). Fix later if the flag is needed.

## Also: DXBC in the .exe

The 58 built-in material shaders live in the .exe `.rdata` as AMATGRMT PF blobs
(`0x141932030`+, table `sub_1402F46E0`), same VSH/FSH/DXBC format. Used by the
McMaterial path (shaderId < 58). The dat AMAT path (above) is the bgfx one.
