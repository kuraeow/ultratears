/* nn::pl shared fonts: the player's system BFTTF files (extracted from their firmware by
 * MacNativeGames tools/extract_shared_fonts.py as <type>-<name>.bfttf), each read when the game first asks for it.
 * After MacNativeGames games/totk/runtime/shared_font.c @b75be8c (MIT): GetSharedFontAddress points 8 bytes past each
 * file start (after the BFTTF header), as the SDK adds the service-provided offset to the shared memory base; the size
 * is the decoded font size. Changes: the directory comes from TOTK_SHARED_FONTS (default data/fonts),
 * and files are checked by their BFTTF header (either word byte order) instead of SHA-256 rows of one firmware version. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "../runtime/hle.h"

#define SHARED_FONT_COUNT 6
static const char *const names[SHARED_FONT_COUNT] = {
    "0-standard", "1-chinese-simplified", "2-chinese-simplified-extension", "3-chinese-traditional", "4-korean",
    "5-nintendo-extension",
};
static uint8_t *font[SHARED_FONT_COUNT];        /* each file as the pl shared memory holds it, read on first use */
static uint64_t font_bytes[SHARED_FONT_COUNT];  /* the decoded font's size */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void need(int c, const char *m) { if (!c) runtime_fail(m); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }

/* BFTTF: 32-bit words XORed with one key; the first decodes to BFTTF_MAGIC, the second to the font's size, then the
   font (sfnt: 0x00010000 or 'OTTO'). The console's form, which the game decodes itself from Address - 8, has the words
   big-endian. Firmware archives of some tools hold every word byte-swapped (GitHub #59: "FATAL: shared font BFTTF
   header" with Chinese and Korean, the only languages the game asks for them; LASRecomp b159129 reads both). The
   decoded size of the console's form, 0 if p is not one. */
#define BFTTF_MAGIC 0x7f9a0218u
static uint64_t bfttf_size(const uint8_t *p, uint64_t len, uint32_t (*rd)(const uint8_t *)) {
    if (len < 12) return 0;
    uint32_t key = rd(p) ^ BFTTF_MAGIC, size = rd(p + 4) ^ key, first = rd(p + 8) ^ key;
    return (uint64_t)size + 8 == len && (first == 0x00010000u || first == 0x4f54544fu) ? size : 0;
}

static int file_exists(int type) {
    const char *dir = getenv("TOTK_SHARED_FONTS");
    char path[4096];
    snprintf(path, sizeof path, "%s/%s.bfttf", dir ? dir : "data/fonts", names[type]);
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static uint8_t *load(int type) {   /* lock held */
    if (font[type]) return font[type];
    const char *dir = getenv("TOTK_SHARED_FONTS");
    if (!dir) dir = "data/fonts";
    char path[4096];
    need(snprintf(path, sizeof path, "%s/%s.bfttf", dir, names[type]) < (int)sizeof path, "shared font path");
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[totk] %s missing (the system fonts are needed for Chinese and Korean text)\n", path); runtime_fail("open shared font"); }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    need(len > 12 && len < (64l << 20), "shared font size");
    uint8_t *p = malloc((size_t)len);
    need(p != NULL, "shared font memory");
    need(fread(p, 1, (size_t)len, f) == (size_t)len, "shared font read");
    fclose(f);
    uint64_t n = bfttf_size(p, (uint64_t)len, be32);
    const char *form = "console byte order";
    if (!n && bfttf_size(p, (uint64_t)len, le32)) {   /* byte-swapped words: into the console's form */
        for (long i = 0; i + 4 <= len; i += 4) { uint8_t t = p[i]; p[i] = p[i + 3]; p[i + 3] = t; t = p[i + 1]; p[i + 1] = p[i + 2]; p[i + 2] = t; }
        n = bfttf_size(p, (uint64_t)len, be32);
        form = "byte-swapped words, converted";
    }
    if (!n) { fprintf(stderr, "[totk] %s is not a BFTTF font in either byte order\n", path); runtime_fail("shared font BFTTF header"); }
    font_bytes[type] = n;
    fprintf(stderr, "[totk] shared font %s: %llu bytes (%s)\n", names[type], (unsigned long long)n, form);
    return font[type] = p;
}
static int valid(int type) {
    if (type < 0 || type >= SHARED_FONT_COUNT) runtime_fail("invalid shared font type");
    return type;
}
static int load_state(int type) { valid(type); return 1; }   /* SharedFontLoadState_Loaded */
static const void *address(int type) {
    pthread_mutex_lock(&lock); uint8_t *p = load(valid(type)); pthread_mutex_unlock(&lock);
    return p + 8;   /* past the BFTTF header, as the SDK adds the service's offset to the shared memory */
}
static size_t size(int type) {
    pthread_mutex_lock(&lock); load(valid(type)); size_t n = (size_t)font_bytes[type]; pthread_mutex_unlock(&lock);
    return n;
}

/* The font itself (sfnt, decoded) for our own text (text_entry.c: CJK glyphs Inter lacks), NULL when the player has no
   such file: never fatal. Decoded once into a copy of its own; the game's form stays as it is. */
const uint8_t *runtime_shared_font_sfnt(int type, size_t *n) {
    static uint8_t *plain[SHARED_FONT_COUNT];
    static int tried[SHARED_FONT_COUNT];
    if (type < 0 || type >= SHARED_FONT_COUNT) return NULL;
    pthread_mutex_lock(&lock);
    if (!tried[type]) {
        tried[type] = 1;
        uint8_t *p = font[type] ? font[type] : file_exists(type) ? load(type) : NULL;
        uint8_t *d = p ? malloc((size_t)font_bytes[type] + 4) : NULL;
        if (d) {
            uint32_t key = be32(p) ^ BFTTF_MAGIC;
            memset(d, 0, (size_t)font_bytes[type] + 4);
            for (uint64_t i = 0; i + 4 <= font_bytes[type]; i += 4) {   /* (an sfnt's tables are 4-byte aligned) */
                uint32_t w = be32(p + 8 + i) ^ key;
                d[i] = (uint8_t)(w >> 24), d[i + 1] = (uint8_t)(w >> 16), d[i + 2] = (uint8_t)(w >> 8), d[i + 3] = (uint8_t)w;
            }
            plain[type] = d;
        }
    }
    const uint8_t *r = plain[type];
    if (r && n) *n = (size_t)font_bytes[type];
    pthread_mutex_unlock(&lock);
    return r;
}

HLE_NATIVE("_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", load_state, 'w')
HLE_NATIVE("_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", address, 'x')
HLE_NATIVE("_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", size, 'x')
