/*
 * cmp_decompress_method0.h -- single-header C17 library
 *
 * Reimplementation of ArenaNet's CmpDecompress Method 0 (Huffman + LZ77),
 * reverse-engineered from Gw2-64.exe (Services/Compress/CmpApi.cpp +
 * CmpHuff.cpp). Verified byte-exact against a real Gw2.dat entry
 * (THIRDPARTYSOFTWAREREADME.txt, 540537 bytes).
 *
 * Function addresses (for cross-reference in IDA):
 *   CmpDecompress                 0x140D921C0
 *   CmpDecompress_Method0_Inflate 0x140D96FC0   <- this is what's implemented here
 *   CmpDecompress_Method1_Delta   0x140D94E20   <- NOT implemented (delta/patch method)
 *   Huffman table builder         0x140D9DF00   (CmpHuff.cpp)
 *
 * Pipeline (Gw2.dat -> plaintext):
 *   1. Read the MFT entry's raw [offset, offset+size) bytes from Gw2.dat.
 *   2. Strip the 4-byte CRC32 inserted every 0x10000 bytes (+ trailing CRC).
 *      See gw2cmp_strip_crc32().
 *   3. Parse an 8-byte header from the stripped buffer:
 *        offset 0..3 : flag dword (low 16 bits observed = compression method
 *                      id 8 == "ANet compress"; meaning only partially understood)
 *        offset 4..7 : uncompressedSize (u32 LE)
 *   4. The remaining bytes are the CmpDecompress bitstream:
 *        - first 4 bits : Method (0 = plain, 1 = delta -- not implemented here)
 *        - next 4 bits  : minMatchAdd-1 (Method 0 only)
 *        - then repeat: two freshly-rebuilt canonical Huffman tables
 *          (literal/length, then distance) followed by up to (nibble+1)<<12
 *          symbols coded against those tables, LZ77-style.
 *
 * Usage:
 *   #include "cmp_decompress_method0.h"          // declarations only
 *   ...
 *   #define GW2CMP_IMPLEMENTATION
 *   #include "cmp_decompress_method0.h"          // in exactly one .c file
 *
 * All allocations use malloc(); free results with gw2cmp_free() (a thin
 * wrapper around free(), provided so callers never need to worry about
 * which allocator was used).
 */

#ifndef GW2CMP_DECOMPRESS_METHOD0_H
#define GW2CMP_DECOMPRESS_METHOD0_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum gw2cmp_status {
    GW2CMP_OK = 0,
    GW2CMP_ERR_ALLOC = 1,           /* malloc/realloc failure                         */
    GW2CMP_ERR_METHOD_UNSUPPORTED,  /* stream uses Method 1 (delta), not implemented  */
    GW2CMP_ERR_HUFFMAN_DECODE,      /* malformed Huffman table or code                */
    GW2CMP_ERR_BACKREF_RANGE,       /* LZ77 back-reference points before the output   */
    GW2CMP_ERR_TRUNCATED_HEADER,    /* input too small to hold the 8-byte header      */
} gw2cmp_status;

/*
 * Full pipeline: raw MFT entry bytes (still containing periodic CRC32
 * checksums and the 8-byte {flag,size} header) -> decompressed bytes.
 * On GW2CMP_OK, out_data is malloc'd (caller frees with gw2cmp_free) and
 * out_size holds its length. On failure, out_data/out_size are untouched.
 */
gw2cmp_status gw2cmp_decompress_entry(const uint8_t *raw, size_t raw_size,
                                       uint8_t **out_data, size_t *out_size);

/*
 * Lower-level entry point: `comp` is the bitstream that comes right after
 * the 8-byte {flag,uncompressedSize} header (CRC32 already stripped).
 * `output_size` is the expected decompressed size (normally read from that
 * header). On success, *out_data is malloc'd with exactly output_size bytes.
 */
gw2cmp_status gw2cmp_decompress_method0(const uint8_t *comp, size_t comp_size,
                                         size_t output_size, uint8_t **out_data);

/*
 * Strips the 4-byte CRC32 checksum Gw2.dat inserts every 0x10000 bytes of a
 * raw on-disk MFT entry (plus one trailing CRC at EOF). On success,
 * *out_data is malloc'd and *out_size holds its length.
 */
gw2cmp_status gw2cmp_strip_crc32(const uint8_t *raw, size_t raw_size,
                                  uint8_t **out_data, size_t *out_size);

/* Frees a buffer returned by any gw2cmp_* function above. */
void gw2cmp_free(void *p);

/* Short human-readable description of a status code. */
const char *gw2cmp_status_string(gw2cmp_status status);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* =========================================================================
 * Implementation
 * ========================================================================= */
#ifdef GW2CMP_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>

/* ---- bit reader: sequential little-endian 32-bit words, MSB-first bits ---- */

typedef struct gw2cmp__bitreader {
    const uint8_t *data;
    size_t end_byte; /* trailing <4 bytes are never read, matches CmpDecompress */
    size_t pos;
    uint64_t acc;
    int bits;
} gw2cmp__bitreader;

static void gw2cmp__br_init(gw2cmp__bitreader *br, const uint8_t *data, size_t size) {
    br->data = data;
    br->end_byte = (size / 4) * 4;
    br->pos = 0;
    br->acc = 0;
    br->bits = 0;
}

static void gw2cmp__br_refill(gw2cmp__bitreader *br, int n) {
    while (br->bits < n) {
        uint32_t word = 0;
        if (br->pos + 4 <= br->end_byte) {
            const uint8_t *p = br->data + br->pos;
            word = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            br->pos += 4;
        }
        br->acc = (br->acc << 32) | word;
        br->bits += 32;
    }
}

static uint32_t gw2cmp__br_read(gw2cmp__bitreader *br, int n) {
    if (n == 0) return 0;
    gw2cmp__br_refill(br, n);
    br->bits -= n;
    uint32_t val = (uint32_t)((br->acc >> br->bits) & (((uint64_t)1 << n) - 1));
    br->acc &= ((uint64_t)1 << br->bits) - 1;
    return val;
}

static uint32_t gw2cmp__br_peek(gw2cmp__bitreader *br, int n) {
    gw2cmp__br_refill(br, n);
    return (uint32_t)((br->acc >> (br->bits - n)) & (((uint64_t)1 << n) - 1));
}

/* ---- static tables, extracted verbatim from the binary via IDA ---- */

/* byte_142061180 (32 entries) -- extra bits for length codes 256..284 */
static const uint8_t GW2CMP__LEN_EXTRA[32] = {
    0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1,
    2, 2, 2, 2,
    3, 3, 3, 3,
    4, 4, 4, 4,
    5, 5, 5, 5,
    0, 0, 0, 0
};

/* byte_142060FA0 (32 entries) -- base length value (before adding minMatchAdd) */
static const uint16_t GW2CMP__LEN_BASE[32] = {
    0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7,
    0x8, 0xa, 0xc, 0xe,
    0x10, 0x14, 0x18, 0x1c,
    0x20, 0x28, 0x30, 0x38,
    0x40, 0x50, 0x60, 0x70,
    0x80, 0xa0, 0xc0, 0xe0,
    0xff, 0x0, 0x0, 0x0
};

/* byte_1420610E0 (32 entries) -- extra bits for distance codes */
static const uint8_t GW2CMP__DIST_EXTRA[32] = {
    0, 0, 0, 0,
    1, 1, 2, 2,
    3, 3, 4, 4,
    5, 5, 6, 6,
    7, 7, 8, 8,
    9, 9, 10, 10,
    11, 11, 12, 12,
    13, 13, 14, 14
};

/* word_142060F60 (32 entries, 16-bit) -- base distance value */
static const uint32_t GW2CMP__DIST_BASE[32] = {
    0x0, 0x1, 0x2, 0x3, 0x4, 0x6, 0x8, 0xc,
    0x10, 0x18, 0x20, 0x30,
    0x40, 0x60, 0x80, 0xc0,
    0x100, 0x180, 0x200, 0x300,
    0x400, 0x600, 0x800, 0xc00,
    0x1000, 0x1800, 0x2000, 0x3000,
    0x4000, 0x6000, 0x0, 0x0
};

/* Fixed "meta Huffman" used to decode the RLE-packed code-length alphabet
 * (unk_142061620: 14 (mask, offset, bitlen) records, mask descending). */
typedef struct gw2cmp__meta_row { uint32_t mask; uint16_t offset; uint8_t bitlen; } gw2cmp__meta_row;

static const gw2cmp__meta_row GW2CMP__META_TABLE[14] = {
    { 0xa0000000u, 2, 3 },
    { 0x60000000u, 6, 4 },
    { 0x40000000u, 10, 5 },
    { 0x20000000u, 18, 6 },
    { 0x12000000u, 25, 7 },
    { 0x0c000000u, 31, 8 },
    { 0x07000000u, 41, 9 },
    { 0x03000000u, 57, 10 },
    { 0x01600000u, 70, 11 },
    { 0x00f00000u, 77, 12 },
    { 0x00c00000u, 83, 13 },
    { 0x00b00000u, 87, 14 },
    { 0x00a00000u, 95, 15 },
    { 0x00000000u, 255, 16 },
};

/* byte_142061690 (256 entries) -- value table indexed by the meta-huffman lookup */
static const uint8_t GW2CMP__META_VALUES[256] = {
    0x8, 0x9, 0xa, 0x0, 0x7, 0xb, 0xc, 0x6, 0x29, 0x2a, 0xe0, 0x4, 0x5, 0x20, 0x28, 0x2b,
    0x2c, 0x40, 0x4a, 0x3, 0xd, 0x25, 0x26, 0x27, 0x48, 0x49, 0x24, 0x47, 0x4b, 0x4c, 0x69, 0x6a,
    0x23, 0x46, 0x60, 0x63, 0x67, 0x68, 0x88, 0x89, 0xa0, 0xe8, 0x1, 0x2, 0x2d, 0x43, 0x44, 0x45,
    0x65, 0x66, 0x80, 0x87, 0x8a, 0xa8, 0xa9, 0xc0, 0xc9, 0xe9, 0xe, 0x4d, 0x64, 0x6b, 0x6c, 0x84,
    0x85, 0x8b, 0xa4, 0xa5, 0xaa, 0xc8, 0xe5, 0x83, 0x86, 0xa6, 0xa7, 0xc7, 0xca, 0xe7, 0x22, 0x2e,
    0x8c, 0xc4, 0xe4, 0xe6, 0x4e, 0x6d, 0xc6, 0xec, 0xf, 0x10, 0x11, 0x8d, 0xab, 0xac, 0xcc, 0xea,
    0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x21, 0x2f,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    0x41, 0x42, 0x4f, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c,
    0x5d, 0x5e, 0x5f, 0x61, 0x62, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x81, 0x82, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94,
    0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0xa1, 0xa2, 0xa3, 0xad, 0xae,
    0xaf, 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe,
    0xbf, 0xc1, 0xc2, 0xc3, 0xc5, 0xcb, 0xcd, 0xce, 0xcf, 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6,
    0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, 0xe1, 0xe2, 0xe3, 0xeb, 0xed, 0xee, 0xef,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};

static int gw2cmp__decode_meta(gw2cmp__bitreader *br, uint8_t *out_value) {
    uint32_t v = gw2cmp__br_peek(br, 32);
    for (int i = 0; i < 14; i++) {
        const gw2cmp__meta_row *row = &GW2CMP__META_TABLE[i];
        if (v >= row->mask) {
            uint32_t rel = (v - row->mask) >> (32 - row->bitlen);
            uint32_t idx = (uint32_t)row->offset - rel;
            if (idx >= 256) return 0;
            gw2cmp__br_read(br, row->bitlen);
            *out_value = GW2CMP__META_VALUES[idx];
            return 1;
        }
    }
    return 0;
}

/* ---- canonical Huffman decode table: sorted (length, code) -> symbol ---- */

typedef struct gw2cmp__huff_entry {
    uint32_t code;
    uint16_t symbol;
    uint8_t length;
} gw2cmp__huff_entry;

typedef struct gw2cmp__huff_table {
    gw2cmp__huff_entry *entries;
    size_t count;
} gw2cmp__huff_table;

static int gw2cmp__huff_cmp(const void *a, const void *b) {
    const gw2cmp__huff_entry *ea = (const gw2cmp__huff_entry *)a;
    const gw2cmp__huff_entry *eb = (const gw2cmp__huff_entry *)b;
    if (ea->length != eb->length) return (int)ea->length - (int)eb->length;
    if (ea->code != eb->code) return (ea->code < eb->code) ? -1 : 1;
    return 0;
}

static void gw2cmp__huff_free(gw2cmp__huff_table *t) {
    free(t->entries);
    t->entries = NULL;
    t->count = 0;
}

/*
 * Mirrors sub_140D9DF00: reads a 16-bit symbol count, then an RLE-encoded
 * array of code lengths (via the fixed meta table), then builds a canonical
 * Huffman decode table.
 *
 * ArenaNet assigns codes in DESCENDING order per length group (ascending
 * symbol index -> descending code value), with the running code counter
 * kept as an UNSIGNED 32-bit value that WRAPS AROUND once a length group
 * fully consumes its available code space -- that wrapped value is exactly
 * what feeds "2*v+1" for the next length. Using a signed/non-wrapping
 * counter here silently produces wrong codes for every length past the
 * point where the first wrap happens.
 */
static gw2cmp_status gw2cmp__build_huffman_table(gw2cmp__bitreader *br, gw2cmp__huff_table *out) {
    out->entries = NULL;
    out->count = 0;

    uint32_t total_symbols = gw2cmp__br_read(br, 16);

    uint8_t *code_lengths = NULL;
    if (total_symbols > 0) {
        code_lengths = (uint8_t *)calloc(total_symbols, sizeof(uint8_t));
        if (!code_lengths) return GW2CMP_ERR_ALLOC;
    }

    long idx = (long)total_symbols - 1;
    while (idx >= 0) {
        uint8_t rle;
        if (!gw2cmp__decode_meta(br, &rle)) { free(code_lengths); return GW2CMP_ERR_HUFFMAN_DECODE; }
        uint32_t repeat = (uint32_t)(rle >> 5) + 1;
        uint8_t length = rle & 0x1F;
        if (length != 0 || total_symbols < 2) {
            for (uint32_t k = 0; k < repeat; k++) {
                if (idx < 0) { free(code_lengths); return GW2CMP_ERR_HUFFMAN_DECODE; }
                code_lengths[idx] = length;
                idx--;
            }
        } else {
            idx -= (long)repeat;
        }
    }

    uint8_t max_len = 0;
    for (uint32_t i = 0; i < total_symbols; i++)
        if (code_lengths[i] > max_len) max_len = code_lengths[i];

    /* Per-length linked lists (head = v66[length], next = v63[symbol]), built
     * by prepending while idx descends -- walking from the head therefore
     * visits symbols in ascending index order, matching the original code. */
    const uint32_t U32_NIL = 0xFFFFFFFFu;
    uint32_t *v66 = (uint32_t *)malloc(sizeof(uint32_t) * ((size_t)max_len + 1));
    uint32_t *v63 = NULL;
    if (total_symbols > 0) v63 = (uint32_t *)malloc(sizeof(uint32_t) * total_symbols);
    if (!v66 || (total_symbols > 0 && !v63)) {
        free(code_lengths); free(v66); free(v63);
        return GW2CMP_ERR_ALLOC;
    }
    for (uint32_t i = 0; i <= (uint32_t)max_len; i++) v66[i] = U32_NIL;

    idx = (long)total_symbols - 1;
    while (idx >= 0) {
        uint8_t l = code_lengths[idx];
        if (l != 0) {
            v63[idx] = v66[l];
            v66[l] = (uint32_t)idx;
        }
        idx--;
    }

    /* Count nonzero-length symbols so we can size the output table exactly. */
    size_t nonzero = 0;
    for (uint32_t i = 0; i < total_symbols; i++) if (code_lengths[i] != 0) nonzero++;
    gw2cmp__huff_entry *entries = NULL;
    if (nonzero > 0) {
        entries = (gw2cmp__huff_entry *)malloc(sizeof(gw2cmp__huff_entry) * nonzero);
        if (!entries) { free(code_lengths); free(v66); free(v63); return GW2CMP_ERR_ALLOC; }
    }

    size_t n = 0;
    uint32_t v35 = 0;
    for (uint32_t length = 0; length <= (uint32_t)max_len; length++) {
        uint32_t sym = v66[length];
        while (sym != U32_NIL && v35 < ((uint32_t)1 << length) && sym < total_symbols) {
            entries[n].length = (uint8_t)length;
            entries[n].code = v35;
            entries[n].symbol = (uint16_t)sym;
            n++;
            v35 = v35 - 1; /* unsigned wraparound intended */
            sym = v63[sym];
        }
        v35 = 2u * v35 + 1u; /* unsigned wraparound intended */
    }

    free(code_lengths);
    free(v66);
    free(v63);

    if (n > 1) qsort(entries, n, sizeof(gw2cmp__huff_entry), gw2cmp__huff_cmp);

    out->entries = entries;
    out->count = n;
    return GW2CMP_OK;
}

static int gw2cmp__decode_symbol(gw2cmp__bitreader *br, const gw2cmp__huff_table *table, uint16_t *out_symbol) {
    uint32_t code = 0;
    gw2cmp__huff_entry key;
    for (int length = 1; length <= 24; length++) {
        code = (code << 1) | gw2cmp__br_read(br, 1);
        key.length = (uint8_t)length;
        key.code = code;
        const gw2cmp__huff_entry *hit = (const gw2cmp__huff_entry *)bsearch(
            &key, table->entries, table->count, sizeof(gw2cmp__huff_entry), gw2cmp__huff_cmp);
        if (hit) { *out_symbol = hit->symbol; return 1; }
    }
    return 0;
}

gw2cmp_status gw2cmp_decompress_method0(const uint8_t *comp, size_t comp_size,
                                         size_t output_size, uint8_t **out_data) {
    gw2cmp__bitreader br;
    gw2cmp__br_init(&br, comp, comp_size);

    uint32_t method = gw2cmp__br_read(&br, 4);
    if (method != 0) return GW2CMP_ERR_METHOD_UNSUPPORTED;
    uint32_t min_match_add = gw2cmp__br_read(&br, 4) + 1;

    uint8_t *out = (uint8_t *)malloc(output_size > 0 ? output_size : 1);
    if (!out) return GW2CMP_ERR_ALLOC;
    size_t out_len = 0;

    while (out_len < output_size) {
        gw2cmp__huff_table lit_table, dist_table;
        gw2cmp_status st = gw2cmp__build_huffman_table(&br, &lit_table);
        if (st != GW2CMP_OK) { free(out); return st; }
        st = gw2cmp__build_huffman_table(&br, &dist_table);
        if (st != GW2CMP_OK) { gw2cmp__huff_free(&lit_table); free(out); return st; }

        uint32_t block_symbols = (gw2cmp__br_read(&br, 4) + 1u) << 12;

        gw2cmp_status err = GW2CMP_OK;
        for (uint32_t i = 0; i < block_symbols && out_len < output_size; i++) {
            uint16_t sym;
            if (!gw2cmp__decode_symbol(&br, &lit_table, &sym)) { err = GW2CMP_ERR_HUFFMAN_DECODE; break; }

            if (sym < 0x100) {
                out[out_len++] = (uint8_t)sym;
            } else {
                uint32_t li = sym - 256;
                uint8_t extra = GW2CMP__LEN_EXTRA[li];
                uint32_t length = GW2CMP__LEN_BASE[li] + (extra ? gw2cmp__br_read(&br, extra) : 0) + min_match_add;

                uint16_t dsym;
                if (!gw2cmp__decode_symbol(&br, &dist_table, &dsym)) { err = GW2CMP_ERR_HUFFMAN_DECODE; break; }
                uint8_t dextra = GW2CMP__DIST_EXTRA[dsym];
                uint32_t dist = GW2CMP__DIST_BASE[dsym] + (dextra ? gw2cmp__br_read(&br, dextra) : 0);

                if (dist + 1 > out_len) { err = GW2CMP_ERR_BACKREF_RANGE; break; }
                size_t start = out_len - dist - 1;
                for (uint32_t k = 0; k < length && out_len < output_size; k++)
                    out[out_len++] = out[start + k];
            }
        }

        gw2cmp__huff_free(&lit_table);
        gw2cmp__huff_free(&dist_table);
        if (err != GW2CMP_OK) { free(out); return err; }
    }

    *out_data = out;
    return GW2CMP_OK;
}

gw2cmp_status gw2cmp_strip_crc32(const uint8_t *raw, size_t raw_size, uint8_t **out_data, size_t *out_size) {
    static const size_t CHUNK_SIZE = 0x10000;
    static const size_t START_INDEX = CHUNK_SIZE - 4;
    static const size_t END_INDEX = CHUNK_SIZE;

    uint8_t *data = (uint8_t *)malloc(raw_size > 0 ? raw_size : 1);
    if (!data) return GW2CMP_ERR_ALLOC;
    memcpy(data, raw, raw_size);
    size_t len = raw_size;

    if (raw_size > CHUNK_SIZE) {
        size_t position = 0;
        while (position + CHUNK_SIZE <= len) {
            size_t erase_start = position + START_INDEX;
            size_t erase_end = position + END_INDEX; /* == erase_start + 4 */
            memmove(data + erase_start, data + erase_end, len - erase_end);
            len -= 4;
            position += CHUNK_SIZE - 4;
        }
        if (len > 4) len -= 4;
    } else if (raw_size == CHUNK_SIZE) {
        memmove(data + START_INDEX, data + END_INDEX, len - END_INDEX);
        len -= 4;
    } else {
        if (len > 4) len -= 4;
    }

    *out_data = data;
    *out_size = len;
    return GW2CMP_OK;
}

gw2cmp_status gw2cmp_decompress_entry(const uint8_t *raw, size_t raw_size, uint8_t **out_data, size_t *out_size) {
    uint8_t *stripped = NULL;
    size_t stripped_size = 0;
    gw2cmp_status st = gw2cmp_strip_crc32(raw, raw_size, &stripped, &stripped_size);
    if (st != GW2CMP_OK) return st;

    if (stripped_size < 8) { free(stripped); return GW2CMP_ERR_TRUNCATED_HEADER; }

    uint32_t uncompressed_size = (uint32_t)stripped[4] | ((uint32_t)stripped[5] << 8) |
                                  ((uint32_t)stripped[6] << 16) | ((uint32_t)stripped[7] << 24);

    uint8_t *result = NULL;
    st = gw2cmp_decompress_method0(stripped + 8, stripped_size - 8, uncompressed_size, &result);
    free(stripped);
    if (st != GW2CMP_OK) return st;

    *out_data = result;
    *out_size = uncompressed_size;
    return GW2CMP_OK;
}

void gw2cmp_free(void *p) { free(p); }

const char *gw2cmp_status_string(gw2cmp_status status) {
    switch (status) {
        case GW2CMP_OK: return "ok";
        case GW2CMP_ERR_ALLOC: return "allocation failure";
        case GW2CMP_ERR_METHOD_UNSUPPORTED: return "unsupported CmpDecompress method (only Method 0 is implemented)";
        case GW2CMP_ERR_HUFFMAN_DECODE: return "malformed Huffman table or code (corrupt stream or decoder bug)";
        case GW2CMP_ERR_BACKREF_RANGE: return "LZ77 back-reference out of range (corrupt stream or decoder bug)";
        case GW2CMP_ERR_TRUNCATED_HEADER: return "input too small to contain the 8-byte header";
        default: return "unknown status";
    }
}

#endif /* GW2CMP_IMPLEMENTATION */

#endif /* GW2CMP_DECOMPRESS_METHOD0_H */
