/* The quick menus' background at more than 30 fps, from MacNativeGames games/totk/scripts/make_romfs_patches.py (MIT,
 * Copyright (c) 2026 MacNativeGames contributors), as totk-pc-recomp's tools/make_romfs_patches.py carries it to 1.4.3
 * (MIT): the weapon / shield / material quick menus capture the game picture for their background when the panes
 * C_CaptureBG_00, C_CaptureClearBG_00 and P_CaptureClearBG_00 become visible at frame 1.0 of
 * anim/ShortCutBG_00_In.bflan (AutoBuilder the same with C_CaptureBG_00, anim/AutoBuilder_00_In.bflan) in
 * UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc.zs. With steps shorter than the shipping 30 fps one the
 * animation reaches frame 1.0 after the world has stopped drawing and the capture is black (2026-10-31: always black
 * with the real-time step, never with TOTK_REALTIME=0 or at 30). The visibility keys move to frame 0.0, so the live
 * world is captured at once. Changes: done at run time on the player's machine from the RomFS file (host/fs.c serves
 * the result from memory), in C, with the archive's own zstd dictionary from Pack/ZsDic.pack.zs. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

static int rd16(const uint8_t *p, size_t n, size_t at, uint16_t *v) { if (at + 2 > n) return 0; memcpy(v, p + at, 2); return 1; }
static int rd32(const uint8_t *p, size_t n, size_t at, uint32_t *v) { if (at + 4 > n) return 0; memcpy(v, p + at, 4); return 1; }

// The member of a SARC named name: [*lo, *hi) in data. 0 if absent or malformed.
static int sarc_member(const uint8_t *d, size_t n, const char *name, size_t *lo, size_t *hi) {
    uint16_t header, count;
    uint32_t start;
    if (n < 0x14 || memcmp(d, "SARC", 4) || !rd16(d, n, 4, &header) || !rd32(d, n, 12, &start) || start > n ||
        header + 12 > n || memcmp(d + header, "SFAT", 4) || !rd16(d, n, header + 6, &count))
        return 0;
    size_t names = header + 12 + (size_t)count * 16;
    if (names + 8 > n || memcmp(d + names, "SFNT", 4)) return 0;
    names += 8;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t attr, a, b;
        size_t node = header + 12 + (size_t)i * 16;
        if (!rd32(d, n, node + 4, &attr) || !rd32(d, n, node + 8, &a) || !rd32(d, n, node + 12, &b)) return 0;
        size_t at = names + (size_t)(attr & 0xffffff) * 4, len = strlen(name);
        if (at + len + 1 <= start && !memcmp(d + at, name, len) && !d[at + len]) {
            if ((size_t)start + b > n || a > b) return 0;
            *lo = start + a, *hi = start + b;
            return 1;
        }
    }
    return 0;
}

/* In the BFLAN at d[lo, hi): every visibility (FLVI) track of the named panes must be the step curve {0.0: 0, 1.0: 1};
   its second key moves to frame 0.0. The number of panes changed, -1 when a track is not as expected. */
static int move_keys(uint8_t *d, size_t lo, size_t hi, const char *const *panes, int npanes) {
    uint16_t sections;
    if (hi - lo < 0x14 || memcmp(d + lo, "FLAN", 4) || !rd16(d, hi, lo + 0x10, &sections)) return -1;
    size_t pos = lo + 0x14;
    int changed = 0;
    for (unsigned s = 0; s < sections; s++) {
        uint32_t size;
        if (!rd32(d, hi, pos + 4, &size) || size < 8 || pos + size > hi) return -1;
        if (!memcmp(d + pos, "pai1", 4)) {
            uint16_t count;
            uint32_t table;
            if (!rd16(d, hi, pos + 14, &count) || !rd32(d, hi, pos + 16, &table)) return -1;
            for (unsigned e = 0; e < count; e++) {
                uint32_t eo_rel;
                if (!rd32(d, hi, pos + table + 4 * e, &eo_rel)) return -1;
                size_t eo = pos + eo_rel;
                if (eo + 32 > hi) return -1;
                char name[29];
                memcpy(name, d + eo, 28);
                name[28] = 0;
                int want = 0;
                for (int k = 0; k < npanes; k++) if (!strcmp(name, panes[k])) want = 1;
                if (!want) continue;
                for (unsigned t = 0; t < d[eo + 28]; t++) {
                    uint32_t to_rel;
                    if (!rd32(d, hi, eo + 32 + 4 * t, &to_rel)) return -1;
                    size_t to = eo + to_rel;
                    if (to + 8 > hi || memcmp(d + to, "FLVI", 4)) continue;
                    for (unsigned g = 0; g < d[to + 4]; g++) {
                        uint32_t go_rel, ko_rel;
                        uint16_t keys;
                        if (!rd32(d, hi, to + 8 + 4 * g, &go_rel)) return -1;
                        size_t go = to + go_rel;
                        if (go + 12 > hi || d[go + 2] != 1 || !rd16(d, hi, go + 4, &keys) || !rd32(d, hi, go + 8, &ko_rel)) return -1;
                        size_t ko = go + ko_rel;
                        float f0, f1;
                        uint16_t v0, v1;
                        if (keys != 2 || ko + 16 > hi) return -1;
                        memcpy(&f0, d + ko, 4); memcpy(&v0, d + ko + 4, 2);
                        memcpy(&f1, d + ko + 8, 4); memcpy(&v1, d + ko + 12, 2);
                        if (f0 != 0.0f || v0 != 0 || f1 != 1.0f || v1 != 1) return -1;
                        f1 = 0.0f;
                        memcpy(d + ko + 8, &f1, 4);
                        changed++;
                    }
                }
            }
        }
        pos += size;
    }
    return changed;
}

/* in: the RomFS file (zstd with a dictionary of dicts, the decompressed Pack/ZsDic.pack.zs); out: the fixed file,
   compressed with the same dictionary (malloc). 1 on success; 0 leaves the game's file as it is. */
int totk_ui_capture_build(const void *in, size_t n, const void *dicts_z, size_t dn, void **out, size_t *out_n) {
    *out = NULL;
    unsigned id = ZSTD_getDictID_fromFrame(in, n);
    unsigned long long raw = ZSTD_getFrameContentSize(in, n), praw = ZSTD_getFrameContentSize(dicts_z, dn);
    if (!id || raw == ZSTD_CONTENTSIZE_UNKNOWN || raw == ZSTD_CONTENTSIZE_ERROR || raw > (256u << 20) ||
        praw == ZSTD_CONTENTSIZE_UNKNOWN || praw == ZSTD_CONTENTSIZE_ERROR || praw > (64u << 20))
        return 0;
    uint8_t *pack = malloc((size_t)praw), *blarc = malloc((size_t)raw);
    void *z = NULL;
    int ok = 0;
    ZSTD_DCtx *dc = ZSTD_createDCtx();
    ZSTD_CCtx *cc = ZSTD_createCCtx();
    const uint8_t *dict = NULL;
    size_t dict_n = 0;
    if (!pack || !blarc || !dc || !cc || ZSTD_decompress(pack, (size_t)praw, dicts_z, dn) != praw) goto done;
    // the pack is a SARC of dictionaries: the one with the frame's id
    {
        uint16_t header, count;
        uint32_t start;
        if (praw >= 0x14 && !memcmp(pack, "SARC", 4) && rd16(pack, (size_t)praw, 4, &header) && rd32(pack, (size_t)praw, 12, &start) &&
            start <= praw && rd16(pack, (size_t)praw, header + 6, &count))
            for (uint32_t i = 0; i < count && !dict; i++) {
                uint32_t a, b;
                size_t node = header + 12 + (size_t)i * 16;
                if (rd32(pack, (size_t)praw, node + 8, &a) && rd32(pack, (size_t)praw, node + 12, &b) && a < b &&
                    (size_t)start + b <= praw && ZSTD_getDictID_fromDict(pack + start + a, b - a) == id)
                    dict = pack + start + a, dict_n = b - a;
            }
    }
    if (!dict || ZSTD_decompress_usingDict(dc, blarc, (size_t)raw, in, n, dict, dict_n) != raw) goto done;
    static const char *const shortcut[] = {"C_CaptureBG_00", "C_CaptureClearBG_00", "P_CaptureClearBG_00"};
    static const char *const builder[] = {"C_CaptureBG_00"};
    size_t lo, hi;
    if (!sarc_member(blarc, (size_t)raw, "anim/ShortCutBG_00_In.bflan", &lo, &hi) || move_keys(blarc, lo, hi, shortcut, 3) != 3 ||
        !sarc_member(blarc, (size_t)raw, "anim/AutoBuilder_00_In.bflan", &lo, &hi) || move_keys(blarc, lo, hi, builder, 1) != 1) {
        fprintf(stderr, "[totk] quick menu background: the layout archive is not as expected; the game keeps its own\n");
        goto done;
    }
    size_t cap = ZSTD_compressBound((size_t)raw);
    z = malloc(cap);
    size_t zn = z ? ZSTD_compress_usingDict(cc, z, cap, blarc, (size_t)raw, dict, dict_n, 3) : 0;
    if (!z || ZSTD_isError(zn)) goto done;
    *out = z, *out_n = zn, z = NULL, ok = 1;
done:
    free(z); free(pack); free(blarc);
    ZSTD_freeDCtx(dc); ZSTD_freeCCtx(cc);
    return ok;
}
