#include <cstdio>
#include <fstream>
#include <iostream>
#include <vector>

#include "cmp_decompress_method0.hpp"

static std::vector<uint8_t> read_file(const char *path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::perror("open"); std::exit(1); }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

int main(int argc, char **argv) {
    const char *in_path = argc > 1 ? argv[1] : "compressed_16.bin";
    const char *out_path = argc > 2 ? argv[2] : "decompressed_cpp20.txt";

    std::vector<uint8_t> raw = read_file(in_path);
    std::cout << "input: " << in_path << " (" << raw.size() << " bytes)\n";

    try {
        std::vector<uint8_t> result = gw2cmp::decompress_entry(raw);
        std::cout << "decompressed: " << result.size() << " bytes\n";

        std::ofstream out(out_path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(result.data()), static_cast<std::streamsize>(result.size()));
        std::cout << "written to: " << out_path << "\n";
    } catch (const gw2cmp::decode_error &e) {
        std::cerr << "decompress failed: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
