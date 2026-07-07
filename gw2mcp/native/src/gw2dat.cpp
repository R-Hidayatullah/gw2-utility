#include "gw2dat.h"

#include <algorithm>
#include <unordered_map>

namespace {

void parse_dat_header(Gw2Dat& data_gw2) {
    read_from_file(data_gw2.dat_file, data_gw2.dat_header.version);
    read_from_file(data_gw2.dat_file, data_gw2.dat_header.identifier);
    read_from_file(data_gw2.dat_file, data_gw2.dat_header.header_size);
    read_from_file(data_gw2.dat_file, data_gw2.dat_header.unknown_field);
    read_from_file(data_gw2.dat_file, data_gw2.dat_header.chunk_size);
    read_from_file(data_gw2.dat_file, data_gw2.dat_header.crc);
    read_from_file(data_gw2.dat_file, data_gw2.dat_header.unknown_field_2);
    read_from_file(data_gw2.dat_file, data_gw2.dat_header.mft_offset);
    read_from_file(data_gw2.dat_file, data_gw2.dat_header.mft_size);
    read_from_file(data_gw2.dat_file, data_gw2.dat_header.flag);

    if (!data_gw2.dat_file.seekg(static_cast<std::streamoff>(data_gw2.dat_header.mft_offset), std::ios::beg)) {
        throw std::runtime_error("Failed to seek into MFT offset.");
    }
}

void parse_mft_header(Gw2Dat& data_gw2) {
    read_from_file(data_gw2.dat_file, data_gw2.mft_header.identifier);
    read_from_file(data_gw2.dat_file, data_gw2.mft_header.unknown_field);
    read_from_file(data_gw2.dat_file, data_gw2.mft_header.mft_entry_size);
    read_from_file(data_gw2.dat_file, data_gw2.mft_header.unknown_field_2);
    read_from_file(data_gw2.dat_file, data_gw2.mft_header.unknown_field_3);
}

void parse_mft_data(Gw2Dat& data_gw2) {
    if (data_gw2.mft_header.mft_entry_size == 0) {
        throw std::runtime_error("MFT reports zero entries.");
    }

    const uint64_t entry_count = data_gw2.mft_header.mft_entry_size - 1;
    data_gw2.mft_data_list.resize(entry_count);

    for (uint64_t i = 0; i < entry_count; i++) {
        MftData& entry = data_gw2.mft_data_list[i];
        entry.original_index = i;
        read_from_file(data_gw2.dat_file, entry.offset);
        read_from_file(data_gw2.dat_file, entry.size);
        read_from_file(data_gw2.dat_file, entry.compression_flag);
        read_from_file(data_gw2.dat_file, entry.entry_flag);
        read_from_file(data_gw2.dat_file, entry.counter);
        read_from_file(data_gw2.dat_file, entry.crc);
    }

    // For entries stored uncompressed (flag == 0), the CRC-stripped size *is*
    // the final size, so it can be computed up front without decompressing.
    for (auto& entry : data_gw2.mft_data_list) {
        if (entry.compression_flag != 0) {
            continue;
        }

        uint32_t uncompressed_size = entry.size;
        if (entry.size > CHUNK_SIZE) {
            uncompressed_size -= ((entry.size % CHUNK_SIZE) + 1) * 4;
        } else {
            uncompressed_size -= 4;
        }
        entry.uncompressed_size = uncompressed_size;
    }
}

void parse_mft_file_id_data(Gw2Dat& data_gw2) {
    if (MFT_ENTRY_INDEX_NUM >= data_gw2.mft_data_list.size()) {
        throw std::runtime_error("MFT_ENTRY_INDEX_NUM is out of bounds.");
    }

    const MftData& index_entry = data_gw2.mft_data_list[MFT_ENTRY_INDEX_NUM];
    const size_t num_entries = index_entry.size / sizeof(MftFileIdData);
    data_gw2.mft_file_id_data_list.resize(num_entries);

    if (!data_gw2.dat_file.seekg(static_cast<std::streamoff>(index_entry.offset), std::ios::beg)) {
        throw std::runtime_error("Failed to seek to MFT file ID data offset.");
    }

    for (size_t i = 0; i < num_entries; i++) {
        read_from_file(data_gw2.dat_file, data_gw2.mft_file_id_data_list[i].file_id);
        read_from_file(data_gw2.dat_file, data_gw2.mft_file_id_data_list[i].base_id);
    }

    std::sort(data_gw2.mft_file_id_data_list.begin(), data_gw2.mft_file_id_data_list.end(),
              [](const MftFileIdData& a, const MftFileIdData& b) { return a.file_id < b.file_id; });

    std::unordered_map<uint32_t, std::vector<uint32_t>> base_to_files;
    for (const auto& entry : data_gw2.mft_file_id_data_list) {
        base_to_files[entry.base_id].push_back(entry.file_id);
    }

    data_gw2.mft_base_id_data_list.reserve(base_to_files.size());
    for (auto& [base_id, file_ids] : base_to_files) {
        data_gw2.mft_base_id_data_list.push_back(MftBaseIdData{std::move(file_ids), base_id});
    }

    std::sort(data_gw2.mft_base_id_data_list.begin(), data_gw2.mft_base_id_data_list.end(),
              [](const MftBaseIdData& a, const MftBaseIdData& b) { return a.base_id < b.base_id; });
}

} // namespace

void load_dat_file(Gw2Dat& data_gw2, const std::string& file_path) {
    data_gw2.dat_file.open(file_path, std::ios::binary | std::ios::ate);
    if (!data_gw2.dat_file.is_open()) {
        throw std::runtime_error("Failed to open file: " + file_path);
    }

    data_gw2.file_info.file_path = file_path;
    data_gw2.file_info.file_size = static_cast<uint64_t>(data_gw2.dat_file.tellg());
    data_gw2.dat_file.seekg(0, std::ios::beg);

    parse_dat_header(data_gw2);
    parse_mft_header(data_gw2);
    parse_mft_data(data_gw2);
    parse_mft_file_id_data(data_gw2);

    // Entry bytes are read on demand through their own independent handles
    // (see read_entry_bytes) -- nothing needs this open past this point, and
    // leaving a 40+ GB archive's handle open for the app's whole lifetime for
    // no reason is just asking for trouble if it ever needs to be replaced.
    data_gw2.dat_file.close();
}

std::vector<uint8_t> read_entry_bytes(const std::string& file_path, const MftData& entry) {
    if (entry.size == 0) {
        return {};
    }

    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + file_path);
    }

    std::vector<uint8_t> raw_data(entry.size);
    file.seekg(static_cast<std::streamoff>(entry.offset));
    file.read(reinterpret_cast<char*>(raw_data.data()), entry.size);

    if (file.gcount() != static_cast<std::streamsize>(entry.size)) {
        throw std::runtime_error("Failed to read full entry at offset: " + std::to_string(entry.offset));
    }

    return raw_data;
}

std::vector<uint8_t> extract_compressed_data(Gw2Dat& data_gw2, uint32_t mft_index) {
    if (mft_index >= data_gw2.mft_data_list.size()) {
        throw std::out_of_range("MFT index out of range: " + std::to_string(mft_index));
    }
    return read_entry_bytes(data_gw2.file_info.file_path, data_gw2.mft_data_list[mft_index]);
}

std::vector<uint32_t> search_by_base_id(Gw2Dat& data_gw2, uint32_t base_id) {
    std::vector<uint32_t> results;

    uint32_t divisor = 1;
    for (uint32_t temp = base_id; temp > 0; temp /= 10) {
        divisor *= 10;
    }

    for (const auto& entry : data_gw2.mft_base_id_data_list) {
        uint32_t num = entry.base_id;
        while (num >= base_id) {
            if (num % divisor == base_id) {
                results.push_back(entry.base_id);
                break;
            }
            num /= 10;
        }
    }

    std::sort(results.begin(), results.end());
    return results;
}

uint32_t get_by_base_id(Gw2Dat& data_gw2, uint32_t file_id) {
    for (const auto& entry : data_gw2.mft_file_id_data_list) {
        if (entry.file_id == file_id) {
            return entry.base_id;
        }
    }
    return 0;
}

std::vector<uint32_t> get_by_file_id(Gw2Dat& data_gw2, uint32_t base_id) {
    for (const auto& entry : data_gw2.mft_base_id_data_list) {
        if (entry.base_id == base_id) {
            std::vector<uint32_t> file_ids = entry.file_id;
            std::sort(file_ids.begin(), file_ids.end());
            return file_ids;
        }
    }
    return {};
}

std::vector<uint32_t> search_by_file_id(Gw2Dat& data_gw2, uint32_t file_id) {
    std::vector<uint32_t> results;

    uint32_t divisor = 1;
    for (uint32_t temp = file_id; temp > 0; temp /= 10) {
        divisor *= 10;
    }

    for (const auto& entry : data_gw2.mft_file_id_data_list) {
        uint32_t num = entry.file_id;
        while (num >= file_id) {
            if (num % divisor == file_id) {
                results.push_back(entry.file_id);
                break;
            }
            num /= 10;
        }
    }

    std::sort(results.begin(), results.end());
    return results;
}
