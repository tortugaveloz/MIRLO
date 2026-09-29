/* Runs an MRDP capture (lang/c/mrdp/test_mrdp_rand's format) through the C
 * model and prints the FNV-1a hashes of the colour and depth images:
 *   mrdp_cap_hash <capture> <fb> <zb> <w> <h>      ->  "<fb hash> <z hash>" */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../lang/c/mrdp/mrdp.h"

static uint8_t *mem;
static uint16_t rd16(void *c, uint32_t a) { (void)c; uint16_t v; memcpy(&v, mem + (a - 0x40000000u), 2); return v; }
static void wr16(void *c, uint32_t a, uint16_t v) { (void)c; memcpy(mem + (a - 0x40000000u), &v, 2); }
static uint32_t fnv(uint32_t addr, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { h ^= mem[addr - 0x40000000u + i]; h *= 16777619u; }
    return h;
}

int main(int argc, char **argv)
{
    if (argc < 6) return 2;
    mem = calloc(64u << 20, 1);
    static mrdp_t r;
    mrdp_init(&r, NULL, rd16, wr16);
    FILE *f = fopen(argv[1], "rb");
    uint32_t h[3];
    while (fread(h, 4, 3, f) == 3) {
        if (h[0] == 1) mrdp_push(&r, h[2]);
        else { if (fread(mem + (h[1] - 0x40000000u), 1, h[2], f) != h[2]) return 1; }
    }
    uint32_t fb = strtoul(argv[2], 0, 0), zb = strtoul(argv[3], 0, 0), w = atoi(argv[4]), hh = atoi(argv[5]);
    printf("0x%08x 0x%08x\n", fnv(fb, w * hh * 2), fnv(zb, w * hh * 2));
    fprintf(stderr, "model: %llu pixels, %u syncs, %u loads, %u unknown\n",
            (unsigned long long)r.pixels_drawn, r.sync_count, r.load_count, r.unknown_ops);
    return 0;
}
