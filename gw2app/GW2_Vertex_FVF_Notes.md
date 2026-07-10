# GW2 — Vertex Shader & GrFvf (Flexible Vertex Format) — Catatan Reverse Engineering

> Sumber: `Gw2-64-disable-aslr.exe.i64` (Guild Wars 2, x64)
> MD5 `43ed4f282619a0464c6cac61dc386bca` · imagebase `0x140000000` · imagesize `0x2bfc000`
> Alat: IDA Pro + ida-pro-mcp. Semua alamat = alamat virtual (imagebase `0x140000000`).
> Build path internal: `D:\Perforce\Live\NAEU\v2\Code\Arena\Engine\Gr\...`

---

## 0. Ringkasan arsitektur

GW2 modern memakai **dua lapisan** untuk shader/geometri:

1. **Amat** = sistem material/shader ArenaNet, di-serialize lewat format **Pack** (tipe ber-versi `...V0/V1/V2`).
2. **bgfx** = backend rendering (jalur **Direct3D 11**, `renderer_d3d11.cpp` / `BgfxBuffer.cpp`). Vertex shader & vertex layout sebenarnya dibuat di sini.

---

## 1. Sistem material Amat (lapisan refleksi Pack)

Tabel deskriptor refleksi ada di `.rdata` (mis. `0x141d04348`, `0x141d4b7d8`); tiap field = record 32-byte `{name_ptr, subtype_ptr, offset/size, type_enum}`.

Hierarki tipe (dari nama field yang terbaca):

```
AmatPass
 └─ effects[]                : AmatEffect
AmatEffect
 ├─ vertexShaderVariants[]   : AmatVertexShaderVariant   (jumlah <= AMAT_VERTEX_SHADER_VARIANTS)
 ├─ pixelShaderIndex
 ├─ shaderPassFlags
 └─ renderState
AmatVertexShaderVariant
 ├─ token
 ├─ vertexShaderIndex        (harus < m_data->shaderCount)
 ├─ renderState
 └─ shaderPassFlags
AmatShader
 ├─ isVertexShader / isPixelShader
 ├─ dx11Shader
 ├─ pbrShader
 └─ variant
AmatShaderConstant           (deklarasi konstanta/uniform)
```

Bukti string: `AmatVertexShaderVariant` (`0x141d04dc8`), `vertexShaderVariants` (`0x141d04e18`),
`effectData->vertexShaderVariantsCount <= unsigned(AMAT_VERTEX_SHADER_VARIANTS)`,
`vertexShader->vertexShaderIndex < m_data->shaderCount`. Tiap tipe punya varian versi
(`AmatVertexShaderVariantV0/V1/V2`, `AmatShaderConstantV0/V1/V2`).

---

## 2. Format biner vertex shader (bgfx)

Fungsi kunci: **`sub_140B5AD60`** = `bgfx::ShaderD3D11::create`
(string *"Failed to create vertex shader. Hash %u. HRESULT(%x)."* @ `renderer_d3d11.cpp:4556`).

Layout blob shader bgfx:

| Bagian | Ukuran | Catatan |
|---|---|---|
| `magic` | 4B | `'V''S''H'`+versi (`'V'`=vertex, `'F'`=fragment, `'C'`=compute) |
| `hashIn` | 4B | hash input signature |
| `hashOut` | 4B | hanya jika versi >= 6 |
| `count` | u16 | jumlah uniform |
| per-uniform | var | `nameLen`(u8),`name`,`type`(u8),`num`(u8),`regIndex`(u16),`regCount`(u16); +texInfo(u16) jika versi>=8; +texFormat(u16) jika versi>=10 |
| `codeSize` | u32 | ukuran bytecode |
| `code` | var | **DXBC** (HLSL terkompilasi) → `ID3D11Device::CreateVertexShader` |
| `attrCount` | u8 | jumlah atribut vertex |
| per-attr | u16 | id atribut → remap ke slot lokal (max 18) |
| `constBufSize` | u16 | ukuran constant buffer (dibulatkan ke kelipatan 16) |

Alur runtime: load `.amat` → pilih `AmatVertexShaderVariant` (via `shaderPassFlags`/render-state) →
`vertexShaderIndex` → `bgfx::ShaderD3D11::create` → `CreateVertexShader` → uniform dipetakan ke handle bgfx →
constant buffer di-bind.

Min-spec check: `sub_1413CBE70` — cek *vertex shader model 3.0* (caps bit `0x400000`),
string *"Below min spec: Failed vertex shader model 3.0 check"*.

---

## 3. Konstanta shader (uniforms)

Relasi jumlah (dijaga assertion):
- `bgfxUniformsCount == amatShader.constantsCount + amatShader.samplersCount - outSamplerUniformHandlesCount` (`0x141d05ce0`)
- `bgfxUniformsCount != shaderData.constantCount + shaderData.samplerCount` (`0x141d05dd0`)
- `bgfxUniformsCount < MAX_UNIFORMS` (`0x141d05cb8`) — MAX_UNIFORMS default bgfx = 512.

### 3.1 Predefined uniform = 12 (bukan 16)

Tabel `off_142540980`, dibaca `sub_140B6C830` (batas `0xC` = 12). Uniform yang diisi otomatis engine:

| # | Uniform | Isi |
|---|---|---|
| 1 | `u_viewRect` | x,y,w,h viewport |
| 2 | `u_viewTexel` | 1/w, 1/h |
| 3 | `u_view` | matriks view |
| 4 | `u_invView` | invers view |
| 5 | `u_proj` | proyeksi |
| 6 | `u_invProj` | invers proyeksi |
| 7 | `u_viewProj` | view x proj |
| 8 | `u_invViewProj` | inversnya |
| 9 | `u_model` | matriks model (array — skinning/instancing) |
| 10 | `u_modelView` | model x view |
| 11 | `u_modelViewProj` | MVP |
| 12 | `u_alphaRef4` | alpha reference |

Konstanta khusus lain: `PackMapEnvDataShaderConstant`, `ModelConstantData` (V15..V50), `ComputeShaderConstant`.

---

## 4. GrFvf — Flexible Vertex Format (LENGKAP)

Bitmask 32-bit. Nama engine = **`GrFvf`** (bukan "ANetFlexibleVertexFormat"), file `Gr\GrFvf.cpp` & `Gr\Bgfx\BgfxBuffer.cpp`.

- Layout GPU dibangun `sub_140B987C0` (`GrFvf` → `bgfx::VertexLayout`) — **urutan `add()` = urutan byte**.
- Stride sumber divalidasi `DDI_STRIDE` = `sub_140B995C0`.
- Enum bgfx **Attrib**: 0=Position,1=Normal,2=Tangent,3=Bitangent,4=Color0,8=Indices,9=Weight,10-17=TexCoord0-7.
- Enum bgfx **AttribType**: 0=Uint8,1=Uint10,2=Int16,3=Half,4=Float.

### 4.1 16 field inti (bit 0-15)

| Bit | Mask | GR_FVF_ | Attrib | Komp | Tipe | Byte | Keterangan |
|---|---|---|---|---|---|---|---|
| 0 | `0x00000001` | POSITION | Position | 3 | Float32 | 12 | x,y,z — **selalu offset 0** |
| 1 | `0x00000002` | WEIGHTS | Weight | 4 | Uint8 norm | 4 | bobot skinning (/255) |
| 2 | `0x00000004` | GROUP | Indices | 4 | Uint8 | 4 | bone index (mentah) |
| 3 | `0x00000008` | NORMAL | Normal | 3 | Float32 | 12 | normal |
| 4 | `0x00000010` | COLOR | Color0 | 4 | Uint8 norm | 4 | **BGRA** (/255) |
| 5 | `0x00000020` | TANGENT | Tangent | 3 | Float32 | 12 | tangent |
| 6 | `0x00000040` | BITANGENT | Bitangent | 3 | Float32 | 12 | bitangent |
| 7 | `0x00000080` | TANGENT_FRAME | N+T+B | 4x3 | Uint8 norm | 12 | TBN terpaket (3x4B) |
| 8 | `0x00000100` | TEXCOORD0 | TexCoord0 | 2 | Float32 | 8 | UV0 |
| 9 | `0x00000200` | TEXCOORD1 | TexCoord1 | 2 | Float32 | 8 | UV1 |
| 10 | `0x00000400` | TEXCOORD2 | TexCoord2 | 2 | Float32 | 8 | UV2 |
| 11 | `0x00000800` | TEXCOORD3 | TexCoord3 | 2 | Float32 | 8 | UV3 |
| 12 | `0x00001000` | TEXCOORD4 | TexCoord4 | 2 | Float32 | 8 | UV4 |
| 13 | `0x00002000` | TEXCOORD5 | TexCoord5 | 2 | Float32 | 8 | UV5 |
| 14 | `0x00004000` | TEXCOORD6 | TexCoord6 | 2 | Float32 | 8 | UV6 |
| 15 | `0x00008000` | TEXCOORD7 | TexCoord7 | 2 | Float32 | 8 | UV7 |

`GR_FVF_TEXCOORD_BITS = 0x0000FF00` · `GR_FVF_TEXCOORD_COUNT(fvf)` = popcount(fvf & 0xFF00).
Struktur: **8 atribut tetap (bit 0-7) + 8 kanal UV (bit 8-15) = 16 field**.

### 4.2 Modifier presisi UV (bit 16-23)

| Mask | GR_FVF_ | Efek |
|---|---|---|
| `0x00FF0000` | TEXCOORD_F16_BITS | Per kanal UV: simpan **2xHalf (4B)** bukan 2xFloat (8B). Bit 16→UV0 … bit 23→UV7. |

`GR_FVF_TEXCOORD_COUNT_F16(fvf)` = popcount(fvf & 0xFF0000).
**Aturan:** F32 (0xFF00) & F16 (0xFF0000) tidak boleh aktif bersamaan (assert `0x141c88150`).

### 4.3 Bit tinggi — varian kompresi (bit 24-29)

Ukuran dari `DDI_STRIDE` (format sumber/on-disk). Sebagian di-decompress sebelum layout GPU (beda dengan builder bgfx):

| Bit | Mask | Byte (DDI) | Di layout GPU | Dugaan makna |
|---|---|---|---|---|
| 24 | `0x01000000` | +48 | tidak dibangun | stream/blok ekstra (legacy) |
| 25 | `0x02000000` | +4 | tidak dibangun | flag ekstra |
| 26 | `0x04000000` | +4 | Normal 4xUint8 norm | NORMAL terkompresi |
| 27 | `0x08000000` | +16 | Position 4xFloat32 | POSITION 4-komponen |
| 28 | `0x10000000` | +6 (!) | Normal 4xUint8 norm (+4) | POSITION_COMPRESSED (kandidat 3xInt16=6B) |
| 29 | `0x20000000` | +12 | tidak dibangun | varian (legacy) |

(!) Bit 28: DDI=6B (cocok 3xint16 posisi terkompresi, sesuai string `GR_FVF_POSITION_COMPRESSED`) tapi builder bgfx
memetakan ke Normal 4B → field ini kemungkinan di-decompress saat upload GPU. Untuk parsing **file model** pakai
ukuran DDI; untuk membaca **buffer GPU** pakai builder.

### 4.4 Dua sumber stride
- **`DDI_STRIDE(fvf)`** (`sub_140B995C0`) = stride format sumber/on-disk (termasuk field terkompresi).
- **`VertexLayout::CalcStride()`** (dibangun `sub_140B987C0`) = stride buffer GPU (setelah dekompresi).
- Assert `DDI_STRIDE(fvf) == CalcStride()` (`0x141c87d98`) → untuk fvf tanpa bit kompresi keduanya identik.
- Validasi: `vertexCount * GrFvfCalcStride(fvf) == vertexBuffer.Bytes()` (`0x1422b6dd8`).

### 4.5 Aturan validitas (dari assertion)
- `fvf & GR_FVF_POSITION` wajib (`0x141c6ed98`).
- `IS_TRUE(TANGENT) == IS_TRUE(BITANGENT)` — selalu berpasangan (`0x141c6edb0`).
- `TANGENT_FRAME` tidak boleh bersamaan dengan `NORMAL|TANGENT|BITANGENT` (`0x141c87d10`).
- `!(fvf & ~supportedFvf)` — hanya bit yang didukung.
- Field bisa dipisah ke beberapa stream: `fvfCount < GR_GEOSET_MAX_VERTEX_BUFFERS` (`0x141c70208`).

### 4.6 Decode tiap tipe komponen
```
Float32   : float LE (4B)
Half16    : IEEE-754 half (2B) -> float
Int16     : int16 LE (2B); posisi terkompresi biasanya *skala + bias
Uint8norm : b / 255.0                (Color = urutan B,G,R,A)
Uint10    : paket R10G10B10A2 (4B) : (v>>0)&0x3FF,(v>>10)&0x3FF,(v>>20)&0x3FF,(v>>30)&0x3
            untuk normal/tangent -> nilai*2-1 ke [-1,1]
Uint8(idx): bone index mentah (tanpa normalisasi)
```

### 4.7 Algoritma parsing
```
stride = 0
for baris di 4.1 (urut bit 0->15):
    if fvf & mask:
        field.offset = stride
        field.size   = byte  (untuk UV: 4B jika bit UV terkait di 0xFF0000 di-set, selain itu 8B)
        stride += field.size
# terapkan bit 26/27/28 jika ada (ganti ukuran Position/Normal)
assert stride == DDI_STRIDE(fvf)
vertexCount = bufferBytes / stride
```

**Contoh** `fvf = 0x1|0x2|0x4|0x8|0x80|0x100` (Position+Weights+Group+Normal+TangentFrame+UV0-F32):
```
Position     off  0  12B
Weights      off 12   4B
Group        off 16   4B
Normal       off 20  12B
TangentFrame off 32  12B
TexCoord0    off 44   8B
stride = 52 byte
```

---

## 5. Alamat & simbol kunci

| Simbol/Alamat | Fungsi |
|---|---|
| `sub_140B5AD60` | `bgfx::ShaderD3D11::create` (parse blob VSH/FSH/CSH + CreateVertexShader) |
| vtable slot `+96 / +120 / +144` | CreateVertexShader / CreatePixelShader / CreateComputeShader |
| `sub_1413CBE70` | cek min-spec (vertex shader model 3.0) |
| `sub_140B6C830` | lookup predefined uniform (12 nama) |
| `off_142540980` | tabel 12 predefined uniform |
| `sub_140B987C0` | **GrFvf -> bgfx::VertexLayout** (definitif urutan/tipe elemen) |
| `sub_140B56EC0` | `bgfx::VertexLayout::add(attrib,num,type,norm,asInt)` |
| `sub_140B995C0` | `DDI_STRIDE` / CalcStride (verifikasi stride) |
| `sub_140BA5190` / `sub_140BA51B0` | hitung jumlah texcoord F32 / F16 |
| `sub_140BA5250` | `GrFvfFieldIndex` (popcount bit bawah) |
| `sub_140B9D9A0` dst | konverter vertex antar-fvf (`GrFvf.cpp`) |
| `s_kTexCoordRemap` @ `0x141c87cf0` | `[10..17]` = TexCoord0..7 |

---

## 6. Tekstur — bagaimana dipakai (usage/diffuse/normal/specular)

Masalah: `.modl` hanya menyimpan **daftar fileId tekstur (angka)**; peran (diffuse/normal/spec) TIDAK
ada di daftar itu dan TIDAK bisa ditentukan dari format gambar. Peran ditentukan oleh **material + shader (Amat)**.

### 6.1 Rantai binding
1. **`.modl` material** (`McMaterial`, ctor `sub_1402F1B90`, file `McMaterial.cpp`):
   - `m_filenames[5]` + `m_fileIdsOrHashes[5]` — daftar tekstur (maks 5)
   - `materialShaderId` @ offset +184 — id shader material
   - `texTokens[]`, `textureFlags` — token & flag per tekstur
   - handle tekstur hasil load @ +192
2. **`materialShaderId` -> shader** via `sub_1402F46E0` (switch 0..57):
   mengembalikan blob shader Amat (`AMATGRMT` @ `0x141932030`+) + **`textureCount` tetap** per shader
   (id0->1, id2->2, id17->3, id20->4, dst). **id 58 = MATERIAL_SHADER_CUSTOM** (dari file `.amat`).
3. **Shader mendefinisikan semantik slot** lewat `AmatSamplerConstant[]` = `{ token, samplerIndex, state }`.
   Inilah yang menentukan slot ke-N = diffuse/normal/specular -> register sampler GPU (`samplerIndex`).
4. **Binding**: cocokkan `model.texTokens[i]` <-> `sampler.token` -> ikat ke `samplerIndex`.
   Assert keras **`textureCount == shaderTextureCount`** (`McMaterial.cpp:809`).
5. **Format (DXT1/5, 3Dc/BC5)** dibaca dari FILE tekstur (format **ATEX**, `ImgAtex.cpp` /
   `ImgNormalMap.cpp`) saat load — BUKAN dari modl.

### 6.2 Kenapa FVF + daftar tekstur tidak cukup
- FVF hanya beri jumlah UV channel & atribut vertex, bukan peran tekstur.
- Peran tekstur hidup di `materialShaderId` + token sampler, bukan urutan daftar / format gambar.

### 6.3 Strategi untuk shader dinamis OpenGL
- **Benar:** baca `materialShaderId` -> resolusi ke daftar sampler (token->samplerIndex->semantik).
  58 shader built-in = blob `AMATGRMT` di `.rdata`; custom (id58) dari `.amat`.
- **Heuristik:** format 3Dc/BC5/ATI2 ("3DCX") => normal; DXT1 => diffuse opaque; DXT5 => diffuse+alpha/spec.
  Gabung dengan `textureCount` + urutan konvensional per shaderId + `texTokens`/`textureFlags`.

### 6.4 Alamat kunci (tekstur/material)
| Simbol | Fungsi |
|---|---|
| `sub_1402F1B90` | ctor `McMaterial` (daftar fileId + shaderId + handle tekstur) |
| `sub_1402F46E0` | tabel 58 material-shader built-in -> blob Amat + `textureCount` |
| `AMATGRMT` @ `0x141932030`+ | shader material Amat tertanam (berisi `AmatSamplerConstant`) |
| `AmatSamplerConstant` | `{ token, samplerIndex, state }` — semantik slot tekstur |
| `AmatSamplerState` | wrap/filter mode sampler |
| `ImgAtex.cpp` / `ImgNormalMap.cpp` | loader format tekstur (ATEX; DXT/3Dc) |

---

## 7. TODO / langkah berikut
- [ ] Struktur **geoset / mesh** di file `.modl` (Pack): lokasi field `fvf` + pointer vertex/index buffer + `vertexCount`.
- [ ] Konfirmasi byte pasti bit 24/25/28/29 (varian terkompresi) via konverter spesifik.
- [ ] Catatan Granny3D (`Granny FVF`, `0x141ede840`) & Havok (`hkVertexFormat`, `0x14233a598`) untuk cloth/physics.

*Catatan kepastian: bit 0-23 terverifikasi kuat (layout builder + DDI + konverter). Bit 24/25/28/29 = kepercayaan menengah (jarang dipakai).*
