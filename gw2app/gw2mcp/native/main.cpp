// gw2dat_cli -- headless command-line front-end over the reverse-engineered
// GW2 archive/decompress/parse code. Emits JSON on stdout so a thin MCP layer
// (see ../server/server.py) can shell out to it. All the heavy lifting stays
// in C++: gw2dat.cpp (MFT), cmp_decompress_method0.hpp (ANet Method0),
// gw2_atex.hpp (ATEX->RGBA), BinaryParser.cpp (packfile template engine).
//
// Commands (every one prints a single JSON object to stdout):
//   info     --dat <path>
//   list     --dat <path> [--limit N] [--offset K]
//   lookup   --dat <path> (--file-id N | --base-id N | --search-file-id N | --search-base-id N)
//   extract  --dat <path> (--index N | --file-id N) --out <file>
//   texture  --dat <path> (--index N | --file-id N) --out <png> [--mip L]
//   parse    (--dat <path> (--index N | --file-id N) | --data <bin>) --template <json>
//                                            [--max-depth D] [--max-nodes N] [--out <json>]
//   sniff    --dat <path> (--index N | --file-id N)
//
// On success exit code is 0 and the JSON has "ok": true; on failure exit code
// is 1 and the JSON is {"ok": false, "error": "..."}.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "gw2dat.h"
#include "cmp_decompress_method0.hpp"
#include "gw2_atex.hpp"
#include "BinaryParser.h"
#include "gw2model.hpp"
#include "granny_anim.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

using json = nlohmann::json;

// ---------------------------------------------------------------------------
//  small utilities
// ---------------------------------------------------------------------------
namespace {

[[noreturn]] void fail(const std::string& msg) {
    json j;
    j["ok"] = false;
    j["error"] = msg;
    std::fputs(j.dump().c_str(), stdout);
    std::fputc('\n', stdout);
    std::exit(1);
}

void emit(const json& j) {
    std::fputs(j.dump().c_str(), stdout);
    std::fputc('\n', stdout);
}

using Args = std::map<std::string, std::string>;

Args parse_args(int argc, char** argv, int start) {
    Args a;
    for (int i = start; i < argc; ++i) {
        std::string tok = argv[i];
        if (tok.rfind("--", 0) == 0) {
            std::string key = tok.substr(2);
            if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
                a[key] = argv[++i];
            } else {
                a[key] = "true"; // bare flag
            }
        }
    }
    return a;
}

std::string need(const Args& a, const std::string& key) {
    auto it = a.find(key);
    if (it == a.end()) fail("missing required argument --" + key);
    return it->second;
}

bool has(const Args& a, const std::string& key) { return a.find(key) != a.end(); }

uint64_t to_u64(const std::string& s) { return std::stoull(s); }

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) fail("cannot open file: " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------------------
//  DAT helpers
// ---------------------------------------------------------------------------

// Resolve --index / --base-id / --file-id (in `a`) to a concrete MFT index.
// NOTE: in this archive model the "baseId" stored in the file-id table *is*
// the physical MFT entry index, so --index and --base-id are the same thing;
// --file-id is the game's logical id and must be looked up first.
uint32_t resolve_index(Gw2Dat& dat, const Args& a) {
    if (has(a, "index")) {
        uint64_t idx = to_u64(a.at("index"));
        if (idx >= dat.mft_data_list.size()) fail("index out of range");
        return static_cast<uint32_t>(idx);
    }
    if (has(a, "base-id")) {
        uint64_t base_id = to_u64(a.at("base-id"));
        if (base_id == 0 || base_id - 1 >= dat.mft_data_list.size()) fail("base-id out of range");
        return static_cast<uint32_t>(base_id - 1);   // ✅ base_id 1-based -> index
    }
    if (has(a, "file-id")) {
        uint32_t file_id = static_cast<uint32_t>(to_u64(a.at("file-id")));
        uint32_t base_id = get_by_base_id(dat, file_id);
        if (base_id == 0) fail("file-id not found: " + std::to_string(file_id));
        if (base_id - 1 >= dat.mft_data_list.size()) fail("resolved MFT index out of range");
        return base_id - 1;   // ✅ tambahkan -1
    }
    fail("need --index or --base-id or --file-id");
}

// Replicates entry_extractor.cpp's decompress_raw_entry() minus the GUI image
// path: strip the per-chunk CRC32C framing, then (if compressed) run Method0.
std::vector<uint8_t> decompress_entry(const std::vector<uint8_t>& raw, uint16_t compression_flag) {
    std::vector<uint8_t> stripped = gw2cmp::strip_crc32(std::span<const uint8_t>(raw));
    if (compression_flag == 0) {
        return stripped;
    }
    if (stripped.size() < 8) throw std::runtime_error("entry too small to contain a Method0 header");
    uint32_t uncompressed_size = static_cast<uint32_t>(stripped[4]) |
                                 (static_cast<uint32_t>(stripped[5]) << 8) |
                                 (static_cast<uint32_t>(stripped[6]) << 16) |
                                 (static_cast<uint32_t>(stripped[7]) << 24);
    return gw2cmp::decompress_method0(std::span<const uint8_t>(stripped).subspan(8), uncompressed_size);
}

// The ATEX-family texture magics gw2atex understands, plus the "C" siblings
// (CTEX/CTTX/CTEC/CTEP/CTEU/CTET) which share the exact same on-disk layout --
// same fourcc + width/height + mip records -- and differ only in this tag.
// alias_texture_magic() below rewrites C->A so the same decoder handles both.
bool is_texture_magic(const std::vector<uint8_t>& d) {
    if (d.size() < 4) return false;
    static const char* MAG[] = {"ATEX", "ATTX", "ATEC", "ATEP", "ATEU", "ATET",
                                "CTEX", "CTTX", "CTEC", "CTEP", "CTEU", "CTET"};
    for (auto* m : MAG)
        if (std::memcmp(d.data(), m, 4) == 0) return true;
    return false;
}

// If `d` starts with a "C" texture magic, flip byte 0 to 'A' in place so
// gw2atex::parse (which whitelists only the A-family) accepts it. Returns the
// original 4-byte magic, or "" if nothing was changed.
std::string alias_texture_magic(std::vector<uint8_t>& d) {
    if (d.size() >= 4 && d[0] == 'C') {
        static const char* CVAR[] = {"CTEX", "CTTX", "CTEC", "CTEP", "CTEU", "CTET"};
        for (auto* m : CVAR) {
            if (std::memcmp(d.data(), m, 4) == 0) {
                std::string orig(reinterpret_cast<char*>(d.data()), 4);
                d[0] = 'A';
                return orig;
            }
        }
    }
    return "";
}

const char* sniff_type(const std::vector<uint8_t>& d) {
    auto m4 = [&](const char* s) { return d.size() >= 4 && std::memcmp(d.data(), s, 4) == 0; };
    if (d.size() >= 2 && d[0] == 'P' && d[1] == 'F') return "packfile";
    if (is_texture_magic(d)) return "texture";
    if (m4("strs")) return "strs";
    if (m4("DDS ")) return "dds";
    if (m4("RIFF")) return "riff";
    if (d.size() >= 4 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return "png";
    if (d.size() >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "jpeg";
    return "binary";
}

// Printable 4-byte tag at `pos` (non-printable -> '.'), or "" if out of range.
std::string tag_at(const std::vector<uint8_t>& d, size_t pos) {
    if (d.size() < pos + 4) return "";
    std::string s;
    for (size_t i = pos; i < pos + 4; ++i) {
        uint8_t c = d[i];
        s += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
    }
    return s;
}

// Compact description of decompressed bytes: 4-byte magic, detected type, and
// (for a PF container) the fourcc at offset 8 that names the real payload
// (MODL/AMAT/ASND/ABNK/...).
json describe(const std::vector<uint8_t>& d) {
    json j;
    j["magic"] = tag_at(d, 0);
    j["type"] = sniff_type(d);
    j["size"] = d.size();
    if (d.size() >= 2 && d[0] == 'P' && d[1] == 'F') j["containerType"] = tag_at(d, 8);
    if (is_texture_magic(d)) j["fourcc"] = tag_at(d, 4); // DXT1/DXT5/3DCX/... without decoding
    return j;
}

// ParsedNode tree -> JSON (depth-capped so huge packfiles stay printable).
json node_to_json(const ParsedNodePtr& n, int depth, int max_depth) {
    json j;
    j["name"] = n->name;
    j["type"] = n->typeName;
    j["offset"] = n->offset;
    j["size"] = n->size;
    if (!n->valueString.empty()) j["value"] = n->valueString;
    if (!n->children.empty()) {
        if (depth >= max_depth) {
            j["childrenTruncated"] = n->children.size();
        } else {
            json arr = json::array();
            for (const auto& c : n->children) arr.push_back(node_to_json(c, depth + 1, max_depth));
            j["children"] = std::move(arr);
        }
    }
    return j;
}

// ---------------------------------------------------------------------------
//  commands
// ---------------------------------------------------------------------------

void cmd_info(const Args& a) {
    Gw2Dat dat;
    load_dat_file(dat, need(a, "dat"));
    json j;
    j["ok"] = true;
    j["filePath"] = dat.file_info.file_path;
    j["fileSize"] = dat.file_info.file_size;
    j["version"] = dat.dat_header.version;
    j["chunkSize"] = dat.dat_header.chunk_size;
    j["mftOffset"] = dat.dat_header.mft_offset;
    j["mftSize"] = dat.dat_header.mft_size;
    j["mftEntryCount"] = dat.mft_data_list.size();
    j["fileIdCount"] = dat.mft_file_id_data_list.size();
    j["baseIdCount"] = dat.mft_base_id_data_list.size();
    emit(j);
}

void cmd_list(const Args& a) {
    Gw2Dat dat;
    load_dat_file(dat, need(a, "dat"));
    size_t offset = has(a, "offset") ? to_u64(a.at("offset")) : 0;
    size_t limit = has(a, "limit") ? to_u64(a.at("limit")) : 100;
    size_t total = dat.mft_data_list.size();
    size_t end = std::min(total, offset + limit);
    json entries = json::array();
    for (size_t i = offset; i < end; ++i) {
        const MftData& e = dat.mft_data_list[i];
        entries.push_back({{"index", i},
                           {"offset", e.offset},
                           {"size", e.size},
                           {"compressionFlag", e.compression_flag},
                           {"entryFlag", e.entry_flag},
                           {"uncompressedSize", e.uncompressed_size},
                           {"crc", e.crc}});
    }
    json j;
    j["ok"] = true;
    j["total"] = total;
    j["offset"] = offset;
    j["count"] = entries.size();
    j["entries"] = std::move(entries);
    emit(j);
}

void cmd_lookup(const Args& a) {
    Gw2Dat dat;
    load_dat_file(dat, need(a, "dat"));
    json j;
    j["ok"] = true;
    if (has(a, "file-id")) {
        uint32_t file_id = static_cast<uint32_t>(to_u64(a.at("file-id")));
        uint32_t base_id = get_by_base_id(dat, file_id);
        j["fileId"] = file_id;
        j["baseId"] = base_id;        // == MFT index used by extract/texture/parse
        j["mftIndex"] = base_id;
        j["found"] = base_id != 0;
    } else if (has(a, "base-id")) {
        uint32_t base_id = static_cast<uint32_t>(to_u64(a.at("base-id")));
        j["baseId"] = base_id;
        j["fileIds"] = get_by_file_id(dat, base_id);
    } else if (has(a, "search-file-id")) {
        j["query"] = a.at("search-file-id");
        j["fileIds"] = search_by_file_id(dat, static_cast<uint32_t>(to_u64(a.at("search-file-id"))));
    } else if (has(a, "search-base-id")) {
        j["query"] = a.at("search-base-id");
        j["baseIds"] = search_by_base_id(dat, static_cast<uint32_t>(to_u64(a.at("search-base-id"))));
    } else {
        fail("lookup needs one of --file-id / --base-id / --search-file-id / --search-base-id");
    }
    emit(j);
}

// Extract + decompress by explicit MFT index; returns describe() or an error
// object (never throws) -- used by `resolve` to probe both interpretations.
json probe_index(Gw2Dat& dat, uint64_t idx) {
    json j;
    if (idx >= dat.mft_data_list.size()) {
        j["valid"] = false;
        j["error"] = "index out of range";
        return j;
    }
    try {
        const MftData& e = dat.mft_data_list[idx];
        std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
        std::vector<uint8_t> data = decompress_entry(raw, e.compression_flag);
        j = describe(data);
        j["valid"] = true;
        j["mftIndex"] = idx;
        j["compressionFlag"] = e.compression_flag;
    } catch (const std::exception& ex) {
        j["valid"] = false;
        j["mftIndex"] = idx;
        j["error"] = ex.what();
    }
    return j;
}

// resolve: show how one number decodes under *both* id namespaces, so a
// curated id of unknown kind (fileId vs baseId/index) can be disambiguated.
void cmd_resolve(const Args& a) {
    Gw2Dat dat;
    load_dat_file(dat, need(a, "dat"));
    uint64_t id = to_u64(need(a, "id"));
    json j;
    j["ok"] = true;
    j["id"] = id;

    // (1) as a baseId / direct MFT index
    j["asBaseId"] = probe_index(dat, id);

    // (2) as a game fileId -> look up its baseId, then probe that
    json as_file;
    uint32_t base = get_by_base_id(dat, static_cast<uint32_t>(id)); // fileId -> baseId
    if (base == 0) {
        as_file["found"] = false;
    } else {
        as_file = probe_index(dat, base);
        as_file["found"] = true;
        as_file["fileId"] = id;
        as_file["baseId"] = base;
    }
    j["asFileId"] = as_file;
    emit(j);
}

// Extract + decompress one entry, returning the decompressed file bytes.
std::vector<uint8_t> extract_bytes(Gw2Dat& dat, const Args& a, uint32_t& idx_out) {
    uint32_t idx = resolve_index(dat, a);
    idx_out = idx;
    const MftData& e = dat.mft_data_list[idx];
    std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
    return decompress_entry(raw, e.compression_flag);
}

void cmd_extract(const Args& a) {
    Gw2Dat dat;
    load_dat_file(dat, need(a, "dat"));
    std::string out = need(a, "out");
    uint32_t idx = 0;
    std::vector<uint8_t> data = extract_bytes(dat, a, idx);
    std::ofstream of(out, std::ios::binary);
    if (!of) fail("cannot write output file: " + out);
    of.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    json j;
    j["ok"] = true;
    j["index"] = idx;
    j["out"] = out;
    j["bytesWritten"] = data.size();
    j["type"] = sniff_type(data);
    emit(j);
}

void cmd_sniff(const Args& a) {
    Gw2Dat dat;
    load_dat_file(dat, need(a, "dat"));
    uint32_t idx = 0;
    std::vector<uint8_t> data = extract_bytes(dat, a, idx);
    json j = describe(data);
    j["ok"] = true;
    j["index"] = idx;
    j["decompressedSize"] = data.size();
    emit(j);
}

// Cheaply read an ATEX-family header (magic/fourcc/w/h) for a DAT entry without
// a full BCn decode -- used to detect the half-res sibling at baseId-1. Returns
// false if the entry isn't a decodable ATEX-family texture.
bool peek_texture_header(Gw2Dat& dat, uint32_t idx, std::string& fourcc, int& w, int& h) {
    if (idx >= dat.mft_data_list.size()) return false;
    try {
        const MftData& e = dat.mft_data_list[idx];
        std::vector<uint8_t> d = decompress_entry(read_entry_bytes(dat.file_info.file_path, e), e.compression_flag);
        if (d.size() >= 4 && d[0] == 'C') d[0] = 'A'; // CTEX->ATEX alias
        if (!is_texture_magic(d) || d.size() < 12) return false;
        fourcc.assign(reinterpret_cast<char*>(d.data() + 4), 4);
        w = d[8] | (d[9] << 8);
        h = d[10] | (d[11] << 8);
        return true;
    } catch (const std::exception&) { return false; }
}

void cmd_texture(const Args& a) {
    std::string out = need(a, "out");
    int mip = has(a, "mip") ? static_cast<int>(to_u64(a.at("mip"))) : 0;
    int idx = -1;

    // Source: raw decompressed file (--data) or a DAT entry.
    Gw2Dat dat;
    bool haveDat = false;
    std::vector<uint8_t> data;
    if (has(a, "data")) {
        data = read_file(a.at("data"));
    } else {
        load_dat_file(dat, need(a, "dat"));
        haveDat = true;
        uint32_t i = 0;
        data = extract_bytes(dat, a, i);
        idx = static_cast<int>(i);
    }

    // Accept the CTEX/CTEU/... siblings by aliasing their magic to the A-family
    // the decoder whitelists; the rest of the container is identical.
    std::string aliased = alias_texture_magic(data);

    try {
        gw2atex::Texture tex = gw2atex::parse(data.data(), data.size());
        gw2atex::Image img = gw2atex::decode(tex, mip);
        if (img.width <= 0 || img.height <= 0 || img.rgba.empty()) fail("texture decoded to an empty image");
        if (!stbi_write_png(out.c_str(), img.width, img.height, 4, img.rgba.data(), img.width * 4))
            fail("failed to write PNG: " + out);
        json j;
        j["ok"] = true;
        if (idx >= 0) j["index"] = idx;
        j["out"] = out;
        j["width"] = img.width;
        j["height"] = img.height;
        j["format"] = tex.fmt_name;
        j["magic"] = aliased.empty() ? std::string(tex.magic, 4) : aliased;
        if (!aliased.empty()) j["aliasedFrom"] = aliased; // original C-family magic
        j["mip"] = mip;
        j["mipCount"] = tex.mips.size();

        // GW2 often stores a texture as a pair at consecutive MFT indices:
        // baseId B = full resolution, baseId B-1 = the half-resolution version
        // (same format, exactly half the dimensions). Report it if present so
        // callers know they're already using the full-size original.
        if (haveDat && idx > 0) {
            std::string sf; int sw = 0, sh = 0;
            if (peek_texture_header(dat, (uint32_t)idx - 1, sf, sw, sh) &&
                sf == std::string(tex.fourcc, 4) && sw == img.width / 2 && sh == img.height / 2 &&
                img.width > 1 && img.height > 1) {
                j["reducedSibling"] = {{"index", idx - 1}, {"width", sw}, {"height", sh}};
                j["isFullResolution"] = true;
            }
        }
        emit(j);
    } catch (const std::exception& ex) {
        fail(std::string("texture decode failed: ") + ex.what());
    }
}

void cmd_parse(const Args& a) {
    // Load template registry.
    std::string tpl_path = need(a, "template");
    std::ifstream tin(tpl_path, std::ios::binary);
    if (!tin) fail("cannot open template: " + tpl_path);
    json tpl;
    try {
        tin >> tpl;
    } catch (const std::exception& ex) {
        fail(std::string("template JSON parse error: ") + ex.what());
    }

    // Source bytes: either a raw file, or extracted+decompressed from the DAT.
    std::vector<uint8_t> data;
    uint32_t idx = 0;
    bool from_dat = has(a, "dat");
    if (has(a, "data")) {
        data = read_file(a.at("data"));
    } else if (from_dat) {
        Gw2Dat dat;
        load_dat_file(dat, a.at("dat"));
        data = extract_bytes(dat, a, idx);
    } else {
        fail("parse needs --data <bin> or --dat <path> with --index/--file-id");
    }

    int max_depth = has(a, "max-depth") ? static_cast<int>(to_u64(a.at("max-depth"))) : 6;

    BinaryParser parser;
    ParsedNodePtr root;
    std::string err;
    if (!parser.parse(data, tpl, root, err)) fail("parse failed: " + err);

    json tree = node_to_json(root, 0, max_depth);
    json j;
    j["ok"] = true;
    if (from_dat) j["index"] = idx;
    j["sourceSize"] = data.size();
    j["type"] = sniff_type(data);
    j["maxDepth"] = max_depth;
    j["tree"] = std::move(tree);

    if (has(a, "out")) {
        std::ofstream of(a.at("out"), std::ios::binary);
        if (!of) fail("cannot write --out: " + a.at("out"));
        of << j.dump();
        json meta;
        meta["ok"] = true;
        if (from_dat) meta["index"] = idx;
        meta["out"] = a.at("out");
        meta["sourceSize"] = data.size();
        meta["type"] = sniff_type(data);
        emit(meta); // keep stdout small when writing to a file
    } else {
        emit(j);
    }
}

// Scan a range of MFT entries (MFT loaded once), decompress each, and report
// packfiles whose container fourcc matches --container (e.g. GRMT). Also counts
// how many contain "DXBC" bytes. For confirming shader storage in the dat.
// Walk an AMAT packfile's BGFX chunk: report the shader list + the matched
// technique[0] VS/PS indices + sampler bindings, and dump the selected pair's
// DXBC to --out-dir (or every shader with --dump-all).
void cmd_amat(const Args& a) {
    std::string tpl_path = need(a, "template");
    std::ifstream tin(tpl_path, std::ios::binary);
    if (!tin) fail("cannot open template: " + tpl_path);
    json tpl; try { tin >> tpl; } catch (const std::exception& ex) { fail(std::string("template JSON: ") + ex.what()); }

    std::vector<uint8_t> data;
    if (has(a, "data")) data = read_file(a.at("data"));
    else { Gw2Dat dat; load_dat_file(dat, need(a, "dat")); uint32_t i = 0; data = extract_bytes(dat, a, i); }

    gw2model::AmatSet set = gw2model::Extractor(data, tpl).extractAmat();
    if (!set.error.empty()) fail(set.error);

    auto sampJson = [](const gw2model::AmatShader& s) {
        json arr = json::array();
        for (const auto& b : s.samplers)
            arr.push_back({{"token", b.token}, {"textureIndex", b.textureIndex}, {"textureSlot", b.textureSlot}});
        return arr;
    };
    auto dump = [&](int idx, const std::string& key) {
        if (idx < 0 || idx >= (int)set.shaders.size()) return;
        const auto& s = set.shaders[idx];
        if (has(a, "out-dir") && !s.dxbc.empty()) {
            std::string fn = a.at("out-dir") + "/" + key + "_" + std::to_string(idx) + ".bgfxsh";
            std::ofstream of(fn, std::ios::binary);
            of.write(reinterpret_cast<const char*>(s.dxbc.data()), (std::streamsize)s.dxbc.size());
        }
    };
    dump(set.vsIndex, "vs");
    dump(set.psIndex, "ps");

    int vsN = 0, psN = 0;
    for (const auto& s : set.shaders) (s.isPixel ? psN : vsN)++;
    json j;
    j["ok"] = true;
    j["shaderCount"] = set.shaders.size();
    j["vertexShaders"] = vsN;
    j["pixelShaders"] = psN;
    j["selected"] = {{"vsIndex", set.vsIndex}, {"psIndex", set.psIndex}, {"renderState", set.renderState}};
    if (set.vsIndex >= 0 && set.vsIndex < (int)set.shaders.size())
        j["selected"]["vsBlobSize"] = set.shaders[set.vsIndex].dxbc.size();
    if (set.psIndex >= 0 && set.psIndex < (int)set.shaders.size()) {
        j["selected"]["psBlobSize"] = set.shaders[set.psIndex].dxbc.size();
        j["selected"]["psSamplers"] = sampJson(set.shaders[set.psIndex]);
    }
    // best transparent-pass effect (shaderPassFlags & AMAT_PASSFLAG_TRANSPARENT), if any -- this
    // is what a material with MODL sortLayer>0 should actually draw with, not the opaque one above.
    j["hasTransparentEffect"] = set.hasTrans;
    if (set.hasTrans) {
        j["transparent"] = {{"vsIndex", set.transVsIndex}, {"psIndex", set.transPsIndex}, {"renderState", set.transRenderState}};
        if (set.transPsIndex >= 0 && set.transPsIndex < (int)set.shaders.size())
            j["transparent"]["psSamplers"] = sampJson(set.shaders[set.transPsIndex]);
    }
    if (has(a, "out-dir")) j["outDir"] = a.at("out-dir");
    emit(j);
}

void cmd_scanpf(const Args& a) {
    Gw2Dat dat;
    load_dat_file(dat, need(a, "dat"));
    std::string want = has(a, "container") ? a.at("container") : "";
    size_t offset = has(a, "offset") ? to_u64(a.at("offset")) : 0;
    size_t count = has(a, "count") ? to_u64(a.at("count")) : 20000;
    size_t step = has(a, "step") ? to_u64(a.at("step")) : 1;
    size_t maxHits = has(a, "limit") ? to_u64(a.at("limit")) : 10;
    size_t minSize = has(a, "min-size") ? to_u64(a.at("min-size")) : 0;
    size_t total = dat.mft_data_list.size();
    size_t end = std::min(total, offset + count);

    json hits = json::array();
    size_t scanned = 0, pfCount = 0, matchCount = 0;
    for (size_t i = offset; i < end && hits.size() < maxHits; i += step) {
        const MftData& e = dat.mft_data_list[i];
        if (e.size == 0 || e.size < minSize) continue; // size pre-filter skips decompressing small entries
        std::vector<uint8_t> data;
        try { data = decompress_entry(read_entry_bytes(dat.file_info.file_path, e), e.compression_flag); }
        catch (...) { continue; }
        ++scanned;
        if (data.size() < 16 || data[0] != 'P' || data[1] != 'F') continue;
        ++pfCount;
        std::string ct = tag_at(data, 8);
        if (!want.empty() && ct != want) continue;
        ++matchCount;
        // does it contain DXBC bytecode?
        bool hasDxbc = false;
        for (size_t k = 0; k + 4 <= data.size(); ++k)
            if (data[k]=='D'&&data[k+1]=='X'&&data[k+2]=='B'&&data[k+3]=='C'){ hasDxbc=true; break; }
        hits.push_back({{"index", i}, {"container", ct}, {"size", data.size()}, {"hasDXBC", hasDxbc}});
    }
    json j;
    j["ok"] = true;
    j["scanned"] = scanned; j["packfiles"] = pfCount; j["matched"] = matchCount;
    j["hits"] = std::move(hits);
    emit(j);
}

void cmd_model(const Args& a) {
    // template
    std::string tpl_path = need(a, "template");
    std::ifstream tin(tpl_path, std::ios::binary);
    if (!tin) fail("cannot open template: " + tpl_path);
    json tpl;
    try { tin >> tpl; } catch (const std::exception& ex) { fail(std::string("template JSON error: ") + ex.what()); }

    // decompressed MODL bytes
    std::vector<uint8_t> data;
    if (has(a, "data")) {
        data = read_file(a.at("data"));
    } else {
        Gw2Dat dat;
        load_dat_file(dat, need(a, "dat"));
        uint32_t idx = 0;
        data = extract_bytes(dat, a, idx);
    }

    gw2model::Model model = gw2model::Extractor(data, tpl).extract();

    // JSON summary
    json meshes = json::array();
    size_t totalV = 0, totalTri = 0;
    for (const auto& m : model.meshes) {
        json bindings = json::array();
        for (size_t k = 0; k < m.boneBindings.size(); ++k) bindings.push_back(m.boneBindings[k]);
        json sampleIdx = json::array();
        for (size_t k = 0; k < m.vertices.size() && k < 6; ++k)
            sampleIdx.push_back({m.vertices[k].boneIdx[0], m.vertices[k].boneIdx[1],
                                 m.vertices[k].boneIdx[2], m.vertices[k].boneIdx[3]});
        meshes.push_back({{"fvf", m.fvf},
                          {"vertexCount", m.vertices.size()},
                          {"declaredVertexCount", m.vertexCount},
                          {"indexCount", m.indices.size()},
                          {"triangles", m.indices.size() / 3},
                          {"materialIndex", m.materialIndex},
                          {"hasSkin", m.hasSkin},
                          {"boneBindingCount", m.boneBindings.size()},
                          {"resolvedBindings", [&]{ int n=0; for(int x:m.boneBindingSkelIndex) if(x>=0)++n; return n; }()},
                          {"boneBindings", bindings},
                          {"sampleBlendIdx", sampleIdx},
                          {"minBound", {m.minB[0], m.minB[1], m.minB[2]}},
                          {"maxBound", {m.maxB[0], m.maxB[1], m.maxB[2]}}});
        totalV += m.vertices.size();
        totalTri += m.indices.size() / 3;
    }
    json mats = json::array();
    for (const auto& mt : model.materials) {
        json texs = json::array();
        for (const auto& t : mt.textures)
            texs.push_back({{"fileId", t.fileId}, {"token", t.token}, {"flags", t.flags}, {"uvIndex", t.uvIndex}});
        json consts = json::array();
        for (const auto& c : mt.constants)
            consts.push_back({{"name", c.name}, {"value", {c.value[0], c.value[1], c.value[2], c.value[3]}}});
        mats.push_back({{"index", mt.index},
                        {"materialId", mt.materialId},
                        {"materialFile", mt.materialFile},
                        {"materialFlags", mt.materialFlags},
                        {"sortOrder", mt.sortOrder},
                        {"sortLayer", mt.sortLayer},
                        {"textureFileIds", mt.textureFileIds()},
                        {"textures", texs},
                        {"constants", consts}});
    }

    // optional OBJ export (positions + uv + normals + faces), for validation
    if (has(a, "obj")) {
        std::ofstream o(a.at("obj"));
        if (!o) fail("cannot write --obj");
        o << "# gw2model export\n";
        size_t voff = 1;
        for (size_t mi = 0; mi < model.meshes.size(); ++mi) {
            const auto& m = model.meshes[mi];
            o << "o mesh" << mi << "\n";
            for (const auto& v : m.vertices) o << "v " << v.px << " " << v.py << " " << v.pz << "\n";
            for (const auto& v : m.vertices) o << "vt " << v.u << " " << (1.0f - v.v) << "\n";
            for (const auto& v : m.vertices) o << "vn " << v.nx << " " << v.ny << " " << v.nz << "\n";
            for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
                uint64_t a0 = voff + m.indices[i], b0 = voff + m.indices[i+1], c0 = voff + m.indices[i+2];
                o << "f " << a0 << "/" << a0 << "/" << a0 << " " << b0 << "/" << b0 << "/" << b0
                  << " " << c0 << "/" << c0 << "/" << c0 << "\n";
            }
            voff += m.vertices.size();
        }
    }

    json j;
    j["ok"] = true;
    j["meshCount"] = model.meshes.size();
    j["totalVertices"] = totalV;
    j["totalTriangles"] = totalTri;
    j["meshes"] = std::move(meshes);
    j["materials"] = std::move(mats);
    if (has(a, "obj")) j["obj"] = a.at("obj");
    emit(j);
}

// ---- tiny pose math for validating the Granny decode against the bind pose ----
// Bones carry local TRS (Position, Orientation quat xyzw, ScaleShear 3x3 row-major).
// Granny transform: v' = pos + R(quat) * (ScaleShear * v). World = compose down
// parents. jointPos = world translation -- must match the InverseWorld-derived
// bind position (Bone.worldPos) for the "zeropose" clip.
namespace posemath {
inline void quatToM3(const float q[4], float m[9]) {
    float x=q[0],y=q[1],z=q[2],w=q[3];
    float n=x*x+y*y+z*z+w*w; float s = n>1e-12f ? 2.0f/n : 0.0f;
    float xs=x*s,ys=y*s,zs=z*s, wx=w*xs,wy=w*ys,wz=w*zs, xx=x*xs,xy=x*ys,xz=x*zs, yy=y*ys,yz=y*zs,zz=z*zs;
    m[0]=1-(yy+zz); m[1]=xy-wz;   m[2]=xz+wy;
    m[3]=xy+wz;     m[4]=1-(xx+zz);m[5]=yz-wx;
    m[6]=xz-wy;     m[7]=yz+wx;   m[8]=1-(xx+yy);
}
inline void m3mul(const float a[9], const float b[9], float o[9]) {
    for (int r=0;r<3;++r) for (int c=0;c<3;++c)
        o[r*3+c]=a[r*3+0]*b[0*3+c]+a[r*3+1]*b[1*3+c]+a[r*3+2]*b[2*3+c];
}
inline void m3vec(const float m[9], const float v[3], float o[3]) {
    for (int r=0;r<3;++r) o[r]=m[r*3+0]*v[0]+m[r*3+1]*v[1]+m[r*3+2]*v[2];
}
} // namespace posemath

// Compose a clip's per-bone local transforms (Granny track if the name matches,
// otherwise the bone's own bind LocalTransform) down the hierarchy; returns the
// per-bone world positions. Assumes parent index < child index (GW2 order).
std::vector<std::array<float,3>> composePose(const gw2model::Skeleton& sk, const granny::Anim& anim) {
    using namespace posemath;
    std::vector<std::array<float,9>> wLin(sk.bones.size());
    std::vector<std::array<float,3>> wPos(sk.bones.size());
    // name -> track index
    std::unordered_map<std::string,int> trackByName;
    for (size_t i=0;i<anim.tracks.size();++i) trackByName[anim.tracks[i].name]=(int)i;
    for (size_t i=0;i<sk.bones.size();++i) {
        const auto& b = sk.bones[i];
        float pos[3]={b.localPos[0],b.localPos[1],b.localPos[2]};
        float quat[4]={b.localQuat[0],b.localQuat[1],b.localQuat[2],b.localQuat[3]};
        float ss[9]; std::memcpy(ss,b.scaleShear,sizeof ss);
        auto it = trackByName.find(b.name);
        if (it!=trackByName.end()) {
            const granny::Track& tr = anim.tracks[it->second];
            granny::sample(tr.pos, 0.0f, pos, 3);
            granny::sample(tr.ori, 0.0f, quat, 4);
            granny::sample(tr.sca, 0.0f, ss, 9);
        }
        float R[9]; quatToM3(quat,R);
        float L[9]; m3mul(R,ss,L);              // linear = R * ScaleShear
        int p = b.parent;
        if (p>=0 && p<(int)i) {
            float wl[9]; m3mul(wLin[p].data(),L,wl);
            float rp[3]; m3vec(wLin[p].data(),pos,rp);
            std::memcpy(wLin[i].data(),wl,sizeof wl);
            for (int k=0;k<3;++k) wPos[i][k]=wPos[p][k]+rp[k];
        } else {
            std::memcpy(wLin[i].data(),L,sizeof L);
            for (int k=0;k<3;++k) wPos[i][k]=pos[k];
        }
    }
    return wPos;
}

// Parse an `area` (map) packfile's prop placement -> the list of placed models
// with their world transforms. Foundation for rendering a whole map scene.
void cmd_map(const Args& a) {
    std::string tpl_path = need(a, "template");
    std::ifstream tin(tpl_path, std::ios::binary);
    if (!tin) fail("cannot open template: " + tpl_path);
    json tpl;
    try { tin >> tpl; } catch (const std::exception& ex) { fail(std::string("template JSON error: ") + ex.what()); }

    std::vector<uint8_t> data;
    if (has(a, "data")) data = read_file(a.at("data"));
    else { Gw2Dat dat; load_dat_file(dat, need(a, "dat")); uint32_t i = 0; data = extract_bytes(dat, a, i); }

    gw2model::Extractor ex(data, tpl);
    auto props = ex.parseMapProps();
    auto terr = ex.parseTerrain();
    auto coll = ex.parseMapCollision();

    // Unique models + overall bounds.
    std::unordered_map<uint32_t,int> uniq;
    float lo[3]={1e30f,1e30f,1e30f}, hi[3]={-1e30f,-1e30f,-1e30f};
    for (const auto& p : props) {
        uniq[p.fileId]++;
        for (int k=0;k<3;++k){ lo[k]=std::min(lo[k],p.pos[k]); hi[k]=std::max(hi[k],p.pos[k]); }
    }
    json sample = json::array();
    for (size_t i=0;i<props.size() && i<10;++i) {
        const auto& p = props[i];
        sample.push_back({{"fileId",p.fileId},{"pos",{p.pos[0],p.pos[1],p.pos[2]}},
                          {"rot",{p.rot[0],p.rot[1],p.rot[2]}},{"scale",p.scale}});
    }
    json j;
    j["ok"] = true;
    j["propCount"] = props.size();
    j["uniqueModels"] = uniq.size();
    if (!props.empty()) { j["boundsMin"]={lo[0],lo[1],lo[2]}; j["boundsMax"]={hi[0],hi[1],hi[2]}; }
    j["sample"] = std::move(sample);
    json tj;
    tj["present"] = terr.present;
    if (terr.present) {
        float hlo = 1e30f, hhi = -1e30f;
        for (float h : terr.heights) { hlo = std::min(hlo, h); hhi = std::max(hhi, h); }
        tj["dims"] = {terr.dimX, terr.dimY};
        tj["heightSamples"] = terr.heights.size();
        tj["vertsPerChunkSide"] = terr.vertsPerChunkSide;
        tj["swapDistance"] = terr.swapDistance;
        tj["heightMin"] = hlo; tj["heightMax"] = hhi;
        if (terr.hasRect) tj["rect"] = {terr.rect[0], terr.rect[1], terr.rect[2], terr.rect[3]};
    }
    j["terrain"] = std::move(tj);
    j["collision"] = {{"present", coll.present}, {"hasWater", coll.hasWater}, {"waterZ", coll.waterZ},
                      {"verts", coll.verts.size() / 3}, {"tris", coll.indices.size() / 3}};
    emit(j);
}

// Dump the skeleton + embedded animation of a MODL packfile (validation aid).
void cmd_skel(const Args& a) {
    std::string tpl_path = need(a, "template");
    std::ifstream tin(tpl_path, std::ios::binary);
    if (!tin) fail("cannot open template: " + tpl_path);
    json tpl;
    try { tin >> tpl; } catch (const std::exception& ex) { fail(std::string("template JSON error: ") + ex.what()); }

    std::vector<uint8_t> data;
    if (has(a, "data")) {
        data = read_file(a.at("data"));
    } else {
        Gw2Dat dat;
        load_dat_file(dat, need(a, "dat"));
        uint32_t idx = 0;
        data = extract_bytes(dat, a, idx);
    }

    gw2model::Model model = gw2model::Extractor(data, tpl).extract();
    const auto& sk = model.skeleton;

    json bones = json::array();
    for (const auto& b : sk.bones)
        bones.push_back({{"name", b.name}, {"parent", b.parent},
                         {"worldPos", {b.worldPos[0], b.worldPos[1], b.worldPos[2]}},
                         {"localPos", {b.localPos[0], b.localPos[1], b.localPos[2]}}});
    json clips = json::array();
    for (const auto& c : model.anim.clips)
        clips.push_back({{"token", c.token}, {"moveSpeed", c.moveSpeed},
                         {"grannyBytes", c.rawGranny.size()}, {"fixups", c.fixups.size()},
                         {"grannyFileOffset", c.grannyFileOffset}});

    // Which clip to inspect/validate (default 0 = usually the zeropose).
    size_t clipIdx = 0;
    if (has(a, "clip")) { long v = std::stol(a.at("clip")); if (v >= 0 && (size_t)v < model.anim.clips.size()) clipIdx = (size_t)v; }

    // Optional: dump the selected clip's raw Granny blob + fixup table for offline analysis.
    if (has(a, "anim-out") && !model.anim.clips.empty()) {
        const auto& c = model.anim.clips[clipIdx];
        std::ofstream ob(a.at("anim-out"), std::ios::binary);
        ob.write(reinterpret_cast<const char*>(c.rawGranny.data()), c.rawGranny.size());
        std::ofstream of(a.at("anim-out") + ".fixups", std::ios::binary);
        of.write(reinterpret_cast<const char*>(c.fixups.data()), c.fixups.size() * 4);
    }

    // Decode clip[0]'s Granny animation and verify: composing its per-bone local
    // transforms down the hierarchy must reproduce the InverseWorld-derived bind
    // positions (the embedded clip is "zeropose"). This validates the decoder +
    // pose math end-to-end.
    json validate = json::object();
    if (!model.anim.clips.empty() && !model.anim.clips[clipIdx].rawGranny.empty() && sk.present) {
        const auto& raw = model.anim.clips[clipIdx].rawGranny;
        granny::Anim anim = granny::parse(raw.data(), raw.size());
        validate["clipName"] = anim.name;
        validate["duration"] = anim.duration;
        validate["trackCount"] = anim.tracks.size();
        // Count keyframed curves (knots>0) -> whether the clip has real motion.
        size_t keyframed = 0, totalKnots = 0;
        for (const auto& tr : anim.tracks)
            for (const granny::Curve* c : {&tr.ori, &tr.pos, &tr.sca}) {
                if (!c->knots.empty()) { ++keyframed; totalKnots += c->knots.size(); }
            }
        validate["keyframedCurves"] = keyframed;
        validate["totalKnots"] = totalKnots;
        if (anim.valid) {
            auto posed = composePose(sk, anim);
            double sum2 = 0; float mx = 0; int matched = 0;
            std::unordered_map<std::string,int> tn;
            for (size_t i=0;i<anim.tracks.size();++i) tn[anim.tracks[i].name]=1;
            for (size_t i=0;i<sk.bones.size();++i) {
                if (tn.count(sk.bones[i].name)) matched++;
                float dx=posed[i][0]-sk.bones[i].worldPos[0];
                float dy=posed[i][1]-sk.bones[i].worldPos[1];
                float dz=posed[i][2]-sk.bones[i].worldPos[2];
                double e=std::sqrt((double)dx*dx+dy*dy+dz*dz);
                sum2+=e*e; if (e>mx) mx=(float)e;
            }
            validate["tracksMatchingBones"] = matched;
            validate["rmsError"] = std::sqrt(sum2/std::max<size_t>(1,sk.bones.size()));
            validate["maxError"] = mx;

            // Skin-matrix identity check (mirrors renderer update_bone_palette):
            // SkinMat = InverseWorld(row) * AnimatedWorld(row) must be ~identity at
            // the bind (zeropose) pose, i.e. skinned mesh == rest mesh.
            using namespace posemath;
            std::vector<std::array<float,9>> wLin(sk.bones.size());
            std::vector<std::array<float,3>> wPos(sk.bones.size());
            std::unordered_map<std::string,int> tbn;
            for (size_t i=0;i<anim.tracks.size();++i) tbn[anim.tracks[i].name]=(int)i;
            for (size_t i=0;i<sk.bones.size();++i) {
                const auto& b=sk.bones[i];
                float pos[3]={b.localPos[0],b.localPos[1],b.localPos[2]};
                float quat[4]={b.localQuat[0],b.localQuat[1],b.localQuat[2],b.localQuat[3]};
                float ss[9]; std::memcpy(ss,b.scaleShear,sizeof ss);
                auto it=tbn.find(b.name);
                if (it!=tbn.end()){granny::sample(anim.tracks[it->second].pos,0,pos,3);
                    granny::sample(anim.tracks[it->second].ori,0,quat,4);
                    granny::sample(anim.tracks[it->second].sca,0,ss,9);}
                float R[9]; quatToM3(quat,R); float L[9]; m3mul(R,ss,L);
                int p=b.parent;
                if(p>=0&&p<(int)i){m3mul(wLin[p].data(),L,wLin[i].data());
                    float rp[3];m3vec(wLin[p].data(),pos,rp);
                    for(int k=0;k<3;++k)wPos[i][k]=wPos[p][k]+rp[k];}
                else{std::memcpy(wLin[i].data(),L,sizeof L);for(int k=0;k<3;++k)wPos[i][k]=pos[k];}
            }
            double skinErr=0; float skinMax=0;
            for (size_t b=0;b<sk.bones.size();++b){
                const float* lin=wLin[b].data(); const float* pos=wPos[b].data();
                float Wr[16]={lin[0],lin[3],lin[6],0, lin[1],lin[4],lin[7],0, lin[2],lin[5],lin[8],0, pos[0],pos[1],pos[2],1};
                const float* IB=sk.bones[b].invWorld;
                for(int r=0;r<4;++r)for(int c=0;c<4;++c){
                    float v=IB[r*4+0]*Wr[c]+IB[r*4+1]*Wr[4+c]+IB[r*4+2]*Wr[8+c]+IB[r*4+3]*Wr[12+c];
                    float id=(r==c)?1.0f:0.0f; float e=std::fabs(v-id);
                    skinErr+=e*e; if(e>skinMax)skinMax=e;
                }
            }
            validate["skinIdentityRms"]=std::sqrt(skinErr/std::max<size_t>(1,sk.bones.size()*16));
            validate["skinIdentityMax"]=skinMax;

            // Skin-MOTION test: pose the rig with the renderer's wobble at t=1.0,
            // build the same skin palette, and skin real mesh vertices. Max
            // displacement > 0 proves the mesh follows the bones (CPU side).
            {
                std::vector<std::array<float,9>> wl(sk.bones.size());
                std::vector<std::array<float,3>> wp(sk.bones.size());
                float t=1.0f;
                for (size_t i=0;i<sk.bones.size();++i){
                    const auto& b=sk.bones[i];
                    float pos[3]={b.localPos[0],b.localPos[1],b.localPos[2]};
                    float quat[4]={b.localQuat[0],b.localQuat[1],b.localQuat[2],b.localQuat[3]};
                    float ss[9]; std::memcpy(ss,b.scaleShear,sizeof ss);
                    if (b.parent>=0){ float a=0.35f*std::sin(2.2f*t+i*0.5f),s2=std::sin(a*0.5f),c2=std::cos(a*0.5f);
                        float ex[4]={s2,0,0,c2},q[4];
                        q[0]=quat[3]*ex[0]+quat[0]*ex[3]+quat[1]*ex[2]-quat[2]*ex[1];
                        q[1]=quat[3]*ex[1]-quat[0]*ex[2]+quat[1]*ex[3]+quat[2]*ex[0];
                        q[2]=quat[3]*ex[2]+quat[0]*ex[1]-quat[1]*ex[0]+quat[2]*ex[3];
                        q[3]=quat[3]*ex[3]-quat[0]*ex[0]-quat[1]*ex[1]-quat[2]*ex[2];
                        std::memcpy(quat,q,sizeof q);}
                    float R[9]; quatToM3(quat,R); float L[9]; m3mul(R,ss,L);
                    int p=b.parent;
                    if(p>=0&&p<(int)i){m3mul(wl[p].data(),L,wl[i].data());float rp[3];m3vec(wl[p].data(),pos,rp);
                        for(int k=0;k<3;++k)wp[i][k]=wp[p][k]+rp[k];}
                    else{std::memcpy(wl[i].data(),L,sizeof L);for(int k=0;k<3;++k)wp[i][k]=pos[k];}
                }
                // palette
                std::vector<std::array<float,16>> pal(sk.bones.size());
                for(size_t b=0;b<sk.bones.size();++b){
                    const float* lin=wl[b].data(); const float* pos=wp[b].data();
                    float Wr[16]={lin[0],lin[3],lin[6],0, lin[1],lin[4],lin[7],0, lin[2],lin[5],lin[8],0, pos[0],pos[1],pos[2],1};
                    const float* IB=sk.bones[b].invWorld;
                    for(int r=0;r<4;++r)for(int c=0;c<4;++c)
                        pal[b][r*4+c]=IB[r*4+0]*Wr[c]+IB[r*4+1]*Wr[4+c]+IB[r*4+2]*Wr[8+c]+IB[r*4+3]*Wr[12+c];
                }
                float maxDisp=0; int tested=0, skinnedVerts=0;
                for (const auto& mesh : model.meshes){
                    if (mesh.boneBindingSkelIndex.empty()) continue;
                    for (size_t vi=0; vi<mesh.vertices.size() && vi<2000; ++vi){
                        const auto& vtx=mesh.vertices[vi];
                        float acc[3]={0,0,0}; float wsum=0;
                        for(int c=0;c<4;++c){
                            int raw=vtx.boneIdx[c];
                            int sb=(raw>=0&&raw<(int)mesh.boneBindingSkelIndex.size())?mesh.boneBindingSkelIndex[raw]:-1;
                            float w=vtx.boneWt[c];
                            if(sb<0||sb>=(int)sk.bones.size()||w<=0) continue;
                            const float* m=pal[sb].data();
                            float x=vtx.px,y=vtx.py,z=vtx.pz;
                            // row-vector * row-major matrix
                            acc[0]+=w*(x*m[0]+y*m[4]+z*m[8]+m[12]);
                            acc[1]+=w*(x*m[1]+y*m[5]+z*m[9]+m[13]);
                            acc[2]+=w*(x*m[2]+y*m[6]+z*m[10]+m[14]);
                            wsum+=w;
                        }
                        if(wsum>1e-6f){++skinnedVerts;
                            float dx=acc[0]-vtx.px,dy=acc[1]-vtx.py,dz=acc[2]-vtx.pz;
                            float dsp=std::sqrt(dx*dx+dy*dy+dz*dz); if(dsp>maxDisp)maxDisp=dsp;}
                        ++tested;
                    }
                }
                validate["wobbleMaxDisplacement"]=maxDisp;
                validate["skinnedVertsTested"]=skinnedVerts;
                // sample vertex 0 bone data
                if(!model.meshes.empty() && !model.meshes[0].vertices.empty()){
                    const auto& v0=model.meshes[0].vertices[0];
                    validate["v0_idx"]={v0.boneIdx[0],v0.boneIdx[1],v0.boneIdx[2],v0.boneIdx[3]};
                    validate["v0_wt"]={v0.boneWt[0],v0.boneWt[1],v0.boneWt[2],v0.boneWt[3]};
                }
            }
        }
    }

    json j;
    j["ok"] = true;
    j["skeleton"] = {{"present", sk.present}, {"fileVersion", sk.fileVersion},
                     {"skelDataType", sk.skelDataType}, {"externalRef", sk.externalRef},
                     {"boneCount", sk.bones.size()}, {"bones", std::move(bones)}};
    j["animation"] = {{"present", model.anim.present}, {"chunkVersion", model.anim.chunkVersion},
                      {"typeKey", model.anim.typeKey}, {"clipCount", model.anim.clips.size()},
                      {"clips", std::move(clips)}};
    j["poseValidation"] = std::move(validate);
    emit(j);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fail("usage: gw2dat_cli <info|list|lookup|resolve|extract|texture|parse|sniff> [--flags]");
    }
    std::string cmd = argv[1];
    Args a = parse_args(argc, argv, 2);
    try {
        if (cmd == "info") cmd_info(a);
        else if (cmd == "list") cmd_list(a);
        else if (cmd == "lookup") cmd_lookup(a);
        else if (cmd == "extract") cmd_extract(a);
        else if (cmd == "texture") cmd_texture(a);
        else if (cmd == "parse") cmd_parse(a);
        else if (cmd == "sniff") cmd_sniff(a);
        else if (cmd == "resolve") cmd_resolve(a);
        else if (cmd == "model") cmd_model(a);
        else if (cmd == "skel") cmd_skel(a);
        else if (cmd == "map") cmd_map(a);
        else if (cmd == "scanpf") cmd_scanpf(a);
        else if (cmd == "amat") cmd_amat(a);
        else fail("unknown command: " + cmd);
    } catch (const std::exception& ex) {
        fail(ex.what());
    }
    return 0;
}
