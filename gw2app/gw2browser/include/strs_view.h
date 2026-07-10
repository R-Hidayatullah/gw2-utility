#ifndef GW2_STRS_VIEW_H
#define GW2_STRS_VIEW_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace gw2strs {

// True if `data` starts with the "strs" (TEXT_STRINGS_SIGNATURE) magic.
bool is_strs(const uint8_t* data, size_t size);

// Decodes a "strs" string-table container to a human-readable, line-per-record
// listing (for the text preview control). Records stored as raw UTF-16LE
// (baseChar == 0, rangeBits == 16) are decoded byte-exact; bit-packed records
// (baseChar != 0) can't be decoded from a standalone file -- they need a
// runtime RC4 key that isn't present here -- so they're listed with their
// framing metadata and flagged "[encrypted -- RC4 key unavailable]".
// See strs_decode.py for the reverse-engineering notes this mirrors.
std::wstring decode(const uint8_t* data, size_t size);

} // namespace gw2strs

#endif // GW2_STRS_VIEW_H
