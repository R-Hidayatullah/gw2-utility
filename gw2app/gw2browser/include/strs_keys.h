#ifndef STRS_KEYS_H
#define STRS_KEYS_H

// Optional RC4 decryption layer for "strs" string tables.
//
// GW2's packed strs records are RC4-encrypted with an 8-byte per-textId
// "password" the client receives at runtime (see gw2-strs-decrypt notes). This
// module loads two capture/derived tables so the browser can decrypt them:
//   * keys     : textId,key8_hex     (from a live-client capture -> textkeys.csv)
//   * textbase : fileId,baseTextId   (from TextPackManifest -> strs_textbase.csv)
// The record's global textId = baseTextId(for this file's fileId) + recordIndex.
// Without both tables the packed records stay flagged (same as before).

#include <cstdint>
#include <string>
#include <vector>

namespace gw2skeys {

// Load "textId,key8_hex" rows (lines starting with '#' or "textId" ignored).
// Merges into the existing table. Returns number of keys after load, -1 on open error.
long load_keys(const std::wstring& path);

// Load "fileId,baseTextId" rows. Returns rows loaded, -1 on open error.
long load_textbase(const std::wstring& path);

bool   keys_ready();
size_t key_count();
bool   textbase_ready();

// baseTextId for whichever of these fileIds appears in the textbase map, or -1.
long long base_for_fileids(const std::vector<uint32_t>& file_ids);

// Full strs listing, decrypting packed records when a key is known.
//   baseTextId < 0  -> packed records are flagged (can't resolve textId).
// Mirrors gw2strs::decode's output format so it can drop into the same surface.
std::wstring decode(const uint8_t* data, size_t size, long long baseTextId);

} // namespace gw2skeys

#endif // STRS_KEYS_H
