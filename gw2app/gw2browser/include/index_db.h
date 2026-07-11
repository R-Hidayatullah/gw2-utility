#ifndef INDEX_DB_H
#define INDEX_DB_H

// Read-only accessor for a gw2index / gw2local SQLite index (see gw2index-tool).
// Lets the browser navigate/filter a huge .dat by pre-computed type/container/
// chunk metadata without re-scanning. Preview still extracts from the .dat.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gw2idx {

bool open(const std::wstring& db_path, std::string& err);
void close();
bool is_open();

std::wstring dat_path();      // meta.dat_path (the archive this index was built from)
size_t       entry_count();

std::vector<std::string> types();       // distinct entries.type,      sorted
std::vector<std::string> containers();  // distinct non-empty container, sorted

// Streams every (base_id, type, container) row so the caller can build an
// in-memory lookup for the list columns (one query, no per-row round trips).
void load_meta_map(const std::function<void(uint32_t base_id, const char* type, const char* container)>& cb);

// Filtered base_ids. type/container empty = any. If id_active, restrict to the
// given id (by base_id, or by file_id when id_by_fileid). Capped at `limit`.
std::vector<uint32_t> query_base_ids(const std::string& type, const std::string& container,
                                     uint32_t id_value, bool id_by_fileid, bool id_active, int limit);

struct EntryInfo {
    bool found = false;
    std::string type, container, error, magic;
    long long size = 0, size_stripped = 0, size_final = 0;
    int comp_flag = 0, actually_compressed = 0;
    std::vector<std::string> chunks;  // "fourcc v# (variant)"
};
EntryInfo lookup(uint32_t base_id);

} // namespace gw2idx

#endif // INDEX_DB_H
