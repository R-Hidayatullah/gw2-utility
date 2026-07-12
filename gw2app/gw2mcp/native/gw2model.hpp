// gw2model.hpp -- extract renderable geometry + material textures from a
// decompressed GW2 .modl packfile, driven by the same type registry the
// BinaryParser uses (templates/gw2_packfile.json).
//
// Path (verified against the registry + real models):
//   GEOM chunk -> ModelFileGeometryV1.meshes[] -> ModelMeshDataV66
//                   .materialIndex, .geometry -> ModelMeshGeometryV1
//                       .verts (ModelMeshVertexDataV1: vertexCount,
//                                mesh=PackVertexType{ fvf, vertices[byte] })
//                       .indices (ModelMeshIndexDataV1: indices[word])
//   MODL chunk -> ModelFileDataV*.permutations[0].materials[] ->
//                   ModelMaterialDataV*.textures[] -> ModelTextureDataV*.filename
//
// The FVF bitmask decode follows GW2_Vertex_FVF_Notes.md (GrFvf).
#ifndef GW2MODEL_HPP
#define GW2MODEL_HPP

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace gw2model {

using nlohmann::json;

struct Vertex {
    float px, py, pz;         // offset 0
    float nx, ny, nz;         // 12
    float tx, ty, tz; // tangent   (from FVF tangent-frame, else 0)  // 24
    float bx, by, bz; // bitangent (from FVF tangent-frame, else 0)  // 36
    float u, v;               // 48  UV0 (TEXCOORD0) — kept named for gw2viewer
    float uv[7][2];           // 56  UV1..UV7 (TEXCOORD1..7); channel c>=1 -> uv[c-1]
    // GrFvf supports 8 UV channels (bits 8-15); absent channels copy UV0.
    uint8_t boneIdx[4] = {0, 0, 0, 0};   // BlendIndices (FVF 0x4): index into Mesh.boneBindings
    float   boneWt[4] = {1, 0, 0, 0};    // BlendWeights (FVF 0x2): normalized [0,1]
};

struct Mesh {
    uint32_t fvf = 0;
    uint32_t vertexCount = 0;
    uint32_t materialIndex = 0;
    bool hasTangents = false;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    float minB[3] = {0, 0, 0}, maxB[3] = {0, 0, 0};
    // Per-mesh bone binding table: a vertex's BlendIndices index into this, and
    // each entry is a token64 identifying a skeleton bone (matched by name hash).
    std::vector<uint64_t> boneBindings;
    std::vector<int> boneBindingSkelIndex; // boneBindings[k] -> skeleton bone index (-1 = unresolved)
    bool hasSkin = false;                 // FVF carries blend weights + indices
};

// GW2 bone-name -> token64 (reverse of engine sub_140E3B5E0, ModelBones.cpp).
// The engine tokenizes a bone name by 5-bit-packing its letters (a-z/A-Z -> 1..26,
// LSB-first, up to 12 chars) and, if the name ends in two decimal digits, storing
// that number in the top nibble (bits 60-63). The "bone:" display prefix is
// stripped first. Verified: matches every boneBindings entry on real models.
inline uint64_t tokenizeBoneName(const std::string& name) {
    const char* s = name.c_str();
    size_t n = name.size();
    // Strip the namespace prefix up to and including the LAST ':' and tokenize only
    // the leaf. GW2 bone names are namespaced ("bone:COG", "Rig1:bone:BCOG" for a
    // merged multi-rig model, "ignore:FK_LowTeeth"/"actionpoint:MountA" for facial /
    // helper bones); the engine keys on the leaf, and only the leaf is unique. A prior
    // "strip up to last 'bone:'" left non-"bone:" namespaces mis-tokenized: their long
    // prefixes overflowed the 12-char/60-bit pack and COLLIDED (e.g. "ignore:FK_LowTeeth"
    // aliased "ignore:FK_LowlidR"), so a mesh vertex bound to the jaw/teeth bone resolved
    // to the wrong bone and the mouth geometry skinned onto the body. Verified: fileId
    // 1762011 (springer) 88/101 -> 101/101 bindings resolve with 0 collisions; fileId
    // 904350 (Marjory & Kasmeer) 200/203 -> 203/203, 0 collisions. Unprefixed names
    // (rfind == npos) are unchanged.
    if (size_t p = name.rfind(':'); p != std::string::npos) { s += p + 1; n -= p + 1; }
    size_t end = n;
    uint64_t top = 0;
    if (n >= 2) {
        unsigned char last = (unsigned char)s[n - 1], sec = (unsigned char)s[n - 2];
        if (last >= '0' && last <= '9' && sec >= '0' && sec <= '9') {
            end = n - 2;
            top = ((uint64_t)last + 10ull * (uint64_t)sec) & 0xFull; // engine's v6<<60 keeps low nibble
        }
    }
    uint64_t tok = top << 60;
    int i = 0;
    for (size_t k = 0; k < end && i < 60; ++k, i += 5) {
        unsigned char c = (unsigned char)s[k];
        uint64_t val = (c >= 'a' && c <= 'z') ? (c - 96) : (c >= 'A' && c <= 'Z') ? (c - 64) : 0;
        tok |= val << i;
    }
    return tok;
}

struct MatTexture {
    uint32_t fileId = 0;    // decoded texture reference
    uint64_t token = 0;     // sampler token (binds to the shader's AmatSamplerConstant)
    uint32_t flags = 0;     // textureFlags
    uint8_t  uvIndex = 0;   // uvPSInputIndex (which UV channel)
};
struct MatConstant {
    uint32_t name = 0;      // token32 hash of the constant name
    float    value[4] = {0, 0, 0, 0};
};
struct Material {
    uint32_t index = 0;
    uint32_t materialId = 0;    // selects the built-in shader (0..57) or 58=custom
    uint32_t materialFile = 0;  // fileId of the material's .amat/GRMT file (0 if none)
    uint32_t materialFlags = 0;
    uint32_t sortOrder = 0;     // draw/render order
    uint32_t sortLayer = 0;     // transparent/sort hint
    std::vector<MatTexture> textures;
    std::vector<MatConstant> constants;
    // convenience: just the fileIds, file order
    std::vector<uint32_t> textureFileIds() const {
        std::vector<uint32_t> v; for (auto& t : textures) v.push_back(t.fileId); return v;
    }
};

// --- skeleton (bind pose) ---
// One rig bone. Bind-pose joint origin in model space (worldPos) is what the
// viewer draws; localPos/localQuat/scaleShear + parent are kept so an animation
// system can later re-pose the hierarchy per frame.
struct Bone {
    std::string name;
    int32_t parent = -1;                                  // index into bones[]; <0 or out-of-range = root
    float localPos[3] = {0, 0, 0};                        // LocalTransform.Position
    float localQuat[4] = {0, 0, 0, 1};                    // LocalTransform.Orientation (x,y,z,w)
    float scaleShear[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};    // LocalTransform.ScaleShear (row-major 3x3)
    float invWorld[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; // InverseWorld4x4 (bind, model->bone), row-major
    float worldPos[3] = {0, 0, 0};                        // bind-pose joint origin in model space
};
struct Skeleton {
    bool present = false;
    int fileVersion = -1;             // ModelFileSkeletonV0/V1 chunk version (0 or 1)
    std::string skelDataType;         // resolved ModelSkeletonDataV* type key
    uint32_t externalRef = 0;         // fileId of a referenced external skeleton (0 = inline)
    std::vector<Bone> bones;
};

// --- embedded animation bank (MODL ANIM chunk) ---
// The actual keyframe curves are Granny-compressed (not decoded here); we expose
// the chunk version and the per-clip token so a UI can list/select clips and a
// future decoder can fill in the curves.
struct AnimClip {
    uint64_t token = 0;               // token64 name hash of the clip
    float moveSpeed = 0;              // ModelAnimationData.moveSpeed (if present)
    // Raw serialized Granny animation blob (self-relative pointers unresolved)
    // + the pointer-fixup table. Decoded lazily by the granny reader.
    std::vector<uint8_t> rawGranny;
    std::vector<uint32_t> fixups;     // byte offsets into rawGranny holding pointers
    size_t grannyFileOffset = 0;      // absolute offset of rawGranny[0] within the packfile
    int ptrSize = 4;                  // blob pointer width (4=32-bit packfile, 8=64-bit)
};
struct AnimInfo {
    bool present = false;
    uint16_t chunkVersion = 0;        // ANIM chunk version (selects the format variant)
    std::string typeKey;              // resolved ModelFileAnimation*/Bank* type key
    std::vector<AnimClip> clips;
};

struct Model {
    std::vector<Mesh> meshes;
    std::vector<Material> materials;
    Skeleton skeleton;
    AnimInfo anim;
};

// --- AMAT (bgfx shader package) extraction ---
struct AmatSamplerBind {
    uint64_t token = 0;
    uint32_t textureIndex = 0; // which material texture
    uint32_t textureSlot = 0;  // -> sampler register (t#/s#)
};
struct AmatShader {
    bool isPixel = false;
    std::vector<uint8_t> dxbc;  // bgfx VSH/FSH blob (contains DXBC)
    std::vector<AmatSamplerBind> samplers;
    // Ordered list of material-constant name tokens (AmatShaderConstant.token,
    // token32). Same order as the shader's material-constant uniforms in the bgfx
    // uniform table, so index i pairs constantTokens[i] <-> the i-th non-global
    // uniform. A MODL MatConstant whose token matches one of these binds its value
    // to that uniform (no name-hash needed: token-to-token match).
    std::vector<uint32_t> constantTokens;
};
struct AmatSet {
    int vsIndex = -1, psIndex = -1;      // best OPAQUE-pass effect (highest sampler count, passFlags without the transparent bit)
    uint64_t renderState = 0;            // bgfx 64-bit state word of the picked opaque effect
    // best TRANSPARENT-pass effect, if the AMAT actually defines one (shaderPassFlags & AMAT_PASSFLAG_TRANSPARENT).
    bool hasTrans = false;
    int transVsIndex = -1, transPsIndex = -1;
    uint64_t transRenderState = 0;
    std::vector<AmatShader> shaders;     // full shader list
    std::string error;
};
// Observed on 291977 eff13: shaderPassFlags=0x4001 (blend effect) vs 0x1 (opaque).
// Bit 0x4000 is the discriminator between the two pass families.
static const uint32_t AMAT_PASSFLAG_TRANSPARENT = 0x4000u;


// --------------------------------------------------------------------------
class Extractor {
public:
    Extractor(const std::vector<uint8_t>& data, const json& tpl)
        : d_(data.data()), n_(data.size()), tpl_(tpl) {
        if (n_ < 12 || d_[0] != 'P' || d_[1] != 'F') throw std::runtime_error("not a PF packfile");
        types_ = tpl_.contains("types") ? &tpl_["types"] : nullptr;
        if (!types_) throw std::runtime_error("template missing 'types'");
        uint16_t pfv = rd16(2);
        ptr_ = (pfv & 4) ? 8 : 4;
        char c[5] = {0};
        std::memcpy(c, d_ + 8, 4);
        container_ = c;
    }

    Model extract() {
        Model m;
        size_t geom = findChunk("GEOM"), modl = findChunk("MODL");
        if (modl) parseMaterials(modl, m);
        if (geom) parseGeometry(geom, m);
        try { parseSkeleton(m); } catch (const std::exception&) { /* skeleton is optional */ }
        try { parseAnim(m); }     catch (const std::exception&) { /* animation is optional */ }
        resolveBoneBindings(m);
        return m;
    }

    // Map zone model: a zone's def model placed at the zone footprint centroid.
    // (Approximation of GW2's zone scatter system -- one representative placement
    // per zone rather than the full per-instance scatter.)
    struct MapZoneInst {
        uint32_t fileId = 0;
        float pos[3] = {0, 0, 0};
        float scale = 1.0f;
    };

    std::vector<MapZoneInst> parseMapZones() {
        std::vector<MapZoneInst> out;
        std::string root; uint16_t ver = 0;
        size_t z = findChunk("zon2", &root, &ver);
        if (!z || root.empty()) return out;
        size_t off; json f;

        // zoneDefArray: token -> def model fileId.
        std::unordered_map<uint32_t, uint32_t> defFile;
        if (fieldOffset(root, "zoneDefArray", off, f)) {
            std::string dt = f["element"].value("struct", std::string());
            int ds = typeSize(dt);
            uint32_t n = 0; size_t base = arrayAt(z + off, n);
            for (uint32_t i = 0; base && ds > 0 && i < n; ++i) {
                size_t e = base + (size_t)i * ds;
                size_t o; json ff;
                uint32_t tok = fieldOffset(dt, "token", o, ff) ? rd32(e + o) : 0;
                uint32_t fid = decodeFilename(dt, e);
                if (tok) defFile[tok] = fid;
            }
        }
        // zoneArray: each zone -> its def model at the polygon centroid + zPos.
        if (fieldOffset(root, "zoneArray", off, f)) {
            std::string zt = f["element"].value("struct", std::string());
            int zs = typeSize(zt);
            uint32_t n = 0; size_t base = arrayAt(z + off, n);
            for (uint32_t i = 0; base && zs > 0 && i < n; ++i) {
                size_t e = base + (size_t)i * zs;
                size_t o; json ff;
                uint32_t tok = fieldOffset(zt, "defToken", o, ff) ? rd32(e + o) : 0;
                auto it = defFile.find(tok);
                if (it == defFile.end() || !it->second) continue;
                float zpos = fieldOffset(zt, "zPos", o, ff) ? rdf(e + o) : 0;
                float cx = 0, cy = 0; uint32_t vc = 0;
                if (fieldOffset(zt, "vertices", o, ff)) {
                    uint32_t vn = 0; size_t vb = arrayAt(e + o, vn);
                    for (uint32_t v = 0; vb && v < vn; ++v) { cx += rdf(vb + 8u*v); cy += rdf(vb + 8u*v + 4); ++vc; }
                }
                MapZoneInst mz;
                mz.fileId = it->second;
                mz.pos[0] = vc ? cx / vc : 0; mz.pos[1] = vc ? cy / vc : 0; mz.pos[2] = zpos;
                out.push_back(mz);
            }
        }
        return out;
    }

    // Map collision: the Havok collision meshes (walls/floors/ramps) stored in the
    // `havk` chunk as plain vertex+index geometry, plus the real water level.
    struct MapCollision {
        bool present = false;
        float waterZ = 0; bool hasWater = false;
        std::vector<float> verts;      // x,y,z triples (Z-up, world space)
        std::vector<uint32_t> indices; // triangle list
    };

    MapCollision parseMapCollision() {
        MapCollision out;
        std::string root; uint16_t ver = 0;
        size_t h = findChunk("havk", &root, &ver);
        if (!h || root.empty()) return out;
        size_t off; json f;
        if (fieldOffset(root, "waterSurfaceZ", off, f)) { out.waterZ = rdf(h + off); out.hasWater = true; }
        if (!fieldOffset(root, "collisions", off, f)) return out;
        std::string ct = f["element"].value("struct", std::string());
        int cs = typeSize(ct);
        uint32_t cn = 0; size_t cbase = arrayAt(h + off, cn);
        if (cn > 100000) cn = 100000;
        for (uint32_t i = 0; cbase && cs > 0 && i < cn; ++i) {
            size_t ce = cbase + (size_t)i * cs;
            size_t vo, io; json vf, iff;
            if (!fieldOffset(ct, "vertices", vo, vf) || !fieldOffset(ct, "indices", io, iff)) continue;
            uint32_t vn = 0; size_t vbase = arrayAt(ce + vo, vn);
            uint32_t in = 0; size_t ibase = arrayAt(ce + io, in);
            if (!vbase || !ibase || !vn || !in) continue;
            uint32_t voff = (uint32_t)(out.verts.size() / 3);
            for (uint32_t v = 0; v < vn; ++v) {
                out.verts.push_back(rdf(vbase + 12u * v));
                out.verts.push_back(rdf(vbase + 12u * v + 4));
                out.verts.push_back(rdf(vbase + 12u * v + 8));
            }
            for (uint32_t k = 0; k < in; ++k) out.indices.push_back(voff + rd16(ibase + 2u * k));
        }
        out.present = !out.verts.empty() && !out.indices.empty();
        return out;
    }

    // Map prop placement: one placed model instance in an `area` (map) packfile.
    struct MapProp {
        uint32_t fileId = 0;   // model .modl fileId (0 = none)
        float pos[3] = {0, 0, 0};
        float rot[3] = {0, 0, 0}; // Euler angles (radians)
        float scale = 1.0f;
        float bounds[4] = {0, 0, 0, 0};
    };

    // Map terrain: a height-map grid (GW2 is Z-up, so heights are the Z axis).
    struct MapTerrain {
        bool present = false;
        uint32_t dimX = 0, dimY = 0;      // grid dimensions (tiles)
        float swapDistance = 0;
        uint32_t vertsPerChunkSide = 0;
        std::vector<float> heights;       // row-major height samples
        float rect[4] = {0, 0, 0, 0};     // map world rect (x0,y0,x1,y1) from the parm chunk
        bool hasRect = false;
    };

    MapTerrain parseTerrain() {
        MapTerrain out;
        // Map world rect (terrain placement) from the parm chunk.
        {
            std::string proot; uint16_t pver = 0;
            size_t parm = findChunk("parm", &proot, &pver);
            size_t po; json pf;
            if (parm && !proot.empty() && fieldOffset(proot, "rect", po, pf)) {
                for (int k = 0; k < 4; ++k) out.rect[k] = rdf(parm + po + 4 * k);
                out.hasRect = true;
            }
        }
        std::string root; uint16_t ver = 0;
        size_t trn = findChunk("trn", &root, &ver);
        if (!trn || root.empty()) return out;
        size_t off; json f;
        if (fieldOffset(root, "dims", off, f)) { out.dimX = rd32(trn + off); out.dimY = rd32(trn + off + 4); }
        if (fieldOffset(root, "swapDistance", off, f)) out.swapDistance = rdf(trn + off);
        if (fieldOffset(root, "verticesPerChunkSide", off, f)) out.vertsPerChunkSide = rd32(trn + off);
        if (fieldOffset(root, "heightMapArray", off, f)) {
            uint32_t n = 0; size_t base = arrayAt(trn + off, n);
            if (n > 16u * 1024 * 1024) n = 16u * 1024 * 1024;
            out.heights.reserve(n);
            for (uint32_t i = 0; base && i < n; ++i) out.heights.push_back(rdf(base + 4u * i));
        }
        out.present = !out.heights.empty() && out.dimX > 0 && out.dimY > 0;
        return out;
    }

    // Parse the `prp2` chunk of an `area` packfile -> the list of placed props
    // (model + world transform). This is what turns a map into a coordinated set
    // of 3D models. Handles both propArray (single placements) and
    // propInstanceArray (a prop reused at many transforms).
    std::vector<MapProp> parseMapProps() {
        std::vector<MapProp> out;
        std::string root; uint16_t ver = 0;
        size_t prp = findChunk("prp2", &root, &ver);
        if (!prp || root.empty()) return out;

        auto readProp = [&](const std::string& objType, size_t e, MapProp& p) {
            size_t o; json f;
            p.fileId = decodeFilename(objType, e);
            if (fieldOffset(objType, "position", o, f)) for (int k=0;k<3;++k) p.pos[k] = rdf(e+o+4*k);
            if (fieldOffset(objType, "rotation", o, f)) for (int k=0;k<3;++k) p.rot[k] = rdf(e+o+4*k);
            if (fieldOffset(objType, "scale", o, f)) p.scale = rdf(e+o);
            if (fieldOffset(objType, "bounds", o, f)) for (int k=0;k<4;++k) p.bounds[k] = rdf(e+o+4*k);
        };

        size_t off; json fj;
        // propArray -- one model instance each.
        if (fieldOffset(root, "propArray", off, fj)) {
            std::string objType = fj["element"].value("struct", std::string());
            int objSize = typeSize(objType);
            uint32_t n = 0; size_t base = arrayAt(prp + off, n);
            for (uint32_t i = 0; base && objSize > 0 && i < n; ++i) {
                MapProp p; readProp(objType, base + (size_t)i * objSize, p);
                if (p.fileId) out.push_back(p);
            }
        }
        // propInstanceArray -- a base prop plus a list of extra transforms.
        if (fieldOffset(root, "propInstanceArray", off, fj)) {
            std::string instType = fj["element"].value("struct", std::string());
            int instSize = typeSize(instType);
            uint32_t n = 0; size_t base = arrayAt(prp + off, n);
            for (uint32_t i = 0; base && instSize > 0 && i < n; ++i) {
                size_t e = base + (size_t)i * instSize;
                MapProp p; readProp(instType, e, p);
                if (!p.fileId) continue;
                out.push_back(p); // the base placement
                // Extra transforms[] (position/rotation/scale) reusing the same model.
                size_t to; json tf;
                if (fieldOffset(instType, "transforms", to, tf)) {
                    std::string trType = tf["element"].value("struct", std::string());
                    int trSize = typeSize(trType);
                    uint32_t tn = 0; size_t tb = arrayAt(e + to, tn);
                    for (uint32_t j = 0; tb && trSize > 0 && j < tn; ++j) {
                        size_t te = tb + (size_t)j * trSize;
                        MapProp q; q.fileId = p.fileId;
                        size_t o; json f;
                        if (fieldOffset(trType, "position", o, f)) for (int k=0;k<3;++k) q.pos[k] = rdf(te+o+4*k);
                        if (fieldOffset(trType, "rotation", o, f)) for (int k=0;k<3;++k) q.rot[k] = rdf(te+o+4*k);
                        if (fieldOffset(trType, "scale", o, f)) q.scale = rdf(te+o);
                        out.push_back(q);
                    }
                }
            }
        }
        return out;
    }

    // Walk the BGFX chunk (AmatMaterialV*) of an AMAT packfile: extract every
    // shader's DXBC + sampler bindings, and resolve technique[0]/pass[0]/
    // effect[0] to the matched vertex+pixel shader indices.
    AmatSet extractAmat() {
        AmatSet out;
        std::string root; uint16_t ver = 0;
        size_t bgfx = findChunk("BGFX", &root, &ver);
        if (!bgfx || root.empty()) { out.error = "no BGFX chunk / schema"; return out; }
        size_t off; json fj;

        // shaders[] (inline AmatShaderV*)
        if (fieldOffset(root, "shaders", off, fj)) {
            uint32_t n = 0; size_t base = arrayAt(bgfx + off, n);
            std::string shType = fj["element"].value("struct", std::string());
            int shSize = typeSize(shType);
            for (uint32_t i = 0; base && i < n; ++i) {
                size_t se = base + (size_t)i * shSize;
                AmatShader sh;
                size_t o; json f;
                if (fieldOffset(shType, "isPixelShader", o, f)) sh.isPixel = rd32(se + o) != 0;
                if (fieldOffset(shType, "dx11Shader", o, f)) {
                    std::string bin = f.value("type", std::string());
                    size_t bs = se + o, o2; json f2;
                    if (fieldOffset(bin, "data", o2, f2)) {
                        uint32_t dn = 0; size_t db = arrayAt(bs + o2, dn);
                        if (db && dn && db + dn <= n_) sh.dxbc.assign(d_ + db, d_ + db + dn);
                    }
                    if (fieldOffset(bin, "samplers", o2, f2)) {
                        std::string smT = f2["element"].value("struct", std::string());
                        int smSize = typeSize(smT);
                        uint32_t sn = 0; size_t sb = arrayAt(bs + o2, sn);
                        for (uint32_t j = 0; sb && j < sn; ++j) {
                            size_t se2 = sb + (size_t)j * smSize; size_t o3; json f3;
                            AmatSamplerBind b;
                            if (fieldOffset(smT, "token", o3, f3)) b.token = rdPtr(se2 + o3);
                            if (fieldOffset(smT, "textureIndex", o3, f3)) b.textureIndex = rd32(se2 + o3);
                            if (fieldOffset(smT, "textureSlot", o3, f3)) b.textureSlot = rd32(se2 + o3);
                            sh.samplers.push_back(b);
                        }
                    }
                    // constants[] (AmatShaderConstantV*) -- ordered token32 list that
                    // pairs with the shader's material-constant uniforms.
                    if (fieldOffset(bin, "constants", o2, f2)) {
                        std::string cT = f2["element"].value("struct", std::string());
                        int cSize = typeSize(cT);
                        uint32_t cn = 0; size_t cb2 = arrayAt(bs + o2, cn);
                        for (uint32_t j = 0; cb2 && cSize > 0 && j < cn; ++j) {
                            size_t ce = cb2 + (size_t)j * cSize; size_t o3; json f3;
                            if (fieldOffset(cT, "token", o3, f3)) sh.constantTokens.push_back(rd32(ce + o3));
                        }
                    }
                }
                out.shaders.push_back(std::move(sh));
            }
        }

        // Enumerate ALL techniques[].passes[].effects[] and pick the effect
        // whose pixel shader has the MOST sampler bindings -- that's the color
        // pass (depth/shadow/pick pre-passes have few or no textures).
        auto iter = [&](const std::string& type, size_t start, const char* field,
                        std::string& elemType, int& elemSize, uint32_t& count) -> size_t {
            size_t o; json f; count = 0;
            if (!fieldOffset(type, field, o, f)) return 0;
            size_t base = arrayAt(start + o, count);
            elemType = f["element"].value("struct", std::string());
            elemSize = typeSize(elemType);
            return base;
        };
        int bestSamp = -1, bestTransSamp = -1;
        std::string tT, pT, eT, vT; int tS = 0, pS = 0, eS = 0, vS = 0; uint32_t tN = 0, pN = 0, eN = 0, vN = 0;
        size_t tb = iter(root, bgfx, "techniques", tT, tS, tN);
        for (uint32_t ti = 0; tb && ti < tN; ++ti) {
            size_t pb = iter(tT, tb + (size_t)ti * tS, "passes", pT, pS, pN);
            for (uint32_t pi = 0; pb && pi < pN; ++pi) {
                size_t eb = iter(pT, pb + (size_t)pi * pS, "effects", eT, eS, eN);
                for (uint32_t ei = 0; eb && ei < eN; ++ei) {
                    size_t ee = eb + (size_t)ei * eS; size_t o; json f;
                    int ps = -1, vs = -1;
                    if (fieldOffset(eT, "pixelShaderIndex", o, f)) ps = (int)rd32(ee + o);
                    uint32_t spf = 0;
                    if (fieldOffset(eT, "shaderPassFlags", o, f)) spf = rd32(ee + o);
                    if (getenv("AMATDBG")) { uint64_t rsv=0; size_t ro; json rf;
                        if (fieldOffset(eT,"renderState",ro,rf)) rsv=rd64(ee+ro);
                        std::fprintf(stderr,"[amat] tech%u pass%u eff%u ps=%d renderState=0x%llx passFlags=0x%x\n",
                            ti,pi,ei,ps,(unsigned long long)rsv,spf); }
                    size_t vb = iter(eT, ee, "vertexShaderVariants", vT, vS, vN);
                    if (vb && vN && fieldOffset(vT, "vertexShaderIndex", o, f)) vs = (int)rd32(vb + o);
                    if (ps >= 0 && ps < (int)out.shaders.size() && vs >= 0) {
                        int ns = (int)out.shaders[ps].samplers.size();
                        uint64_t eff=0, var=0; size_t eo=0, vo=0; bool ef=false, vf=false;
                        if ((ef=fieldOffset(eT, "renderState", o, f))) { eo=o; eff=rd64(ee + o); }
                        if (vb && (vf=fieldOffset(vT, "renderState", o, f))) { vo=o; var=rd64(vb + o); }
                        uint64_t rstate = eff ? eff : var; // prefer whichever carries a nonzero bgfx state
                        // A COLOUR effect's PS samples the diffuse/albedo at slot 0 (ss0);
                        // depth-only and depth-peel passes (0 samplers, or a lone gSs15/gSs12
                        // global) never do. Gate selection on a real slot-0 sampler so those
                        // utility passes are never chosen as the material's shader -- otherwise
                        // the model renders INVISIBLE. Needed because some AMATs (e.g. chicken
                        // 6012) flag their COLOUR effects with 0x4040 -- the same 0x4000 bit the
                        // opaque/transparent split keys on -- leaving the "opaque" family with
                        // only the 0x40 depth passes, so the depth-peel PS (1 global sampler)
                        // would otherwise win "most samplers".
                        bool isColor = false;
                        for (const auto& sb : out.shaders[ps].samplers) if (sb.textureSlot == 0) { isColor = true; break; }
                        bool isTrans = (spf & AMAT_PASSFLAG_TRANSPARENT) != 0;
                        if (isColor && isTrans) {
                            if (ns > bestTransSamp) { bestTransSamp = ns; out.hasTrans = true;
                                out.transPsIndex = ps; out.transVsIndex = vs; out.transRenderState = rstate; }
                        } else if (isColor) {
                            if (ns > bestSamp) { bestSamp = ns; out.psIndex = ps; out.vsIndex = vs; out.renderState = rstate; }
                        }
                        if (getenv("AMATDBG")) std::fprintf(stderr,
                            "[amat] effT=%s ef=%d@%zu=0x%llx  varT=%s vf=%d@%zu=0x%llx trans=%d\n",
                            eT.c_str(),ef,eo,(unsigned long long)eff, vT.c_str(),vf,vo,(unsigned long long)var,isTrans);
                    }
                }
            }
        }
        // Fallback: some AMATs only ever define one family of effects (no
        // separate opaque/transparent split) -- if we found nothing "opaque"
        // but did find a transparent-flagged effect, use that as the base too.
        if (out.psIndex < 0 && out.hasTrans) { out.psIndex = out.transPsIndex; out.vsIndex = out.transVsIndex; out.renderState = out.transRenderState; }
        return out;
    }

private:
    const uint8_t* d_;
    size_t n_;
    const json& tpl_;
    const json* types_ = nullptr;
    int ptr_ = 8;
    std::string container_;

    // ---- low-level reads (bounds-checked) ----
    void chk(size_t pos, size_t k) const {
        if (pos + k > n_) throw std::runtime_error("read out of bounds");
    }
    uint16_t rd16(size_t p) const { chk(p, 2); return d_[p] | (d_[p + 1] << 8); }
    uint32_t rd32(size_t p) const { chk(p, 4); return d_[p] | (d_[p+1]<<8) | (d_[p+2]<<16) | ((uint32_t)d_[p+3]<<24); }
    uint64_t rd64(size_t p) const { return (uint64_t)rd32(p) | ((uint64_t)rd32(p+4) << 32); }
    uint64_t rdPtr(size_t p) const {
        chk(p, ptr_);
        uint64_t v = 0;
        for (int i = 0; i < ptr_; ++i) v |= (uint64_t)d_[p + i] << (8 * i);
        return v;
    }
    float rdf(size_t p) const { uint32_t u = rd32(p); float f; std::memcpy(&f, &u, 4); return f; }
    static float half2float(uint16_t h) {
        uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF, out;
        if (e == 0) { if (m == 0) out = s << 31; else { while (!(m & 0x400)) { m <<= 1; --e; } ++e; m &= ~0x400u; out = (s<<31)|((e+112)<<23)|(m<<13); } }
        else if (e == 31) out = (s<<31)|0x7F800000|(m<<13);
        else out = (s<<31)|((e+112)<<23)|(m<<13);
        float f; std::memcpy(&f, &out, 4); return f;
    }

    // self-relative pointer at field position p -> absolute target (0 if null)
    size_t follow(size_t p) const { uint64_t s = rdPtr(p); return s ? p + (size_t)s : 0; }
    // array_ptr header at p: out count + base offset of elements (0 if empty)
    size_t arrayAt(size_t p, uint32_t& count) const {
        count = rd32(p);
        size_t pp = p + 4;
        uint64_t s = rdPtr(pp);
        return (s && count) ? pp + (size_t)s : 0;
    }

    // ---- template-driven type geometry ----
    int scalarSize(const std::string& k) const {
        static const std::pair<const char*, int> tbl[] = {
            {"byte",1},{"byte3",3},{"byte4",4},{"word",2},{"word3",6},{"dword",4},
            {"dword2",8},{"dword4",16},{"qword",8},{"float",4},{"float2",8},{"float3",12},
            {"float4",16},{"double",8},{"fileref",4},{"token32",4},{"token64",8}};
        for (auto& e : tbl) if (k == e.first) return e.second;
        return -1;
    }
    int fieldSize(const json& f) const {
        std::string k = f.value("kind", std::string("dword"));
        if (k == "ptr" || k == "char_ptr" || k == "wchar_ptr" || k == "filename") return ptr_;
        if (k == "array_ptr" || k == "ptr_array_ptr") return 4 + ptr_;
        if (k == "struct") return typeSize(f.value("type", std::string()));
        if (k == "array") {
            int cnt = f.value("count", 0);
            const json& el = f.contains("element") ? f["element"] : json("dword");
            int es = el.is_string() ? scalarSize(el.get<std::string>())
                                    : (el.is_object() && el.contains("struct") ? typeSize(el["struct"]) : 4);
            return cnt * (es < 0 ? 4 : es);
        }
        int s = scalarSize(k);
        return s < 0 ? 4 : s;
    }
    int typeSize(const std::string& typeName) const {
        if (!types_->contains(typeName)) return 0;
        int total = 0;
        for (const auto& f : (*types_)[typeName]["fields"]) total += fieldSize(f);
        return total;
    }
    // offset of a named field within a struct type + the field json
    bool fieldOffset(const std::string& typeName, const std::string& name, size_t& off, json& out) const {
        if (!types_->contains(typeName)) return false;
        size_t o = 0;
        for (const auto& f : (*types_)[typeName]["fields"]) {
            if (f.value("name", std::string()) == name) { off = o; out = f; return true; }
            o += fieldSize(f);
        }
        return false;
    }

    // resolve the concrete versioned type key for a chunk fourcc
    std::string chunkType(const std::string& fourcc, uint16_t version) const {
        std::string vs = std::to_string(version);
        const json& ft = tpl_.contains("fileTypes") ? tpl_["fileTypes"] : json::object();
        if (ft.contains(container_) && ft[container_].contains(fourcc) && ft[container_][fourcc].contains(vs))
            return ft[container_][fourcc][vs].get<std::string>();
        const json& ch = tpl_.contains("chunks") ? tpl_["chunks"] : json::object();
        if (ch.contains(fourcc) && ch[fourcc].contains(vs)) return ch[fourcc][vs].get<std::string>();
        return "";
    }

    // walk chunks; return the data-start offset of the first matching fourcc (0 if none)
    size_t findChunk(const std::string& want, std::string* typeKeyOut = nullptr, uint16_t* verOut = nullptr) const {
        size_t pos = rd16(6); // headerSize
        while (pos + 16 <= n_) {
            char fourcc[5] = {0};
            std::memcpy(fourcc, d_ + pos, 4);
            uint32_t chunkSize = rd32(pos + 4);
            uint16_t version = rd16(pos + 8);
            uint16_t chunkHdr = rd16(pos + 10);
            if (want == fourcc) {
                if (typeKeyOut) *typeKeyOut = chunkType(fourcc, version);
                if (verOut) *verOut = version;
                return pos + chunkHdr;
            }
            size_t next = pos + 8 + chunkSize;
            if (next <= pos) break;
            pos = next;
        }
        return 0;
    }

    // ---- geometry ----
    void parseGeometry(size_t geomStart, Model& out) {
        std::string root; uint16_t ver;
        (void)findChunk("GEOM", &root, &ver);
        if (root.empty()) return;
        size_t off; json fj;
        if (!fieldOffset(root, "meshes", off, fj)) return;
        uint32_t count = 0;
        size_t base = arrayAt(geomStart + off, count);
        if (!base) return;
        // meshes is ptr_array_ptr: array of pointers to ModelMeshData*
        std::string meshType = fj["element"].value("struct", std::string());
        for (uint32_t i = 0; i < count; ++i) {
            size_t elemPtrPos = base + (size_t)i * ptr_;
            size_t meshStart = follow(elemPtrPos);
            if (!meshStart) continue;
            try {
                out.meshes.push_back(parseMesh(meshType, meshStart));
            } catch (const std::exception&) { /* skip a bad mesh, keep going */ }
        }
    }

    Mesh parseMesh(const std::string& meshType, size_t s) {
        Mesh mesh;
        size_t off; json fj;
        if (fieldOffset(meshType, "materialIndex", off, fj)) mesh.materialIndex = rd32(s + off);
        if (fieldOffset(meshType, "minBound", off, fj)) for (int i=0;i<3;++i) mesh.minB[i]=rdf(s+off+4*i);
        if (fieldOffset(meshType, "maxBound", off, fj)) for (int i=0;i<3;++i) mesh.maxB[i]=rdf(s+off+4*i);
        // boneBindings: token64[] -- vertex BlendIndices index into this table.
        if (fieldOffset(meshType, "boneBindings", off, fj)) {
            uint32_t bc = 0; size_t bb = arrayAt(s + off, bc);
            if (bc > 4096) bc = 4096;
            for (uint32_t i = 0; bb && i < bc; ++i) mesh.boneBindings.push_back(rd64(bb + 8u * i));
        }
        if (!fieldOffset(meshType, "geometry", off, fj)) throw std::runtime_error("no geometry field");
        size_t geo = follow(s + off);
        if (!geo) throw std::runtime_error("null geometry");
        std::string geoType = fj["target"].value("struct", std::string());
        parseGeom(geoType, geo, mesh);
        return mesh;
    }

    void parseGeom(const std::string& geoType, size_t g, Mesh& mesh) {
        size_t off; json fj;
        // verts (inline struct ModelMeshVertexDataV1)
        if (!fieldOffset(geoType, "verts", off, fj)) throw std::runtime_error("no verts");
        std::string vType = fj.value("type", std::string());
        size_t vs = g + off;
        size_t voff; json vfj;
        fieldOffset(vType, "vertexCount", voff, vfj);
        mesh.vertexCount = rd32(vs + voff);
        fieldOffset(vType, "mesh", voff, vfj); // PackVertexType (inline)
        std::string pvt = vfj.value("type", std::string());
        size_t ps = vs + voff;
        size_t poff; json pfj;
        fieldOffset(pvt, "fvf", poff, pfj);
        mesh.fvf = rd32(ps + poff);
        fieldOffset(pvt, "vertices", poff, pfj); // array_ptr<byte>, count = byte length
        uint32_t vbytes = 0;
        size_t vbase = arrayAt(ps + poff, vbytes);

        // indices (inline struct ModelMeshIndexDataV1)
        std::string iType;
        if (fieldOffset(geoType, "indices", off, fj)) {
            iType = fj.value("type", std::string());
            size_t is = g + off, ioff; json ifj;
            if (fieldOffset(iType, "indices", ioff, ifj)) {
                uint32_t icount = 0;
                size_t ibase = arrayAt(is + ioff, icount);
                if (ibase)
                    for (uint32_t i = 0; i < icount; ++i) mesh.indices.push_back(rd16(ibase + 2u * i));
            }
        }

        if (vbase && mesh.vertexCount) decodeVertices(vbase, vbytes, mesh);
    }

    // ---- FVF decode (GrFvf, GW2_Vertex_FVF_Notes.md) ----
    void decodeVertices(size_t base, uint32_t vbytes, Mesh& mesh) {
        uint32_t fvf = mesh.fvf;
        int stride = 0;
        int posOff = -1, nrmOff = -1, tfOff = -1, uvOff = -1, wtOff = -1, idxOff = -1;
        bool uvHalf = false;
        int uvOffN[8]; bool uvHalfN[8]; for(int c=0;c<8;++c){uvOffN[c]=-1;uvHalfN[c]=false;} // 8 UV channels (GrFvf TEXCOORD0..7)
        auto add = [&](int sz) { int o = stride; stride += sz; return o; };
        if (fvf & 0x1)  posOff = add((fvf & 0x08000000) ? 16 : (fvf & 0x10000000) ? 6 : 12);
        if (fvf & 0x2)  wtOff  = add(4);                      // blend weights (4 x u8 norm)
        if (fvf & 0x4)  idxOff = add(4);                      // blend indices (4 x u8 -> boneBindings)
        if (fvf & 0x8)  nrmOff = add((fvf & 0x04000000) ? 4 : 12); // normal
        if (fvf & 0x10) add(4);                               // color BGRA
        if (fvf & 0x20) add(12);                              // tangent
        if (fvf & 0x40) add(12);                              // bitangent
        if (fvf & 0x80) tfOff = add(12);                      // tangent frame (N,T,B as u8x4)
        for (int i = 0; i < 8; ++i) {
            // A UV channel is present if EITHER its F32 presence bit (0x100<<i)
            // OR its F16 bit (0x10000<<i) is set; the F16 bit alone means the
            // channel exists as 2xHalf (4B). (F32/F16 are mutually exclusive.)
            bool f32 = (fvf & (0x100u << i)) != 0;
            bool f16 = (fvf & (0x10000u << i)) != 0;
            if (f32 || f16) {
                bool h = f16;
                int o = add(h ? 4 : 8);
                if (i == 0) { uvOff = o; uvHalf = h; }
                uvOffN[i] = o; uvHalfN[i] = h;   // all 8 channels
            }
        }
        if (stride <= 0) return;
        mesh.hasTangents = (tfOff >= 0);
        mesh.hasSkin = (wtOff >= 0 && idxOff >= 0);
        uint32_t vc = vbytes / (uint32_t)stride;
        if (mesh.vertexCount && vc > mesh.vertexCount) vc = mesh.vertexCount;
        mesh.vertices.reserve(vc);
        auto u8n = [&](size_t p) { return d_[p] / 127.5f - 1.0f; }; // u8 [0,255] -> [-1,1]
        for (uint32_t i = 0; i < vc; ++i) {
            size_t v = base + (size_t)i * stride;
            Vertex out{};
            if (posOff >= 0) { out.px = rdf(v + posOff); out.py = rdf(v + posOff + 4); out.pz = rdf(v + posOff + 8); }
            if (tfOff >= 0) {
                // tangent frame: 3 x (4 x u8 norm) = Normal, Tangent, Bitangent
                out.nx = u8n(v + tfOff);     out.ny = u8n(v + tfOff + 1);  out.nz = u8n(v + tfOff + 2);
                out.tx = u8n(v + tfOff + 4); out.ty = u8n(v + tfOff + 5);  out.tz = u8n(v + tfOff + 6);
                out.bx = u8n(v + tfOff + 8); out.by = u8n(v + tfOff + 9);  out.bz = u8n(v + tfOff + 10);
            } else if (nrmOff >= 0) {
                out.nx = rdf(v + nrmOff); out.ny = rdf(v + nrmOff + 4); out.nz = rdf(v + nrmOff + 8);
            } else out.nz = 1.0f;
            // read all present UV channels (0..7); missing channels fall back to
            // UV0 so a shader that samples UV1..7 still gets sane coords.
            float uvs[8][2];
            for (int c = 0; c < 8; ++c) {
                if (uvOffN[c] >= 0) {
                    if (uvHalfN[c]) { uvs[c][0] = half2float(rd16(v + uvOffN[c])); uvs[c][1] = half2float(rd16(v + uvOffN[c] + 2)); }
                    else { uvs[c][0] = rdf(v + uvOffN[c]); uvs[c][1] = rdf(v + uvOffN[c] + 4); }
                } else { uvs[c][0] = 1e30f; uvs[c][1] = 1e30f; } // sentinel: fall back below
            }
            if (uvOffN[0] < 0) { uvs[0][0] = uvs[0][1] = 0; }
            out.u = uvs[0][0]; out.v = uvs[0][1];
            if (idxOff >= 0) for (int c = 0; c < 4; ++c) out.boneIdx[c] = d_[v + idxOff + c];
            if (wtOff >= 0) {
                int sum = 0; for (int c = 0; c < 4; ++c) sum += d_[v + wtOff + c];
                if (sum <= 0) { out.boneWt[0] = 1; for (int c = 1; c < 4; ++c) out.boneWt[c] = 0; }
                else for (int c = 0; c < 4; ++c) out.boneWt[c] = d_[v + wtOff + c] / (float)sum;
            }
            for (int c = 1; c < 8; ++c) {
                bool have = (uvOffN[c] >= 0);
                out.uv[c-1][0] = have ? uvs[c][0] : out.u;
                out.uv[c-1][1] = have ? uvs[c][1] : out.v;
            }
            mesh.vertices.push_back(out);
        }
    }

    // ---- materials / textures ----
    void parseMaterials(size_t modlStart, Model& out) {
        std::string root; (void)findChunk("MODL", &root);
        if (root.empty()) return;
        size_t off; json fj;
        if (!fieldOffset(root, "permutations", off, fj)) return;
        uint32_t pcount = 0;
        size_t pbase = arrayAt(modlStart + off, pcount);
        if (!pbase || !pcount) return;
        std::string permType = fj["element"].value("struct", std::string());
        // use permutation 0
        size_t perm = pbase; // array_ptr<struct>: elements are inline structs back-to-back
        int permSize = typeSize(permType);
        (void)permSize;
        size_t moff; json mfj;
        if (!fieldOffset(permType, "materials", moff, mfj)) return;
        uint32_t mcount = 0;
        size_t mbase = arrayAt(perm + moff, mcount); // ptr_array_ptr
        if (!mbase) return;
        std::string matType = mfj["element"].value("struct", std::string());
        for (uint32_t i = 0; i < mcount; ++i) {
            size_t matPtr = follow(mbase + (size_t)i * ptr_);
            if (!matPtr) continue;
            Material mat; mat.index = i;
            size_t off; json fj;
            if (fieldOffset(matType, "materialId", off, fj))    mat.materialId    = rd32(matPtr + off);
            if (fieldOffset(matType, "materialFlags", off, fj)) mat.materialFlags = rd32(matPtr + off);
            if (fieldOffset(matType, "sortOrder", off, fj))     mat.sortOrder     = rd32(matPtr + off);
            if (fieldOffset(matType, "sortLayer", off, fj))     mat.sortLayer     = rd32(matPtr + off);
            mat.materialFile = decodeFilename(matType, matPtr); // the material's own 'filename' field

            // textures[] -- fileId + sampler token + flags + uv channel
            if (fieldOffset(matType, "textures", off, fj)) {
                uint32_t tcount = 0;
                size_t tbase = arrayAt(matPtr + off, tcount);
                std::string texType = fj["element"].value("struct", std::string());
                int texSize = typeSize(texType);
                for (uint32_t t = 0; tbase && t < tcount; ++t) {
                    size_t texElem = tbase + (size_t)t * texSize;
                    MatTexture mt;
                    mt.fileId = decodeFilename(texType, texElem);
                    size_t so; json sfj;
                    if (fieldOffset(texType, "token", so, sfj)) mt.token = rdPtr(texElem + so);
                    if (fieldOffset(texType, "textureFlags", so, sfj)) mt.flags = rd32(texElem + so);
                    if (fieldOffset(texType, "uvPSInputIndex", so, sfj)) mt.uvIndex = (uint8_t)d_[texElem + so];
                    mat.textures.push_back(mt);
                }
            }
            // constants[] -- tint/parameter float4 by token32 name
            if (fieldOffset(matType, "constants", off, fj)) {
                uint32_t ccount = 0;
                size_t cbase = arrayAt(matPtr + off, ccount);
                std::string cType = fj["element"].value("struct", std::string());
                int cSize = typeSize(cType);
                for (uint32_t c = 0; cbase && c < ccount; ++c) {
                    size_t cElem = cbase + (size_t)c * cSize;
                    MatConstant mc;
                    size_t so; json sfj;
                    if (fieldOffset(cType, "name", so, sfj)) mc.name = rd32(cElem + so);
                    if (fieldOffset(cType, "value", so, sfj)) for (int k = 0; k < 4; ++k) mc.value[k] = rdf(cElem + so + 4 * k);
                    mat.constants.push_back(mc);
                }
            }
            out.materials.push_back(std::move(mat));
        }
    }

    // decode a "filename" field of a struct -> fileId (0 if none/invalid)
    uint32_t decodeFilename(const std::string& typeName, size_t structStart) {
        size_t off; json fj;
        if (!fieldOffset(typeName, "filename", off, fj)) return 0;
        size_t t = follow(structStart + off);
        if (!t) return 0;
        uint16_t lo = rd16(t), hi = rd16(t + 2);
        uint16_t third = (t + 6 <= n_) ? rd16(t + 4) : 0;
        uint16_t fourth = (t + 8 <= n_) ? rd16(t + 6) : 0;
        if (lo < 0x100 || hi < 0x100 || (third != 0 && (third < 0x100 || fourth != 0))) return 0;
        return (uint32_t)(0xFF00L * hi + lo - 0xFF00FF);
    }

    // ---- skeleton ----
    std::string readCStr(size_t p, size_t cap = 256) const {
        std::string s;
        for (size_t i = 0; i < cap && p + i < n_; ++i) {
            char c = (char)d_[p + i];
            if (!c) break;
            s.push_back(c);
        }
        return s;
    }
    // struct target of a "ptr" field ("" if it isn't a struct ptr)
    static std::string ptrTargetStruct(const json& f) {
        if (f.contains("target") && f["target"].is_object() && f["target"].contains("struct"))
            return f["target"]["struct"].get<std::string>();
        return "";
    }
    // 3x3 inverse of a row-major matrix; false if singular.
    static bool inv3x3(const float m[9], float o[9]) {
        float a=m[0],b=m[1],c=m[2],d=m[3],e=m[4],f=m[5],g=m[6],h=m[7],i=m[8];
        float det = a*(e*i-f*h) - b*(d*i-f*g) + c*(d*h-e*g);
        if (std::abs(det) < 1e-20f) return false;
        float id = 1.0f/det;
        o[0]=(e*i-f*h)*id; o[1]=(c*h-b*i)*id; o[2]=(b*f-c*e)*id;
        o[3]=(f*g-d*i)*id; o[4]=(a*i-c*g)*id; o[5]=(c*d-a*f)*id;
        o[6]=(d*h-e*g)*id; o[7]=(b*g-a*h)*id; o[8]=(a*e-b*d)*id;
        return true;
    }

    void parseSkeleton(Model& out) {
        std::string root; uint16_t ver = 0;
        size_t skel = findChunk("SKEL", &root, &ver);
        if (!skel || root.empty()) return;
        out.skeleton.fileVersion = (int)ver;
        size_t off; json fj;
        // Optional external skeleton reference (this .modl uses another rig).
        if (fieldOffset(root, "fileReference", off, fj)) out.skeleton.externalRef = decodeFilenameAt(skel + off);
        if (!fieldOffset(root, "skeletonData", off, fj)) return;
        std::string sdType = ptrTargetStruct(fj);
        out.skeleton.skelDataType = sdType;
        size_t sd = follow(skel + off);
        if (!sd || sdType.empty()) return; // inline data absent (external rig)

        if (!fieldOffset(sdType, "grannyModel", off, fj)) return;
        std::string gmType = ptrTargetStruct(fj);
        size_t gm = follow(sd + off);
        if (!gm || gmType.empty()) return;

        if (!fieldOffset(gmType, "Skeleton", off, fj)) return;
        std::string gsType = ptrTargetStruct(fj);
        size_t gs = follow(gm + off);
        if (!gs || gsType.empty()) return;

        if (!fieldOffset(gsType, "Bones", off, fj)) return;
        std::string boneType = fj.contains("element") ? fj["element"].value("struct", std::string()) : std::string();
        if (boneType.empty()) return;
        int boneSize = typeSize(boneType);
        uint32_t bn = 0;
        size_t bbase = arrayAt(gs + off, bn);
        if (bn > 4096) bn = 4096; // sanity guard
        for (uint32_t i = 0; bbase && boneSize > 0 && i < bn; ++i) {
            size_t be = bbase + (size_t)i * boneSize;
            Bone bone;
            size_t o; json f;
            if (fieldOffset(boneType, "Name", o, f)) { size_t s = follow(be + o); if (s) bone.name = readCStr(s); }
            if (fieldOffset(boneType, "ParentIndex", o, f)) bone.parent = (int32_t)rd32(be + o);
            if (fieldOffset(boneType, "LocalTransform", o, f)) {
                std::string tt = f.value("type", std::string());
                size_t ts = be + o, to; json tf;
                if (fieldOffset(tt, "Position", to, tf))    for (int k=0;k<3;++k) bone.localPos[k]  = rdf(ts+to+4*k);
                if (fieldOffset(tt, "Orientation", to, tf)) for (int k=0;k<4;++k) bone.localQuat[k] = rdf(ts+to+4*k);
                if (fieldOffset(tt, "ScaleShear", to, tf))  for (int k=0;k<9;++k) bone.scaleShear[k]= rdf(ts+to+4*k);
            }
            if (fieldOffset(boneType, "InverseWorld4x4", o, f)) for (int k=0;k<16;++k) bone.invWorld[k] = rdf(be+o+4*k);
            out.skeleton.bones.push_back(std::move(bone));
        }
        computeBonePositions(out.skeleton);
        out.skeleton.present = !out.skeleton.bones.empty();
    }

    // decode a raw "filename" pointer field at position p (like decodeFilename but by offset)
    uint32_t decodeFilenameAt(size_t p) {
        size_t t = follow(p);
        if (!t) return 0;
        uint16_t lo = rd16(t), hi = rd16(t + 2);
        uint16_t third = (t + 6 <= n_) ? rd16(t + 4) : 0;
        uint16_t fourth = (t + 8 <= n_) ? rd16(t + 6) : 0;
        if (lo < 0x100 || hi < 0x100 || (third != 0 && (third < 0x100 || fourth != 0))) return 0;
        return (uint32_t)(0xFF00L * hi + lo - 0xFF00FF);
    }

    // Bind-pose joint origin = translation of inverse(InverseWorld). Each bone's
    // InverseWorld is the exact model->bone bind matrix used to skin the mesh, so
    // its inverse translation is the joint position in the very space the mesh is
    // in -- no hierarchy accumulation, guaranteed to line up with the geometry.
    // Falls back to hierarchical local-transform composition if a matrix is
    // singular (e.g. all-zero InverseWorld on a helper bone).
    void computeBonePositions(Skeleton& sk) {
        bool anyGood = false;
        std::vector<bool> ok(sk.bones.size(), false);
        for (size_t i = 0; i < sk.bones.size(); ++i) {
            const float* IW = sk.bones[i].invWorld;
            float R[9] = {IW[0],IW[1],IW[2], IW[4],IW[5],IW[6], IW[8],IW[9],IW[10]};
            float t[3] = {IW[12],IW[13],IW[14]};
            float Ri[9];
            if (inv3x3(R, Ri)) {
                // worldPos = -t * Ri (t as a row vector)
                for (int j = 0; j < 3; ++j)
                    sk.bones[i].worldPos[j] = -(t[0]*Ri[0+j] + t[1]*Ri[3+j] + t[2]*Ri[6+j]);
                ok[i] = anyGood = true;
            }
        }
        if (anyGood) {
            // Patch any singular bones with their parent's position (visual only).
            for (size_t i = 0; i < sk.bones.size(); ++i)
                if (!ok[i]) {
                    int p = sk.bones[i].parent;
                    if (p >= 0 && p < (int)sk.bones.size() && ok[p])
                        for (int j = 0; j < 3; ++j) sk.bones[i].worldPos[j] = sk.bones[p].worldPos[j];
                }
            return;
        }
        // No usable InverseWorld anywhere: compose local translations down the
        // hierarchy (rotation-free approximation; good enough to see the rig).
        for (size_t i = 0; i < sk.bones.size(); ++i) {
            int p = sk.bones[i].parent;
            float base[3] = {0,0,0};
            if (p >= 0 && p < (int)i) for (int j=0;j<3;++j) base[j] = sk.bones[p].worldPos[j];
            for (int j = 0; j < 3; ++j) sk.bones[i].worldPos[j] = base[j] + sk.bones[i].localPos[j];
        }
    }

    // ---- embedded animation bank ----
    void parseAnim(Model& out) {
        std::string root; uint16_t ver = 0;
        size_t anim = findChunk("ANIM", &root, &ver);
        if (!anim) return;
        out.anim.present = true;
        out.anim.chunkVersion = ver;
        out.anim.typeKey = root;
        if (root.empty()) return;
        size_t off; json fj;
        // The clip list ("animations") lives directly in older ModelFileAnimation*
        // / *Bank* types, but newer ModelFileAnimationV24/V25 wrap it in a `bank`
        // pointer -> ModelFileAnimationBankV*; follow that first when present.
        std::string listType = root;
        size_t listStart = anim;
        if (!fieldOffset(listType, "animations", off, fj)) {
            if (!fieldOffset(root, "bank", off, fj)) return;
            std::string bankType = ptrTargetStruct(fj);
            size_t bank = follow(anim + off);
            if (!bank || bankType.empty()) return;
            listType = bankType; listStart = bank;
            if (!fieldOffset(listType, "animations", off, fj)) return;
        }
        std::string clipType = fj.contains("element") ? fj["element"].value("struct", std::string()) : std::string();
        uint32_t an = 0;
        size_t abase = arrayAt(listStart + off, an);
        if (an > 8192) an = 8192;
        for (uint32_t i = 0; abase && i < an; ++i) {
            size_t clip = follow(abase + (size_t)i * ptr_); // ptr_array_ptr: array of pointers
            if (!clip) continue;
            AnimClip c;
            c.ptrSize = ptr_;   // granny blob pointers match the packfile's width
            size_t o; json f;
            if (!clipType.empty()) {
                if (fieldOffset(clipType, "token", o, f)) c.token = rd64(clip + o);
                if (fieldOffset(clipType, "moveSpeed", o, f)) c.moveSpeed = rdf(clip + o);
                // data (PackGrannyAnimationType*) -> raw granny blob + fixup table
                if (fieldOffset(clipType, "data", o, f)) {
                    std::string gt = f.value("type", std::string());
                    size_t gs = clip + o, go; json gf;
                    if (!gt.empty() && fieldOffset(gt, "animation", go, gf)) {
                        uint32_t bn2 = 0; size_t bb = arrayAt(gs + go, bn2);
                        if (bb && bn2 && bb + bn2 <= n_) {
                            c.rawGranny.assign(d_ + bb, d_ + bb + bn2);
                            c.grannyFileOffset = bb;
                        }
                    }
                    if (!gt.empty() && fieldOffset(gt, "pointers", go, gf)) {
                        uint32_t pn = 0; size_t pb = arrayAt(gs + go, pn);
                        for (uint32_t k = 0; pb && k < pn && pb + 4u*k + 4 <= n_; ++k)
                            c.fixups.push_back(rd32(pb + 4u*k));
                    }
                }
            }
            out.anim.clips.push_back(std::move(c));
        }
    }

    // Map every mesh's boneBindings token -> skeleton bone index, using the
    // engine's bone-name tokenizer. This is what lets the per-vertex BlendIndices
    // (which index boneBindings) resolve to skeleton bones for skinning.
    void resolveBoneBindings(Model& out) {
        if (out.skeleton.bones.empty()) return;
        std::unordered_map<uint64_t, int> tokMap;
        tokMap.reserve(out.skeleton.bones.size() * 2);
        for (size_t i = 0; i < out.skeleton.bones.size(); ++i)
            tokMap[tokenizeBoneName(out.skeleton.bones[i].name)] = (int)i;
        for (auto& mesh : out.meshes) {
            mesh.boneBindingSkelIndex.assign(mesh.boneBindings.size(), -1);
            for (size_t k = 0; k < mesh.boneBindings.size(); ++k) {
                auto it = tokMap.find(mesh.boneBindings[k]);
                if (it != tokMap.end()) mesh.boneBindingSkelIndex[k] = it->second;
            }
        }
    }
};

// Human-readable breakdown of a GrFvf bitmask: one label per present vertex
// component (with its byte size + offset) and the total per-vertex stride.
// Mirrors Extractor::decodeVertices exactly -- keep the two in sync.
inline std::vector<std::string> describeFvf(uint32_t fvf, int* strideOut = nullptr) {
    std::vector<std::string> comps;
    int stride = 0;
    auto add = [&](const char* label, int sz) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s (%dB @%d)", label, sz, stride);
        comps.emplace_back(buf);
        stride += sz;
    };
    if (fvf & 0x1)
        add((fvf & 0x08000000) ? "Position float4" : (fvf & 0x10000000) ? "Position half3" : "Position float3",
            (fvf & 0x08000000) ? 16 : (fvf & 0x10000000) ? 6 : 12);
    if (fvf & 0x2)  add("BlendWeights", 4);
    if (fvf & 0x4)  add("BlendIndices", 4);
    if (fvf & 0x8)  add((fvf & 0x04000000) ? "Normal packed" : "Normal float3", (fvf & 0x04000000) ? 4 : 12);
    if (fvf & 0x10) add("Color BGRA", 4);
    if (fvf & 0x20) add("Tangent float3", 12);
    if (fvf & 0x40) add("Bitangent float3", 12);
    if (fvf & 0x80) add("TangentFrame u8x12", 12);
    for (int i = 0; i < 8; ++i) {
        bool f32 = (fvf & (0x100u << i)) != 0;
        bool f16 = (fvf & (0x10000u << i)) != 0;
        if (f32 || f16) {
            char label[32];
            std::snprintf(label, sizeof label, "UV%d %s", i, f16 ? "half2" : "float2");
            add(label, f16 ? 4 : 8);
        }
    }
    if (strideOut) *strideOut = stride;
    return comps;
}

} // namespace gw2model
#endif
