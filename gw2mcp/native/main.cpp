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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "gw2dat.h"
#include "cmp_decompress_method0.hpp"
#include "gw2_atex.hpp"
#include "BinaryParser.h"

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

// Resolve --index / --file-id (in `a`) to a concrete MFT index in `dat`.
uint32_t resolve_index(Gw2Dat& dat, const Args& a) {
    if (has(a, "index")) {
        uint64_t idx = to_u64(a.at("index"));
        if (idx >= dat.mft_data_list.size()) fail("index out of range");
        return static_cast<uint32_t>(idx);
    }
    if (has(a, "file-id")) {
        uint32_t file_id = static_cast<uint32_t>(to_u64(a.at("file-id")));
        uint32_t base_id = get_by_base_id(dat, file_id); // fileId -> MFT index ("baseId")
        if (base_id == 0) fail("file-id not found: " + std::to_string(file_id));
        if (base_id >= dat.mft_data_list.size()) fail("resolved MFT index out of range");
        return base_id;
    }
    fail("need --index or --file-id");
}

// Replicates entry_extractor.cpp's decompress_raw_entry() minus the GUI image
// path: strip the per-chunk CRC32C framing, then (if compressed) run Method0.
std::vector<uint8_t> decompress_entry(const std::vector<uint8_t>& raw, uint16_t compression_flag) {
    std::vector<uint8_t> stripped = gw2cmp::strip_crc32(std::span<const uint8_t>(raw));
    if (compression_flag == 0) {
        return stripped;
    }
    if (stripped.size() < 8) fail("entry too small to contain a Method0 header");
    uint32_t uncompressed_size = static_cast<uint32_t>(stripped[4]) |
                                 (static_cast<uint32_t>(stripped[5]) << 8) |
                                 (static_cast<uint32_t>(stripped[6]) << 16) |
                                 (static_cast<uint32_t>(stripped[7]) << 24);
    return gw2cmp::decompress_method0(std::span<const uint8_t>(stripped).subspan(8), uncompressed_size);
}

const char* sniff_type(const std::vector<uint8_t>& d) {
    auto m4 = [&](const char* s) { return d.size() >= 4 && std::memcmp(d.data(), s, 4) == 0; };
    if (d.size() >= 2 && d[0] == 'P' && d[1] == 'F') return "packfile";
    if (m4("ATEX") || m4("ATTX") || m4("ATEC") || m4("ATEP") || m4("ATEU") || m4("ATET")) return "texture";
    if (m4("DDS ")) return "dds";
    if (d.size() >= 4 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return "png";
    if (d.size() >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "jpeg";
    return "binary";
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
    json j;
    j["ok"] = true;
    j["index"] = idx;
    j["decompressedSize"] = data.size();
    j["type"] = sniff_type(data);
    emit(j);
}

void cmd_texture(const Args& a) {
    Gw2Dat dat;
    load_dat_file(dat, need(a, "dat"));
    std::string out = need(a, "out");
    int mip = has(a, "mip") ? static_cast<int>(to_u64(a.at("mip"))) : 0;
    uint32_t idx = 0;
    std::vector<uint8_t> data = extract_bytes(dat, a, idx);
    try {
        gw2atex::Texture tex = gw2atex::parse(data.data(), data.size());
        gw2atex::Image img = gw2atex::decode(tex, mip);
        if (img.width <= 0 || img.height <= 0 || img.rgba.empty()) fail("texture decoded to an empty image");
        if (!stbi_write_png(out.c_str(), img.width, img.height, 4, img.rgba.data(), img.width * 4))
            fail("failed to write PNG: " + out);
        json j;
        j["ok"] = true;
        j["index"] = idx;
        j["out"] = out;
        j["width"] = img.width;
        j["height"] = img.height;
        j["format"] = tex.fmt_name;
        j["mip"] = mip;
        j["mipCount"] = tex.mips.size();
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

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fail("usage: gw2dat_cli <info|list|lookup|extract|texture|parse|sniff> [--flags]");
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
        else fail("unknown command: " + cmd);
    } catch (const std::exception& ex) {
        fail(ex.what());
    }
    return 0;
}
