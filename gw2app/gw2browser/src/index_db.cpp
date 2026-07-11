#include "index_db.h"

#include "sqlite3.h"

#include <cstdio>
#include <cstring>

namespace gw2idx {
namespace {

sqlite3* g_db = nullptr;

const char* col_text(sqlite3_stmt* st, int i) {
    const unsigned char* t = sqlite3_column_text(st, i);
    return t ? reinterpret_cast<const char*>(t) : "";
}

std::vector<std::string> distinct(const char* col) {
    std::vector<std::string> out;
    if (!g_db) return out;
    char sql[128];
    std::snprintf(sql, sizeof sql,
                  "SELECT DISTINCT %s FROM entries WHERE %s IS NOT NULL AND %s != '' ORDER BY %s",
                  col, col, col, col);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) out.emplace_back(col_text(st, 0));
    }
    sqlite3_finalize(st);
    return out;
}

} // namespace

bool open(const std::wstring& db_path, std::string& err) {
    close();
    // sqlite3_open16 takes a UTF-16 path (wchar_t is UTF-16 on Windows).
    if (sqlite3_open16(db_path.c_str(), &g_db) != SQLITE_OK) {
        err = g_db ? sqlite3_errmsg(g_db) : "sqlite open failed";
        close();
        return false;
    }
    sqlite3_exec(g_db, "PRAGMA query_only=1;", nullptr, nullptr, nullptr);
    // sanity: the entries table must exist
    sqlite3_stmt* st = nullptr;
    bool ok = sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM entries", -1, &st, nullptr) == SQLITE_OK;
    sqlite3_finalize(st);
    if (!ok) { err = "not a gw2index database (no 'entries' table)"; close(); return false; }
    return true;
}

void close() {
    if (g_db) { sqlite3_close(g_db); g_db = nullptr; }
}

bool is_open() { return g_db != nullptr; }

std::wstring dat_path() {
    std::wstring out;
    if (!g_db) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db, "SELECT value FROM meta WHERE key='dat_path'", -1, &st, nullptr) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        const void* w = sqlite3_column_text16(st, 0);
        if (w) out.assign(reinterpret_cast<const wchar_t*>(w));
    }
    sqlite3_finalize(st);
    return out;
}

size_t entry_count() {
    if (!g_db) return 0;
    sqlite3_stmt* st = nullptr; size_t n = 0;
    if (sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM entries", -1, &st, nullptr) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        n = static_cast<size_t>(sqlite3_column_int64(st, 0));
    sqlite3_finalize(st);
    return n;
}

std::vector<std::string> types()      { return distinct("type"); }
std::vector<std::string> containers() { return distinct("container"); }

void load_meta_map(const std::function<void(uint32_t, const char*, const char*)>& cb) {
    if (!g_db) return;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db, "SELECT base_id,type,container FROM entries", -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW)
            cb(static_cast<uint32_t>(sqlite3_column_int64(st, 0)), col_text(st, 1), col_text(st, 2));
    }
    sqlite3_finalize(st);
}

std::vector<uint32_t> query_base_ids(const std::string& type, const std::string& container,
                                     uint32_t id_value, bool id_by_fileid, bool id_active, int limit) {
    std::vector<uint32_t> out;
    if (!g_db) return out;
    std::string sql = "SELECT base_id FROM entries WHERE 1=1";
    if (!type.empty())      sql += " AND type=?";
    if (!container.empty()) sql += " AND container=?";
    if (id_active) {
        if (id_by_fileid) sql += " AND base_id IN (SELECT base_id FROM file_ids WHERE file_id=?)";
        else              sql += " AND base_id=?";
    }
    sql += " ORDER BY base_id LIMIT ?";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
    int p = 1;
    if (!type.empty())      sqlite3_bind_text(st, p++, type.c_str(), -1, SQLITE_TRANSIENT);
    if (!container.empty()) sqlite3_bind_text(st, p++, container.c_str(), -1, SQLITE_TRANSIENT);
    if (id_active)          sqlite3_bind_int64(st, p++, id_value);
    sqlite3_bind_int(st, p++, limit);
    while (sqlite3_step(st) == SQLITE_ROW)
        out.push_back(static_cast<uint32_t>(sqlite3_column_int64(st, 0)));
    sqlite3_finalize(st);
    return out;
}

EntryInfo lookup(uint32_t base_id) {
    EntryInfo e;
    if (!g_db) return e;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db,
            "SELECT type,container,error,magic,size,size_stripped,size_final,comp_flag,actually_compressed "
            "FROM entries WHERE base_id=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, base_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            e.found = true;
            e.type = col_text(st, 0); e.container = col_text(st, 1);
            e.error = col_text(st, 2); e.magic = col_text(st, 3);
            e.size = sqlite3_column_int64(st, 4);
            e.size_stripped = sqlite3_column_int64(st, 5);
            e.size_final = sqlite3_column_int64(st, 6);
            e.comp_flag = sqlite3_column_int(st, 7);
            e.actually_compressed = sqlite3_column_int(st, 8);
        }
    }
    sqlite3_finalize(st);
    if (!e.found) return e;

    if (sqlite3_prepare_v2(g_db,
            "SELECT fourcc,version,struct_variant FROM chunks WHERE base_id=? ORDER BY seq",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, base_id);
        while (sqlite3_step(st) == SQLITE_ROW) {
            char line[96];
            const char* var = col_text(st, 2);
            std::snprintf(line, sizeof line, "%s v%d%s%s", col_text(st, 0),
                          sqlite3_column_int(st, 1), var[0] ? " " : "", var);
            e.chunks.emplace_back(line);
        }
    }
    sqlite3_finalize(st);
    return e;
}

} // namespace gw2idx
