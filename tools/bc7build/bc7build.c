// Fills the BC7 cache (src/gfx/vkb_bc7.c) from the player's RomFS before the game runs: every ASTC surface of the
// TexToGo textures (.txtg) is decoded and encoded to BC7 here, so the game uploads BC7 instead of decoding ASTC.
// Development and the player's own machine only: it reads the player's unpacked RomFS; nothing from the game goes into
// the repository or a build (README.md, Legal).
//
// usage: bc7build <romfs.bin | RomFS folder> <list> <cache dir> [threads] [quality] [max block area]
//   list: lines "<offset> <size> <path>" (tools/bc7_build.py writes it): the offset in romfs.bin, or 0 with a folder of
//   plain files (the player's dump), where each file is opened by its path
//   [max block area]: ASTC with larger blocks is left out (12x12 = 144: the interface's maps, 9x the bytes as BC7)
//
// TexToGo (facts of the format as described by Switch Toolbox's TXTG.cs and TxtgSharp; this is the port's own code):
//   0x00 u16 header size, 0x02 u16 version, 0x04 "6PK0", 0x08 u16 width, 0x0A u16 height, 0x0C u16 array layers,
//   0x0E u8 mip levels, 0x3C u16 format, 0x44 u32 block info (ASTC: 0x7F00 | (block width - 1) << 4 | block height - 1).
//   At the header size: per surface {u16 layer, u8 level, u8 1}, then per surface {u32 size, u32 6}, then the surfaces,
//   each a zstd frame of block-linear (Tegra X1) data cut at its last byte. ASTC formats: 0x1xx (sRGB: 0x105, 0x109).
// BNTX (Nintendo's texture container, .bntx / .bntx.zs: the interface and some effects): "NX  " at 0x20 with the texture
//   count (u32 0x24) and the offset of its BRTI pointers (u64 0x28); a BRTI: mip levels u16 0x16, format u32 0x1C
//   (ASTC: 0x2D..0x3A << 8 | 1 UNORM / 6 sRGB), width 0x24, height 0x28, depth 0x2C, array layers 0x30, block height
//   log2 in the low 3 bits of 0x34, image size u32 0x50, mip offsets u64[] at the offset in 0x70 (layer 0; each layer
//   image size / layers further). Data block-linear with that block height (smaller for small mips, as the GPU does).
// The key of a surface is that of the game's upload (tex/bc7key.c): its blocks in rows, its size and block size.
// Textures with large ASTC blocks (more than 100 texels: the interface's 12x12 maps) become BC1 when none of their
// texels is transparent (half the bytes of BC7; such low-rate ASTC holds no detail BC1 would lose), else BC7.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <zstd.h>

#include "../../src/gfx/tex/astc.h"
#include "../../src/gfx/tex/bc7.h"
#include "../../src/gfx/tex/bc7key.h"

typedef struct { uint64_t off, size; char path[192]; } file_t;
static file_t *g_files;
static int g_nfiles, g_quality = 2, g_max_area = 144;   // block area at most (12x12 = 144): larger ones skipped
enum { BC1_AREA = 100, BC1_LEVEL = 10 };   // larger ASTC blocks: BC1 when opaque (rgbcx level)
static _Atomic uint64_t g_bc1;
static const char *g_romfs;
static int g_folder;   // g_romfs is a folder of plain files, not romfs.bin
static _Atomic int g_next, g_done;
static _Atomic unsigned long long g_done_bytes;
static _Atomic long long g_said_at;   // the second of the last progress line
static unsigned long long g_total_bytes;
static _Atomic uint64_t g_surfaces, g_astc_files, g_encoded, g_had, g_bad, g_texels;

// ---- the cache files (the same records as vkb_bc7.c)
typedef struct { uint64_t a, b, off; uint32_t len, fmt; } rec_t;   // fmt: 0 BC7, 1 BC1
static FILE *g_pack, *g_idx;
static rec_t *g_tab;
static size_t g_cap, g_n;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static rec_t *slot(uint64_t a, uint64_t b) {
    size_t h = (size_t)(a ^ (b * 0x9E3779B97F4A7C15ull)) & (g_cap - 1);
    while ((g_tab[h].a || g_tab[h].b) && !(g_tab[h].a == a && g_tab[h].b == b)) h = (h + 1) & (g_cap - 1);
    return &g_tab[h];
}
static void put(const rec_t *r) {
    if ((g_n + 1) * 2 > g_cap) {
        rec_t *old = g_tab;
        size_t n = g_cap;
        g_cap = g_cap ? g_cap * 2 : 1 << 16;
        g_tab = calloc(g_cap, sizeof *g_tab);
        if (!g_tab) { fprintf(stderr, "out of memory\n"); exit(1); }
        for (size_t i = 0; i < n; i++) if (old[i].a || old[i].b) *slot(old[i].a, old[i].b) = old[i];
        free(old);
    }
    rec_t *s = slot(r->a, r->b);
    if (!s->a && !s->b) g_n++;
    *s = *r;
}
static int claim(bc7_key k) {   // 1: not in the cache yet and now reserved for this thread (len 0 until written)
    pthread_mutex_lock(&g_mu);
    int mine = !(g_cap && (slot(k.a, k.b)->a || slot(k.a, k.b)->b));
    if (mine) { rec_t r = {k.a, k.b, 0, 0, 0}; put(&r); }
    pthread_mutex_unlock(&g_mu);
    return mine;
}
static void store(bc7_key k, const uint8_t *data, size_t n, int fmt) {
    pthread_mutex_lock(&g_mu);
    _fseeki64(g_pack, 0, SEEK_END);
    rec_t r = {k.a, k.b, (uint64_t)_ftelli64(g_pack), (uint32_t)n, (uint32_t)fmt};
    if (fwrite(data, 1, n, g_pack) == n) {
        fflush(g_pack);
        _fseeki64(g_idx, 0, SEEK_END);
        fwrite(&r, sizeof r, 1, g_idx);
        fflush(g_idx);
        put(&r);
    }
    pthread_mutex_unlock(&g_mu);
}
static void open_cache(const char *dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/bc7.pack", dir);
    if (!(g_pack = fopen(path, "r+b")) && !(g_pack = fopen(path, "w+b"))) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    _fseeki64(g_pack, 0, SEEK_END);
    int64_t pack_size = _ftelli64(g_pack);
    snprintf(path, sizeof path, "%s/bc7.idx", dir);
    if (!(g_idx = fopen(path, "r+b")) && !(g_idx = fopen(path, "w+b"))) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    rec_t r;
    int64_t good = 0;
    while (fread(&r, sizeof r, 1, g_idx) == 1 && (int64_t)(r.off + r.len) <= pack_size) { put(&r); good += sizeof r; }
    _fseeki64(g_idx, good, SEEK_SET);
    printf("cache %s: %zu surfaces, %.1f MiB\n", dir, g_n, pack_size / 1048576.0);
}

// ---- Tegra X1 block-linear: a GOB is 64 bytes x 8 rows; GOBs stack `bhm` high
static uint32_t block_height0(uint32_t hb) {   // the base level's GOBs per block, from its height in blocks
    uint32_t x = hb + hb / 2;
    return x >= 128 ? 16 : x >= 64 ? 8 : x >= 32 ? 4 : x >= 16 ? 2 : 1;
}
static uint32_t pow2_at_least(uint32_t v) { uint32_t p = 1; while (p < v) p <<= 1; return p; }
static int deswizzle(const uint8_t *src, size_t len, uint32_t wb, uint32_t hb, uint32_t bpb, uint32_t bhm, uint8_t *dst) {
    uint32_t row = wb * bpb, gobs = (row + 63) / 64;
    for (uint32_t y = 0; y < hb; y++)
        for (uint32_t x = 0; x < row; x += bpb) {
            size_t o = (size_t)(y / (8 * bhm)) * (512u * bhm * gobs) + (size_t)(x / 64) * (512u * bhm) + ((y % (8 * bhm)) / 8) * 512u +
                       ((x % 64) / 32) * 256u + ((y % 8) / 2) * 64u + ((x % 32) / 16) * 32u + (y % 2) * 16u + (x % 16);
            if (o + bpb > len) return 0;
            memcpy(dst + (size_t)y * row + x, src + o, bpb);
        }
    return 1;
}

// The surfaces of one texture: decoded as they come; with large blocks, kept until the texture's format is known.
typedef struct { bc7_key k; uint8_t *rgba; uint32_t lw, lh; } dec_t;
typedef struct { dec_t *d; int n, cap, opaque, srgb, large; } tex_t;
static void encode_one(const dec_t *d, int fmt, int srgb) {
    size_t n = fmt ? bc1_size(d->lw, d->lh, 1) : bc7_size(d->lw, d->lh, 1);
    uint8_t *out = malloc(n);
    if (!out) return;
    if (fmt) { bc1_encode(d->rgba, d->lw, d->lh, 1, BC1_LEVEL, out); g_bc1++; }
    else bc7_encode(d->rgba, d->lw, d->lh, 1, srgb, g_quality, out);
    store(d->k, out, n, fmt);
    g_encoded++;
    g_texels += (uint64_t)d->lw * d->lh;
    free(out);
}
static void tex_end(tex_t *t) {   // a texture with large blocks: BC1 if no texel of it is transparent, else BC7
    for (int i = 0; i < t->n; i++) { encode_one(&t->d[i], t->opaque ? 1 : 0, t->srgb); free(t->d[i].rgba); }
    t->n = 0;
}
// One ASTC surface in block-linear form: deswizzled, keyed, decoded and encoded unless the cache has it.
static void do_surface(tex_t *t, const uint8_t *sw, size_t len, uint32_t lw, uint32_t lh, uint32_t bw, uint32_t bh, uint32_t bhm, int srgb) {
    uint32_t wb = (lw + bw - 1) / bw, hb = (lh + bh - 1) / bh;
    uint8_t *rows = malloc((size_t)wb * hb * 16);
    if (!rows || !deswizzle(sw, len, wb, hb, 16, bhm, rows)) { g_bad++; free(rows); return; }
    g_surfaces++;
    bc7_key k = bc7_key_of(rows, (size_t)wb * hb * 16, lw, lh, 1, bw, bh);
    if (claim(k)) {
        dec_t d = {k, malloc((size_t)lw * lh * 4), lw, lh};
        if (d.rgba) {
            astc_decode_ldr(rows, lw, lh, bw, bh, srgb, d.rgba);
            if (!t->large) { encode_one(&d, 0, srgb); free(d.rgba); }
            else {
                for (size_t i = 3; i < (size_t)lw * lh * 4 && t->opaque; i += 4) t->opaque = d.rgba[i] == 255;
                if (t->n == t->cap) { t->cap = t->cap ? t->cap * 2 : 16; t->d = realloc(t->d, sizeof *t->d * (size_t)t->cap); }
                t->d[t->n++] = d;
            }
        }
    } else g_had++;
    free(rows);
}
static void tex_begin(tex_t *t, uint32_t bw, uint32_t bh, int srgb) { t->n = 0; t->opaque = 1; t->srgb = srgb; t->large = bw * bh > BC1_AREA; }
static uint32_t mip_block_height(uint32_t hb, uint32_t bh0) {   // GOBs per block of a level: as the GPU shrinks it for small levels
    uint32_t bhm = pow2_at_least((hb + 7) / 8);
    if (bhm > 16) bhm = 16;
    return bhm > bh0 ? bh0 : bhm;
}

static const uint8_t g_astc_dims[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
static void do_bntx(const uint8_t *d, size_t n) {
    if (n < 0x30 || memcmp(d, "BNTX", 4) || memcmp(d + 0x20, "NX  ", 4)) { g_bad++; return; }
    uint32_t count; uint64_t arr;
    memcpy(&count, d + 0x24, 4); memcpy(&arr, d + 0x28, 8);
    for (uint32_t t = 0; t < count && arr + 8 * (t + 1) <= n; t++) {
        uint64_t o; memcpy(&o, d + arr + 8 * t, 8);
        if (o + 0x78 > n || memcmp(d + o, "BRTI", 4)) { g_bad++; continue; }
        uint16_t mips; uint32_t fmt, layout, image; int32_t w, h, depth, layers; uint64_t ptrs;
        memcpy(&mips, d + o + 0x16, 2); memcpy(&fmt, d + o + 0x1C, 4); memcpy(&w, d + o + 0x24, 4); memcpy(&h, d + o + 0x28, 4);
        memcpy(&depth, d + o + 0x2C, 4); memcpy(&layers, d + o + 0x30, 4); memcpy(&layout, d + o + 0x34, 4);
        memcpy(&image, d + o + 0x50, 4); memcpy(&ptrs, d + o + 0x70, 8);
        uint32_t type = fmt >> 8;
        if (type < 0x2D || type > 0x3A || depth > 1 || w <= 0 || h <= 0 || ptrs + 8ull * mips > n) continue;   // not 2D ASTC
        uint32_t bw = g_astc_dims[type - 0x2D][0], bh = g_astc_dims[type - 0x2D][1];
        if (bw * bh > (uint32_t)g_max_area) continue;
        int srgb = (fmt & 0xFF) == 6;
        if (layers < 1) layers = 1;
        g_astc_files++;
        tex_t tx = {0};
        tex_begin(&tx, bw, bh, srgb);
        uint32_t bh0 = 1u << (layout & 7);
        uint64_t stride = (uint64_t)image / (uint32_t)layers;
        for (int32_t layer = 0; layer < layers; layer++)
            for (uint32_t m = 0; m < mips; m++) {
                uint64_t mo; memcpy(&mo, d + ptrs + 8 * m, 8);
                mo += stride * (uint64_t)layer;
                if (mo >= n) { g_bad++; continue; }
                uint32_t lw = (uint32_t)w >> m ? (uint32_t)w >> m : 1, lh = (uint32_t)h >> m ? (uint32_t)h >> m : 1;
                uint32_t hb = (lh + bh - 1) / bh;
                do_surface(&tx, d + mo, n - mo, lw, lh, bw, bh, mip_block_height(hb, bh0), srgb);
            }
        tex_end(&tx);
        free(tx.d);
    }
}

// The file's bytes (f->size of them) into buf: from the image at its offset, or from its own file in the folder.
static int read_file(FILE *rf, const file_t *f, uint8_t *buf) {
    if (!g_folder) {
        _fseeki64(rf, (int64_t)f->off, SEEK_SET);
        return fread(buf, 1, f->size, rf) == f->size;
    }
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", g_romfs, f->path);
    FILE *ff = fopen(path, "rb");
    if (!ff) return 0;
    int ok = fread(buf, 1, f->size, ff) == f->size;
    fclose(ff);
    return ok;
}
static void do_file(FILE *rf, const file_t *f, uint8_t **buf, size_t *cap) {
    size_t pl = strlen(f->path);
    int bntx = pl > 5 && (!strcmp(f->path + pl - 5, ".bntx") || (pl > 8 && !strcmp(f->path + pl - 8, ".bntx.zs")));
    if (bntx) {
        if (*cap < f->size) { free(*buf); *cap = f->size; if (!(*buf = malloc(*cap))) { fprintf(stderr, "out of memory\n"); exit(1); } }
        if (!read_file(rf, f, *buf)) { g_bad++; return; }
        if (f->path[pl - 1] != 's') { do_bntx(*buf, f->size); return; }
        unsigned long long raw = ZSTD_getFrameContentSize(*buf, f->size);
        if (raw == ZSTD_CONTENTSIZE_ERROR || raw == ZSTD_CONTENTSIZE_UNKNOWN || raw > (1ull << 32)) { g_bad++; return; }
        uint8_t *d = malloc(raw ? raw : 1);
        if (!d || ZSTD_isError(ZSTD_decompress(d, raw, *buf, f->size))) { g_bad++; free(d); return; }
        do_bntx(d, raw);
        free(d);
        return;
    }
    if (f->size < 0x50) return;
    if (*cap < f->size) { free(*buf); *cap = f->size; if (!(*buf = malloc(*cap))) { fprintf(stderr, "out of memory\n"); exit(1); } }
    uint8_t *b = *buf;
    if (!read_file(rf, f, b) || memcmp(b + 4, "6PK0", 4)) { g_bad++; return; }
    uint16_t hs, w, h, layers, fmt; uint8_t mips; uint32_t info;
    memcpy(&hs, b, 2); memcpy(&w, b + 8, 2); memcpy(&h, b + 0xA, 2); memcpy(&layers, b + 0xC, 2); mips = b[0xE];
    memcpy(&fmt, b + 0x3C, 2); memcpy(&info, b + 0x44, 4);
    if ((fmt >> 8) != 1 || (info & 0xFFFFFF00u) != 0x7F00) return;   // not ASTC: the GPU samples it as it is
    uint32_t bw = ((info >> 4) & 15) + 1, bh = (info & 15) + 1;
    if (bw * bh > (uint32_t)g_max_area) return;
    int srgb = fmt == 0x105 || fmt == 0x109;
    uint32_t ns = (uint32_t)mips * (layers ? layers : 1);
    if (!ns || hs + (size_t)ns * 12 > f->size) { g_bad++; return; }
    g_astc_files++;
    tex_t tx = {0};
    tex_begin(&tx, bw, bh, srgb);
    const uint8_t *ent = b + hs, *sizes = ent + ns * 4, *data = sizes + ns * 8;
    uint32_t bh0 = block_height0((h + bh - 1) / bh);
    for (uint32_t i = 0; i < ns; i++) {
        uint16_t layer; memcpy(&layer, ent + i * 4, 2);
        uint8_t level = ent[i * 4 + 2];
        uint32_t zs; memcpy(&zs, sizes + i * 8, 4);
        if (data + zs > b + f->size) { g_bad++; break; }
        const uint8_t *z = data;
        data += zs;
        (void)layer;
        uint32_t lw = w >> level ? w >> level : 1, lh = h >> level ? h >> level : 1;
        uint32_t hb = (lh + bh - 1) / bh;
        unsigned long long raw = ZSTD_getFrameContentSize(z, zs);
        if (raw == ZSTD_CONTENTSIZE_ERROR || raw == ZSTD_CONTENTSIZE_UNKNOWN || raw > (1ull << 31)) { g_bad++; continue; }
        uint8_t *sw = malloc(raw ? raw : 1);
        if (!sw || ZSTD_isError(ZSTD_decompress(sw, raw, z, zs))) { g_bad++; free(sw); continue; }
        do_surface(&tx, sw, raw, lw, lh, bw, bh, mip_block_height(hb, bh0), srgb);
        free(sw);
    }
    tex_end(&tx);
    free(tx.d);
}

static void *worker(void *unused) {
    (void)unused;
    FILE *rf = NULL;   // the image; with a folder, files are opened one by one (read_file)
    if (!g_folder && !(rf = fopen(g_romfs, "rb"))) { fprintf(stderr, "cannot open %s\n", g_romfs); exit(1); }
    uint8_t *buf = NULL;
    size_t cap = 0;
    for (int i; (i = atomic_fetch_add(&g_next, 1)) < g_nfiles;) {
        do_file(rf, &g_files[i], &buf, &cap);
        // progress once a second, counted in finished files and their bytes (tools/bc7_build.py draws it): the
        // largest files go first, so "every 500 files" said nothing for minutes at the start
        int done = atomic_fetch_add(&g_done, 1) + 1;
        unsigned long long bytes = atomic_fetch_add(&g_done_bytes, g_files[i].size) + g_files[i].size;
        long long now = (long long)time(NULL), last = atomic_load(&g_said_at);
        if ((now != last && atomic_compare_exchange_strong(&g_said_at, &last, now)) || done == g_nfiles)
            printf("%d / %d files, %llu / %llu bytes, %llu surfaces encoded\n", done, g_nfiles, bytes, g_total_bytes,
                   (unsigned long long)g_encoded);
    }
    free(buf);
    if (rf) fclose(rf);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: bc7build <romfs.bin> <list> <cache dir> [threads] [quality]\n"); return 2; }
    g_romfs = argv[1];
    setvbuf(stdout, NULL, _IONBF, 0);   // read through a pipe (tools/bc7_build.py): every line as it is printed
    int threads = argc > 4 ? atoi(argv[4]) : 8;
    if (argc > 5) g_quality = atoi(argv[5]);
    if (argc > 6) g_max_area = atoi(argv[6]);
    FILE *lf = fopen(argv[2], "r");
    if (!lf) { fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
    int cap = 0;
    file_t f;
    struct stat st;
    g_folder = !stat(g_romfs, &st) && S_ISDIR(st.st_mode);
    while (fscanf(lf, "%llu %llu %191[^\n]", (unsigned long long *)&f.off, (unsigned long long *)&f.size, f.path) == 3) {
        if (g_nfiles == cap) { cap = cap ? cap * 2 : 4096; g_files = realloc(g_files, sizeof *g_files * (size_t)cap); }
        g_files[g_nfiles++] = f;
        g_total_bytes += f.size;
    }
    fclose(lf);
    open_cache(argv[3]);
    printf("%d files, %d threads, quality %d\n", g_nfiles, threads, g_quality);
    pthread_t t[64];
    if (threads < 1) threads = 1;
    if (threads > 64) threads = 64;
    for (int i = 0; i < threads; i++) pthread_create(&t[i], NULL, worker, NULL);
    for (int i = 0; i < threads; i++) pthread_join(t[i], NULL);
    printf("done: %llu ASTC files, %llu surfaces (%llu encoded, of them %llu BC1; %llu already there), %llu unreadable, %.0f Mtexels encoded; cache %zu surfaces\n",
           (unsigned long long)g_astc_files, (unsigned long long)g_surfaces, (unsigned long long)g_encoded, (unsigned long long)g_bc1, (unsigned long long)g_had,
           (unsigned long long)g_bad, g_texels / 1e6, g_n);
    fclose(g_pack); fclose(g_idx);
    return 0;
}
