// Check of src/gfx/tex/nvn_deswizzle.c nvn_swizzle_region (WriteTexels' region path): for random textures (2D, 2D
// arrays, 3D; formats of 1-16 bytes a block, 4x4 compressed ones; levels; layers) and random regions, the region written
// straight into the storage must leave the storage byte for byte as the whole-level way does (deswizzle the level,
// patch the region in, swizzle back). Build and run from the repository (MSYS2 CLANG64 clang):
//   clang -O2 -Isrc/gfx -Isrc/gfx/tex -Isrc/gfx/nvn tools/tex_region_check.c src/gfx/tex/nvn_deswizzle.c src/gfx/nvn/nvn_tex.c -o tex_region_check.exe
// Exit code 0 and "0 differences": the two agree.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_deswizzle.h"

static uint64_t rng = 0x243f6a8885a308d3ull;
static uint32_t rnd(uint32_t n) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return n ? (uint32_t)(rng % n) : 0; }
static uint32_t mip(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }

int main(void) {
    static const int formats[] = {0x01, 0x05, 0x0A, 0x16, 0x2E, 0x19, 0x1D, 0x22, 0x41, 0x47, 0x4D};
    static const int targets[] = {1, 1, 1, 2, 5 - 1};   // 2D (more often), 3D, 2D array (4)
    unsigned checks = 0, diffs = 0, skipped = 0;
    for (int it = 0; it < 4000; it++) {
        nvn_texdesc d;
        memset(&d, 0, sizeof d);
        d.target = targets[rnd(5)];
        d.format = formats[rnd(sizeof formats / sizeof *formats)];
        d.width = 1 + rnd(700); d.height = 1 + rnd(500);
        d.depth = d.target == 2 ? 1 + rnd(40) : d.target == 4 ? 1 + rnd(6) : 1;
        d.levels = 1 + rnd(6);
        nvn_texlayout L;
        nvn_tex_layout(&d, &L);
        if (!L.size || L.size > (256u << 20)) { skipped++; continue; }
        int bw, bh, bpb = nvn_fmt_bpb(d.format, &bw, &bh);
        unsigned level = rnd(d.levels), layer = d.target == 4 ? rnd(L.layers) : 0;
        uint32_t lbx = (mip(d.width, level) + bw - 1) / bw, lby = (mip(d.height, level) + bh - 1) / bh;
        uint32_t ld = d.target == 2 ? mip(d.depth, level) : 1, pitch = lbx * (uint32_t)bpb;
        uint32_t sx = rnd(lbx), sy = rnd(lby), bx = 1 + rnd(lbx - sx), by = 1 + rnd(lby - sy);
        uint32_t sz = d.target == 2 ? rnd(ld) : 0, nz = d.target == 2 ? 1 + rnd(ld - sz) : 1;
        size_t row = (size_t)bx * bpb + rnd(3) * 16, img = row * by + rnd(2) * 64;
        uint8_t *a = malloc(L.size), *b = malloc(L.size), *src = malloc(img * nz), *lin = malloc((size_t)pitch * lby * ld);
        for (uint64_t i = 0; i < L.size; i++) a[i] = (uint8_t)rnd(256);
        for (size_t i = 0; i < img * nz; i++) src[i] = (uint8_t)rnd(256);
        memcpy(b, a, L.size);
        // the whole-level way (vkb_tex.c mtl_write_texels_mem before)
        if (nvn_deswizzle(&d, level, layer, a, lin, pitch)) { skipped++; goto next; }
        for (uint32_t s = 0; s < nz; s++)
            for (uint32_t y = 0; y < by; y++)
                memcpy(lin + ((size_t)(sz + s) * lby + sy + y) * pitch + (size_t)sx * bpb, src + s * img + y * row, (size_t)bx * bpb);
        nvn_swizzle(&d, level, layer, lin, pitch, a);
        // the region way
        if (nvn_swizzle_region(&d, level, layer, sx * (uint32_t)bpb, sy, sz, bx * (uint32_t)bpb, by, nz, src, row, img, b)) {
            printf("refused: target %d format %#x %ux%ux%u level %u\n", d.target, d.format, d.width, d.height, d.depth, level);
            diffs++;
            goto next;
        }
        checks++;
        if (memcmp(a, b, L.size)) {
            if (diffs++ < 10)
                printf("differs: target %d format %#x %ux%ux%u levels %u, level %u layer %u, region %u,%u,%u %ux%ux%u\n", d.target,
                       d.format, d.width, d.height, d.depth, d.levels, level, layer, sx, sy, sz, bx, by, nz);
        }
    next:
        free(a); free(b); free(src); free(lin);
    }
    printf("%u checks, %u skipped, %u differences\n", checks, skipped, diffs);
    return diffs != 0;
}
