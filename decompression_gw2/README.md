# decompressgw2

Reverse-engineered decompressor for `Gw2.dat` (Guild Wars 2) MFT entries,
implementing ArenaNet's **CmpDecompress Method 0** (a custom Huffman + LZ77
codec). Reverse-engineered from `Gw2-64-disable-aslr.exe` via IDA Pro /
Hex-Rays. Verified **byte-exact** against a real entry
(`THIRDPARTYSOFTWAREREADME.txt`, 540537 bytes).

Available in three languages, all implementing the identical algorithm and
cross-verified to produce identical output:

| File | Language | Style |
|---|---|---|
| [`cmp_decompress_method0.py`](cmp_decompress_method0.py) | Python 3 | single module |
| [`cmp_decompress_method0.h`](cmp_decompress_method0.h) | C17 | single-header (stb-style) |
| [`cmp_decompress_method0.hpp`](cmp_decompress_method0.hpp) | C++20 | single-header (header-only) |

## Pipeline

```
Gw2.dat
  -> MFT entry: seek(offset), read(size)                      [raw bytes, still compressed]
  -> strip CRC32 (4 bytes every 0x10000, + trailing CRC)
  -> parse 8-byte header: {flag: u32, uncompressedSize: u32}
  -> CmpDecompress Method 0 bitstream (Huffman + LZ77)
  -> decompressed bytes (exactly uncompressedSize long)
```

For plain files (text, config, etc.) the decompressed bytes are the final
result. For texture entries (`ATEX` container) the decompressed bytes are
only the *container* — the per-mip-level pixel data needs a second,
separate decompression pass that is **not yet implemented** (see
[`ATEX_RESEARCH_NOTES.md`](ATEX_RESEARCH_NOTES.md) for reverse-engineering
progress on that).

**Only Method 0 (plain) is implemented.** Method 1 (delta/patch, used for
incremental client updates) is detected and rejected with a clear error —
it isn't needed for reading assets out of a single `Gw2.dat` snapshot.

---

## Python

No dependencies, pure standard library.

```python
from cmp_decompress_method0 import decompress_gw2_entry

with open("compressed_16.bin", "rb") as f:
    raw = f.read()          # raw MFT entry bytes: seek(offset) + read(size) from Gw2.dat

data = decompress_gw2_entry(raw)   # bytes, exactly uncompressedSize long
```

Command-line (defaults to the bundled sample if no args given):
```sh
python cmp_decompress_method0.py <input.bin> <output.bin>
```

Lower-level functions, if you've already stripped CRCs / parsed the header
yourself:
```python
from cmp_decompress_method0 import remove_crc32_data, cmp_decompress_method0

stripped = remove_crc32_data(raw)
flag, uncompressed_size = struct.unpack_from("<II", stripped, 0)
data = cmp_decompress_method0(stripped[8:], uncompressed_size)
```

---

## C17

Single header, [stb](https://github.com/nothings/stb)-style: include
normally for declarations, and additionally `#define GW2CMP_IMPLEMENTATION`
in **exactly one** `.c` file before including it to pull in the
implementation.

```c
#define GW2CMP_IMPLEMENTATION
#include "cmp_decompress_method0.h"

uint8_t *raw = /* ... read the MFT entry bytes ... */;
size_t raw_size = /* ... */;

uint8_t *out = NULL;
size_t out_size = 0;
gw2cmp_status st = gw2cmp_decompress_entry(raw, raw_size, &out, &out_size);
if (st != GW2CMP_OK) {
    fprintf(stderr, "decompress failed: %s\n", gw2cmp_status_string(st));
    exit(1);
}

/* use out[0 .. out_size) */

gw2cmp_free(out);   /* every buffer returned by gw2cmp_* must be freed this way */
```

In other translation units that also need the declarations, just
`#include "cmp_decompress_method0.h"` **without** the `GW2CMP_IMPLEMENTATION`
macro.

Build (any C17 compiler):
```sh
gcc -std=c17 -O2 your_program.c -o your_program
```

### C API reference

```c
typedef enum gw2cmp_status {
    GW2CMP_OK = 0,
    GW2CMP_ERR_ALLOC,
    GW2CMP_ERR_METHOD_UNSUPPORTED,   /* stream uses Method 1 (delta) */
    GW2CMP_ERR_HUFFMAN_DECODE,
    GW2CMP_ERR_BACKREF_RANGE,
    GW2CMP_ERR_TRUNCATED_HEADER,
} gw2cmp_status;

/* Full pipeline: raw MFT entry -> decompressed bytes. */
gw2cmp_status gw2cmp_decompress_entry(const uint8_t *raw, size_t raw_size,
                                       uint8_t **out_data, size_t *out_size);

/* Lower-level: CRC already stripped, header already parsed. */
gw2cmp_status gw2cmp_decompress_method0(const uint8_t *comp, size_t comp_size,
                                         size_t output_size, uint8_t **out_data);

/* Just the CRC32-stripping step. */
gw2cmp_status gw2cmp_strip_crc32(const uint8_t *raw, size_t raw_size,
                                  uint8_t **out_data, size_t *out_size);

void gw2cmp_free(void *p);
const char *gw2cmp_status_string(gw2cmp_status status);
```

---

## C++20

Pure header-only — no implementation macro needed, just include it.
Everything lives in `namespace gw2cmp`; errors are reported via the
`gw2cmp::decode_error` exception (derived from `std::runtime_error`).

```cpp
#include "cmp_decompress_method0.hpp"

std::vector<uint8_t> raw = /* ... read the MFT entry bytes ... */;

try {
    std::vector<uint8_t> data = gw2cmp::decompress_entry(raw);
    // use data
} catch (const gw2cmp::decode_error &e) {
    std::cerr << "decompress failed: " << e.what() << "\n";
}
```

Build (any C++20 compiler):
```sh
g++ -std=c++20 -O2 your_program.cpp -o your_program
```

### C++ API reference

```cpp
namespace gw2cmp {
    class decode_error : public std::runtime_error { /* ... */ };

    // Full pipeline: raw MFT entry -> decompressed bytes.
    std::vector<uint8_t> decompress_entry(std::span<const uint8_t> raw);

    // Lower-level: CRC already stripped, header already parsed.
    std::vector<uint8_t> decompress_method0(std::span<const uint8_t> comp, size_t output_size);

    // Just the CRC32-stripping step.
    std::vector<uint8_t> strip_crc32(std::span<const uint8_t> raw);
}
```

---

## Testing / verification

Sample files included:
- `compressed_16.bin` — a text entry (decompresses to `THIRDPARTYSOFTWAREREADME.txt`, 540537 bytes)
- `compressed_3178.bin` — a texture (ATEX/DXT5) entry, 1024x1024, decompresses to a 269208-byte ATEX container

`test_c17.c` and `test_cpp20.cpp` are minimal example programs (also used
to verify the C/C++ ports against the Python reference and against a real
Gw2.dat-extracted file — all three produce byte-identical output). Build
and run either one directly:

```sh
gcc -std=c17 test_c17.c -o test_c17 && ./test_c17 compressed_16.bin out.txt
g++ -std=c++20 test_cpp20.cpp -o test_cpp20 && ./test_cpp20 compressed_16.bin out.txt
python cmp_decompress_method0.py compressed_16.bin out.txt
```

## Known limitations

- **Method 1 (delta/patch) is not implemented** in any of the three ports
  — only Method 0 (plain). Attempting to decompress a Method-1 stream
  raises a clear error rather than producing garbage.
- **ATEX texture pixel data needs a second decompression pass** that
  hasn't been cracked yet (RLE/Golomb duplicate-block scheme, separate
  from CmpDecompress). See `ATEX_RESEARCH_NOTES.md`.
- BC6H/BC7 texture formats are explicitly out of scope for the ATEX work.
