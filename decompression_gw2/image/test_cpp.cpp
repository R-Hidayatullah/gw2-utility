// test harness: decode an ATEX file with the C++20 header, dump mip0 RGBA to stdout.
#include "gw2_atex.hpp"
#include <cstdio>
#include <vector>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s in.bin out.raw\n", argv[0]); return 2; }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::perror("open"); return 1; }
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(n);
    if (std::fread(data.data(), 1, n, f) != (size_t)n) { return 1; }
    std::fclose(f);

    try {
        auto tex = gw2atex::parse(data.data(), data.size());
        auto img = gw2atex::decode(tex, 0);
        std::fprintf(stderr, "%s fmt=%s %dx%d mips=%zu\n", argv[1],
                     tex.fmt_name.c_str(), tex.width, tex.height, tex.mips.size());
        FILE* o = std::fopen(argv[2], "wb");
        std::fwrite(img.rgba.data(), 1, img.rgba.size(), o);
        std::fclose(o);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
