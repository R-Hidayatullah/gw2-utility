// gw2index -- fast, resumable, multithreaded indexer for a GW2 .dat archive.
//
// For every MFT entry it records: baseId, all fileIds, the raw MFT header (offset,
// size, compression flag, CRC, MFT-declared uncompressed size), the *verified*
// compression truth (some entries are flagged compressed but are not), the sizes
// before/after CRC32-strip and decompression, the sniffed type/container, and -- for
// packfiles -- the list of chunks (fourcc + version + the template struct variant that
// applies). No raw file data is stored. Output is a queryable SQLite database.
//
// Resumable: each entry's fingerprint (offset|size|crc|flag) is stored; a re-run skips
// unchanged entries and only re-processes patched/new ones. Multithreaded workers do
// the read+decompress+parse; a single writer commits to SQLite in batches.
//
// Usage: gw2index --dat <path> --out <index.db> [--template <gw2_packfile.json>]
//                 [--threads N] [--full]     (--full ignores fingerprints, re-does all)

#include "sqlite3.h"

#include "gw2dat.h"
#include "cmp_decompress_method0.hpp"
#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using nlohmann::json;

// ------------------------------------------------------------------ result model
struct ChunkInfo { std::string fourcc; int version; std::string variant; uint32_t size; };
struct EntryResult {
    uint32_t base_id = 0;
    uint64_t offset = 0;
    uint32_t size = 0;              // on-disk (stored) size
    uint16_t comp_flag = 0;
    uint32_t crc = 0;
    uint32_t mft_usize = 0;         // MFT-declared uncompressed size
    int      actually_compressed = 0;
    uint32_t size_stripped = 0;     // after CRC32 strip
    uint32_t size_final = 0;        // after decompress (== stripped if not compressed)
    std::string magic, type, container, error, fingerprint;
    std::vector<uint32_t> file_ids;
    std::vector<ChunkInfo> chunks;
};

static std::string fourcc_str(const uint8_t* b) {
    std::string s;
    for (int i = 0; i < 4; ++i) s.push_back((b[i] >= 32 && b[i] < 127) ? (char)b[i] : '.');
    return s;
}

// ------------------------------------------------------------------ classification
// Sniff the decompressed bytes into a coarse type + (for packfiles) container fourcc.
static void classify(const std::vector<uint8_t>& d, EntryResult& r) {
    if (d.size() >= 4) r.magic = fourcc_str(d.data());
    if (d.size() < 4) { r.type = "empty"; return; }
    auto eq = [&](size_t off, const char* m, size_t n) {
        return d.size() >= off + n && std::memcmp(d.data() + off, m, n) == 0;
    };
    if (d[0] == 'P' && d[1] == 'F') {
        r.type = "packfile";
        if (d.size() >= 12) r.container = fourcc_str(d.data() + 8);
        return;
    }
    static const char* atex[] = {"ATEX", "ATTX", "ATEC", "ATEP", "ATEU", "ATET", "CTEX"};
    for (const char* m : atex) if (eq(0, m, 4)) { r.type = "texture"; return; }
    if (eq(0, "DDS ", 4)) { r.type = "dds"; return; }
    if (eq(0, "strs", 4)) { r.type = "strs"; return; }
    if (eq(0, "RIFF", 4)) { r.type = "riff"; return; }
    if (eq(0, "\x89PNG", 4)) { r.type = "png"; return; }
    if (d[0] == 0xFF && d[1] == 0xD8) { r.type = "jpeg"; return; }
    if (eq(0, "MZ", 2)) { r.type = "exe"; return; }
    if (eq(0, "asnd", 4)) { r.type = "asnd"; return; }
    r.type = "binary";
}

// Resolve a chunk's struct variant from the template: container-specific
// fileTypes[container][fourcc][version], else global chunks[fourcc][version].
static std::string resolve_variant(const json& tpl, const std::string& container,
                                   const std::string& fourcc, int version) {
    const std::string vs = std::to_string(version);
    auto ft = tpl.find("fileTypes");
    if (ft != tpl.end()) {
        auto c = ft->find(container);
        if (c != ft->end()) {
            auto f = c->find(fourcc);
            if (f != c->end()) { auto v = f->find(vs); if (v != f->end()) return v->get<std::string>(); }
        }
    }
    auto ch = tpl.find("chunks");
    if (ch != tpl.end()) {
        auto f = ch->find(fourcc);
        if (f != ch->end()) { auto v = f->find(vs); if (v != f->end()) return v->get<std::string>();
            if (!f->empty()) return "?v" + vs; } // fourcc known, version not mapped
    }
    return ""; // unknown chunk
}

// Walk a packfile's chunk table: fourcc + version (rd16 at chunk-data start) + variant.
static void parse_chunks(const std::vector<uint8_t>& d, const json& tpl, EntryResult& r) {
    if (d.size() < 12) return;
    auto rd16 = [&](size_t p) -> uint32_t { return (p + 2 <= d.size()) ? (d[p] | (d[p + 1] << 8)) : 0; };
    auto rd32 = [&](size_t p) -> uint32_t {
        return (p + 4 <= d.size()) ? (d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24)) : 0;
    };
    size_t pos = rd16(6); // headerSize
    int guard = 0;
    while (pos + 8 <= d.size() && guard++ < 4096) {
        std::string fourcc = fourcc_str(d.data() + pos);
        uint32_t chunk_size = rd32(pos + 4);
        int version = (int)rd16(pos + 8);           // chunk data starts here; first field = version
        r.chunks.push_back({fourcc, version, resolve_variant(tpl, r.container, fourcc, version), chunk_size});
        size_t next = pos + 8 + chunk_size;
        if (next <= pos) break;
        pos = next;
    }
}

// ------------------------------------------------------------------ per-entry work
static void process_entry(const std::string& dat_path, const MftData& e, uint32_t base_id,
                          const json& tpl, EntryResult& r) {
    r.base_id = base_id;
    r.offset = e.offset; r.size = e.size; r.comp_flag = e.compression_flag;
    r.crc = e.crc; r.mft_usize = e.uncompressed_size;
    char fp[96];
    std::snprintf(fp, sizeof fp, "%llu|%u|%u|%u", (unsigned long long)e.offset, e.size, e.crc, e.compression_flag);
    r.fingerprint = fp;
    if (e.size == 0) { r.type = "empty"; return; }

    std::vector<uint8_t> raw;
    try { raw = read_entry_bytes(dat_path, e); }
    catch (const std::exception& ex) { r.error = std::string("read: ") + ex.what(); return; }

    std::vector<uint8_t> stripped = gw2cmp::strip_crc32(std::span<const uint8_t>(raw));
    r.size_stripped = (uint32_t)stripped.size();

    std::vector<uint8_t> final;
    if (e.compression_flag != 0 && stripped.size() >= 8) {
        uint32_t usize = stripped[4] | (stripped[5] << 8) | (stripped[6] << 16) | ((uint32_t)stripped[7] << 24);
        try {
            final = gw2cmp::decompress_method0(std::span<const uint8_t>(stripped).subspan(8), usize);
            r.actually_compressed = 1;
        } catch (const std::exception&) {
            // Flagged compressed but the payload is not a valid Method0 stream.
            r.actually_compressed = 0;
            r.error = "flagged-compressed-but-not";
            final = stripped; // index what it actually is
        }
    } else {
        final = std::move(stripped);
    }
    r.size_final = (uint32_t)final.size();

    classify(final, r);
    if (r.type == "packfile") parse_chunks(final, tpl, r);
}

// ------------------------------------------------------------------ SQLite schema
static void exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::fprintf(stderr, "sqlite: %s\n", err ? err : "?");
        sqlite3_free(err);
    }
}
static void init_schema(sqlite3* db) {
    exec(db,
        "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"
        "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);"
        "CREATE TABLE IF NOT EXISTS entries("
        " base_id INTEGER PRIMARY KEY, offset INTEGER, size INTEGER, comp_flag INTEGER, crc INTEGER,"
        " mft_usize INTEGER, fingerprint TEXT, actually_compressed INTEGER,"
        " size_stripped INTEGER, size_final INTEGER, magic TEXT, type TEXT, container TEXT, error TEXT);"
        "CREATE TABLE IF NOT EXISTS file_ids(file_id INTEGER PRIMARY KEY, base_id INTEGER);"
        "CREATE TABLE IF NOT EXISTS chunks(base_id INTEGER, seq INTEGER, fourcc TEXT, version INTEGER,"
        " struct_variant TEXT, chunk_size INTEGER);"
        "CREATE INDEX IF NOT EXISTS ix_fileids_base ON file_ids(base_id);"
        "CREATE INDEX IF NOT EXISTS ix_chunks_base ON chunks(base_id);"
        "CREATE INDEX IF NOT EXISTS ix_chunks_fourcc ON chunks(fourcc);"
        "CREATE INDEX IF NOT EXISTS ix_entries_type ON entries(type);"
        "CREATE INDEX IF NOT EXISTS ix_entries_container ON entries(container);");
}

// ------------------------------------------------------------------ work queue
struct Queue {
    std::mutex m; std::condition_variable cv;
    std::queue<EntryResult> q; bool done = false;
    void push(EntryResult&& r) { { std::lock_guard<std::mutex> l(m); q.push(std::move(r)); } cv.notify_one(); }
    bool pop(EntryResult& out) {
        std::unique_lock<std::mutex> l(m);
        cv.wait(l, [&] { return !q.empty() || done; });
        if (q.empty()) return false;
        out = std::move(q.front()); q.pop(); return true;
    }
    void finish() { { std::lock_guard<std::mutex> l(m); done = true; } cv.notify_all(); }
};

int main(int argc, char** argv) {
    std::string dat_path, out_path, tpl_path;
    int nthreads = (int)std::max(2u, std::thread::hardware_concurrency());
    bool full = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
        if (a == "--dat") dat_path = next();
        else if (a == "--out") out_path = next();
        else if (a == "--template") tpl_path = next();
        else if (a == "--threads") nthreads = std::max(1, std::atoi(next().c_str()));
        else if (a == "--full") full = true;
    }
    if (dat_path.empty() || out_path.empty()) {
        std::fprintf(stderr, "usage: gw2index --dat <path> --out <index.db> [--template j] [--threads N] [--full]\n");
        return 2;
    }

    json tpl = json::object();
    if (!tpl_path.empty()) { std::ifstream t(tpl_path, std::ios::binary); if (t) t >> tpl; }

    std::fprintf(stderr, "loading MFT from %s ...\n", dat_path.c_str());
    Gw2Dat dat;
    try { load_dat_file(dat, dat_path); }
    catch (const std::exception& ex) { std::fprintf(stderr, "load failed: %s\n", ex.what()); return 1; }
    const size_t N = dat.mft_data_list.size();

    // baseId (1-based MFT index) -> fileIds.
    std::unordered_map<uint32_t, std::vector<uint32_t>> base_to_files;
    for (auto& b : dat.mft_base_id_data_list) base_to_files[b.base_id] = b.file_id;

    sqlite3* db = nullptr;
    if (sqlite3_open(out_path.c_str(), &db) != SQLITE_OK) { std::fprintf(stderr, "cannot open db\n"); return 1; }
    init_schema(db);

    // Resumable: load existing fingerprints unless --full.
    std::unordered_map<uint32_t, std::string> existing;
    if (!full) {
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "SELECT base_id, fingerprint FROM entries", -1, &st, nullptr);
        while (sqlite3_step(st) == SQLITE_ROW)
            existing[(uint32_t)sqlite3_column_int64(st, 0)] =
                reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        sqlite3_finalize(st);
    }
    std::fprintf(stderr, "MFT entries: %zu (already indexed: %zu). Using %d threads.\n",
                 N, existing.size(), nthreads);

    // ---- producers: workers process changed/new entries into a queue ----
    Queue queue;
    std::atomic<size_t> cursor{0};
    std::atomic<size_t> processed{0}, skipped{0};
    auto worker = [&]() {
        for (;;) {
            size_t i = cursor.fetch_add(1);
            if (i >= N) break;
            const MftData& e = dat.mft_data_list[i];
            uint32_t base_id = (uint32_t)(i + 1);
            char fp[96];
            std::snprintf(fp, sizeof fp, "%llu|%u|%u|%u", (unsigned long long)e.offset, e.size, e.crc, e.compression_flag);
            auto it = existing.find(base_id);
            if (it != existing.end() && it->second == fp) { skipped.fetch_add(1); continue; }
            EntryResult r;
            process_entry(dat_path, e, base_id, tpl, r);
            auto f = base_to_files.find(base_id);
            if (f != base_to_files.end()) r.file_ids = f->second;
            processed.fetch_add(1);
            queue.push(std::move(r));
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < nthreads; ++t) pool.emplace_back(worker);

    // ---- consumer: single writer, batched transactions ----
    std::thread writer([&]() {
        sqlite3_stmt *insE, *insF, *insC, *delF, *delC;
        sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO entries(base_id,offset,size,comp_flag,crc,mft_usize,fingerprint,"
            "actually_compressed,size_stripped,size_final,magic,type,container,error)"
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &insE, nullptr);
        sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO file_ids(file_id,base_id) VALUES(?,?)", -1, &insF, nullptr);
        sqlite3_prepare_v2(db, "INSERT INTO chunks(base_id,seq,fourcc,version,struct_variant,chunk_size) VALUES(?,?,?,?,?,?)", -1, &insC, nullptr);
        sqlite3_prepare_v2(db, "DELETE FROM file_ids WHERE base_id=?", -1, &delF, nullptr);
        sqlite3_prepare_v2(db, "DELETE FROM chunks WHERE base_id=?", -1, &delC, nullptr);
        exec(db, "BEGIN");
        size_t batch = 0;
        EntryResult r;
        while (queue.pop(r)) {
            sqlite3_reset(insE);
            sqlite3_bind_int64(insE, 1, r.base_id);   sqlite3_bind_int64(insE, 2, r.offset);
            sqlite3_bind_int64(insE, 3, r.size);      sqlite3_bind_int(insE, 4, r.comp_flag);
            sqlite3_bind_int64(insE, 5, r.crc);       sqlite3_bind_int64(insE, 6, r.mft_usize);
            sqlite3_bind_text(insE, 7, r.fingerprint.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(insE, 8, r.actually_compressed);
            sqlite3_bind_int64(insE, 9, r.size_stripped); sqlite3_bind_int64(insE, 10, r.size_final);
            sqlite3_bind_text(insE, 11, r.magic.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insE, 12, r.type.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insE, 13, r.container.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insE, 14, r.error.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(insE);
            // refresh child rows for this entry
            sqlite3_reset(delF); sqlite3_bind_int64(delF, 1, r.base_id); sqlite3_step(delF);
            sqlite3_reset(delC); sqlite3_bind_int64(delC, 1, r.base_id); sqlite3_step(delC);
            for (uint32_t fid : r.file_ids) {
                sqlite3_reset(insF); sqlite3_bind_int64(insF, 1, fid); sqlite3_bind_int64(insF, 2, r.base_id); sqlite3_step(insF);
            }
            for (size_t s = 0; s < r.chunks.size(); ++s) {
                const auto& c = r.chunks[s];
                sqlite3_reset(insC);
                sqlite3_bind_int64(insC, 1, r.base_id); sqlite3_bind_int(insC, 2, (int)s);
                sqlite3_bind_text(insC, 3, c.fourcc.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(insC, 4, c.version);
                sqlite3_bind_text(insC, 5, c.variant.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(insC, 6, c.size);
                sqlite3_step(insC);
            }
            if (++batch % 20000 == 0) { exec(db, "COMMIT"); exec(db, "BEGIN");
                std::fprintf(stderr, "  ...committed %zu\n", batch); }
        }
        exec(db, "COMMIT");
        sqlite3_finalize(insE); sqlite3_finalize(insF); sqlite3_finalize(insC);
        sqlite3_finalize(delF); sqlite3_finalize(delC);
    });

    for (auto& t : pool) t.join();
    queue.finish();
    writer.join();

    // meta
    {
        sqlite3_stmt* st;
        sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)", -1, &st, nullptr);
        auto put = [&](const char* k, const std::string& v) {
            sqlite3_reset(st); sqlite3_bind_text(st, 1, k, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, v.c_str(), -1, SQLITE_TRANSIENT); sqlite3_step(st);
        };
        put("dat_path", dat_path);
        put("dat_size", std::to_string(dat.file_info.file_size));
        put("mft_entries", std::to_string(N));
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    std::fprintf(stderr, "done. processed %zu, skipped(unchanged) %zu, total %zu -> %s\n",
                 processed.load(), skipped.load(), N, out_path.c_str());
    return 0;
}
