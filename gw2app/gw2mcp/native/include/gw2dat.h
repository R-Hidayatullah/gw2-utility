#ifndef GW2DAT_H
#define GW2DAT_H

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

constexpr size_t DAT_MAGIC_NUMBER = 3;
constexpr size_t MFT_MAGIC_NUMBER = 4;
constexpr size_t MFT_ENTRY_INDEX_NUM = 1;
constexpr size_t CHUNK_SIZE = 0x10000;

#pragma pack(push, 1)
struct DatHeader {
    uint8_t version;
    char identifier[DAT_MAGIC_NUMBER];
    uint32_t header_size;
    uint32_t unknown_field;
    uint32_t chunk_size;
    uint32_t crc;
    uint32_t unknown_field_2;
    uint64_t mft_offset;
    uint32_t mft_size;
    uint32_t flag;
};

struct MftHeader {
    char identifier[MFT_MAGIC_NUMBER];
    uint64_t unknown_field;
    uint32_t mft_entry_size;
    uint32_t unknown_field_2;
    uint32_t unknown_field_3;
};
#pragma pack(pop)

struct MftData {
    uint64_t original_index = 0;
    uint64_t offset = 0;
    uint32_t size = 0;
    uint16_t compression_flag = 0;
    uint16_t entry_flag = 0;
    uint32_t counter = 0;
    uint32_t crc = 0;
    uint32_t uncompressed_size = 0;
};

struct MftFileIdData {
    uint32_t file_id;
    uint32_t base_id;
};

struct MftBaseIdData {
    std::vector<uint32_t> file_id;
    uint32_t base_id;
};

struct FileInfo {
    std::string file_path;
    uint64_t file_size = 0;
};

struct Gw2Dat {
    std::ifstream dat_file;
    FileInfo file_info;
    DatHeader dat_header{};
    MftHeader mft_header{};
    std::vector<MftData> mft_data_list;
    std::vector<MftFileIdData> mft_file_id_data_list;
    std::vector<MftBaseIdData> mft_base_id_data_list;
};

template <typename T>
void read_from_file(std::ifstream& file, T& value) {
    if (!file.read(reinterpret_cast<char*>(&value), sizeof(T))) {
        throw std::runtime_error("Failed to read binary data from file.");
    }
}

template <typename T, size_t N>
void read_from_file(std::ifstream& file, T (&array)[N]) {
    if (!file.read(reinterpret_cast<char*>(array), sizeof(T) * N)) {
        throw std::runtime_error("Failed to read binary array from file.");
    }
}

// Parses the whole .dat (header + MFT). Only reads the header/MFT region;
// entry bytes are read on demand via read_entry_bytes() below, each through
// its own independent file handle, so this doesn't keep data_gw2.dat_file
// open (safe to call from multiple threads concurrently on the same archive,
// and safe to reload/replace a Gw2Dat while background reads of the old one
// are still in flight). Throws std::runtime_error on any parse failure.
void load_dat_file(Gw2Dat& data_gw2, const std::string& file_path);

// Reads the raw, still CRC32C-laced bytes for one MFT entry straight off
// disk, via a fresh file handle scoped to this call -- thread-safe to call
// concurrently (including from a background thread) as long as `entry` and
// `file_path` outlive the call, which is easiest to guarantee by copying
// them (both are cheap: a path string and one small POD struct).
std::vector<uint8_t> read_entry_bytes(const std::string& file_path, const MftData& entry);

// Convenience wrapper: looks up data_gw2.mft_data_list[mft_index] and calls
// read_entry_bytes(). Only safe to call from the thread that owns data_gw2
// (no synchronization -- for cross-thread use, copy the MftData entry and
// call read_entry_bytes() directly instead, see entry_extractor.h).
std::vector<uint8_t> extract_compressed_data(Gw2Dat& data_gw2, uint32_t mft_index);

std::vector<uint32_t> search_by_base_id(Gw2Dat& data_gw2, uint32_t base_id);
uint32_t get_by_base_id(Gw2Dat& data_gw2, uint32_t file_id);
std::vector<uint32_t> get_by_file_id(Gw2Dat& data_gw2, uint32_t base_id);
std::vector<uint32_t> search_by_file_id(Gw2Dat& data_gw2, uint32_t file_id);

#endif // GW2DAT_H
