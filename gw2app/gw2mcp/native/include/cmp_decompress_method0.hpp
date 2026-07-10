// cmp_decompress_method0.hpp -- single-header C++20 library
//
// Reimplementation of ArenaNet's CmpDecompress Method 0 (Huffman + LZ77),
// reverse-engineered from Gw2-64.exe (Services/Compress/CmpApi.cpp +
// CmpHuff.cpp). Verified byte-exact against a real Gw2.dat entry
// (THIRDPARTYSOFTWAREREADME.txt, 540537 bytes).
//
// Function addresses (for cross-reference in IDA):
//   CmpDecompress                 0x140D921C0
//   CmpDecompress_Method0_Inflate 0x140D96FC0   <- this is what's implemented here
//   CmpDecompress_Method1_Delta   0x140D94E20   <- NOT implemented (delta/patch method)
//   Huffman table builder         0x140D9DF00   (CmpHuff.cpp)
//
// Pipeline (Gw2.dat -> plaintext):
//   1. Read the MFT entry's raw [offset, offset+size) bytes from Gw2.dat.
//   2. Strip the 4-byte CRC32 inserted every 0x10000 bytes (+ trailing CRC).
//      See gw2cmp::strip_crc32().
//   3. Parse an 8-byte header from the stripped buffer:
//        offset 0..3 : flag dword (low 16 bits observed = compression method
//                      id 8 == "ANet compress"; meaning only partially understood)
//        offset 4..7 : uncompressedSize (u32 LE)
//   4. The remaining bytes are the CmpDecompress bitstream:
//        - first 4 bits : Method (0 = plain, 1 = delta -- not implemented here)
//        - next 4 bits  : minMatchAdd-1 (Method 0 only)
//        - then repeat: two freshly-rebuilt canonical Huffman tables
//          (literal/length, then distance) followed by up to (nibble+1)<<12
//          symbols coded against those tables, LZ77-style.
//
// Usage: header-only, just #include it. Everything lives in namespace gw2cmp.
// Errors are reported via exceptions (gw2cmp::decode_error).
//
//   std::vector<uint8_t> raw = ...;                 // raw Gw2.dat MFT entry bytes
//   std::vector<uint8_t> text = gw2cmp::decompress_entry(raw);

#ifndef GW2CMP_DECOMPRESS_METHOD0_HPP
#define GW2CMP_DECOMPRESS_METHOD0_HPP

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace gw2cmp {

class decode_error : public std::runtime_error {
public:
    explicit decode_error(const std::string &what) : std::runtime_error(what) {}
};

namespace detail {

// ---- bit reader: sequential little-endian 32-bit words, MSB-first bits ----
class BitReader {
public:
    explicit BitReader(std::span<const uint8_t> data)
        : data_(data), end_byte_((data.size() / 4) * 4) {}

    uint32_t read(int n) {
        if (n == 0) return 0;
        refill(n);
        bits_ -= n;
        uint32_t val = static_cast<uint32_t>((acc_ >> bits_) & mask(n));
        acc_ &= mask(bits_);
        return val;
    }

    uint32_t peek(int n) {
        refill(n);
        return static_cast<uint32_t>((acc_ >> (bits_ - n)) & mask(n));
    }

private:
    static uint64_t mask(int n) { return (n >= 64) ? ~uint64_t{0} : ((uint64_t{1} << n) - 1); }

    void refill(int n) {
        while (bits_ < n) {
            uint32_t word = 0;
            if (pos_ + 4 <= end_byte_) {
                const uint8_t *p = data_.data() + pos_;
                word = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                       (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
                pos_ += 4;
            }
            acc_ = (acc_ << 32) | word;
            bits_ += 32;
        }
    }

    std::span<const uint8_t> data_;
    size_t end_byte_;
    size_t pos_ = 0;
    uint64_t acc_ = 0;
    int bits_ = 0;
};

// ---- static tables, extracted verbatim from the binary via IDA ----

// byte_142061180 (32 entries) -- extra bits for length codes 256..284
inline constexpr uint8_t LEN_EXTRA[32] = {
    0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1,
    2, 2, 2, 2,
    3, 3, 3, 3,
    4, 4, 4, 4,
    5, 5, 5, 5,
    0, 0, 0, 0,
};

// byte_142060FA0 (32 entries) -- base length value (before adding minMatchAdd)
inline constexpr uint16_t LEN_BASE[32] = {
    0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7,
    0x8, 0xa, 0xc, 0xe,
    0x10, 0x14, 0x18, 0x1c,
    0x20, 0x28, 0x30, 0x38,
    0x40, 0x50, 0x60, 0x70,
    0x80, 0xa0, 0xc0, 0xe0,
    0xff, 0x0, 0x0, 0x0,
};

// byte_1420610E0 (32 entries) -- extra bits for distance codes
inline constexpr uint8_t DIST_EXTRA[32] = {
    0, 0, 0, 0,
    1, 1, 2, 2,
    3, 3, 4, 4,
    5, 5, 6, 6,
    7, 7, 8, 8,
    9, 9, 10, 10,
    11, 11, 12, 12,
    13, 13, 14, 14,
};

// word_142060F60 (32 entries, 16-bit) -- base distance value
inline constexpr uint32_t DIST_BASE[32] = {
    0x0, 0x1, 0x2, 0x3, 0x4, 0x6, 0x8, 0xc,
    0x10, 0x18, 0x20, 0x30,
    0x40, 0x60, 0x80, 0xc0,
    0x100, 0x180, 0x200, 0x300,
    0x400, 0x600, 0x800, 0xc00,
    0x1000, 0x1800, 0x2000, 0x3000,
    0x4000, 0x6000, 0x0, 0x0,
};

// Fixed "meta Huffman" used to decode the RLE-packed code-length alphabet
// (unk_142061620: 14 (mask, offset, bitlen) records, mask descending).
struct MetaRow { uint32_t mask; uint16_t offset; uint8_t bitlen; };

inline constexpr MetaRow META_TABLE[14] = {
    { 0xa0000000u, 2, 3 },
    { 0x60000000u, 6, 4 },
    { 0x40000000u, 10, 5 },
    { 0x20000000u, 18, 6 },
    { 0x12000000u, 25, 7 },
    { 0x0c000000u, 31, 8 },
    { 0x07000000u, 41, 9 },
    { 0x03000000u, 57, 10 },
    { 0x01600000u, 70, 11 },
    { 0x00f00000u, 77, 12 },
    { 0x00c00000u, 83, 13 },
    { 0x00b00000u, 87, 14 },
    { 0x00a00000u, 95, 15 },
    { 0x00000000u, 255, 16 },
};

// byte_142061690 (256 entries) -- value table indexed by the meta-huffman lookup
inline constexpr uint8_t META_VALUES[256] = {
    0x8, 0x9, 0xa, 0x0, 0x7, 0xb, 0xc, 0x6, 0x29, 0x2a, 0xe0, 0x4, 0x5, 0x20, 0x28, 0x2b,
    0x2c, 0x40, 0x4a, 0x3, 0xd, 0x25, 0x26, 0x27, 0x48, 0x49, 0x24, 0x47, 0x4b, 0x4c, 0x69, 0x6a,
    0x23, 0x46, 0x60, 0x63, 0x67, 0x68, 0x88, 0x89, 0xa0, 0xe8, 0x1, 0x2, 0x2d, 0x43, 0x44, 0x45,
    0x65, 0x66, 0x80, 0x87, 0x8a, 0xa8, 0xa9, 0xc0, 0xc9, 0xe9, 0xe, 0x4d, 0x64, 0x6b, 0x6c, 0x84,
    0x85, 0x8b, 0xa4, 0xa5, 0xaa, 0xc8, 0xe5, 0x83, 0x86, 0xa6, 0xa7, 0xc7, 0xca, 0xe7, 0x22, 0x2e,
    0x8c, 0xc4, 0xe4, 0xe6, 0x4e, 0x6d, 0xc6, 0xec, 0xf, 0x10, 0x11, 0x8d, 0xab, 0xac, 0xcc, 0xea,
    0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x21, 0x2f,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    0x41, 0x42, 0x4f, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c,
    0x5d, 0x5e, 0x5f, 0x61, 0x62, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x81, 0x82, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94,
    0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0xa1, 0xa2, 0xa3, 0xad, 0xae,
    0xaf, 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe,
    0xbf, 0xc1, 0xc2, 0xc3, 0xc5, 0xcb, 0xcd, 0xce, 0xcf, 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6,
    0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, 0xe1, 0xe2, 0xe3, 0xeb, 0xed, 0xee, 0xef,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};

inline uint8_t decode_meta(BitReader &br) {
    uint32_t v = br.peek(32);
    for (const auto &row : META_TABLE) {
        if (v >= row.mask) {
            uint32_t rel = (v - row.mask) >> (32 - row.bitlen);
            uint32_t idx = static_cast<uint32_t>(row.offset) - rel;
            if (idx >= 256) throw decode_error("meta huffman decode failed (index out of range)");
            br.read(row.bitlen);
            return META_VALUES[idx];
        }
    }
    throw decode_error("meta huffman decode failed (no matching mask)");
}

// ---- canonical Huffman decode table: sorted (length, code) -> symbol ----
struct HuffEntry {
    uint32_t code;
    uint16_t symbol;
    uint8_t length;

    auto operator<=>(const HuffEntry &other) const {
        if (auto c = length <=> other.length; c != 0) return c;
        return code <=> other.code;
    }
};

class HuffTable {
public:
    // Mirrors sub_140D9DF00: reads a 16-bit symbol count, then an RLE-encoded
    // array of code lengths (via the fixed meta table), then builds a
    // canonical Huffman decode table.
    //
    // ArenaNet assigns codes in DESCENDING order per length group (ascending
    // symbol index -> descending code value), with the running code counter
    // kept as an UNSIGNED 32-bit value that WRAPS AROUND once a length group
    // fully consumes its available code space -- that wrapped value is
    // exactly what feeds "2*v+1" for the next length. Using a
    // non-wrapping counter here silently produces wrong codes for every
    // length past the point where the first wrap happens.
    static HuffTable build(BitReader &br) {
        uint32_t total_symbols = br.read(16);

        std::vector<uint8_t> code_lengths(total_symbols, 0);
        long idx = static_cast<long>(total_symbols) - 1;
        while (idx >= 0) {
            uint8_t rle = decode_meta(br);
            uint32_t repeat = static_cast<uint32_t>(rle >> 5) + 1;
            uint8_t length = rle & 0x1F;
            if (length != 0 || total_symbols < 2) {
                for (uint32_t k = 0; k < repeat; k++) {
                    if (idx < 0) throw decode_error("code length RLE underflow");
                    code_lengths[static_cast<size_t>(idx)] = length;
                    idx--;
                }
            } else {
                idx -= static_cast<long>(repeat);
            }
        }

        uint8_t max_len = code_lengths.empty() ? 0 : *std::max_element(code_lengths.begin(), code_lengths.end());

        // Per-length linked lists (head = v66[length], next = v63[symbol]),
        // built by prepending while idx descends -- walking from the head
        // therefore visits symbols in ascending index order, matching the
        // original code exactly.
        constexpr uint32_t NIL = 0xFFFFFFFFu;
        std::vector<uint32_t> v66(static_cast<size_t>(max_len) + 1, NIL);
        std::vector<uint32_t> v63(total_symbols, 0);
        idx = static_cast<long>(total_symbols) - 1;
        while (idx >= 0) {
            uint8_t l = code_lengths[static_cast<size_t>(idx)];
            if (l != 0) {
                v63[static_cast<size_t>(idx)] = v66[l];
                v66[l] = static_cast<uint32_t>(idx);
            }
            idx--;
        }

        HuffTable table;
        uint32_t v35 = 0;
        for (uint32_t length = 0; length <= max_len; length++) {
            uint32_t sym = v66[length];
            while (sym != NIL && v35 < (uint32_t{1} << length) && sym < total_symbols) {
                table.entries_.push_back({v35, static_cast<uint16_t>(sym), static_cast<uint8_t>(length)});
                v35 = v35 - 1; // unsigned wraparound intended
                sym = v63[sym];
            }
            v35 = 2u * v35 + 1u; // unsigned wraparound intended
        }

        std::sort(table.entries_.begin(), table.entries_.end());
        return table;
    }

    uint16_t decode(BitReader &br) const {
        uint32_t code = 0;
        for (int length = 1; length <= 24; length++) {
            code = (code << 1) | br.read(1);
            HuffEntry key{code, 0, static_cast<uint8_t>(length)};
            auto it = std::lower_bound(entries_.begin(), entries_.end(), key,
                                        [](const HuffEntry &a, const HuffEntry &b) {
                                            if (a.length != b.length) return a.length < b.length;
                                            return a.code < b.code;
                                        });
            if (it != entries_.end() && it->length == length && it->code == code) return it->symbol;
        }
        throw decode_error("huffman decode failed (no matching code)");
    }

private:
    std::vector<HuffEntry> entries_;
};

} // namespace detail

// Decompresses a CmpDecompress Method-0 bitstream. `comp` is the bitstream
// that comes right after the 8-byte {flag,uncompressedSize} header (with
// CRC32 checksums already stripped). `output_size` is normally read from
// that header. Throws gw2cmp::decode_error on malformed input.
inline std::vector<uint8_t> decompress_method0(std::span<const uint8_t> comp, size_t output_size) {
    detail::BitReader br(comp);

    uint32_t method = br.read(4);
    if (method != 0)
        throw decode_error("expected Method 0, got Method " + std::to_string(method) +
                            " (Method 1 = delta, not implemented)");
    uint32_t min_match_add = br.read(4) + 1;

    std::vector<uint8_t> out;
    out.reserve(output_size);

    while (out.size() < output_size) {
        detail::HuffTable lit_table = detail::HuffTable::build(br);
        detail::HuffTable dist_table = detail::HuffTable::build(br);
        uint32_t block_symbols = (br.read(4) + 1u) << 12;

        for (uint32_t i = 0; i < block_symbols && out.size() < output_size; i++) {
            uint16_t sym = lit_table.decode(br);
            if (sym < 0x100) {
                out.push_back(static_cast<uint8_t>(sym));
            } else {
                uint32_t li = sym - 256;
                uint8_t extra = detail::LEN_EXTRA[li];
                uint32_t length = detail::LEN_BASE[li] + (extra ? br.read(extra) : 0) + min_match_add;

                uint16_t dsym = dist_table.decode(br);
                uint8_t dextra = detail::DIST_EXTRA[dsym];
                uint32_t dist = detail::DIST_BASE[dsym] + (dextra ? br.read(dextra) : 0);

                if (dist + 1 > out.size())
                    throw decode_error("back-reference distance out of range (corrupt stream or decoder bug)");
                size_t start = out.size() - dist - 1;
                for (uint32_t k = 0; k < length && out.size() < output_size; k++)
                    out.push_back(out[start + k]);
            }
        }
    }

    return out;
}

// Strips the 4-byte CRC32 checksum Gw2.dat inserts every 0x10000 bytes of a
// raw on-disk MFT entry (plus one trailing CRC at EOF).
inline std::vector<uint8_t> strip_crc32(std::span<const uint8_t> raw) {
    constexpr size_t CHUNK_SIZE = 0x10000;
    constexpr size_t START_INDEX = CHUNK_SIZE - 4;
    constexpr size_t END_INDEX = CHUNK_SIZE;

    std::vector<uint8_t> data(raw.begin(), raw.end());

    if (raw.size() > CHUNK_SIZE) {
        size_t position = 0;
        while (position + CHUNK_SIZE <= data.size()) {
            data.erase(data.begin() + static_cast<long>(position + START_INDEX),
                       data.begin() + static_cast<long>(position + END_INDEX));
            position += CHUNK_SIZE - 4;
        }
        if (data.size() > 4) data.resize(data.size() - 4);
    } else if (raw.size() == CHUNK_SIZE) {
        data.erase(data.begin() + static_cast<long>(START_INDEX), data.begin() + static_cast<long>(END_INDEX));
    } else {
        if (data.size() > 4) data.resize(data.size() - 4);
    }

    return data;
}

// Full pipeline: raw MFT entry bytes (still containing periodic CRC32
// checksums and the 8-byte {flag,size} header) -> decompressed bytes.
inline std::vector<uint8_t> decompress_entry(std::span<const uint8_t> raw) {
    std::vector<uint8_t> stripped = strip_crc32(raw);
    if (stripped.size() < 8) throw decode_error("input too small to contain the 8-byte header");

    uint32_t uncompressed_size = static_cast<uint32_t>(stripped[4]) | (static_cast<uint32_t>(stripped[5]) << 8) |
                                  (static_cast<uint32_t>(stripped[6]) << 16) | (static_cast<uint32_t>(stripped[7]) << 24);

    return decompress_method0(std::span<const uint8_t>(stripped).subspan(8), uncompressed_size);
}

} // namespace gw2cmp

#endif // GW2CMP_DECOMPRESS_METHOD0_HPP
