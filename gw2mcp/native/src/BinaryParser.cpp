#include "BinaryParser.h"
#include <cstring>
#include <cstdio>
#include <algorithm>

bool BinaryParser::hasBytes(size_t n) const {
    return m_buffer && (m_cursor + n <= m_buffer->size());
}

template <typename T>
T BinaryParser::readRaw() {
    T value{};
    if (!hasBytes(sizeof(T))) {
        // Clamp cursor to end so callers relying on m_cursor for offsets stay sane.
        m_cursor = m_buffer ? m_buffer->size() : 0;
        return value;
    }
    std::memcpy(&value, m_buffer->data() + m_cursor, sizeof(T));
    m_cursor += sizeof(T);
    if (!m_littleEndian && sizeof(T) > 1) {
        auto* bytes = reinterpret_cast<uint8_t*>(&value);
        std::reverse(bytes, bytes + sizeof(T));
    }
    return value;
}

int64_t BinaryParser::resolveCount(const nlohmann::json& field) {
    if (!field.contains("count")) return 1;
    const auto& c = field["count"];
    if (c.is_number_integer()) return c.get<int64_t>();
    if (c.is_string()) {
        auto it = m_values.find(c.get<std::string>());
        return (it != m_values.end()) ? it->second : 0;
    }
    return 1;
}

ParsedNodePtr BinaryParser::parseField(const nlohmann::json& field) {
    auto node = std::make_shared<ParsedNode>();
    std::string name = field.value("name", std::string("field"));
    std::string type = field.value("type", std::string("uint8"));
    node->name = name;
    node->typeName = type;
    node->offset = m_cursor;

    // --- Nested struct (optionally repeated) ---
    if (type == "struct" && field.contains("fields")) {
        if (!field.contains("count")) {
            auto sub = parseFieldList(field["fields"], name);
            node->children = sub->children;
            node->size = m_cursor - node->offset;
            return node;
        }
        int64_t count = resolveCount(field);
        for (int64_t i = 0; i < count; ++i) {
            node->children.push_back(parseFieldList(field["fields"], name + "[" + std::to_string(i) + "]"));
        }
        node->size = m_cursor - node->offset;
        node->valueString = "[" + std::to_string(count) + " elements]";
        return node;
    }

    // --- skip ---
    if (type == "skip") {
        int64_t count = resolveCount(field);
        m_cursor += (size_t)std::max<int64_t>(count, 0);
        node->size = (size_t)std::max<int64_t>(count, 0);
        node->valueString = "(skipped " + std::to_string(count) + " bytes)";
        return node;
    }

    // --- strings ---
    if (type == "cstring") {
        std::string s;
        while (hasBytes(1)) {
            uint8_t c = readRaw<uint8_t>();
            if (c == 0) break;
            s.push_back((char)c);
        }
        node->valueString = s;
        node->size = m_cursor - node->offset;
        return node;
    }
    if (type == "char") {
        int64_t count = resolveCount(field);
        std::string s;
        for (int64_t i = 0; i < count && hasBytes(1); ++i) {
            s.push_back((char)readRaw<uint8_t>());
        }
        node->valueString = s;
        node->size = m_cursor - node->offset;
        return node;
    }

    // --- numeric scalar / array ---
    auto readOne = [&]() -> std::string {
        if (!hasBytes(1)) return "<eof>";
        if (type == "int8")   return std::to_string((int)(int8_t)readRaw<uint8_t>());
        if (type == "uint8")  { int64_t v = readRaw<uint8_t>();  m_values[name] = v; return std::to_string(v); }
        if (type == "int16")  return std::to_string(readRaw<int16_t>());
        if (type == "uint16") { int64_t v = readRaw<uint16_t>(); m_values[name] = v; return std::to_string(v); }
        if (type == "int32")  return std::to_string(readRaw<int32_t>());
        if (type == "uint32") { int64_t v = readRaw<uint32_t>(); m_values[name] = v; return std::to_string(v); }
        if (type == "int64")  return std::to_string(readRaw<int64_t>());
        if (type == "uint64") { int64_t v = (int64_t)readRaw<uint64_t>(); m_values[name] = v; return std::to_string(v); }
        if (type == "float")  return std::to_string(readRaw<float>());
        if (type == "double") return std::to_string(readRaw<double>());
        return "<unknown type: " + type + ">";
    };

    bool isArray = field.contains("count");
    if (!isArray) {
        node->valueString = readOne();
    } else {
        int64_t count = resolveCount(field);
        std::string joined;
        for (int64_t i = 0; i < count; ++i) {
            if (!hasBytes(1)) { joined += "<eof>"; break; }
            joined += readOne();
            if (i + 1 < count) joined += ", ";
        }
        node->valueString = "[" + joined + "]";
    }
    node->size = m_cursor - node->offset;
    return node;
}

ParsedNodePtr BinaryParser::parseFieldList(const nlohmann::json& fields, const std::string& groupName) {
    auto group = std::make_shared<ParsedNode>();
    group->name = groupName;
    group->typeName = "struct";
    group->offset = m_cursor;
    for (const auto& f : fields) {
        group->children.push_back(parseField(f));
    }
    group->size = m_cursor - group->offset;
    return group;
}

bool BinaryParser::parse(const std::vector<uint8_t>& data,
                          const nlohmann::json& templateJson,
                          ParsedNodePtr& outRoot,
                          std::string& error) {
    m_buffer = &data;
    m_cursor = 0;
    m_values.clear();
    m_littleEndian = templateJson.value("endian", std::string("little")) != "big";
    m_types = nullptr;
    m_depth = 0;
    m_nodeCount = 0;

    // --- GW2 packfile registry mode ---
    if (templateJson.value("format", std::string()) == "gw2packfile") {
        try {
            outRoot = parseGw2Packfile(templateJson, error);
        } catch (const std::exception& e) {
            error = std::string("GW2 parse error: ") + e.what();
            return false;
        }
        return outRoot != nullptr;
    }

    if (!templateJson.contains("fields") || !templateJson["fields"].is_array()) {
        error = "Template must contain a top-level 'fields' array.";
        return false;
    }

    try {
        std::string rootName = templateJson.value("name", std::string("Root"));
        outRoot = parseFieldList(templateJson["fields"], rootName);
    } catch (const std::exception& e) {
        error = std::string("Parse error: ") + e.what();
        return false;
    }
    return true;
}

// =====================================================================
// GW2 packfile mode (data-driven; follows ArenaNet self-relative pointers)
// =====================================================================
static const std::unordered_map<std::string,int> kScalarSize = {
    {"byte",1},{"byte3",3},{"byte4",4},{"byte16",16},{"word",2},{"word3",6},
    {"dword",4},{"dword2",8},{"dword4",16},{"qword",8},{"float",4},{"float2",8},
    {"float3",12},{"float4",16},{"double",8},{"fileref",4},{"token32",4},{"token64",8},
};

bool BinaryParser::inBounds(size_t pos, size_t n) const {
    return m_buffer && pos <= m_buffer->size() && n <= m_buffer->size() - pos;
}

template <typename T>
T BinaryParser::readAt(size_t pos) const {
    T v{};
    if (inBounds(pos, sizeof(T))) std::memcpy(&v, m_buffer->data() + pos, sizeof(T));
    return v;
}

uint64_t BinaryParser::readUIntAt(size_t pos, int bytes) const {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i)
        if (inBounds(pos + (size_t)i, 1)) v |= (uint64_t)(*m_buffer)[pos + i] << (8 * i);
    return v;
}

std::string BinaryParser::gwScalar(const std::string& k) {
    auto it = kScalarSize.find(k);
    if (it == kScalarSize.end()) return std::string("<?>");
    size_t p = m_cursor; int sz = it->second;
    std::string out;
    auto flt = [&](int off){ return std::to_string(readAt<float>(p + off)); };
    if      (k == "byte")    out = std::to_string((unsigned)readAt<uint8_t>(p));
    else if (k == "word")    out = std::to_string(readAt<uint16_t>(p));
    else if (k == "dword" || k == "fileref") out = std::to_string(readAt<uint32_t>(p));
    else if (k == "qword")   out = std::to_string((unsigned long long)readAt<uint64_t>(p));
    else if (k == "float")   out = flt(0);
    else if (k == "double")  out = std::to_string(readAt<double>(p));
    else if (k == "float2")  out = flt(0) + ", " + flt(4);
    else if (k == "float3")  out = flt(0) + ", " + flt(4) + ", " + flt(8);
    else if (k == "float4")  out = flt(0) + ", " + flt(4) + ", " + flt(8) + ", " + flt(12);
    else if (k == "dword2")  out = std::to_string(readAt<uint32_t>(p)) + ", " + std::to_string(readAt<uint32_t>(p+4));
    else if (k == "dword4")  { for (int i=0;i<4;i++){ if(i) out+=", "; out+=std::to_string(readAt<uint32_t>(p+4*i)); } }
    else if (k == "word3")   { for (int i=0;i<3;i++){ if(i) out+=", "; out+=std::to_string(readAt<uint16_t>(p+2*i)); } }
    else if (k == "token32") { char b[16]; std::snprintf(b,sizeof b,"0x%08X",readAt<uint32_t>(p)); out=b; }
    else if (k == "token64") { char b[24]; std::snprintf(b,sizeof b,"0x%016llX",(unsigned long long)readAt<uint64_t>(p)); out=b; }
    else { for (int i=0;i<sz;i++){ char b[4]; std::snprintf(b,sizeof b,"%02X ",(unsigned)readAt<uint8_t>(p+i)); out+=b; } }
    m_cursor += (size_t)sz;
    return out;
}

ParsedNodePtr BinaryParser::gwString(const std::string& kind, const std::string& nodeName) {
    auto node = std::make_shared<ParsedNode>();
    node->name = nodeName; node->typeName = kind; node->offset = m_cursor;
    size_t ptrPos = m_cursor;
    uint64_t stored = readUIntAt(ptrPos, m_ptrBytes);
    m_cursor = ptrPos + (size_t)m_ptrBytes;
    if (stored == 0) { node->valueString = "(null)"; node->size = m_cursor - node->offset; return node; }
    size_t t = ptrPos + (size_t)stored;
    if (!inBounds(t, 1)) { node->valueString = "(OOB)"; node->size = m_cursor - node->offset; return node; }
    if (kind == "filename") {
        uint16_t lo = readAt<uint16_t>(t), hi = readAt<uint16_t>(t + 2);
        long id = (lo || hi) ? (0xFF00L * ((long)hi - 0x100) + ((long)lo - 0x100) + 1) : 0;
        node->valueString = "fileId=" + std::to_string(id);
    } else if (kind == "char_ptr") {
        std::string s; size_t q = t;
        for (int i = 0; i < 4096 && inBounds(q, 1); ++i) { char c = (char)(*m_buffer)[q++]; if (!c) break; s += c; }
        node->valueString = s;
    } else { // wchar_ptr, UTF-16LE
        std::string s; size_t q = t;
        for (int i = 0; i < 4096 && inBounds(q, 2); ++i) { uint16_t c = readAt<uint16_t>(q); q += 2; if (!c) break; s += (c < 128) ? (char)c : '?'; }
        node->valueString = s;
    }
    node->size = m_cursor - node->offset;
    return node;
}

ParsedNodePtr BinaryParser::gwElement(const nlohmann::json& elem, const std::string& nodeName) {
    if (elem.is_string()) {
        std::string k = elem.get<std::string>();
        if (k == "filename" || k == "wchar_ptr" || k == "char_ptr") return gwString(k, nodeName);
        auto node = std::make_shared<ParsedNode>();
        node->name = nodeName; node->typeName = k; node->offset = m_cursor;
        node->valueString = kScalarSize.count(k) ? gwScalar(k) : ("<unknown elem: " + k + ">");
        node->size = m_cursor - node->offset;
        return node;
    }
    if (elem.is_object() && elem.contains("struct"))
        return gwStruct(elem["struct"].get<std::string>(), nodeName);
    auto node = std::make_shared<ParsedNode>();
    node->name = nodeName; node->valueString = "<bad elem>";
    return node;
}

ParsedNodePtr BinaryParser::gwField(const nlohmann::json& f) {
    auto node = std::make_shared<ParsedNode>();
    std::string name = f.value("name", std::string("field"));
    std::string kind = f.value("kind", std::string("dword"));
    node->name = name; node->typeName = kind; node->offset = m_cursor;
    if (++m_nodeCount > 4000000u) { node->valueString = "(node cap)"; return node; }

    if (kind == "struct") {
        auto sub = gwStruct(f.value("type", std::string()), name);
        node->children = sub->children;
        node->typeName = sub->typeName;
        if (node->children.empty()) node->valueString = sub->valueString;
        node->size = m_cursor - node->offset;
        return node;
    }
    if (kind == "array") {
        int64_t count = f.value("count", (int64_t)0);
        const nlohmann::json& el = f.contains("element") ? f["element"] : nlohmann::json("dword");
        for (int64_t i = 0; i < count; ++i) {
            if (!inBounds(m_cursor, 1)) break;
            node->children.push_back(gwElement(el, name + "[" + std::to_string(i) + "]"));
        }
        node->valueString = "[" + std::to_string(count) + "]";
        node->size = m_cursor - node->offset;
        return node;
    }
    if (kind == "ptr") {
        size_t ptrPos = m_cursor;
        uint64_t stored = readUIntAt(ptrPos, m_ptrBytes);
        m_cursor = ptrPos + (size_t)m_ptrBytes;
        if (stored != 0) {
            size_t target = ptrPos + (size_t)stored;
            if (inBounds(target, 1)) {
                size_t save = m_cursor; m_cursor = target;
                node->children.push_back(gwElement(f.contains("target") ? f["target"] : nlohmann::json("dword"), name + "*"));
                m_cursor = save;
            } else node->valueString = "(ptr OOB)";
        } else node->valueString = "(null)";
        node->size = m_cursor - node->offset;
        return node;
    }
    if (kind == "array_ptr" || kind == "ptr_array_ptr") {
        uint32_t count = (uint32_t)readUIntAt(m_cursor, 4);
        size_t ptrPos = m_cursor + 4;
        uint64_t stored = readUIntAt(ptrPos, m_ptrBytes);
        m_cursor = ptrPos + (size_t)m_ptrBytes;
        node->valueString = "count=" + std::to_string(count);
        const nlohmann::json& el = f.contains("element") ? f["element"] : nlohmann::json("dword");
        if (stored != 0 && count != 0 && count < 5000000u) {
            size_t target = ptrPos + (size_t)stored;
            if (inBounds(target, 1)) {
                size_t save = m_cursor; m_cursor = target;
                for (uint32_t i = 0; i < count; ++i) {
                    if (!inBounds(m_cursor, 1)) break;
                    if (kind == "array_ptr") {
                        node->children.push_back(gwElement(el, name + "[" + std::to_string(i) + "]"));
                    } else {
                        size_t pp = m_cursor;
                        uint64_t st2 = readUIntAt(pp, m_ptrBytes);
                        m_cursor = pp + (size_t)m_ptrBytes;
                        if (st2 != 0) {
                            size_t t2 = pp + (size_t)st2;
                            if (inBounds(t2, 1)) { size_t s2 = m_cursor; m_cursor = t2; node->children.push_back(gwElement(el, name + "[" + std::to_string(i) + "]")); m_cursor = s2; }
                        }
                    }
                }
                m_cursor = save;
            }
        }
        node->size = m_cursor - node->offset;
        return node;
    }
    if (kind == "filename" || kind == "wchar_ptr" || kind == "char_ptr") {
        auto n = gwString(kind, name);
        return n;
    }
    // scalar
    node->valueString = kScalarSize.count(kind) ? gwScalar(kind) : ("<unknown kind: " + kind + ">");
    node->size = m_cursor - node->offset;
    return node;
}

ParsedNodePtr BinaryParser::gwStruct(const std::string& key, const std::string& nodeName) {
    auto node = std::make_shared<ParsedNode>();
    node->name = nodeName; node->typeName = key; node->offset = m_cursor;
    if (++m_depth > 200 || !m_types || !m_types->contains(key)) {
        node->valueString = (m_depth > 200) ? "(max depth)" : ("(missing type: " + key + ")");
        --m_depth;
        return node;
    }
    const auto& def = (*m_types)[key];
    if (def.contains("fields"))
        for (const auto& f : def["fields"]) node->children.push_back(gwField(f));
    node->size = m_cursor - node->offset;
    --m_depth;
    return node;
}

ParsedNodePtr BinaryParser::parseGw2Packfile(const nlohmann::json& tpl, std::string& error) {
    m_types = tpl.contains("types") ? &tpl["types"] : nullptr;
    if (!m_types) { error = "gw2packfile template missing 'types'."; return nullptr; }
    static const nlohmann::json emptyObj = nlohmann::json::object();
    const nlohmann::json& chunks    = tpl.contains("chunks")    ? tpl["chunks"]    : emptyObj;
    const nlohmann::json& fileTypes = tpl.contains("fileTypes") ? tpl["fileTypes"] : emptyObj;
    // Per-chunk explicit schema pick from the UI: { "<chunkOffset>": "<typeKey>" }.
    // Lets the user apply a DIFFERENT strucTab to a chunk than the file-type default.
    const nlohmann::json& overrides = tpl.contains("chunkOverrides") ? tpl["chunkOverrides"] : emptyObj;

    size_t size = m_buffer->size();
    auto root = std::make_shared<ParsedNode>();
    root->name = "PackFile"; root->typeName = "gw2packfile"; root->offset = 0; root->size = size;

    if (size < 12 || (*m_buffer)[0] != 'P' || (*m_buffer)[1] != 'F') {
        error = "Not a PF packfile (missing 'PF' signature)."; return nullptr;
    }
    uint16_t pfVersion = readAt<uint16_t>(2);
    // Pointer width is encoded in PF version bit 2 (value & 4):
    //   set   -> 64-bit (8-byte) pointers,  clear -> 32-bit (4-byte).
    // Matches Gw2 Packfile.cpp:  v48 = (pfVersion & 4) ? 8 : 4.
    // A template may still force a width via "forcePointerSize": 32|64.
    int forced = tpl.value("forcePointerSize", 0);
    m_ptrBytes = (forced == 32) ? 4 : (forced == 64) ? 8 : ((pfVersion & 4) ? 8 : 4);

    // Container type = the file-type fourcc at offset 8; it selects which set of
    // strucTabs (chunk schemas) to use, because the same chunk fourcc (e.g. SKEL)
    // is registered by several file-types with DIFFERENT strucTabs.
    // A UI may override it via "containerOverride".
    std::string container = tpl.value("containerOverride", std::string());
    if (container.empty()) {
        char ctype[5] = {0};
        for (int i = 0; i < 4; ++i) ctype[i] = (char)(*m_buffer)[8 + i];
        container = ctype;
    }
    const nlohmann::json& group = fileTypes.contains(container) ? fileTypes[container] : emptyObj;

    uint16_t headerSize = readAt<uint16_t>(6);
    root->valueString = "container=" + container + ", PF v" + std::to_string(pfVersion) +
                        ", " + std::to_string(m_ptrBytes * 8) + "-bit ptr" +
                        (forced ? " (forced)" : " (auto)") +
                        (group.empty() ? "  [no file-type schema, using global]" : "");

    // Header detail node (so opening a file shows its header up-front).
    {
        auto h = std::make_shared<ParsedNode>();
        h->name = "Header"; h->typeName = "PF"; h->offset = 0; h->size = headerSize;
        auto add = [&](const std::string& n, const std::string& t, size_t off, size_t sz, const std::string& v){
            auto x = std::make_shared<ParsedNode>(); x->name=n; x->typeName=t; x->offset=off; x->size=sz; x->valueString=v; h->children.push_back(x);
        };
        add("signature","char[2]",0,2,"PF");
        add("pfVersion","uint16",2,2,std::to_string(pfVersion)+((pfVersion&4)?" (64-bit)":" (32-bit)"));
        add("headerSize","uint16",6,2,std::to_string(headerSize));
        add("containerType","char[4]",8,4,container);
        root->children.push_back(h);
    }

    size_t pos = headerSize >= 12 ? (size_t)headerSize : 12;

    while (pos + 16 <= size) {
        char fourcc[5] = {0};
        for (int i = 0; i < 4; ++i) fourcc[i] = (char)(*m_buffer)[pos + i];
        uint32_t chunkSize = (uint32_t)readUIntAt(pos + 4, 4);
        uint16_t version   = (uint16_t)readUIntAt(pos + 8, 2);
        uint16_t chunkHdr  = (uint16_t)readUIntAt(pos + 10, 2);
        size_t   dataStart = pos + chunkHdr;

        auto cnode = std::make_shared<ParsedNode>();
        cnode->name = std::string(fourcc) + "  (v" + std::to_string(version) + ")";
        cnode->typeName = "chunk"; cnode->offset = pos; cnode->size = (size_t)chunkSize + 8;

        // Resolve schema: explicit per-chunk override first, then file-type group, then global.
        std::string key; std::string vs = std::to_string(version);
        std::string offKey = std::to_string(pos);
        if (overrides.contains(offKey))                                  key = overrides[offKey].get<std::string>();
        else if (group.contains(fourcc) && group[fourcc].contains(vs))   key = group[fourcc][vs].get<std::string>();
        else if (chunks.contains(fourcc) && chunks[fourcc].contains(vs)) key = chunks[fourcc][vs].get<std::string>();
        if (!key.empty() && inBounds(dataStart, 1)) {
            m_cursor = dataStart; m_depth = 0;
            auto s = gwStruct(key, key);
            for (auto& ch : s->children) cnode->children.push_back(ch);
            if (s->children.empty() && !s->valueString.empty()) cnode->valueString = s->valueString;
        } else {
            cnode->valueString = key.empty() ? "(no schema for this chunk/version)" : "(bad data offset)";
        }
        root->children.push_back(cnode);

        size_t next = pos + 8 + (size_t)chunkSize;
        if (next <= pos) break;
        pos = next;
    }
    if (root->children.empty()) error = "No chunks found after PF header.";
    return root;
}
