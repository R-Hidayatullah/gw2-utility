#ifndef GW2_WIC_IMAGE_H
#define GW2_WIC_IMAGE_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace gw2wic {

struct WicImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba; // tightly packed, pitch == width * 4, DXGI_FORMAT_R8G8B8A8_UNORM order
};

// Decodes an in-memory PNG/JPEG/BMP/GIF/TIFF buffer via the Windows Imaging
// Component (wincodec.h) -- ships with Windows, no third-party library.
// Returns std::nullopt if WIC can't decode this buffer.
std::optional<WicImage> decode(const uint8_t* data, size_t size);

} // namespace gw2wic

#endif // GW2_WIC_IMAGE_H
