#include "dds.h"

#include <cstring>
#include <dxgiformat.h>

namespace gw2dds {

namespace {

constexpr uint32_t FOURCC(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

constexpr uint32_t DDS_MAGIC = FOURCC('D', 'D', 'S', ' ');
constexpr uint32_t FOURCC_DX10 = FOURCC('D', 'X', '1', '0');
constexpr uint32_t FOURCC_DXT1 = FOURCC('D', 'X', 'T', '1');
constexpr uint32_t FOURCC_DXT3 = FOURCC('D', 'X', 'T', '3');
constexpr uint32_t FOURCC_DXT5 = FOURCC('D', 'X', 'T', '5');
constexpr uint32_t FOURCC_ATI1 = FOURCC('A', 'T', 'I', '1');
constexpr uint32_t FOURCC_ATI2 = FOURCC('A', 'T', 'I', '2');

constexpr uint32_t DDPF_ALPHAPIXELS = 0x1;
constexpr uint32_t DDPF_ALPHA = 0x2;
constexpr uint32_t DDPF_FOURCC = 0x4;
constexpr uint32_t DDPF_RGB = 0x40;
constexpr uint32_t DDPF_LUMINANCE = 0x20000;
constexpr uint32_t D3D10_RESOURCE_DIMENSION_TEXTURE2D = 3;

#pragma pack(push, 1)
struct DdsPixelFormat {
    uint32_t size = 32;
    uint32_t flags = 0;
    uint32_t four_cc = 0;
    uint32_t rgb_bit_count = 0;
    uint32_t r_bit_mask = 0;
    uint32_t g_bit_mask = 0;
    uint32_t b_bit_mask = 0;
    uint32_t a_bit_mask = 0;
};

struct DdsHeader {
    uint32_t size = 124;
    uint32_t flags = 0;
    uint32_t height = 0;
    uint32_t width = 0;
    uint32_t pitch_or_linear_size = 0;
    uint32_t depth = 0;
    uint32_t mip_map_count = 0;
    uint32_t reserved1[11] = {};
    DdsPixelFormat pixel_format;
    uint32_t caps = 0;
    uint32_t caps2 = 0;
    uint32_t caps3 = 0;
    uint32_t caps4 = 0;
    uint32_t reserved2 = 0;
};

struct DdsHeaderDxt10 {
    uint32_t dxgi_format = 0;
    uint32_t resource_dimension = D3D10_RESOURCE_DIMENSION_TEXTURE2D;
    uint32_t misc_flag = 0;
    uint32_t array_size = 1;
    uint32_t misc_flags2 = 0;
};
#pragma pack(pop)

// A block-compression family the DDS parser recognizes.
struct BcFormat {
    uint32_t block_bytes;
    uint32_t classic_four_cc; // 0 if this format requires the DX10 header instead
    uint32_t dxgi_format;
};

std::optional<BcFormat> bc_format_from_dxgi(uint32_t dxgi_format) {
    switch (dxgi_format) {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
        return BcFormat{8, FOURCC_DXT1, dxgi_format};
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
        return BcFormat{16, FOURCC_DXT3, dxgi_format};
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
        return BcFormat{16, FOURCC_DXT5, dxgi_format};
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        return BcFormat{8, FOURCC_ATI1, dxgi_format};
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
        return BcFormat{16, FOURCC_ATI2, dxgi_format};
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return BcFormat{16, 0, dxgi_format};
    default:
        return std::nullopt;
    }
}

std::optional<uint32_t> dxgi_from_classic_four_cc(uint32_t four_cc) {
    if (four_cc == FOURCC_DXT1) return DXGI_FORMAT_BC1_UNORM;
    if (four_cc == FOURCC('D', 'X', 'T', '2')) return DXGI_FORMAT_BC2_UNORM;
    if (four_cc == FOURCC_DXT3) return DXGI_FORMAT_BC2_UNORM;
    if (four_cc == FOURCC('D', 'X', 'T', '4')) return DXGI_FORMAT_BC3_UNORM;
    if (four_cc == FOURCC_DXT5) return DXGI_FORMAT_BC3_UNORM;
    if (four_cc == FOURCC_ATI1 || four_cc == FOURCC('B', 'C', '4', 'U')) return DXGI_FORMAT_BC4_UNORM;
    if (four_cc == FOURCC_ATI2 || four_cc == FOURCC('B', 'C', '5', 'U')) return DXGI_FORMAT_BC5_UNORM;
    return std::nullopt;
}

uint32_t blocks_across(uint32_t extent) { return (extent + 3) / 4; }

// An uncompressed (non-block) pixel layout the parser can hand straight to the
// GPU: its DXGI format + bytes per pixel (for the row pitch).
struct UncFormat {
    uint32_t dxgi_format;
    uint32_t bytes_per_pixel;
};

// Recognize the common uncompressed DDS layouts from the legacy pixel-format
// masks. GW2 ships plain 32-bit BGRA/RGBA DDS textures (no FourCC) for some
// assets; those were previously rejected. The GPU handles the B/R swizzle from
// the DXGI format, so no CPU conversion is needed.
std::optional<UncFormat> uncompressed_from_pixel_format(const DdsPixelFormat& pf) {
    const bool rgb = (pf.flags & DDPF_RGB) != 0;
    const bool lum = (pf.flags & DDPF_LUMINANCE) != 0;
    const bool alphaOnly = (pf.flags & DDPF_ALPHA) != 0;
    if (pf.rgb_bit_count == 32 && (rgb || (pf.r_bit_mask | pf.g_bit_mask | pf.b_bit_mask))) {
        // BGRA (r mask in the high bytes) vs RGBA.
        if (pf.b_bit_mask == 0x000000FFu && pf.r_bit_mask == 0x00FF0000u)
            return UncFormat{DXGI_FORMAT_B8G8R8A8_UNORM, 4};
        if (pf.r_bit_mask == 0x000000FFu && pf.b_bit_mask == 0x00FF0000u)
            return UncFormat{DXGI_FORMAT_R8G8B8A8_UNORM, 4};
        // A2B10G10R10 / A2R10G10B10 (10-bit) -> R10G10B10A2.
        if (pf.rgb_bit_count == 32 && pf.a_bit_mask == 0xC0000000u)
            return UncFormat{DXGI_FORMAT_R10G10B10A2_UNORM, 4};
        return UncFormat{DXGI_FORMAT_B8G8R8A8_UNORM, 4}; // BGRX / unknown 32bpp
    }
    if (pf.rgb_bit_count == 16 && (rgb || pf.r_bit_mask)) {
        if (pf.a_bit_mask == 0xF000u) return UncFormat{DXGI_FORMAT_B4G4R4A4_UNORM, 2}; // 4444
        if (pf.a_bit_mask == 0x8000u) return UncFormat{DXGI_FORMAT_B5G5R5A1_UNORM, 2}; // 1555
        return UncFormat{DXGI_FORMAT_B5G6R5_UNORM, 2};                                  // 565
    }
    if (pf.rgb_bit_count == 8 && (lum || alphaOnly || pf.r_bit_mask == 0xFFu || pf.a_bit_mask == 0xFFu)) {
        return UncFormat{DXGI_FORMAT_R8_UNORM, 1}; // grayscale/alpha (samples as red; acceptable preview)
    }
    return std::nullopt;
}

// Bytes per pixel for the uncompressed DXGI formats a DX10-header DDS may use.
std::optional<uint32_t> uncompressed_bpp_from_dxgi(uint32_t fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return 4;
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_B4G4R4A4_UNORM:
        return 2;
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_A8_UNORM:
        return 1;
    default:
        return std::nullopt;
    }
}

} // namespace

std::optional<DdsInfo> parse_dds(const uint8_t* data, size_t size) {
    if (data == nullptr || size < sizeof(uint32_t) + sizeof(DdsHeader)) {
        return std::nullopt;
    }

    uint32_t magic = 0;
    std::memcpy(&magic, data, sizeof(magic));
    if (magic != DDS_MAGIC) {
        return std::nullopt;
    }

    DdsHeader header;
    std::memcpy(&header, data + sizeof(magic), sizeof(header));
    size_t offset = sizeof(magic) + sizeof(header);

    if (header.width == 0 || header.height == 0) {
        return std::nullopt;
    }

    std::optional<BcFormat> bc_format;
    std::optional<UncFormat> unc_format;
    if (header.pixel_format.four_cc == FOURCC_DX10) {
        if (size < offset + sizeof(DdsHeaderDxt10)) {
            return std::nullopt;
        }
        DdsHeaderDxt10 dxt10;
        std::memcpy(&dxt10, data + offset, sizeof(dxt10));
        offset += sizeof(dxt10);
        bc_format = bc_format_from_dxgi(dxt10.dxgi_format);
        if (!bc_format) {
            if (auto bpp = uncompressed_bpp_from_dxgi(dxt10.dxgi_format))
                unc_format = UncFormat{dxt10.dxgi_format, *bpp};
        }
    } else if (header.pixel_format.flags & DDPF_FOURCC) {
        auto dxgi_format = dxgi_from_classic_four_cc(header.pixel_format.four_cc);
        if (dxgi_format) {
            bc_format = bc_format_from_dxgi(*dxgi_format);
        }
    } else {
        // No FourCC: a legacy uncompressed layout described by the masks.
        unc_format = uncompressed_from_pixel_format(header.pixel_format);
    }

    DdsInfo info;
    info.width = header.width;
    info.height = header.height;
    info.data_offset = offset;
    if (bc_format) {
        info.dxgi_format = bc_format->dxgi_format;
        info.sys_mem_pitch = blocks_across(header.width) * bc_format->block_bytes;
        return info;
    }
    if (unc_format) {
        info.dxgi_format = unc_format->dxgi_format;
        info.sys_mem_pitch = header.width * unc_format->bytes_per_pixel;
        return info;
    }
    return std::nullopt;
}

} // namespace gw2dds
