#pragma once
#include <cstdint>
#include <vector>

// Renders a classic hex-dump: offset | hex bytes | ascii, with an optional
// highlighted byte range (driven by TreeView selection).
class HexViewer {
public:
    void draw(const char* childId, const std::vector<uint8_t>& data, size_t selOffset, size_t selSize);

    int bytesPerRow = 16;
};
