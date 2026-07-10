// gw2_atex.hpp -- ArenaNet ATEX/ATEP/... texture decoder, single header (C++20)
//
// Reverse-engineered from Gw2-64.exe sub_140B83040 (Arena\Engine\Gr\Img\ImgAtex.cpp).
// Header-only, no external dependencies. Produces RGBA8888 (row-major, top-left).
//
//   #include "gw2_atex.hpp"
//   auto tex = gw2atex::parse(data, size);          // throws std::runtime_error
//   gw2atex::Image img = gw2atex::decode(tex, 0);   // mip 0 -> img.rgba (w*h*4)
//
// Supported: DXT1/2/3/4/5, DXTA, DXTL, DXTN, 3DCX, BC5, BC7.
//
#ifndef GW2_ATEX_HPP
#define GW2_ATEX_HPP

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <array>
#include <vector>
#include <string>
#include <stdexcept>

namespace gw2atex {

// ---------------------------------------------------------------------------
//  Format tables (extracted verbatim from the binary)
// ---------------------------------------------------------------------------
inline const char* format_name(int e) {
    static const char* N[] = {
        "ARGB32323232F","ARGB16161616F","ARGB2101010","ARGB8888","XRGB8888",
        "ARGB4444","ARGB1555","RGB888","RGB565","RGB555","RG1616","RG1616F",
        "RG3232F","R16F","R32F","AL88","AL44","AL8","L8","A8","P8","VU88",
        "DXT1","DXT2","DXT3","DXT4","DXT5","DXTA","DXTL","DXTN","3DCX","BC5",
        "BC7","D24","SHADOWMAP","ABGR8888","R32UINT","UNKNOWN" };
    return (e >= 0 && e < 38) ? N[e] : "UNKNOWN";
}

// s_flags[] @ dword_141C73CF0  (bit0 = block-compressed; 0x210/0x280 select planes)
inline constexpr uint32_t FORMAT_FLAGS[38] = {
    0xB2,0xB2,0xB2,0xB2,0x12,0xB2,0x72,0x12,0x12,0x12,0x12,0x12,0x12,0x100,
    0x100,0x1A4,0x1A4,0x1A4,0x104,0xA2,0x78,0x400,0x71,0xB1,0xB1,0xB1,0xB1,
    0xA1,0x11,0x201,0x201,0x201,0xB1,0x00,0x00,0xB2,0x12,0x00 };

inline int fourcc_to_enum(const uint8_t* f) {
    struct M { char c[4]; int e; };
    static const M m[] = {
        {{'D','X','T','1'},22},{{'D','X','T','2'},23},{{'D','X','T','3'},24},
        {{'D','X','T','4'},25},{{'D','X','T','5'},26},{{'D','X','T','A'},27},
        {{'D','X','T','L'},28},{{'D','X','T','N'},29},{{'3','D','C','X'},30},
        {{'B','C','5','X'},31},{{'B','C','7','X'},32} };
    for (auto& e : m)
        if (!std::memcmp(f, e.c, 4)) return e.e;
    return -1;
}

// RLE Huffman table @ byte_141C86390 : 64 (len,val) pairs indexed by top 6 bits.
inline void huff_tables(uint8_t len[64], uint8_t val[64]) {
    for (int k = 0; k < 16; ++k) { len[k] = 6; val[k] = uint8_t(16 - k); }
    for (int k = 16; k < 32; ++k) { len[k] = 2; val[k] = 0x11; }
    for (int k = 32; k < 64; ++k) { len[k] = 1; val[k] = 0x00; }
}

// ---------------------------------------------------------------------------
//  MSB-first bit reader fed by 32-bit little-endian words
// ---------------------------------------------------------------------------
class BitReader {
public:
    BitReader(const uint8_t* buf, size_t start, size_t end)
        : buf_(buf), pos_(start), end_(end) {}

    size_t pos() const { return pos_; }

    uint64_t read(int n) {
        if (n == 0) return 0;
        need(n);
        uint64_t v = head_ >> (64 - n);
        head_ <<= n;
        bits_ -= n;
        return v;
    }

    // one RLE token: {count = symbol+1, filled}
    void read_run(const uint8_t* hlen, const uint8_t* hval, int& count, int& filled) {
        need(7);
        uint32_t k = uint32_t(head_ >> 58);   // top 6 bits
        int clen = hlen[k];
        int sym = hval[k];
        head_ <<= clen;
        bits_ -= clen;
        filled = (int)read(1);
        count = sym + 1;
    }

    // Switch to byte-aligned reads (matches the game's realign branch):
    //   bits >= 32 -> keep exactly one word (rewind 4); else drop everything.
    void align_to_word() {
        if (bits_ >= 32) pos_ -= 4;
        head_ = 0;
        bits_ = 0;
    }

private:
    void pull_word() {
        uint32_t w = 0;
        for (int i = 0; i < 4; ++i)
            if (pos_ + i < end_) w |= uint32_t(buf_[pos_ + i]) << (8 * i);
        pos_ += 4;
        head_ |= (uint64_t)w << (32 - bits_);
        bits_ += 32;
    }
    void need(int n) { while (bits_ < n) pull_word(); }

    const uint8_t* buf_;
    size_t pos_, end_;
    uint64_t head_ = 0;
    int bits_ = 0;
};

// ---------------------------------------------------------------------------
//  encode_bc1_color -- port of sub_140BF7470
//  Encodes one RGB888 colour into an 8-byte BC1 colour block.
// ---------------------------------------------------------------------------
inline std::array<uint8_t, 8> encode_bc1_color(uint32_t rgb, bool is_dxt1) {
    int v3 = rgb & 0xFF, v4 = (rgb >> 8) & 0xFF, v5 = (rgb >> 16) & 0xFF;
    int v6 = v4 - (v4 >> 6);
    int v7 = (v3 - (v3 >> 5)) >> 3;
    int v8 = v5 >> 5;
    int v9 = 8 * v7 + ((v3 - (v3 >> 5)) >> 5);
    int v31 = v6 >> 2;
    int v32 = (v5 - v8) >> 3;
    int v10 = v7 + 1, v30 = v7;
    int v11 = 4 * (v6 >> 2);
    int v12 = v11 + (v6 >> 6);
    int den1 = ((v7 + 1) >> 2) - v9 + 8 * v7 + 8;
    int v13 = den1 ? 12 * (v3 - v9) / den1 : 0;
    int v14 = (v6 >> 2) + 1;
    int v15 = v32 + 1;
    int den2 = (v14 >> 4) - v12 + 4 + v11;
    int v16 = den2 ? 12 * (v4 - v12) / den2 : 0;
    int den3 = ((v32 + 1) >> 2) - ((v5 - v8) >> 5) + 8;
    int v17 = den3 ? 12 * (v5 - (8 * v32 + ((v5 - v8) >> 5))) / den3 : 0;
    int v18, v19, v20;

    if (v13 < 2) { v10 = v30; v18 = v10; }
    else if (v13 >= 6) { if (v13 >= 0xA) { v18 = v10; } else { v18 = v10; v10 = v30; } }
    else v18 = v30;

    if (v16 < 2) { v14 = v31; v19 = v14; }
    else if (v16 >= 6) { if (v16 >= 0xA) { v19 = v14; } else { v19 = v14; v14 = v31; } }
    else v19 = v31;

    if (v17 >= 2) {
        if (v17 < 6) v20 = v32;
        else if (v17 < 0xA) { v20 = v32 + 1; v15 = v32; }
        else v20 = v15;
    } else { v15 = v32; v20 = v15; }

    int v22 = (v18 | (32 * (v19 | (v20 << 6)))) & 0xFFFF;
    int v23 = (v10 | (32 * (v14 | (v15 << 6)))) & 0xFFFF;

    int v24 = 0, v25 = 0;
    if (v18 != v10) { v24 = v13; v25 = 1; if (v18 != v30) v24 = 12 - v13; }
    if (v19 != v14) { if (v19 != v31) v16 = 12 - v16; v24 += v16; ++v25; }
    if (v20 != v15) { if (v20 != v32) v17 = 12 - v17; v24 += v17; ++v25; }
    if (v25) v24 = (v24 + (v25 >> 1)) / v25;

    int v26 = (is_dxt1 && ((v24 - 5 >= 0 && v24 - 5 <= 1) || v25 == 0)) ? 1 : 0;
    if (v25 == 0 && !v26) {
        if (v23 == 0xFFFF) { v24 = 12; v22 = (v22 - 1) & 0xFFFF; }
        else { v24 = 0; v23 = (v23 + 1) & 0xFFFF; }
    }
    if (v26 != (v22 <= v23 ? 1 : 0)) { int t = v22; v22 = v23; v23 = t; v24 = 12 - v24; }

    int v21 = 0;
    if (!v26) {
        if (v24 >= 2) { if (v24 < 6) v21 = 2; else { v21 = 1; if (v24 < 0xA) v21 = 3; } }
    } else v21 = 2;

    uint32_t idx = (uint32_t)(v21 | (4 * v21) | (16 * (v21 | (4 * v21))));
    idx = (idx | (idx << 8)) & 0xFFFF;
    idx = (idx | (idx << 16));
    std::array<uint8_t, 8> out{};
    out[0] = v22 & 0xFF; out[1] = (v22 >> 8) & 0xFF;
    out[2] = v23 & 0xFF; out[3] = (v23 >> 8) & 0xFF;
    out[4] = idx & 0xFF; out[5] = (idx >> 8) & 0xFF;
    out[6] = (idx >> 16) & 0xFF; out[7] = (idx >> 24) & 0xFF;
    return out;
}

// ---------------------------------------------------------------------------
//  Terrain-atlas border mirroring (port of sub_140B82AF0), 256x256 DXT3/DXT4
// ---------------------------------------------------------------------------
inline uint32_t ror4(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }
inline uint32_t bswap32(uint32_t x) {
    return ((x >> 24) & 0xFF) | ((x >> 8) & 0xFF00) |
           ((x << 8) & 0xFF0000) | ((x << 24) & 0xFF000000);
}

inline void terrain_mirror(uint8_t* surf, int nbBlocks) {
    const uint32_t BORDERS = 0xC0000003u;
    for (int i = 0; i < nbBlocks; ++i) {
        uint32_t v5 = (1u << (i >> 6)) & BORDERS;
        uint32_t v6 = (1u << (i & 31)) & BORDERS;  // note: matches (1<<i)&mask semantics
        if (!(v6 || v5)) continue;
        int v7 = v6 ? ((i & 0x3F) ^ 3) : (i & 0x3F);
        int v8 = v5 ? ((i >> 6) ^ 3) : (i >> 6);
        int src = 16 * (v7 + (v8 << 6));
        uint32_t a, b, c, d;
        std::memcpy(&a, surf + src + 0, 4);
        std::memcpy(&b, surf + src + 4, 4);
        std::memcpy(&c, surf + src + 8, 4);
        std::memcpy(&d, surf + src + 12, 4);
        if (v6) {
            b = a;
            a = (16 * (a & 0xF000F0 | ((a & 0xFFFF000F) << 8))) |
                ((a & 0xF000F00 | (a >> 8) & 0xF000F0) >> 4);
            d = ((d & 0x30303030 | (d >> 4) & 0xC0C0C0C) >> 2) |
                (4 * (d & 0xC0C0C0C | (16 * (d & 0xFF030303))));
        }
        if (v5) { uint32_t t = ror4(b, 16); b = ror4(a, 16); a = t; d = bswap32(d); }
        std::memcpy(surf + 16 * i + 0, &a, 4);
        std::memcpy(surf + 16 * i + 4, &b, 4);
        std::memcpy(surf + 16 * i + 8, &c, 4);
        std::memcpy(surf + 16 * i + 12, &d, 4);
    }
}

// ---------------------------------------------------------------------------
//  Structures
// ---------------------------------------------------------------------------
struct Mip {
    int level, width, height, block_w, block_h, bytes_per_block;
    uint32_t flags;
    bool raw;
    std::vector<uint8_t> surface;
};
struct Texture {
    char magic[4], fourcc[4];
    int fmt_enum;
    std::string fmt_name;
    int width, height;
    std::vector<Mip> mips;
};
struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;   // width*height*4, RGBA, top-left origin
};

// ---------------------------------------------------------------------------
//  RLE constant fill
// ---------------------------------------------------------------------------
inline bool bt(const std::vector<uint32_t>& bm, int i) { return (bm[i >> 5] >> (i & 31)) & 1; }
inline void bs(std::vector<uint32_t>& bm, int i) { bm[i >> 5] |= (1u << (i & 31)); }

inline void rle_fill(BitReader& br, const uint8_t* hlen, const uint8_t* hval,
                     std::vector<uint8_t>& surf, int nbBlocks, int bpb, int within_off,
                     const uint8_t* val, int n, std::vector<uint32_t>& read_bm,
                     std::vector<uint32_t>* mark0, std::vector<uint32_t>* mark1,
                     bool two_value) {
    std::vector<uint8_t> zero(n, 0);
    int i = 0;
    while (i < nbBlocks) {
        int count, filled;
        br.read_run(hlen, hval, count, filled);
        const uint8_t* cur = val;
        if (filled && two_value) cur = br.read(1) ? val : zero.data();
        while (count > 0 && i < nbBlocks) {
            if (!bt(read_bm, i)) {
                if (filled) {
                    std::memcpy(surf.data() + i * bpb + within_off, cur, n);
                    if (mark0) bs(*mark0, i);
                    if (mark1) bs(*mark1, i);
                }
                --count;
            }
            ++i;
        }
        while (i < nbBlocks && bt(read_bm, i)) ++i;
    }
}

// ---------------------------------------------------------------------------
//  Per-mip inflate
// ---------------------------------------------------------------------------
inline void inflate_mip(const uint8_t* buf, size_t payload_start, size_t payload_end,
                        uint32_t flags, int fmt_enum, int block_w, int block_h,
                        std::vector<uint8_t>& surf, int& bpb_out) {
    uint32_t ff = FORMAT_FLAGS[fmt_enum];
    int a2 = (ff & 0x280) ? 2 : 0;
    int dxtl = (fmt_enum == 28) ? 2 : 0;
    int a210 = (ff & 0x210) ? 2 : 0;
    int units = a2 + a210 + dxtl;
    int bpb = 4 * units;
    int nbBlocks = block_w * block_h;
    surf.assign((size_t)nbBlocks * bpb, 0);
    bpb_out = bpb;

    bool has_A = (ff & 0x280) || fmt_enum == 28;
    bool has_BC = (ff & 0x210) != 0;
    int offA = 0, offB = 4 * (dxtl + a2), offC = 4 * (dxtl + a2 + 1);

    std::vector<uint32_t> bmA((nbBlocks + 31) >> 5, 0), bmB((nbBlocks + 31) >> 5, 0);
    bool is256 = (block_w == 64 && block_h == 64);
    bool do_terrain = (flags & 0x10) && is256 && (fmt_enum == 23 || fmt_enum == 24);

    const uint32_t BORDERS = 0xC0000003u;
    if (do_terrain)
        for (int i = 0; i < nbBlocks; ++i)
            if (((1u << (i & 0x3F)) & BORDERS) || ((1u << (i >> 6)) & BORDERS)) { bs(bmA, i); bs(bmB, i); }

    uint8_t hlen[64], hval[64];
    huff_tables(hlen, hval);
    BitReader br(buf, payload_start, payload_end);

    // ---- constant-fill (bit-stream) passes
    if ((flags & 0x01) && (ff & 0x210) && !(ff & 0x280) && fmt_enum != 28) {
        uint8_t val[8]; std::memset(val, 0xFF, 8);
        rle_fill(br, hlen, hval, surf, nbBlocks, bpb, offB, val, 8, bmB, &bmA, &bmB, false);
    }
    if ((flags & 0x02) && (fmt_enum == 23 || fmt_enum == 24)) {
        int nib = (int)br.read(4);
        uint8_t b = (uint8_t)((nib | (nib << 4)) & 0xFF);
        uint8_t val[8]; std::memset(val, b, 8);
        rle_fill(br, hlen, hval, surf, nbBlocks, bpb, offA, val, 8, bmB, &bmA, nullptr, true);
    }
    if ((flags & 0x04) && (fmt_enum >= 25 && fmt_enum <= 28)) {
        uint8_t a = (uint8_t)br.read(8);
        uint8_t val[8] = { a, a, 0, 0, 0, 0, 0, 0 };
        rle_fill(br, hlen, hval, surf, nbBlocks, bpb, offA, val, 8, bmB, &bmA, nullptr, true);
    }
    if ((flags & 0x08) && has_BC) {
        uint32_t rgb = (uint32_t)br.read(24);
        auto val = encode_bc1_color(rgb, fmt_enum == 22);
        rle_fill(br, hlen, hval, surf, nbBlocks, bpb, offB, val.data(), 8, bmB, &bmB, nullptr, false);
    }

    // ---- byte-aligned plane reads
    br.align_to_word();
    size_t pos = br.pos();
    auto copy = [&](int dst, int nn) {
        size_t avail = 0;
        if (pos < payload_end) avail = std::min((size_t)nn, payload_end - pos);
        if (avail) std::memcpy(surf.data() + dst, buf + pos, avail);
        pos += nn;
    };
    if (has_A)
        for (int i = 0; i < nbBlocks; ++i)
            if (!bt(bmA, i)) copy(i * bpb + offA, 8);
    if (has_BC) {
        for (int i = 0; i < nbBlocks; ++i)
            if (!bt(bmB, i)) copy(i * bpb + offB, 4);
        for (int i = 0; i < nbBlocks; ++i)
            if (!bt(bmB, i)) copy(i * bpb + offC, 4);
    }
    if (do_terrain) terrain_mirror(surf.data(), nbBlocks);
}

// ---------------------------------------------------------------------------
//  Parse
// ---------------------------------------------------------------------------
inline Texture parse(const uint8_t* data, size_t n) {
    if (n < 12) throw std::runtime_error("ATEX: too short");
    static const char* MAG[] = { "ATEX","ATTX","ATEC","ATEP","ATEU","ATET" };
    bool ok = false;
    for (auto* m : MAG) if (!std::memcmp(data, m, 4)) ok = true;
    if (!ok) throw std::runtime_error("ATEX: bad magic");
    int fe = fourcc_to_enum(data + 4);
    if (fe < 0) throw std::runtime_error("ATEX: unsupported fourCC");

    Texture t{};
    std::memcpy(t.magic, data, 4);
    std::memcpy(t.fourcc, data + 4, 4);
    t.fmt_enum = fe;
    t.fmt_name = format_name(fe);
    t.width = data[8] | (data[9] << 8);
    t.height = data[10] | (data[11] << 8);

    size_t pos = 12;
    int w = t.width, h = t.height, level = 0;
    bool block = FORMAT_FLAGS[fe] & 1;
    while (pos + 8 <= n && w >= 1 && h >= 1) {
        uint32_t dataSize, flags;
        std::memcpy(&dataSize, data + pos, 4);
        std::memcpy(&flags, data + pos + 4, 4);
        if (dataSize < 8) break;
        size_t pstart = pos + 8, pend = std::min(pos + dataSize, n);
        int bw = block ? (w + 3) >> 2 : w;
        int bh = block ? (h + 3) >> 2 : h;
        Mip mip{};
        mip.level = level; mip.width = w; mip.height = h;
        mip.block_w = bw; mip.block_h = bh; mip.flags = flags; mip.raw = (flags == 0);
        inflate_mip(data, pstart, pend, flags, fe, bw, bh, mip.surface, mip.bytes_per_block);
        t.mips.push_back(std::move(mip));
        pos += dataSize; ++level;
        w = w > 1 ? w >> 1 : 1;
        h = h > 1 ? h >> 1 : 1;
    }
    return t;
}

// ===========================================================================
//  Block decoders
// ===========================================================================
namespace detail {

inline void expand565(int c, int& r, int& g, int& b) {
    int rr = (c >> 11) & 0x1F, gg = (c >> 5) & 0x3F, bb = c & 0x1F;
    r = (rr << 3) | (rr >> 2); g = (gg << 2) | (gg >> 4); b = (bb << 3) | (bb >> 2);
}

// BC1 colour -> 16 RGBA
inline void bc1_colors(const uint8_t* blk, int off, bool dxt1_alpha, uint8_t out[16][4]) {
    int c0 = blk[off] | (blk[off + 1] << 8);
    int c1 = blk[off + 2] | (blk[off + 3] << 8);
    uint32_t bits;
    std::memcpy(&bits, blk + off + 4, 4);
    int r0, g0, b0, r1, g1, b1;
    expand565(c0, r0, g0, b0); expand565(c1, r1, g1, b1);
    uint8_t pal[4][4];
    pal[0][0] = r0; pal[0][1] = g0; pal[0][2] = b0; pal[0][3] = 255;
    pal[1][0] = r1; pal[1][1] = g1; pal[1][2] = b1; pal[1][3] = 255;
    if (c0 > c1 || !dxt1_alpha) {
        pal[2][0] = (2 * r0 + r1) / 3; pal[2][1] = (2 * g0 + g1) / 3; pal[2][2] = (2 * b0 + b1) / 3; pal[2][3] = 255;
        pal[3][0] = (r0 + 2 * r1) / 3; pal[3][1] = (g0 + 2 * g1) / 3; pal[3][2] = (b0 + 2 * b1) / 3; pal[3][3] = 255;
    } else {
        pal[2][0] = (r0 + r1) / 2; pal[2][1] = (g0 + g1) / 2; pal[2][2] = (b0 + b1) / 2; pal[2][3] = 255;
        pal[3][0] = 0; pal[3][1] = 0; pal[3][2] = 0; pal[3][3] = 0;
    }
    for (int k = 0; k < 16; ++k) {
        int idx = (bits >> (2 * k)) & 3;
        for (int j = 0; j < 4; ++j) out[k][j] = pal[idx][j];
    }
}

// BC3 interpolated alpha (also BC4) -> 16 values
inline void bc3_alpha(const uint8_t* blk, int off, uint8_t out[16]) {
    int a0 = blk[off], a1 = blk[off + 1];
    int lut[8] = { a0, a1, 0, 0, 0, 0, 0, 0 };
    if (a0 > a1) for (int i = 1; i < 7; ++i) lut[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    else { for (int i = 1; i < 5; ++i) lut[i + 1] = ((5 - i) * a0 + i * a1) / 5; lut[6] = 0; lut[7] = 255; }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= (uint64_t)blk[off + 2 + i] << (8 * i);
    for (int k = 0; k < 16; ++k) out[k] = (uint8_t)lut[(bits >> (3 * k)) & 7];
}

// BC2 explicit 4-bit alpha -> 16 values
inline void bc2_alpha(const uint8_t* blk, int off, uint8_t out[16]) {
    for (int k = 0; k < 16; ++k) {
        int a = (blk[off + (k >> 1)] >> ((k & 1) * 4)) & 0xF;
        out[k] = (a << 4) | a;
    }
}

inline void place(Image& im, int bx, int by, const uint8_t px[16][4]) {
    int x0 = bx * 4, y0 = by * 4;
    for (int j = 0; j < 4; ++j) {
        int y = y0 + j; if (y >= im.height) break;
        for (int i = 0; i < 4; ++i) {
            int x = x0 + i; if (x >= im.width) break;
            uint8_t* d = &im.rgba[(size_t)(y * im.width + x) * 4];
            const uint8_t* s = px[j * 4 + i];
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }
}

// ---- BC7 -----------------------------------------------------------------
struct Bc7Mode { int ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2; };
inline const Bc7Mode BC7_MODES[8] = {
    {3,4,0,0,4,0,1,0,3,0},{2,6,0,0,6,0,0,1,3,0},{3,6,0,0,5,0,0,0,2,0},
    {2,6,0,0,7,0,1,0,2,0},{1,0,2,1,5,6,0,0,2,3},{1,0,2,0,7,8,0,0,2,2},
    {1,0,0,0,7,7,1,0,4,0},{2,6,0,0,5,5,1,0,2,0} };
inline const uint16_t BC7_P2[64] = {
    0xCCCC,0x8888,0xEEEE,0xECC8,0xC880,0xFEEC,0xFEC8,0xEC80,0xC800,0xFFEC,
    0xFE80,0xE800,0xFFE8,0xFF00,0xFFF0,0xF000,0xF710,0x008E,0x7100,0x08CE,
    0x008C,0x7310,0x3100,0x8CCE,0x088C,0x3110,0x6666,0x366C,0x17E8,0x0FF0,
    0x718E,0x399C,0xAAAA,0xF0F0,0x5A5A,0x33CC,0x3C3C,0x55AA,0x9696,0xA55A,
    0x73CE,0x13C8,0x324C,0x3BDC,0x6996,0xC33C,0x9966,0x0660,0x0272,0x04E4,
    0x4E40,0x2720,0xC936,0x936C,0x39C6,0x639C,0x9336,0x9CC6,0x817E,0xE718,
    0xCCF0,0x0FCC,0x7744,0xEE22 };
inline const uint32_t BC7_P3[64] = {
    0xAA685050,0x6A5A5040,0x5A5A4200,0x5450A0A8,0xA5A50000,0xA0A05050,0x5555A0A0,
    0x5A5A5050,0xAA550000,0xAA555500,0xAAAA5500,0x90909090,0x94949494,0xA4A4A4A4,
    0xA9A59450,0x2A0A4250,0xA5945040,0x0A425054,0xA5A5A500,0x55A0A0A0,0xA8A85454,
    0x6A6A4040,0xA4A45000,0x1A1A0500,0x0050A4A4,0xAAA59090,0x14696914,0x69691400,
    0xA08585A0,0xAA821414,0x50A4A450,0x6A5A0200,0xA9A58000,0x5090A0A8,0xA8A09050,
    0x24242424,0x00AA5500,0x24924924,0x24499224,0x50A50A50,0x500AA550,0xAAAA4444,
    0x66660000,0xA5A0A5A0,0x50A050A0,0x69286928,0x44AAAA44,0x66666600,0xAA444444,
    0x54A854A8,0x95809580,0x96969600,0xA85454A8,0x80959580,0xAA141414,0x96960000,
    0xAAAA1414,0xA05050A0,0xA0A5A5A0,0x96000000,0x40804080,0xA9A8A9A8,0xAAAAAA44,
    0x2A4A5254 };
inline const uint8_t BC7_A2[64] = {
    15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,2,8,2,2,8,8,15,2,8,2,2,8,8,2,2,
    15,15,6,8,2,8,15,15,2,8,2,2,2,15,15,6,6,2,6,8,15,15,2,2,15,15,15,15,15,2,2,15 };
inline const uint8_t BC7_A3a[64] = {
    3,3,15,15,8,3,15,15,8,8,6,6,6,5,3,3,3,3,8,15,3,3,6,10,5,8,8,6,8,5,15,15,
    8,15,3,5,6,10,8,15,15,3,15,5,15,15,15,15,3,15,5,5,5,8,5,10,5,10,8,13,15,12,3,3 };
inline const uint8_t BC7_A3b[64] = {
    15,8,8,3,15,15,3,8,15,15,15,15,15,15,15,8,15,8,15,3,15,8,15,8,3,15,6,10,15,15,10,8,
    15,3,15,10,10,8,9,10,6,15,8,15,3,6,6,8,15,3,15,15,15,15,15,15,15,15,15,15,3,15,15,8 };
inline int bc7_weight(int bits, int i) {
    static const int w2[] = {0,21,43,64};
    static const int w3[] = {0,9,18,27,37,46,55,64};
    static const int w4[] = {0,4,9,13,17,21,26,30,34,38,43,47,51,55,60,64};
    return bits == 2 ? w2[i] : bits == 3 ? w3[i] : w4[i];
}
inline int bc7_interp(int e0, int e1, int idx, int bits) {
    int w = bc7_weight(bits, idx);
    return (e0 * (64 - w) + e1 * w + 32) >> 6;
}
struct Bc7Bits {
    const uint8_t* d; int pos = 0;
    int get(int n) { int v = 0; for (int i = 0; i < n; ++i) { v |= ((d[pos >> 3] >> (pos & 7)) & 1) << i; ++pos; } return v; }
};
inline void decode_bc7_block(const uint8_t* blk, uint8_t out[16][4]) {
    Bc7Bits bs{ blk };
    int mode = 0;
    while (mode < 8 && bs.get(1) == 0) ++mode;
    if (mode == 8) { for (int k = 0; k < 16; ++k) { out[k][0] = out[k][1] = out[k][2] = 0; out[k][3] = 255; } return; }
    const Bc7Mode& m = BC7_MODES[mode];
    int ns = m.ns, ne = ns * 2;
    int part = m.pb ? bs.get(m.pb) : 0;
    int rot = m.rb ? bs.get(m.rb) : 0;
    int isb = m.isb ? bs.get(m.isb) : 0;
    int cb = m.cb, ab = m.ab;
    int r[6], g[6], b[6], a[6];
    for (int i = 0; i < ne; ++i) r[i] = bs.get(cb);
    for (int i = 0; i < ne; ++i) g[i] = bs.get(cb);
    for (int i = 0; i < ne; ++i) b[i] = bs.get(cb);
    for (int i = 0; i < ne; ++i) a[i] = ab ? bs.get(ab) : 255;
    if (m.epb) {
        int pbv[6]; for (int i = 0; i < ne; ++i) pbv[i] = bs.get(1);
        for (int i = 0; i < ne; ++i) { r[i] = (r[i] << 1) | pbv[i]; g[i] = (g[i] << 1) | pbv[i]; b[i] = (b[i] << 1) | pbv[i]; if (ab) a[i] = (a[i] << 1) | pbv[i]; }
        cb++; if (ab) ab++;
    } else if (m.spb) {
        int sp[2] = { bs.get(1), bs.get(1) };
        for (int i = 0; i < ne; ++i) { int s = sp[i / (ne / 2)]; r[i] = (r[i] << 1) | s; g[i] = (g[i] << 1) | s; b[i] = (b[i] << 1) | s; if (ab) a[i] = (a[i] << 1) | s; }
        cb++; if (ab) ab++;
    }
    auto sc = [](int v, int bits) { v <<= (8 - bits); return v | (v >> bits); };
    for (int i = 0; i < ne; ++i) { r[i] = sc(r[i], cb); g[i] = sc(g[i], cb); b[i] = sc(b[i], cb); a[i] = ab ? sc(a[i], ab) : 255; }

    int ib = m.ib, ib2 = m.ib2;
    int parts[16], anchors[3] = { 0, 0, 0 };
    if (ns == 1) { for (int k = 0; k < 16; ++k) parts[k] = 0; }
    else if (ns == 2) { for (int k = 0; k < 16; ++k) parts[k] = (BC7_P2[part] >> k) & 1; anchors[1] = BC7_A2[part]; }
    else { for (int k = 0; k < 16; ++k) parts[k] = (BC7_P3[part] >> (2 * k)) & 3; anchors[1] = BC7_A3a[part]; anchors[2] = BC7_A3b[part]; }

    int idx1[16], idx2[16]; bool has2 = ib2 != 0;
    for (int k = 0; k < 16; ++k) {
        bool anc = false;
        for (int s = 0; s < ns; ++s) if (k == anchors[s]) anc = true;
        idx1[k] = bs.get(anc ? ib - 1 : ib);
    }
    if (has2) for (int k = 0; k < 16; ++k) idx2[k] = bs.get(k == 0 ? ib2 - 1 : ib2);

    for (int k = 0; k < 16; ++k) {
        int s = parts[k], e0 = s * 2, e1 = s * 2 + 1;
        int ci, cbits, ai, abits;
        if (!has2) { ci = ai = idx1[k]; cbits = abits = ib; }
        else if (isb == 0) { ci = idx1[k]; cbits = ib; ai = idx2[k]; abits = ib2; }
        else { ci = idx2[k]; cbits = ib2; ai = idx1[k]; abits = ib; }
        int cr = bc7_interp(r[e0], r[e1], ci, cbits);
        int cg = bc7_interp(g[e0], g[e1], ci, cbits);
        int cbl = bc7_interp(b[e0], b[e1], ci, cbits);
        int ca = m.ab ? bc7_interp(a[e0], a[e1], ai, abits) : 255;
        if (rot == 1) { int t = ca; ca = cr; cr = t; }
        else if (rot == 2) { int t = ca; ca = cg; cg = t; }
        else if (rot == 3) { int t = ca; ca = cbl; cbl = t; }
        out[k][0] = (uint8_t)cr; out[k][1] = (uint8_t)cg; out[k][2] = (uint8_t)cbl; out[k][3] = (uint8_t)ca;
    }
}

} // namespace detail

// ---------------------------------------------------------------------------
//  Surface -> RGBA
// ---------------------------------------------------------------------------
inline Image decode_surface(int fmt_enum, const uint8_t* surf, size_t /*surf_n*/, int W, int H) {
    using namespace detail;
    Image im; im.width = W; im.height = H; im.rgba.assign((size_t)W * H * 4, 0);
    int bw = (W + 3) / 4, bh = (H + 3) / 4;
    uint8_t px[16][4]; uint8_t c[16][4]; uint8_t al[16]; uint8_t ch0[16], ch1[16];

    for (int by = 0; by < bh; ++by) for (int bx = 0; bx < bw; ++bx) {
        int bi = by * bw + bx;
        switch (fmt_enum) {
        case 22: { // DXT1
            bc1_colors(surf, bi * 8, true, px); place(im, bx, by, px); break; }
        case 23: case 24: { // DXT2/3 (BC2)
            int o = bi * 16; bc1_colors(surf, o + 8, false, c); bc2_alpha(surf, o, al);
            for (int k = 0; k < 16; ++k) { px[k][0] = c[k][0]; px[k][1] = c[k][1]; px[k][2] = c[k][2]; px[k][3] = al[k]; }
            place(im, bx, by, px); break; }
        case 25: case 26: { // DXT4/5 (BC3)
            int o = bi * 16; bc1_colors(surf, o + 8, false, c); bc3_alpha(surf, o, al);
            for (int k = 0; k < 16; ++k) { px[k][0] = c[k][0]; px[k][1] = c[k][1]; px[k][2] = c[k][2]; px[k][3] = al[k]; }
            place(im, bx, by, px); break; }
        case 27: { // DXTA
            bc1_colors(surf, bi * 8, false, c);
            for (int k = 0; k < 16; ++k) { px[k][0] = px[k][1] = px[k][2] = c[k][0]; px[k][3] = 255; }
            place(im, bx, by, px); break; }
        case 28: { // DXTL
            int o = bi * 16; bc1_colors(surf, o + 8, false, c); bc3_alpha(surf, o, al);
            for (int k = 0; k < 16; ++k) { px[k][0] = c[k][0] * al[k] / 255; px[k][1] = c[k][1] * al[k] / 255; px[k][2] = c[k][2] * al[k] / 255; px[k][3] = 255; }
            place(im, bx, by, px); break; }
        case 29: case 30: case 31: { // DXTN/3DCX/BC5
            int o = bi * 16; bc3_alpha(surf, o, ch0); bc3_alpha(surf, o + 8, ch1);
            for (int k = 0; k < 16; ++k) {
                double fx = ch0[k] / 127.5 - 1.0, fy = ch1[k] / 127.5 - 1.0, fz = 1.0 - fx * fx - fy * fy;
                int z = fz > 0 ? (int)((std::sqrt(fz) * 0.5 + 0.5) * 255) : 128;
                px[k][0] = ch0[k]; px[k][1] = ch1[k]; px[k][2] = (uint8_t)z; px[k][3] = 255;
            }
            place(im, bx, by, px); break; }
        case 32: { // BC7
            decode_bc7_block(surf + bi * 16, px); place(im, bx, by, px); break; }
        default: break;
        }
    }
    return im;
}

inline Image decode(const Texture& t, int level = 0) {
    const Mip& m = t.mips.at(level);
    return decode_surface(t.fmt_enum, m.surface.data(), m.surface.size(), m.width, m.height);
}

} // namespace gw2atex
#endif // GW2_ATEX_HPP
