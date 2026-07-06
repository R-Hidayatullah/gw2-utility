/* gw2_atex.h -- ArenaNet ATEX/ATEP/... texture decoder, single header (C17)
 *
 * Reverse-engineered from Gw2-64.exe sub_140B83040 (Arena\Engine\Gr\Img\ImgAtex.cpp).
 * stb-style single header: declarations always; define GW2_ATEX_IMPLEMENTATION in
 * exactly ONE translation unit before including to pull in the definitions.
 *
 *     #define GW2_ATEX_IMPLEMENTATION
 *     #include "gw2_atex.h"
 *
 *     gw2_atex_tex t;
 *     if (gw2_atex_parse(data, size, &t) == 0) {
 *         int w, h;
 *         unsigned char *rgba = gw2_atex_decode_mip(&t, 0, &w, &h);  // malloc'd w*h*4
 *         ... use rgba (RGBA, top-left) ...
 *         free(rgba);
 *         gw2_atex_free(&t);
 *     }
 *
 * Supported: DXT1/2/3/4/5, DXTA, DXTL, DXTN, 3DCX, BC5, BC7.  No dependencies
 * beyond the C standard library.
 */
#ifndef GW2_ATEX_H
#define GW2_ATEX_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      level, width, height;
    int      block_w, block_h, bytes_per_block;
    uint32_t flags;
    int      raw;                 /* flags == 0 */
    uint8_t *surface;             /* malloc'd, block_w*block_h*bytes_per_block */
    size_t   surface_size;
} gw2_atex_mip;

typedef struct {
    char          magic[4], fourcc[4];
    int           fmt_enum;
    int           width, height;
    int           num_mips;
    gw2_atex_mip *mips;           /* malloc'd array of num_mips */
} gw2_atex_tex;

/* Parse an ATEX blob and inflate every mip level. Returns 0 on success, <0 on error. */
int          gw2_atex_parse(const uint8_t *data, size_t n, gw2_atex_tex *out);

/* Free everything allocated by gw2_atex_parse. */
void         gw2_atex_free(gw2_atex_tex *t);

/* Format enum -> printable name ("DXT5", "BC7", ...). */
const char  *gw2_atex_format_name(int fmt_enum);

/* Decode a raw block surface to RGBA8888 (malloc'd w*h*4; caller frees). NULL on error. */
uint8_t     *gw2_atex_decode_surface(int fmt_enum, const uint8_t *surf, int w, int h);

/* Convenience: decode one mip of a parsed texture. Writes *w,*h. */
uint8_t     *gw2_atex_decode_mip(const gw2_atex_tex *t, int level, int *w, int *h);

#ifdef __cplusplus
}
#endif

/* ======================================================================== */
#ifdef GW2_ATEX_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- format tables (extracted verbatim from the binary) ---------------- */
static const uint32_t gw2__flags[38] = {
    0xB2,0xB2,0xB2,0xB2,0x12,0xB2,0x72,0x12,0x12,0x12,0x12,0x12,0x12,0x100,
    0x100,0x1A4,0x1A4,0x1A4,0x104,0xA2,0x78,0x400,0x71,0xB1,0xB1,0xB1,0xB1,
    0xA1,0x11,0x201,0x201,0x201,0xB1,0x00,0x00,0xB2,0x12,0x00 };

const char *gw2_atex_format_name(int e) {
    static const char *N[38] = {
        "ARGB32323232F","ARGB16161616F","ARGB2101010","ARGB8888","XRGB8888",
        "ARGB4444","ARGB1555","RGB888","RGB565","RGB555","RG1616","RG1616F",
        "RG3232F","R16F","R32F","AL88","AL44","AL8","L8","A8","P8","VU88",
        "DXT1","DXT2","DXT3","DXT4","DXT5","DXTA","DXTL","DXTN","3DCX","BC5",
        "BC7","D24","SHADOWMAP","ABGR8888","R32UINT","UNKNOWN" };
    return (e >= 0 && e < 38) ? N[e] : "UNKNOWN";
}

static int gw2__fourcc_enum(const uint8_t *f) {
    static const struct { char c[4]; int e; } m[] = {
        {{'D','X','T','1'},22},{{'D','X','T','2'},23},{{'D','X','T','3'},24},
        {{'D','X','T','4'},25},{{'D','X','T','5'},26},{{'D','X','T','A'},27},
        {{'D','X','T','L'},28},{{'D','X','T','N'},29},{{'3','D','C','X'},30},
        {{'B','C','5','X'},31},{{'B','C','7','X'},32} };
    int i;
    for (i = 0; i < (int)(sizeof m / sizeof m[0]); ++i)
        if (memcmp(f, m[i].c, 4) == 0) return m[i].e;
    return -1;
}

static void gw2__huff(uint8_t len[64], uint8_t val[64]) {
    int k;
    for (k = 0; k < 16; ++k) { len[k] = 6; val[k] = (uint8_t)(16 - k); }
    for (k = 16; k < 32; ++k) { len[k] = 2; val[k] = 0x11; }
    for (k = 32; k < 64; ++k) { len[k] = 1; val[k] = 0x00; }
}

/* ---- MSB-first bit reader fed by 32-bit little-endian words ------------- */
typedef struct { const uint8_t *buf; size_t pos, end; uint64_t head; int bits; } gw2__br;

static void gw2__br_init(gw2__br *b, const uint8_t *buf, size_t s, size_t e) {
    b->buf = buf; b->pos = s; b->end = e; b->head = 0; b->bits = 0;
}
static void gw2__br_pull(gw2__br *b) {
    uint32_t w = 0; int i;
    for (i = 0; i < 4; ++i)
        if (b->pos + i < b->end) w |= (uint32_t)b->buf[b->pos + i] << (8 * i);
    b->pos += 4;
    b->head |= (uint64_t)w << (32 - b->bits);
    b->bits += 32;
}
static uint64_t gw2__br_read(gw2__br *b, int n) {
    uint64_t v;
    if (n == 0) return 0;
    while (b->bits < n) gw2__br_pull(b);
    v = b->head >> (64 - n);
    b->head <<= n; b->bits -= n;
    return v;
}
static void gw2__br_run(gw2__br *b, const uint8_t *hl, const uint8_t *hv,
                        int *count, int *filled) {
    uint32_t k; int clen, sym;
    while (b->bits < 7) gw2__br_pull(b);
    k = (uint32_t)(b->head >> 58);
    clen = hl[k]; sym = hv[k];
    b->head <<= clen; b->bits -= clen;
    *filled = (int)gw2__br_read(b, 1);
    *count = sym + 1;
}
static void gw2__br_align(gw2__br *b) {
    if (b->bits >= 32) b->pos -= 4;
    b->head = 0; b->bits = 0;
}

/* ---- encode_bc1_color : port of sub_140BF7470 -------------------------- */
static void gw2__enc_bc1(uint32_t rgb, int is_dxt1, uint8_t out[8]) {
    int v3 = rgb & 0xFF, v4 = (rgb >> 8) & 0xFF, v5 = (rgb >> 16) & 0xFF;
    int v6 = v4 - (v4 >> 6);
    int v7 = (v3 - (v3 >> 5)) >> 3;
    int v8 = v5 >> 5;
    int v9 = 8 * v7 + ((v3 - (v3 >> 5)) >> 5);
    int v31 = v6 >> 2;
    int v32 = (v5 - v8) >> 3;
    int v10 = v7 + 1, v30 = v7;
    int v11 = 4 * (v6 >> 2);
    int v12 = v11 + (v6 >> 6);
    int den1 = ((v7 + 1) >> 2) - v9 + 8 * v7 + 8;
    int v13 = den1 ? 12 * (v3 - v9) / den1 : 0;
    int v14 = (v6 >> 2) + 1;
    int v15 = v32 + 1;
    int den2 = (v14 >> 4) - v12 + 4 + v11;
    int v16 = den2 ? 12 * (v4 - v12) / den2 : 0;
    int den3 = ((v32 + 1) >> 2) - ((v5 - v8) >> 5) + 8;
    int v17 = den3 ? 12 * (v5 - (8 * v32 + ((v5 - v8) >> 5))) / den3 : 0;
    int v18, v19, v20, v22, v23, v24, v25, v26, v21;
    uint32_t idx;

    if (v13 < 2) { v10 = v30; v18 = v10; }
    else if (v13 >= 0xA) v18 = v10;
    else if (v13 >= 6) { v18 = v10; v10 = v30; }
    else v18 = v30;

    if (v16 < 2) { v14 = v31; v19 = v14; }
    else if (v16 >= 0xA) v19 = v14;
    else if (v16 >= 6) { v19 = v14; v14 = v31; }
    else v19 = v31;

    if (v17 >= 2) {
        if (v17 < 6) v20 = v32;
        else if (v17 < 0xA) { v20 = v32 + 1; v15 = v32; }
        else v20 = v15;
    } else { v15 = v32; v20 = v15; }

    v22 = (v18 | (32 * (v19 | (v20 << 6)))) & 0xFFFF;
    v23 = (v10 | (32 * (v14 | (v15 << 6)))) & 0xFFFF;

    v24 = 0; v25 = 0;
    if (v18 != v10) { v24 = v13; v25 = 1; if (v18 != v30) v24 = 12 - v13; }
    if (v19 != v14) { if (v19 != v31) v16 = 12 - v16; v24 += v16; ++v25; }
    if (v20 != v15) { if (v20 != v32) v17 = 12 - v17; v24 += v17; ++v25; }
    if (v25) v24 = (v24 + (v25 >> 1)) / v25;

    v26 = (is_dxt1 && ((v24 - 5 >= 0 && v24 - 5 <= 1) || v25 == 0)) ? 1 : 0;
    if (v25 == 0 && !v26) {
        if (v23 == 0xFFFF) { v24 = 12; v22 = (v22 - 1) & 0xFFFF; }
        else { v24 = 0; v23 = (v23 + 1) & 0xFFFF; }
    }
    if (v26 != (v22 <= v23 ? 1 : 0)) { int t = v22; v22 = v23; v23 = t; v24 = 12 - v24; }

    v21 = 0;
    if (!v26) { if (v24 >= 2) { if (v24 < 6) v21 = 2; else { v21 = 1; if (v24 < 0xA) v21 = 3; } } }
    else v21 = 2;

    idx = (uint32_t)(v21 | (4 * v21) | (16 * (v21 | (4 * v21))));
    idx = (idx | (idx << 8)) & 0xFFFF;
    idx = (idx | (idx << 16));
    out[0] = v22 & 0xFF; out[1] = (v22 >> 8) & 0xFF;
    out[2] = v23 & 0xFF; out[3] = (v23 >> 8) & 0xFF;
    out[4] = idx & 0xFF; out[5] = (idx >> 8) & 0xFF;
    out[6] = (idx >> 16) & 0xFF; out[7] = (idx >> 24) & 0xFF;
}

/* ---- terrain-atlas border mirroring : port of sub_140B82AF0 ------------ */
static uint32_t gw2__ror4(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }
static uint32_t gw2__bswap(uint32_t x) {
    return ((x >> 24) & 0xFF) | ((x >> 8) & 0xFF00) |
           ((x << 8) & 0xFF0000) | ((x << 24) & 0xFF000000);
}
static void gw2__terrain(uint8_t *surf, int nb) {
    const uint32_t B = 0xC0000003u; int i;
    for (i = 0; i < nb; ++i) {
        uint32_t v5 = (1u << (i >> 6)) & B;
        uint32_t v6 = (1u << (i & 31)) & B;
        int v7, v8, src; uint32_t a, b, c, d;
        if (!(v6 || v5)) continue;
        v7 = v6 ? ((i & 0x3F) ^ 3) : (i & 0x3F);
        v8 = v5 ? ((i >> 6) ^ 3) : (i >> 6);
        src = 16 * (v7 + (v8 << 6));
        memcpy(&a, surf + src + 0, 4); memcpy(&b, surf + src + 4, 4);
        memcpy(&c, surf + src + 8, 4); memcpy(&d, surf + src + 12, 4);
        if (v6) {
            b = a;
            a = (uint32_t)((16u * ((a & 0xF000F0u) | ((a & 0xFFFF000Fu) << 8))) |
                          (((a & 0xF000F00u) | ((a >> 8) & 0xF000F0u)) >> 4));
            d = (uint32_t)(((( d & 0x30303030u) | ((d >> 4) & 0xC0C0C0Cu)) >> 2) |
                          (4u * ((d & 0xC0C0C0Cu) | (16u * (d & 0xFF030303u)))));
        }
        if (v5) { uint32_t t = gw2__ror4(b, 16); b = gw2__ror4(a, 16); a = t; d = gw2__bswap(d); }
        memcpy(surf + 16 * i + 0, &a, 4); memcpy(surf + 16 * i + 4, &b, 4);
        memcpy(surf + 16 * i + 8, &c, 4); memcpy(surf + 16 * i + 12, &d, 4);
    }
}

/* ---- bitmap helpers ---------------------------------------------------- */
static int  gw2__bt(const uint32_t *bm, int i) { return (bm[i >> 5] >> (i & 31)) & 1; }
static void gw2__bs(uint32_t *bm, int i) { bm[i >> 5] |= (1u << (i & 31)); }

/* ---- RLE constant fill ------------------------------------------------- */
static void gw2__rle(gw2__br *br, const uint8_t *hl, const uint8_t *hv,
                     uint8_t *surf, int nb, int bpb, int off,
                     const uint8_t *val, int n, uint32_t *rd,
                     uint32_t *m0, uint32_t *m1, int two) {
    uint8_t zero[8] = {0,0,0,0,0,0,0,0};
    int i = 0;
    while (i < nb) {
        int count, filled;
        const uint8_t *cur;
        gw2__br_run(br, hl, hv, &count, &filled);
        cur = val;
        if (filled && two) cur = gw2__br_read(br, 1) ? val : zero;
        while (count > 0 && i < nb) {
            if (!gw2__bt(rd, i)) {
                if (filled) {
                    memcpy(surf + i * bpb + off, cur, n);
                    if (m0) gw2__bs(m0, i);
                    if (m1) gw2__bs(m1, i);
                }
                --count;
            }
            ++i;
        }
        while (i < nb && gw2__bt(rd, i)) ++i;
    }
}

/* ---- per-mip inflate --------------------------------------------------- */
static int gw2__inflate(const uint8_t *buf, size_t ps, size_t pe, uint32_t flags,
                        int fe, int bw, int bh, uint8_t **surf_out, int *bpb_out) {
    uint32_t ff = gw2__flags[fe];
    int a2 = (ff & 0x280) ? 2 : 0;
    int dxtl = (fe == 28) ? 2 : 0;
    int a210 = (ff & 0x210) ? 2 : 0;
    int units = a2 + a210 + dxtl;
    int bpb = 4 * units;
    int nb = bw * bh;
    int has_A = (ff & 0x280) || fe == 28;
    int has_BC = (ff & 0x210) != 0;
    int offA = 0, offB = 4 * (dxtl + a2), offC = 4 * (dxtl + a2 + 1);
    int nw = (nb + 31) >> 5, is256, do_terr, i;
    uint32_t *bmA, *bmB;
    uint8_t hl[64], hv[64];
    gw2__br br;
    size_t pos;
    uint8_t *surf = (uint8_t *)calloc((size_t)nb * bpb, 1);
    if (!surf) return -1;
    bmA = (uint32_t *)calloc(nw > 0 ? nw : 1, sizeof(uint32_t));
    bmB = (uint32_t *)calloc(nw > 0 ? nw : 1, sizeof(uint32_t));
    if (!bmA || !bmB) { free(surf); free(bmA); free(bmB); return -1; }

    is256 = (bw == 64 && bh == 64);
    do_terr = (flags & 0x10) && is256 && (fe == 23 || fe == 24);
    if (do_terr) {
        const uint32_t B = 0xC0000003u;
        for (i = 0; i < nb; ++i)
            if (((1u << (i & 0x3F)) & B) || ((1u << (i >> 6)) & B)) { gw2__bs(bmA, i); gw2__bs(bmB, i); }
    }

    gw2__huff(hl, hv);
    gw2__br_init(&br, buf, ps, pe);

    if ((flags & 0x01) && (ff & 0x210) && !(ff & 0x280) && fe != 28) {
        uint8_t val[8]; memset(val, 0xFF, 8);
        gw2__rle(&br, hl, hv, surf, nb, bpb, offB, val, 8, bmB, bmA, bmB, 0);
    }
    if ((flags & 0x02) && (fe == 23 || fe == 24)) {
        int nib = (int)gw2__br_read(&br, 4);
        uint8_t b = (uint8_t)((nib | (nib << 4)) & 0xFF), val[8];
        memset(val, b, 8);
        gw2__rle(&br, hl, hv, surf, nb, bpb, offA, val, 8, bmB, bmA, NULL, 1);
    }
    if ((flags & 0x04) && fe >= 25 && fe <= 28) {
        uint8_t a = (uint8_t)gw2__br_read(&br, 8);
        uint8_t val[8] = { 0,0,0,0,0,0,0,0 };
        val[0] = a; val[1] = a;
        gw2__rle(&br, hl, hv, surf, nb, bpb, offA, val, 8, bmB, bmA, NULL, 1);
    }
    if ((flags & 0x08) && has_BC) {
        uint32_t rgb = (uint32_t)gw2__br_read(&br, 24);
        uint8_t val[8];
        gw2__enc_bc1(rgb, fe == 22, val);
        gw2__rle(&br, hl, hv, surf, nb, bpb, offB, val, 8, bmB, bmB, NULL, 0);
    }

    gw2__br_align(&br);
    pos = br.pos;
    if (has_A) {
        for (i = 0; i < nb; ++i) if (!gw2__bt(bmA, i)) {
            size_t av = (pos < pe) ? (pe - pos < 8 ? pe - pos : 8) : 0;
            if (av) memcpy(surf + i * bpb + offA, buf + pos, av);
            pos += 8;
        }
    }
    if (has_BC) {
        for (i = 0; i < nb; ++i) if (!gw2__bt(bmB, i)) {
            size_t av = (pos < pe) ? (pe - pos < 4 ? pe - pos : 4) : 0;
            if (av) memcpy(surf + i * bpb + offB, buf + pos, av);
            pos += 4;
        }
        for (i = 0; i < nb; ++i) if (!gw2__bt(bmB, i)) {
            size_t av = (pos < pe) ? (pe - pos < 4 ? pe - pos : 4) : 0;
            if (av) memcpy(surf + i * bpb + offC, buf + pos, av);
            pos += 4;
        }
    }
    if (do_terr) gw2__terrain(surf, nb);

    free(bmA); free(bmB);
    *surf_out = surf; *bpb_out = bpb;
    return 0;
}

/* ---- parse ------------------------------------------------------------- */
int gw2_atex_parse(const uint8_t *data, size_t n, gw2_atex_tex *t) {
    static const char *MAG[6] = { "ATEX","ATTX","ATEC","ATEP","ATEU","ATET" };
    int fe, ok = 0, i, w, h, level = 0, block, cap = 8;
    size_t pos = 12;
    if (n < 12) return -1;
    for (i = 0; i < 6; ++i) if (memcmp(data, MAG[i], 4) == 0) ok = 1;
    if (!ok) return -2;
    fe = gw2__fourcc_enum(data + 4);
    if (fe < 0) return -3;

    memset(t, 0, sizeof *t);
    memcpy(t->magic, data, 4); memcpy(t->fourcc, data + 4, 4);
    t->fmt_enum = fe;
    t->width = data[8] | (data[9] << 8);
    t->height = data[10] | (data[11] << 8);
    t->mips = (gw2_atex_mip *)calloc(cap, sizeof(gw2_atex_mip));
    if (!t->mips) return -1;

    w = t->width; h = t->height;
    block = gw2__flags[fe] & 1;
    while (pos + 8 <= n && w >= 1 && h >= 1) {
        uint32_t ds, flags; size_t pe; int bw, bh;
        gw2_atex_mip *mp;
        memcpy(&ds, data + pos, 4); memcpy(&flags, data + pos + 4, 4);
        if (ds < 8) break;
        pe = (pos + ds < n) ? pos + ds : n;
        bw = block ? (w + 3) >> 2 : w;
        bh = block ? (h + 3) >> 2 : h;
        if (level >= cap) {
            cap *= 2;
            t->mips = (gw2_atex_mip *)realloc(t->mips, cap * sizeof(gw2_atex_mip));
            if (!t->mips) return -1;
        }
        mp = &t->mips[level];
        memset(mp, 0, sizeof *mp);
        mp->level = level; mp->width = w; mp->height = h;
        mp->block_w = bw; mp->block_h = bh; mp->flags = flags; mp->raw = (flags == 0);
        if (gw2__inflate(data, pos + 8, pe, flags, fe, bw, bh, &mp->surface, &mp->bytes_per_block) != 0)
            return -1;
        mp->surface_size = (size_t)bw * bh * mp->bytes_per_block;
        ++level;
        pos += ds;
        w = w > 1 ? w >> 1 : 1;
        h = h > 1 ? h >> 1 : 1;
    }
    t->num_mips = level;
    return 0;
}

void gw2_atex_free(gw2_atex_tex *t) {
    int i;
    if (!t || !t->mips) return;
    for (i = 0; i < t->num_mips; ++i) free(t->mips[i].surface);
    free(t->mips);
    t->mips = NULL; t->num_mips = 0;
}

/* ======================================================================== */
/*  Block decoders                                                          */
/* ======================================================================== */
static void gw2__565(int c, int *r, int *g, int *b) {
    int rr = (c >> 11) & 0x1F, gg = (c >> 5) & 0x3F, bb = c & 0x1F;
    *r = (rr << 3) | (rr >> 2); *g = (gg << 2) | (gg >> 4); *b = (bb << 3) | (bb >> 2);
}
static void gw2__bc1(const uint8_t *blk, int off, int dxt1a, uint8_t out[16][4]) {
    int c0 = blk[off] | (blk[off + 1] << 8), c1 = blk[off + 2] | (blk[off + 3] << 8);
    uint32_t bits; int r0, g0, b0, r1, g1, b1, k; uint8_t pal[4][4];
    memcpy(&bits, blk + off + 4, 4);
    gw2__565(c0, &r0, &g0, &b0); gw2__565(c1, &r1, &g1, &b1);
    pal[0][0] = r0; pal[0][1] = g0; pal[0][2] = b0; pal[0][3] = 255;
    pal[1][0] = r1; pal[1][1] = g1; pal[1][2] = b1; pal[1][3] = 255;
    if (c0 > c1 || !dxt1a) {
        pal[2][0] = (2*r0+r1)/3; pal[2][1] = (2*g0+g1)/3; pal[2][2] = (2*b0+b1)/3; pal[2][3] = 255;
        pal[3][0] = (r0+2*r1)/3; pal[3][1] = (g0+2*g1)/3; pal[3][2] = (b0+2*b1)/3; pal[3][3] = 255;
    } else {
        pal[2][0] = (r0+r1)/2; pal[2][1] = (g0+g1)/2; pal[2][2] = (b0+b1)/2; pal[2][3] = 255;
        pal[3][0] = 0; pal[3][1] = 0; pal[3][2] = 0; pal[3][3] = 0;
    }
    for (k = 0; k < 16; ++k) { int idx = (bits >> (2*k)) & 3; int j; for (j = 0; j < 4; ++j) out[k][j] = pal[idx][j]; }
}
static void gw2__bc3a(const uint8_t *blk, int off, uint8_t out[16]) {
    int a0 = blk[off], a1 = blk[off + 1], lut[8], i, k; uint64_t bits = 0;
    lut[0] = a0; lut[1] = a1; for (i = 2; i < 8; ++i) lut[i] = 0;
    if (a0 > a1) for (i = 1; i < 7; ++i) lut[i + 1] = ((7 - i)*a0 + i*a1)/7;
    else { for (i = 1; i < 5; ++i) lut[i + 1] = ((5 - i)*a0 + i*a1)/5; lut[6] = 0; lut[7] = 255; }
    for (i = 0; i < 6; ++i) bits |= (uint64_t)blk[off + 2 + i] << (8*i);
    for (k = 0; k < 16; ++k) out[k] = (uint8_t)lut[(bits >> (3*k)) & 7];
}
static void gw2__bc2a(const uint8_t *blk, int off, uint8_t out[16]) {
    int k;
    for (k = 0; k < 16; ++k) { int a = (blk[off + (k >> 1)] >> ((k & 1) * 4)) & 0xF; out[k] = (a << 4) | a; }
}
static void gw2__place(uint8_t *img, int W, int H, int bx, int by, uint8_t px[16][4]) {
    int x0 = bx * 4, y0 = by * 4, i, j;
    for (j = 0; j < 4; ++j) { int y = y0 + j; if (y >= H) break;
        for (i = 0; i < 4; ++i) { int x = x0 + i; uint8_t *d; if (x >= W) break;
            d = img + (size_t)(y*W + x)*4;
            d[0] = px[j*4+i][0]; d[1] = px[j*4+i][1]; d[2] = px[j*4+i][2]; d[3] = px[j*4+i][3];
        } }
}

/* ---- BC7 --------------------------------------------------------------- */
typedef struct { int ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2; } gw2__bc7mode;
static const gw2__bc7mode gw2__BC7M[8] = {
    {3,4,0,0,4,0,1,0,3,0},{2,6,0,0,6,0,0,1,3,0},{3,6,0,0,5,0,0,0,2,0},
    {2,6,0,0,7,0,1,0,2,0},{1,0,2,1,5,6,0,0,2,3},{1,0,2,0,7,8,0,0,2,2},
    {1,0,0,0,7,7,1,0,4,0},{2,6,0,0,5,5,1,0,2,0} };
static const uint16_t gw2__P2[64] = {
    0xCCCC,0x8888,0xEEEE,0xECC8,0xC880,0xFEEC,0xFEC8,0xEC80,0xC800,0xFFEC,
    0xFE80,0xE800,0xFFE8,0xFF00,0xFFF0,0xF000,0xF710,0x008E,0x7100,0x08CE,
    0x008C,0x7310,0x3100,0x8CCE,0x088C,0x3110,0x6666,0x366C,0x17E8,0x0FF0,
    0x718E,0x399C,0xAAAA,0xF0F0,0x5A5A,0x33CC,0x3C3C,0x55AA,0x9696,0xA55A,
    0x73CE,0x13C8,0x324C,0x3BDC,0x6996,0xC33C,0x9966,0x0660,0x0272,0x04E4,
    0x4E40,0x2720,0xC936,0x936C,0x39C6,0x639C,0x9336,0x9CC6,0x817E,0xE718,
    0xCCF0,0x0FCC,0x7744,0xEE22 };
static const uint32_t gw2__P3[64] = {
    0xAA685050,0x6A5A5040,0x5A5A4200,0x5450A0A8,0xA5A50000,0xA0A05050,0x5555A0A0,
    0x5A5A5050,0xAA550000,0xAA555500,0xAAAA5500,0x90909090,0x94949494,0xA4A4A4A4,
    0xA9A59450,0x2A0A4250,0xA5945040,0x0A425054,0xA5A5A500,0x55A0A0A0,0xA8A85454,
    0x6A6A4040,0xA4A45000,0x1A1A0500,0x0050A4A4,0xAAA59090,0x14696914,0x69691400,
    0xA08585A0,0xAA821414,0x50A4A450,0x6A5A0200,0xA9A58000,0x5090A0A8,0xA8A09050,
    0x24242424,0x00AA5500,0x24924924,0x24499224,0x50A50A50,0x500AA550,0xAAAA4444,
    0x66660000,0xA5A0A5A0,0x50A050A0,0x69286928,0x44AAAA44,0x66666600,0xAA444444,
    0x54A854A8,0x95809580,0x96969600,0xA85454A8,0x80959580,0xAA141414,0x96960000,
    0xAAAA1414,0xA05050A0,0xA0A5A5A0,0x96000000,0x40804080,0xA9A8A9A8,0xAAAAAA44,
    0x2A4A5254 };
static const uint8_t gw2__A2[64] = {
    15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,2,8,2,2,8,8,15,2,8,2,2,8,8,2,2,
    15,15,6,8,2,8,15,15,2,8,2,2,2,15,15,6,6,2,6,8,15,15,2,2,15,15,15,15,15,2,2,15 };
static const uint8_t gw2__A3a[64] = {
    3,3,15,15,8,3,15,15,8,8,6,6,6,5,3,3,3,3,8,15,3,3,6,10,5,8,8,6,8,5,15,15,
    8,15,3,5,6,10,8,15,15,3,15,5,15,15,15,15,3,15,5,5,5,8,5,10,5,10,8,13,15,12,3,3 };
static const uint8_t gw2__A3b[64] = {
    15,8,8,3,15,15,3,8,15,15,15,15,15,15,15,8,15,8,15,3,15,8,15,8,3,15,6,10,15,15,10,8,
    15,3,15,10,10,8,9,10,6,15,8,15,3,6,6,8,15,3,15,15,15,15,15,15,15,15,15,15,3,15,15,8 };
static int gw2__bc7w(int bits, int i) {
    static const int w2[4] = {0,21,43,64};
    static const int w3[8] = {0,9,18,27,37,46,55,64};
    static const int w4[16] = {0,4,9,13,17,21,26,30,34,38,43,47,51,55,60,64};
    return bits == 2 ? w2[i] : bits == 3 ? w3[i] : w4[i];
}
static int gw2__bc7i(int e0, int e1, int idx, int bits) {
    int w = gw2__bc7w(bits, idx);
    return (e0 * (64 - w) + e1 * w + 32) >> 6;
}
typedef struct { const uint8_t *d; int pos; } gw2__lsb;
static int gw2__lget(gw2__lsb *s, int n) {
    int v = 0, i;
    for (i = 0; i < n; ++i) { v |= ((s->d[s->pos >> 3] >> (s->pos & 7)) & 1) << i; ++s->pos; }
    return v;
}
static void gw2__bc7(const uint8_t *blk, uint8_t out[16][4]) {
    gw2__lsb bs; int mode = 0, ns, ne, part, rot, isb, cb, ab, i, k;
    const gw2__bc7mode *m;
    int r[6], g[6], b[6], a[6], parts[16], anchors[3], idx1[16], idx2[16], has2, ib, ib2;
    bs.d = blk; bs.pos = 0;
    while (mode < 8 && gw2__lget(&bs, 1) == 0) ++mode;
    if (mode == 8) { for (k = 0; k < 16; ++k) { out[k][0]=out[k][1]=out[k][2]=0; out[k][3]=255; } return; }
    m = &gw2__BC7M[mode]; ns = m->ns; ne = ns * 2;
    part = m->pb ? gw2__lget(&bs, m->pb) : 0;
    rot = m->rb ? gw2__lget(&bs, m->rb) : 0;
    isb = m->isb ? gw2__lget(&bs, m->isb) : 0;
    cb = m->cb; ab = m->ab;
    for (i = 0; i < ne; ++i) r[i] = gw2__lget(&bs, cb);
    for (i = 0; i < ne; ++i) g[i] = gw2__lget(&bs, cb);
    for (i = 0; i < ne; ++i) b[i] = gw2__lget(&bs, cb);
    for (i = 0; i < ne; ++i) a[i] = ab ? gw2__lget(&bs, ab) : 255;
    if (m->epb) {
        int pbv[6]; for (i = 0; i < ne; ++i) pbv[i] = gw2__lget(&bs, 1);
        for (i = 0; i < ne; ++i) { r[i]=(r[i]<<1)|pbv[i]; g[i]=(g[i]<<1)|pbv[i]; b[i]=(b[i]<<1)|pbv[i]; if (ab) a[i]=(a[i]<<1)|pbv[i]; }
        cb++; if (ab) ab++;
    } else if (m->spb) {
        int sp[2]; sp[0]=gw2__lget(&bs,1); sp[1]=gw2__lget(&bs,1);
        for (i = 0; i < ne; ++i) { int s = sp[i/(ne/2)]; r[i]=(r[i]<<1)|s; g[i]=(g[i]<<1)|s; b[i]=(b[i]<<1)|s; if (ab) a[i]=(a[i]<<1)|s; }
        cb++; if (ab) ab++;
    }
    for (i = 0; i < ne; ++i) {
        int vr=r[i]<<(8-cb), vg=g[i]<<(8-cb), vb=b[i]<<(8-cb);
        r[i]=vr|(vr>>cb); g[i]=vg|(vg>>cb); b[i]=vb|(vb>>cb);
        if (ab) { int va=a[i]<<(8-ab); a[i]=va|(va>>ab); } else a[i]=255;
    }
    ib = m->ib; ib2 = m->ib2; has2 = ib2 != 0;
    anchors[0]=0; anchors[1]=0; anchors[2]=0;
    if (ns == 1) { for (k=0;k<16;++k) parts[k]=0; }
    else if (ns == 2) { for (k=0;k<16;++k) parts[k]=(gw2__P2[part]>>k)&1; anchors[1]=gw2__A2[part]; }
    else { for (k=0;k<16;++k) parts[k]=(gw2__P3[part]>>(2*k))&3; anchors[1]=gw2__A3a[part]; anchors[2]=gw2__A3b[part]; }
    for (k = 0; k < 16; ++k) {
        int anc = 0, s; for (s = 0; s < ns; ++s) if (k == anchors[s]) anc = 1;
        idx1[k] = gw2__lget(&bs, anc ? ib - 1 : ib);
    }
    if (has2) for (k = 0; k < 16; ++k) idx2[k] = gw2__lget(&bs, k == 0 ? ib2 - 1 : ib2);
    for (k = 0; k < 16; ++k) {
        int s = parts[k], e0 = s*2, e1 = s*2+1, ci, cbits, ai, abits, cr, cg, cbl, ca;
        if (!has2) { ci = ai = idx1[k]; cbits = abits = ib; }
        else if (isb == 0) { ci = idx1[k]; cbits = ib; ai = idx2[k]; abits = ib2; }
        else { ci = idx2[k]; cbits = ib2; ai = idx1[k]; abits = ib; }
        cr = gw2__bc7i(r[e0], r[e1], ci, cbits);
        cg = gw2__bc7i(g[e0], g[e1], ci, cbits);
        cbl = gw2__bc7i(b[e0], b[e1], ci, cbits);
        ca = m->ab ? gw2__bc7i(a[e0], a[e1], ai, abits) : 255;
        if (rot == 1) { int t = ca; ca = cr; cr = t; }
        else if (rot == 2) { int t = ca; ca = cg; cg = t; }
        else if (rot == 3) { int t = ca; ca = cbl; cbl = t; }
        out[k][0]=(uint8_t)cr; out[k][1]=(uint8_t)cg; out[k][2]=(uint8_t)cbl; out[k][3]=(uint8_t)ca;
    }
}

/* ---- surface -> RGBA --------------------------------------------------- */
uint8_t *gw2_atex_decode_surface(int fe, const uint8_t *surf, int W, int H) {
    int bw = (W + 3) / 4, bh = (H + 3) / 4, bx, by, k;
    uint8_t px[16][4], c[16][4], al[16], ch0[16], ch1[16];
    uint8_t *img = (uint8_t *)calloc((size_t)W * H * 4, 1);
    if (!img) return NULL;
    for (by = 0; by < bh; ++by) for (bx = 0; bx < bw; ++bx) {
        int bi = by * bw + bx;
        switch (fe) {
        case 22: gw2__bc1(surf, bi*8, 1, px); gw2__place(img,W,H,bx,by,px); break;
        case 23: case 24: {
            int o = bi*16; gw2__bc1(surf,o+8,0,c); gw2__bc2a(surf,o,al);
            for (k=0;k<16;++k){px[k][0]=c[k][0];px[k][1]=c[k][1];px[k][2]=c[k][2];px[k][3]=al[k];}
            gw2__place(img,W,H,bx,by,px); break; }
        case 25: case 26: {
            int o = bi*16; gw2__bc1(surf,o+8,0,c); gw2__bc3a(surf,o,al);
            for (k=0;k<16;++k){px[k][0]=c[k][0];px[k][1]=c[k][1];px[k][2]=c[k][2];px[k][3]=al[k];}
            gw2__place(img,W,H,bx,by,px); break; }
        case 27: {
            gw2__bc1(surf, bi*8, 0, c);
            for (k=0;k<16;++k){px[k][0]=px[k][1]=px[k][2]=c[k][0];px[k][3]=255;}
            gw2__place(img,W,H,bx,by,px); break; }
        case 28: {
            int o = bi*16; gw2__bc1(surf,o+8,0,c); gw2__bc3a(surf,o,al);
            for (k=0;k<16;++k){px[k][0]=c[k][0]*al[k]/255;px[k][1]=c[k][1]*al[k]/255;px[k][2]=c[k][2]*al[k]/255;px[k][3]=255;}
            gw2__place(img,W,H,bx,by,px); break; }
        case 29: case 30: case 31: {
            int o = bi*16; gw2__bc3a(surf,o,ch0); gw2__bc3a(surf,o+8,ch1);
            for (k=0;k<16;++k){
                double fx=ch0[k]/127.5-1.0, fy=ch1[k]/127.5-1.0, fz=1.0-fx*fx-fy*fy;
                int z = fz>0 ? (int)((sqrt(fz)*0.5+0.5)*255) : 128;
                px[k][0]=ch0[k];px[k][1]=ch1[k];px[k][2]=(uint8_t)z;px[k][3]=255;
            }
            gw2__place(img,W,H,bx,by,px); break; }
        case 32: gw2__bc7(surf + bi*16, px); gw2__place(img,W,H,bx,by,px); break;
        default: break;
        }
    }
    return img;
}

uint8_t *gw2_atex_decode_mip(const gw2_atex_tex *t, int level, int *w, int *h) {
    const gw2_atex_mip *m;
    if (!t || level < 0 || level >= t->num_mips) return NULL;
    m = &t->mips[level];
    if (w) *w = m->width;
    if (h) *h = m->height;
    return gw2_atex_decode_surface(t->fmt_enum, m->surface, m->width, m->height);
}

#endif /* GW2_ATEX_IMPLEMENTATION */
#endif /* GW2_ATEX_H */
