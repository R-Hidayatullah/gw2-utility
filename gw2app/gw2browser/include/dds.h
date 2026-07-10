#ifndef GW2_DDS_H
#define GW2_DDS_H

#include <cstddef>
#include <cstdint>
#include <optional>

namespace gw2dds {

// Just enough info to hand a decoded block off to Direct3D 11: the texture
// dimensions actually used by the GPU, the DXGI format, the byte offset of
// pixel data inside the buffer, and the D3D11 SysMemPitch (bytes per row of
// blocks -- NOT the same as a DDS file's dwPitchOrLinearSize field, which for
// block-compressed textures holds the *total* mip size instead).
struct DdsInfo {
    uint32_t dxgi_format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t sys_mem_pitch = 0;
    size_t data_offset = 0;
};

// Parses a standalone "DDS " file buffer (a real DDS stored directly in the
// archive) into a DdsInfo, so its native BCn blocks can be uploaded straight
// to the GPU. Returns std::nullopt if the buffer isn't a recognized BC1-BC7 DDS.
std::optional<DdsInfo> parse_dds(const uint8_t* data, size_t size);

} // namespace gw2dds

#endif // GW2_DDS_H
