/* Game resolution: the size the game composes its frame in (its output: the canvas, the UI, the window it presents)
 * and the size of its 3D scene, the runtime half of the output_size and scene_size code patches (tools/lifter/patches.py).
 * As totk-pc-recomp's runtime/resolution.c (MIT, Copyright (c) 2026 Valiantsin Dzerakh), whose patch sites are
 * MacNativeGames' (games/totk/runtime/game_patches_v143.c, MIT, Copyright (c) 2026 MacNativeGames contributors): the game
 * renders at the sizes asked, as with the community resolution mods, and its own final pass (FSR 1 EASU) scales the
 * scene to the output; the present scales the output to the window.
 *
 * Settings (ultratears.cfg, read at the start):
 *   resolution= (TOTK_OUTPUT_RES): <w>x<h>, or "display" (the primary monitor's desktop size); unset 1920x1080, the game's.
 *   scale= (TOTK_RENDER_SCALE): the scene's size in % of the output, 25..400; unset the game's 5/6 (1600x900 at 1080p).
 *     Above 100 the game supersamples its scene (smoother edges, more GPU time).
 * 1.4.3 scales the scene size it reads by output width / 1920, so the size handed to it is the wanted one divided by that.
 *
 * Heap: the render targets of the output and the scene live in agl's DynamicTextureAllocatorMem, sized in
 * System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs (113664 KiB, carved from "Graphics", 201728 KiB); at 2560x1440 the
 * shipped size runs out at the title screen. Both grow by the shipped size times the output's pixel ratio above 1920x1080
 * plus the scene's above 1600x900 (totk-pc-recomp's formula, from MacNativeGames' measurement); fs.c serves the rewritten
 * table (totk_heapdef_build) in place of the original and svc.c adds the amount to the application pool, so no other
 * heap shrinks. Without the rewritten table the game keeps its own sizes. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include <zstd.h>

#define BASE_W 1920
#define BASE_H 1080
#define SCENE_W 1600
#define SCENE_H 900
#define DTA_KIB 113664u   // DynamicTextureAllocatorMem, shipped
#define SCENE_MAX 4096    // per side

static struct {
    int decided;
    unsigned ow, oh, sw, sh;
    int scene_set;    // the scene differs from the game's 5/6 of the output
    int heap_ok;      // fs.c serves the grown HeapDef (needed when anything is above the shipped sizes)
} R;

static void decide(void) {
    if (R.decided) return;
    R.decided = 1;
    R.ow = BASE_W, R.oh = BASE_H;
    const char *o = getenv("TOTK_OUTPUT_RES");
    if (o && *o) {
        unsigned w = 0, h = 0;
        char tail;
        if (!strcmp(o, "display")) {
            if (SDL_InitSubSystem(SDL_INIT_VIDEO)) {
                const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
                if (m) w = (unsigned)m->w, h = (unsigned)m->h;
            }
        } else if (sscanf(o, "%ux%u%c", &w, &h, &tail) != 2)
            w = h = 0;
        if (w >= 1280 && h >= 720 && w <= 7680 && h <= 4320)
            R.ow = w & ~1u, R.oh = h & ~1u;
        else if (w || h || strcmp(o, "display"))
            fprintf(stderr, "[totk] resolution: \"%s\" is not <width>x<height> in 1280x720..7680x4320; 1920x1080\n", o);
    }
    double k = 5.0 / 6.0;
    const char *s = getenv("TOTK_RENDER_SCALE");
    if (s && *s) {
        char *end;
        double pct = strtod(s, &end);
        if (end != s && !*end && pct >= 25 && pct <= 400)
            k = pct / 100.0, R.scene_set = 1;
        else
            fprintf(stderr, "[totk] resolution: scale \"%s\" is not a percentage 25..400; the game's 5/6\n", s);
    }
    double sw = R.ow * k, sh = R.oh * k, fit = fmin(1.0, fmin(SCENE_MAX / sw, SCENE_MAX / sh));
    if (fit < 1.0) R.scene_set = 1;
    R.sw = (unsigned)(sw * fit + 0.5), R.sh = (unsigned)(sh * fit + 0.5);
}

static int changed(void) { decide(); return R.ow != BASE_W || R.oh != BASE_H || R.scene_set; }

uint64_t totk_heap_extra_kib(void) {
    if (!changed()) return 0;
    double grow = fmax(0.0, (double)R.ow * R.oh / (BASE_W * BASE_H) - 1.0) +
                  fmax(0.0, (double)R.sw * R.sh / (SCENE_W * SCENE_H) - 1.0);
    if (grow <= 0) return 0;
    return (uint64_t)ceil(DTA_KIB * (1.0 + grow) / 1024.0) * 1024 - DTA_KIB;
}

uint64_t totk_heap_extra_bytes(void) { return ((totk_heap_extra_kib() << 10) + 0x1fffff) & ~0x1fffffull; }

static int active(void) { return changed() && (R.heap_ok || !totk_heap_extra_kib()); }

// The patched game code asks these (scene_size, output_size); 0: keep the game's own.
int totk_output_size(uint64_t *packed) {
    if (!active() || (R.ow == BASE_W && R.oh == BASE_H)) return 0;
    *packed = (uint64_t)R.oh << 32 | R.ow;
    return 1;
}

int totk_scene_size(float *w, float *h) {
    if (!active() || !R.scene_set) return 0;
    double k = (double)R.ow / BASE_W;   // 1.4.3 scales what it reads by the output width
    *w = (float)(R.sw / k), *h = (float)(R.sh / k);
    return 1;
}

void totk_resolution_log(void) {   // once, after the RomFS is up (fs.c)
    if (!changed()) {
        fprintf(stderr, "[totk] resolution: the game's own (output 1920x1080, scene 1600x900)\n");
        return;
    }
    if (!active()) {
        fprintf(stderr, "[totk] resolution: output %ux%u, scene %ux%u asked, but the render-target heap could not be grown; "
                "the game keeps its own\n", R.ow, R.oh, R.sw, R.sh);
        return;
    }
    fprintf(stderr, "[totk] resolution: output %ux%u, scene %ux%u%s; render-target heap +%llu KiB\n", R.ow, R.oh, R.sw,
            R.sh, R.scene_set ? "" : " (the game's own 5/6 of the output)", (unsigned long long)totk_heap_extra_kib());
}

// ---- the HeapDef table (totk-pc-recomp res_heapdef_build). BYML version 2..7, little endian ("YB"): a hash node is
// {u8 0xC1, u24 count} and count entries {u24 key index, u8 type, u32 value}; an int (type 0xD1) is stored in the entry,
// so a size changes in place.
typedef struct { uint8_t *p; size_t n; uint32_t keys; } Byml;

static int u32_at(const Byml *b, size_t off, uint32_t *v) {
    if (off + 4 > b->n) return 0;
    memcpy(v, b->p + off, 4);
    return 1;
}

static const char *key_name(const Byml *b, uint32_t k) {
    uint32_t head, at;
    if (!u32_at(b, b->keys, &head) || (head & 0xff) != 0xc2 || k >= head >> 8 || !u32_at(b, b->keys + 4 + 4 * k, &at) ||
        b->keys + at >= b->n || !memchr(b->p + b->keys + at, 0, b->n - b->keys - at))
        return NULL;
    return (const char *)b->p + b->keys + at;
}

// The offset of the value of hash node `node`'s entry `name`, which must have type `type`; 0 when there is none.
static size_t field(const Byml *b, uint32_t node, const char *name, uint8_t type) {
    uint32_t head;
    if (!u32_at(b, node, &head) || (head & 0xff) != 0xc1) return 0;
    for (uint32_t i = 0; i < head >> 8; i++) {
        size_t e = node + 4 + 8 * (size_t)i;
        uint32_t kt;
        if (!u32_at(b, e, &kt)) return 0;
        const char *k = key_name(b, kt & 0xffffff);
        if (k && !strcmp(k, name)) return (kt >> 24) == type ? e + 4 : 0;
    }
    return 0;
}

// "Heaps" -> <heap> -> "Size" (an int)
static size_t heap_size_field(const Byml *b, uint32_t heaps, const char *heap) {
    size_t at = field(b, heaps, heap, 0xc1);
    uint32_t node;
    return at && u32_at(b, at, &node) ? field(b, node, "Size", 0xd1) : 0;
}

int totk_heapdef_build(const void *in, size_t n, void **out, size_t *out_n) {
    uint64_t extra = totk_heap_extra_kib();
    *out = NULL;
    if (!extra) return 0;
    if (ZSTD_getDictID_fromFrame(in, n)) {
        fprintf(stderr, "[totk] resolution: the HeapDef table needs a zstd dictionary; the game keeps its own resolution\n");
        return 0;
    }
    unsigned long long raw = ZSTD_getFrameContentSize(in, n);
    if (raw == ZSTD_CONTENTSIZE_UNKNOWN || raw == ZSTD_CONTENTSIZE_ERROR || raw > (64u << 20)) return 0;
    Byml b = {malloc((size_t)raw), (size_t)raw, 0};
    if (!b.p) return 0;
    size_t got = ZSTD_decompress(b.p, b.n, in, n);
    uint32_t root, heaps = 0;
    uint16_t ver = 0;
    if (!ZSTD_isError(got) && got == b.n && b.n >= 16) memcpy(&ver, b.p + 2, 2);
    size_t heaps_at = 0, dta_at = 0, gfx_at = 0;
    if (ver >= 2 && ver <= 7 && !memcmp(b.p, "YB", 2) && u32_at(&b, 4, &b.keys) && u32_at(&b, 12, &root) &&
        (heaps_at = field(&b, root, "Heaps", 0xc1)) && u32_at(&b, heaps_at, &heaps)) {
        dta_at = heap_size_field(&b, heaps, "DynamicTextureAllocatorMem");
        gfx_at = heap_size_field(&b, heaps, "Graphics");
    }
    if (!dta_at || !gfx_at) {
        fprintf(stderr, "[totk] resolution: the HeapDef table has no DynamicTextureAllocatorMem / Graphics sizes\n");
        free(b.p);
        return 0;
    }
    int32_t dta, gfx;
    memcpy(&dta, b.p + dta_at, 4);
    memcpy(&gfx, b.p + gfx_at, 4);
    int32_t dta2 = dta + (int32_t)extra, gfx2 = gfx + (int32_t)extra;
    memcpy(b.p + dta_at, &dta2, 4);
    memcpy(b.p + gfx_at, &gfx2, 4);
    size_t cap = ZSTD_compressBound(b.n);
    void *z = malloc(cap);
    size_t zn = z ? ZSTD_compress(z, cap, b.p, b.n, 9) : 0;
    free(b.p);
    if (!z || ZSTD_isError(zn)) {
        free(z);
        return 0;
    }
    *out = z, *out_n = zn;
    R.heap_ok = 1;
    fprintf(stderr, "[totk] resolution: HeapDef DynamicTextureAllocatorMem %d -> %d KiB, Graphics %d -> %d KiB\n", dta,
            dta2, gfx, gfx2);
    return 1;
}
