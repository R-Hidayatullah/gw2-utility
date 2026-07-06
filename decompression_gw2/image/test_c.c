/* C17 test harness: decode mip0 with gw2_atex.h and dump RGBA. */
#define GW2_ATEX_IMPLEMENTATION
#include "gw2_atex.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    FILE *f, *o;
    long n;
    uint8_t *data;
    gw2_atex_tex t;
    uint8_t *rgba;
    int w, h, rc;
    if (argc < 3) { fprintf(stderr, "usage: %s in.bin out.raw\n", argv[0]); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    data = (uint8_t *)malloc(n);
    if (fread(data, 1, n, f) != (size_t)n) return 1;
    fclose(f);

    rc = gw2_atex_parse(data, (size_t)n, &t);
    if (rc != 0) { fprintf(stderr, "parse error %d\n", rc); return 1; }
    fprintf(stderr, "%s fmt=%s %dx%d mips=%d\n", argv[1],
            gw2_atex_format_name(t.fmt_enum), t.width, t.height, t.num_mips);
    rgba = gw2_atex_decode_mip(&t, 0, &w, &h);
    if (!rgba) { fprintf(stderr, "decode failed\n"); return 1; }
    o = fopen(argv[2], "wb");
    fwrite(rgba, 1, (size_t)w * h * 4, o);
    fclose(o);
    free(rgba);
    gw2_atex_free(&t);
    free(data);
    return 0;
}
