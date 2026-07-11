#include "strs_keys.h"

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <unordered_map>

namespace gw2skeys {
namespace {

std::unordered_map<uint32_t, uint64_t>  g_keys;      // textId -> 8-byte password
std::unordered_map<uint32_t, long long> g_textbase;  // fileId -> baseTextId

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

inline uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

// Port of CptRc4 key-expansion (Gw2 sub_140D9F630): 8-byte key -> 20-byte key.
void expand_key(uint64_t key8, uint8_t out20[20]) {
    uint8_t kb[8];
    for (int i = 0; i < 8; ++i) kb[i] = static_cast<uint8_t>(key8 >> (8 * i));
    uint8_t buf[20];
    for (int i = 0; i < 20; ++i) buf[i] = kb[i % 8];
    uint32_t A, B, C, D, E;
    std::memcpy(&A, buf + 0, 4);  std::memcpy(&B, buf + 4, 4);
    std::memcpy(&C, buf + 8, 4);  std::memcpy(&D, buf + 12, 4);
    std::memcpy(&E, buf + 16, 4);
    uint32_t t   = A - 1615554381u;
    uint32_t v15 = rol(t, 5) + B + 1722862861u;
    uint32_t v16 = rol(t, 30);
    uint32_t v17 = ((~(t & 0x22222222u)) & 0x7BF36AE2u) - 214083945u + C + rol(v15, 5);
    uint32_t v18 = (v15 & (v16 ^ 0x59D148C0u)) ^ 0x59D148C0u;
    uint32_t v19 = rol(v15, 30);
    uint32_t v20 = v18 - 696916869u + D + rol(v17, 5);
    uint32_t A2 = A + rol(v20, 5) + E + (v16 ^ (v17 & (v19 ^ v16))) - 1269579175u;
    uint32_t B2 = B + v20;
    uint32_t C2 = C + rol(v17, 30);
    uint32_t D2 = D + v19;
    uint32_t E2 = E + v16;
    std::memcpy(out20 + 0, &A2, 4);  std::memcpy(out20 + 4, &B2, 4);
    std::memcpy(out20 + 8, &C2, 4);  std::memcpy(out20 + 12, &D2, 4);
    std::memcpy(out20 + 16, &E2, 4);
}

// Standard RC4 (CptRc4 KSA + PRGA) over `payload` with the 20-byte key, in place.
void rc4(const uint8_t key[20], std::vector<uint8_t>& buf) {
    uint8_t S[256];
    for (int i = 0; i < 256; ++i) S[i] = static_cast<uint8_t>(i);
    int j = 0;
    for (int i = 0; i < 256; ++i) {
        j = (j + S[i] + key[i % 20]) & 0xFF;
        std::swap(S[i], S[j]);
    }
    int a = 0, b = 0;
    for (auto& c : buf) {
        a = (a + 1) & 0xFF;
        b = (b + S[a]) & 0xFF;
        std::swap(S[a], S[b]);
        c ^= S[(S[a] + S[b]) & 0xFF];
    }
}

// Range/bit-packing unpack (Gw2 sub_1410CFC50 core loop).
const wchar_t* kSymTable = L"0123456strnum()[]<>%#/:-'\" ,.!\n"; // symbols 1..31

std::wstring bitunpack(const uint8_t* pl, size_t n, uint16_t baseChar, uint8_t rangeBits) {
    std::wstring out;
    if (rangeBits == 0 || rangeBits > 16) return out;
    uint32_t acc = 0; int nbits = 0; size_t p = 0;
    size_t maxout = 8 * n / rangeBits + 1;
    const size_t tableLen = wcslen(kSymTable);
    for (size_t k = 0; k < maxout; ++k) {
        while (nbits <= 24) {
            if (p < n) acc |= static_cast<uint32_t>(pl[p++]) << nbits;
            nbits += 8;
        }
        uint32_t sym = acc & ((1u << rangeBits) - 1);
        acc >>= rangeBits; nbits -= rangeBits;
        if (sym == 0) break;
        if (sym < 0x20) {
            if (sym - 1 < tableLen) out.push_back(kSymTable[sym - 1]);
        } else {
            out.push_back(static_cast<wchar_t>(sym + baseChar - 32));
        }
    }
    return out;
}

// Parse "a,b" integer rows; skip blank / '#' / header. cb(col0,col1_str).
template <class F>
long parse_csv(const std::wstring& path, F&& cb) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return -1;
    long n = 0; char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char* comma = std::strchr(line, ',');
        if (!comma) continue;
        *comma = 0;
        // skip header row ("textId,..")
        if (line[0] < '0' || line[0] > '9') continue;
        cb(line, comma + 1);
        ++n;
    }
    fclose(f);
    return n;
}

} // namespace

long load_keys(const std::wstring& path) {
    long r = parse_csv(path, [](const char* a, const char* b) {
        uint32_t id = static_cast<uint32_t>(strtoul(a, nullptr, 10));
        uint64_t key = strtoull(b, nullptr, 16);
        if (id != 0xFFFFFFFFu && key) g_keys[id] = key;
    });
    return r < 0 ? -1 : static_cast<long>(g_keys.size());
}

long load_textbase(const std::wstring& path) {
    return parse_csv(path, [](const char* a, const char* b) {
        g_textbase[static_cast<uint32_t>(strtoul(a, nullptr, 10))] =
            static_cast<long long>(strtoll(b, nullptr, 10));
    });
}

bool   keys_ready()     { return !g_keys.empty(); }
size_t key_count()      { return g_keys.size(); }
bool   textbase_ready() { return !g_textbase.empty(); }

long long base_for_fileids(const std::vector<uint32_t>& file_ids) {
    for (uint32_t fid : file_ids) {
        auto it = g_textbase.find(fid);
        if (it != g_textbase.end()) return it->second;
    }
    return -1;
}

std::wstring decode(const uint8_t* data, size_t size, long long baseTextId) {
    if (size < 4 || std::memcmp(data, "strs", 4) != 0) return L"(not a strs container)";
    std::wstring out;
    wchar_t line[160];
    size_t off = 4;
    uint32_t index = 0, raw_count = 0, dec_count = 0, locked_count = 0;

    while (off + 6 <= size) {
        uint16_t rec_len = rd16(data + off);
        if (rec_len < 6 || off + rec_len > size) break;
        uint16_t base_char = rd16(data + off + 2);
        uint8_t  range_bits = data[off + 4];
        const uint8_t* payload = data + off + 6;
        size_t payload_len = static_cast<size_t>(rec_len) - 6;

        if (base_char == 0 && range_bits == 16 && (payload_len % 2) == 0) {
            std::wstring text(reinterpret_cast<const wchar_t*>(payload), payload_len / 2);
            if (!text.empty() && text.back() == L'\0') text.pop_back();
            swprintf(line, 160, L"#%u  ", index);
            out += line; out += text; out += L"\r\n";
            ++raw_count;
        } else {
            long long textId = (baseTextId >= 0) ? baseTextId + index : -1;
            auto it = (textId >= 0) ? g_keys.find(static_cast<uint32_t>(textId)) : g_keys.end();
            if (it != g_keys.end()) {
                std::vector<uint8_t> buf(payload, payload + payload_len);
                uint8_t key20[20];
                expand_key(it->second, key20);
                rc4(key20, buf);
                std::wstring text = bitunpack(buf.data(), buf.size(), base_char, range_bits);
                swprintf(line, 160, L"#%u  ", index);
                out += line; out += text; out += L"\r\n";
                ++dec_count;
            } else {
                swprintf(line, 160,
                         L"#%u  [packed base=0x%04X rb=%u len=%u]  <no key%s>\r\n",
                         index, base_char, range_bits, rec_len,
                         textId < 0 ? L": textbase unknown" : L" for this textId");
                out += line;
                ++locked_count;
            }
        }
        off += rec_len;
        ++index;
    }

    wchar_t header[320];
    swprintf(header, 320,
             L"=== strs string table ===\r\n%u records (%u raw UTF-16, %u decrypted, %u locked)"
             L"  [keys loaded: %zu]\r\n\r\n",
             index, raw_count, dec_count, locked_count, g_keys.size());
    return std::wstring(header) + out;
}

} // namespace gw2skeys
