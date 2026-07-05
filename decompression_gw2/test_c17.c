#include <stdio.h>
#include <stdlib.h>

#define GW2CMP_IMPLEMENTATION
#include "cmp_decompress_method0.h"

static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *out_size = (size_t)sz;
    return buf;
}

int main(int argc, char **argv) {
    const char *in_path = argc > 1 ? argv[1] : "compressed_16.bin";
    const char *out_path = argc > 2 ? argv[2] : "decompressed_c17.txt";

    size_t raw_size;
    uint8_t *raw = read_file(in_path, &raw_size);
    printf("input: %s (%zu bytes)\n", in_path, raw_size);

    uint8_t *result = NULL;
    size_t result_size = 0;
    gw2cmp_status st = gw2cmp_decompress_entry(raw, raw_size, &result, &result_size);
    if (st != GW2CMP_OK) {
        fprintf(stderr, "decompress failed: %s\n", gw2cmp_status_string(st));
        return 1;
    }
    printf("decompressed: %zu bytes\n", result_size);

    FILE *out = fopen(out_path, "wb");
    fwrite(result, 1, result_size, out);
    fclose(out);
    printf("written to: %s\n", out_path);

    gw2cmp_free(result);
    free(raw);
    return 0;
}
