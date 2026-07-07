#pragma once
#include "ParsedNode.h"
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <string>

// Interprets a raw byte buffer according to a JSON schema ("template") and
// produces a ParsedNode tree that TreeView can render.
//
// Two template shapes are supported:
//
// 1) Linear template (010-Editor-like), a top-level "fields" array:
//    { "name":"MyFormat", "endian":"little", "fields":[ ... ] }
//    Field "type" values:
//      int8/uint8/int16/uint16/int32/uint32/int64/uint64/float/double
//      char (fixed str, needs "count") | cstring | skip | struct (+"fields")
//    "count": int literal OR name of an earlier sibling scalar.
//
// 2) GW2 packfile registry (data-driven, no recompile per format):
//    { "format":"gw2packfile", "pointerSize":64|32,
//      "chunks": { "MODL": {"70":"<typeKey>", ...}, ... },
//      "types":  { "<typeKey>": { "fields":[ <gwField>... ] }, ... } }
//    The engine walks the PF header + chunk headers, routes each chunk to its
//    root struct via (fourcc,version), and follows ArenaNet self-relative
//    pointers (width = pointerSize/8 bytes). See gen_gw2_json.py for encoding.
class BinaryParser {
public:
    bool parse(const std::vector<uint8_t>& data,
               const nlohmann::json& templateJson,
               ParsedNodePtr& outRoot,
               std::string& error);

private:
    const std::vector<uint8_t>* m_buffer = nullptr;
    size_t m_cursor = 0;
    bool m_littleEndian = true;

    // Flat namespace of the last-seen scalar values, keyed by field name.
    // Used to resolve dynamic "count" references like "count": "numEntries".
    std::unordered_map<std::string, int64_t> m_values;

    ParsedNodePtr parseFieldList(const nlohmann::json& fields, const std::string& groupName);
    ParsedNodePtr parseField(const nlohmann::json& field);
    int64_t resolveCount(const nlohmann::json& field);
    bool hasBytes(size_t n) const;

    template <typename T>
    T readRaw();

    // --- GW2 packfile mode ---
    const nlohmann::json* m_types = nullptr;  // -> templateJson["types"]
    int m_ptrBytes = 8;                       // 4 (32-bit) or 8 (64-bit)
    int m_depth = 0;                          // recursion guard
    size_t m_nodeCount = 0;                   // total-node cap guard

    ParsedNodePtr parseGw2Packfile(const nlohmann::json& tpl, std::string& error);
    ParsedNodePtr gwStruct(const std::string& key, const std::string& nodeName);
    ParsedNodePtr gwField(const nlohmann::json& f);
    ParsedNodePtr gwElement(const nlohmann::json& elem, const std::string& nodeName);
    std::string   gwScalar(const std::string& kind);   // reads + advances m_cursor
    ParsedNodePtr gwString(const std::string& kind, const std::string& nodeName);

    bool inBounds(size_t pos, size_t n) const;
    uint64_t readUIntAt(size_t pos, int bytes) const;  // little-endian
    template <typename T> T readAt(size_t pos) const;  // no cursor move
};
