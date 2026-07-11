#ifndef ENTRY_EXTRACTOR_H
#define ENTRY_EXTRACTOR_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "gw2dat.h"
#include "granny_anim.hpp"

// What the Preview tab should show for a given entry.
enum class PreviewKind {
    None,  // unrecognized binary -- hex only
    Image, // ATEX/DDS/PNG/JPEG/BMP (preview_* fields below)
    Model, // .modl packfile (model below; may be null if no struct template loaded)
    Map,   // mapc/area packfile with placed props -> map scene below
    Text,  // printable text (html/css/js/xml/...) -> text_preview
    Strs,  // "strs" string table -> text_preview
    Audio, // ASND packfile / raw audio -> audio_data below (MP3/Ogg)
    Content, // cntc content datastore -> content_asset_ids (interactive asset browser)
};

// One vertex, laid out exactly like gw2viewer.cpp's GVertex (14 floats:
// position, normal, tangent, bitangent, UV0) so the model renderer can upload
// it straight into its vertex buffer / input layout.
struct GVertex {
    float px, py, pz;      // 0
    float nx, ny, nz;      // 12
    float tx, ty, tz;      // 24
    float bx, by, bz;      // 36
    float u, v;            // 48  UV0 (TEXCOORD0)
    // UV1..UV7 (TEXCOORD1..7), decoded by gw2model (F16/F32 handled there);
    // absent channels copy UV0. Needed by the game (DXBC) shaders' detail/decal
    // layers -- without them multi-UV materials sample UV0 everywhere (garbled).
    float uv1[7][2] = {};  // 56  -> ends at 112
    // Skinning: bone indices are already remapped to skeleton bone indices
    // (via the boneBindings token map); weights are normalized. All zero/default
    // (bidx=0, bwt={1,0,0,0}) for unskinned meshes -> bone 0 identity.
    uint32_t bidx[4] = {0, 0, 0, 0};  // 112
    float bwt[4] = {1, 0, 0, 0};      // 128  -> stride 144
};

// A decoded texture referenced by a model material (deduped by fileId).
struct ModelTextureCPU {
    uint32_t fileId = 0;
    int width = 0;
    int height = 0;
    std::string fmt;             // source format label, e.g. "DXT5", "3DCX/BC5"
    std::vector<uint8_t> rgba;   // width*height*4, RGBA8888
    bool isNormal = false;       // classified as a normal map (BC5/3DC/ATI2)
};

struct ModelMaterialCPU {
    uint32_t index = 0;
    float tint[4] = {1, 1, 1, 1};
    bool isEffect = false;
    int kind = 0;                        // 0 normal, 1 terrain, 2 water (procedural shading)
    int diffuseTex = -1;                 // index into ModelPreview::textures (-1 = none)
    int normalTex = -1;
    std::vector<uint32_t> textureFileIds; // all textures the material references (for info)
};

// ---- game-shader (real DXBC) material payload -----------------------------
// Built on the background thread (needs the dat + struct template) so the model
// renderer can draw a submesh with the GAME's own bgfx VS+PS pulled from the
// material's AMAT package, instead of our reconstructed HLSL. All CPU-side: raw
// DXBC bytes + the bgfx uniform table + resolved sampler bindings; the renderer
// creates the D3D shaders/layout/cbuffers/SRVs on the UI thread. Mirrors the
// pipeline in gw2mcp/viewer/gw2gsviewer.cpp.
struct GameShaderUniform {
    std::string name;
    int type = 0;      // bgfx UniformType low nibble (0 = sampler, skipped in cbuffers)
    int stage = 0;     // 0 vertex, 1/2 fragment/sampler
    int byteOff = 0;   // byte offset into the cbuffer (bgfx `reg` is a byte offset)
    int vec4s = 0;     // register count (each = one float4)
};
struct GameSamplerCPU {
    int slot = 0;      // t#/s# register the PS binds this texture to
    int gameTex = -1;  // index into ModelPreview::textures, or -1 for a global stand-in
    int global = 0;    // 0 = material texture (gameTex), 1 = white 1x1, 2 = env cubemap grey
};
// A resolved per-material constant: the MODL MatConstant value written straight
// into the shader cbuffer at `byteOff`. Bound by token (AMAT constants[] order
// pairs with the shader's material uniforms), so no name-hash is needed.
struct GameConstOverride {
    int byteOff = 0;
    float value[4] = {0, 0, 0, 0};
};
struct GameMaterial {
    uint32_t index = 0;          // matches ModelMaterialCPU::index / mesh materialIndex
    bool ok = false;             // false = no game shaders available (fall back to reconstruction)
    std::vector<uint8_t> vsDXBC, psDXBC;
    std::vector<GameShaderUniform> vsUniforms, psUniforms;
    uint32_t vsConstBuf = 0, psConstBuf = 0;
    std::vector<GameSamplerCPU> samplers;
    // Per-material constant values (glow/spec/scroll/tint/etc.) resolved from the
    // MODL MatConstants, applied after the shared uniform fill each frame.
    std::vector<GameConstOverride> vsConsts, psConsts;
    uint64_t renderState = 0;    // bgfx 64-bit state word (blend / depth-write)
};

struct ModelMeshCPU {
    std::vector<GVertex> vertices;
    std::vector<uint32_t> indices;
    uint32_t materialIndex = 0;
    uint32_t fvf = 0;
    uint32_t vertexCount = 0;
    int stride = 0;              // decoded per-vertex byte stride (from the FVF)
    bool hasTangents = false;
    bool hasSkin = false;        // vertices carry (resolved) bone indices + weights
};

// One rig joint in bind pose: its model-space origin + parent index (into
// ModelPreview::joints; -1 = root). Enough for the renderer to draw the
// skeleton as a bone line-list overlaid on the mesh.
struct ModelJoint {
    float pos[3] = {0, 0, 0};        // bind-pose (InverseWorld) model-space origin
    int parent = -1;
    std::string name;
    // Bind local transform (used to compose an animated pose; bones with no
    // matching animation track fall back to this).
    float localPos[3] = {0, 0, 0};
    float localQuat[4] = {0, 0, 0, 1};
    float localScale[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float invWorld[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; // InverseWorld4x4 (model->bone bind, row-major)
};

// Everything needed to render + describe a model, all CPU-side so it can be
// produced on a background thread; the renderer uploads it on the UI thread.
struct ModelPreview {
    std::vector<ModelMeshCPU> meshes;
    std::vector<ModelMaterialCPU> materials;
    std::vector<ModelTextureCPU> textures;
    float center[3] = {0, 0, 0};
    float radius = 1.0f;
    uint32_t totalVerts = 0;
    uint32_t totalTris = 0;

    // Skeleton (bind pose). Empty when the model has no inline rig.
    std::vector<ModelJoint> joints;
    int skeletonVersion = -1;        // ModelFileSkeleton chunk version (-1 = none)
    std::string skeletonType;        // resolved ModelSkeletonDataV* key
    uint32_t externalSkeletonRef = 0; // fileId of a referenced external rig (0 = inline/none)

    // Embedded animation bank. Curves are decoded from the Granny blob into
    // animClips (constant/identity fully; keyframe formats best-effort). Most
    // embedded clips are the static "zeropose"; real locomotion lives in
    // external anim files (not yet loaded).
    bool hasAnimation = false;
    int animationVersion = -1;       // ANIM chunk version (selects the format variant)
    std::string animationType;       // resolved ModelFileAnimation*/Bank* key
    std::vector<uint64_t> animationTokens; // per-clip token64 name hashes
    std::vector<granny::Anim> animClips;   // decoded clips (name/duration/tracks)

    // Real game shaders (bgfx DXBC) per material, for the "Shader" render mode.
    // Empty unless the entry was extracted for single-model preview (the map
    // scene path skips this to stay fast). Indexed by GameMaterial::index.
    std::vector<GameMaterial> gameMaterials;
};

// One placed prop in a map: which loaded model + its world transform.
struct MapInstance {
    int model = 0;            // index into MapScene::models
    float pos[3] = {0, 0, 0};
    float rot[3] = {0, 0, 0}; // Euler radians (GW2 is Z-up; rot[2] = yaw)
    float scale = 1.0f;
    int layer = 0;            // 0 prop, 1 terrain, 2 collision, 3 zone (matches gw2m3d::SceneLayer)
};

// A whole map's coordinated model set: the unique prop models + every placement.
struct MapScene {
    std::vector<ModelPreview> models;     // unique prop models (deduped by fileId)
    std::vector<MapInstance> instances;   // one per placed prop
    uint32_t totalProps = 0;
    uint32_t loadedModels = 0;
};

// One decoded audio sound (kind == Audio). ASND/raw = a single clip; ABNK bank = N.
struct AudioClipCPU {
    std::string codec;         // "MP3", "Ogg Vorbis", ...
    std::vector<uint8_t> data; // a complete audio file, ready for gw2snd::play/probe
};

struct ExtractedEntry {
    std::vector<uint8_t> compressed;   // raw on-disk bytes for this MFT entry (CRC32C still present) -- "before"
    std::vector<uint8_t> decompressed; // the fully decompressed file bytes -- "after" (real .atex/.dds/.png/... ),
                                        // exactly what "Export Decompressed" writes; the preview below is derived.

    PreviewKind kind = PreviewKind::None;
    bool is_image = false; // == (kind == Image); kept for existing call sites

    // Normalized, ready-to-upload preview payload -- filled whenever is_image
    // is true, regardless of which decoder produced it. Callers just do:
    //   gw2gfx::set_texture(preview_dxgi_format, preview_width, preview_height, preview_pitch, preview_pixels.data());
    // Textures (ATEX family) and PNG/JPEG/BMP are fully decoded to RGBA8888
    // here (preview_dxgi_format == DXGI_FORMAT_R8G8B8A8_UNORM); a passthrough
    // "DDS " file keeps its native BCn blocks for the GPU to decode.
    uint32_t preview_dxgi_format = 0;
    uint32_t preview_width = 0;
    uint32_t preview_height = 0;
    uint32_t preview_pitch = 0;
    std::vector<uint8_t> preview_pixels;
    std::string preview_format_label; // human-readable source format, e.g. "DXT5", "BC7", "DDS BC1", "PNG"

    // Filled for kind == Text or kind == Strs.
    std::wstring text_preview;

    // Filled for kind == Model when a struct template was available at extract
    // time; null if it's a model but no template was loaded yet.
    std::shared_ptr<ModelPreview> model;

    // Filled for kind == Map (a mapc/area packfile with a prp2 prop chunk).
    std::shared_ptr<MapScene> map;

    // Filled for kind == Audio: one entry per embedded sound (an ASND / raw asnd has
    // one; an ABNK bank has several). Each holds a complete audio file (MP3/Ogg/...).
    std::vector<AudioClipCPU> audio_clips;

    // Filled for kind == Content (cntc): every external asset fileId the content blob
    // references (textures/models/audio/...). The UI lists these; clicking one loads it.
    std::vector<uint32_t> content_asset_ids;
};

// Extracts and decompresses one MFT entry.
//   - Everything is first run through gw2cmp::decompress_method0()
//     (cmp_decompress_method0.hpp) -- the generic ANet Method0 stream.
//   - The decompressed bytes are then sniffed for a known payload:
//       * ATEX/ATTX/ATEC/ATEP/ATEU/ATET -> gw2_atex.hpp (accurate RE decoder).
//       * a standalone "DDS " file -> native BCn on the GPU.
//       * PNG/JPEG/BMP -> decoded to RGBA via WIC (wic_image.h).
//       * "strs" -> string table decoded to text (strs_view.h).
//       * "PF" packfile with a GEOM chunk -> model (gw2model.hpp), if a struct
//         template is loaded (struct_template.h); textures decoded from the dat.
//       * otherwise printable text -> shown as plain text.
// Throws std::runtime_error / gw2cmp::decode_error on malformed data.
// Only safe to call from the thread that owns data_gw2; for background-thread
// use, prefer the file-path overload below.
ExtractedEntry extract_entry(Gw2Dat& data_gw2, uint32_t mft_index);

// Self-contained overload safe to run on a background thread: takes a copy of
// the one MftData entry and the archive's file path directly, touching no
// shared mutable state (own file handle inside, copies of everything else).
// The struct template (if any) is captured internally as a shared_ptr snapshot.
ExtractedEntry extract_entry(const std::string& file_path, const MftData& entry);

// Texture resolution preference for model materials. GW2 stores many textures as a
// full/reduced pair at consecutive MFT indices (full = baseId B, reduced = B-1); a
// material may reference either. When true (default) the loader picks the full-res
// member, when false the reduced one. Takes effect on the next model (re)load.
void set_texture_full_res(bool full);
bool texture_full_res();

// Lazily builds the zone-model layer for an already-decompressed map packfile
// (parses zon2, loads the zone def models in parallel). Returns null if the map
// has no zone models. Instances are tagged layer 3 (zone). Safe to call from a
// worker thread. `map_bytes` = the map's decompressed packfile bytes.
std::shared_ptr<MapScene> build_map_zone_layer(const std::vector<uint8_t>& map_bytes, const std::string& dat_path);

#endif // ENTRY_EXTRACTOR_H
