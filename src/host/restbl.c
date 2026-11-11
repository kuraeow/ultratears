/* The game's resource size table with player mods (fs.c mods_restbl): System/Resource/ResourceSizeTable.Product.143.
 * Nin_NX_NVN.rsizetable.zs. The game allocates the buffer a resource loads into from the file's entry there: a mod
 * file bigger than the game's own overruns it (a crash when the file loads, possibly long after the start), and a file
 * a mod adds has no entry at all. So with mods on, the table is served rebuilt: the tables the mods ship merged in
 * (only raising entries), then the entry of every file a mod serves raised for its size, the members of the packs
 * (Pack/<folders>/<name>.pack.zs, archives whose members load as resources of their own) included.
 *
 * Decompressed layout, little-endian: "RESTBL", u32 version, u32 name length (160), u32 hashed count, u32 named count;
 * {u32 CRC-32 of the key, u32 size} sorted by the hash; {char key[name length], u32 size} for keys whose hash collides.
 * A key is the RomFS path without a trailing .zs or .mc.
 *
 * Sizes: an entry is the decompressed size times a factor of the file's type plus some room. The factors are what the
 * 1.4.3 table gives its own files (bfres 1.5, bkres 2.05, bgyml 3.85, asb 1.4, bstar 1.2, bphsc 1.1, others 1.0;
 * an unknown type 1.5) plus 8 KiB rounded to 4 KiB for a file the game lacks. A file the game has grows its entry by
 * the growth times the factor. A mesh codec file (.mc) is compressed in a way that does not tell the decompressed size:
 * its entry grows in proportion to the file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "host.h"
#include "mods.h"

typedef struct { uint32_t hash, size; } Hashed;
typedef struct {
    uint32_t version, name_len;
    Hashed *h; size_t nh, caph;
    uint8_t *named; size_t nn;   // name_len + 4 bytes each, as in the file
} Table;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }

static uint32_t crc32_str(const char *s) {
    static uint32_t tab[256];
    if (!tab[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; s++) c = tab[(c ^ (uint8_t)*s) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static int parse(Table *t, const uint8_t *d, size_t n) {
    memset(t, 0, sizeof *t);
    if (n < 22 || memcmp(d, "RESTBL", 6)) return 0;
    t->version = rd32(d + 6), t->name_len = rd32(d + 10);
    uint64_t nh = rd32(d + 14), nn = rd32(d + 18);
    if (t->name_len < 8 || t->name_len > 1024 || 22 + nh * 8 + nn * (t->name_len + 4) > n) return 0;
    t->h = malloc((nh + 64) * sizeof *t->h), t->caph = nh + 64, t->nh = nh;
    t->named = malloc(nn * (t->name_len + 4) + 1), t->nn = nn;
    if (!t->h || !t->named) return 0;
    for (size_t i = 0; i < nh; i++) t->h[i].hash = rd32(d + 22 + i * 8), t->h[i].size = rd32(d + 26 + i * 8);
    memcpy(t->named, d + 22 + nh * 8, nn * (t->name_len + 4));
    return 1;
}
static void table_free(Table *t) { free(t->h); free(t->named); memset(t, 0, sizeof *t); }

static uint8_t *named_at(const Table *t, const char *key) {
    size_t len = strlen(key);
    if (len >= t->name_len) return NULL;
    for (size_t i = 0; i < t->nn; i++) {
        uint8_t *e = t->named + i * (t->name_len + 4);
        if (!memcmp(e, key, len + 1)) return e;
    }
    return NULL;
}
static size_t hashed_pos(const Table *t, uint32_t h) {
    size_t lo = 0, hi = t->nh;
    while (lo < hi) { size_t m = lo + (hi - lo) / 2; if (t->h[m].hash < h) lo = m + 1; else hi = m; }
    return lo;
}
static int get(const Table *t, const char *key, uint32_t *size) {
    uint8_t *e = named_at(t, key);
    if (e) { *size = rd32(e + t->name_len); return 1; }
    uint32_t h = crc32_str(key);
    size_t i = hashed_pos(t, h);
    if (i < t->nh && t->h[i].hash == h) { *size = t->h[i].size; return 1; }
    return 0;
}
// The hashed entry h raised to size (added when missing); 1 when it changed.
static int raise_hashed(Table *t, uint32_t h, uint32_t size) {
    size_t i = hashed_pos(t, h);
    if (i < t->nh && t->h[i].hash == h) {
        if (t->h[i].size >= size) return 0;
        t->h[i].size = size;
        return 1;
    }
    if (t->nh == t->caph) {
        Hashed *g = realloc(t->h, (t->caph * 2 + 64) * sizeof *g);
        if (!g) return 0;
        t->h = g, t->caph = t->caph * 2 + 64;
    }
    memmove(t->h + i + 1, t->h + i, (t->nh - i) * sizeof *t->h);
    t->h[i] = (Hashed){h, size};
    t->nh++;
    return 1;
}
// The entry of key raised to size (added when missing); 1 when it changed.
static int raise_to(Table *t, const char *key, uint32_t size) {
    uint8_t *e = named_at(t, key);
    if (e) {
        if (rd32(e + t->name_len) >= size) return 0;
        wr32(e + t->name_len, size);
        return 1;
    }
    return raise_hashed(t, crc32_str(key), size);
}
static uint8_t *write_table(const Table *t, size_t *n) {
    *n = 22 + t->nh * 8 + t->nn * (t->name_len + 4);
    uint8_t *d = malloc(*n);
    if (!d) return NULL;
    memcpy(d, "RESTBL", 6);
    wr32(d + 6, t->version), wr32(d + 10, t->name_len), wr32(d + 14, (uint32_t)t->nh), wr32(d + 18, (uint32_t)t->nn);
    for (size_t i = 0; i < t->nh; i++) wr32(d + 22 + i * 8, t->h[i].hash), wr32(d + 26 + i * 8, t->h[i].size);
    memcpy(d + 22 + t->nh * 8, t->named, t->nn * (t->name_len + 4));
    return d;
}

static void key_of(const char *path, char *key, size_t cap) {
    size_t len = strlen(path);
    if (len >= 3 && (!strcmp(path + len - 3, ".zs") || !strcmp(path + len - 3, ".mc"))) len -= 3;
    if (len >= cap) len = cap - 1;
    memcpy(key, path, len);
    key[len] = 0;
}
static int ends(const char *s, const char *tail) {
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && !strcmp(s + a - b, tail);
}
static unsigned factor(const char *key) {   // per mille; 0: the table never lists the type (streamed)
    static const struct { const char *ext; unsigned f; } k[] = {
        {"bfres", 1500}, {"bkres", 2050}, {"bgyml", 3850}, {"asb", 1400}, {"bstar", 1200}, {"bphsc", 1100},
        {"bwav", 0}, {"webm", 0}};
    const char *leaf = strrchr(key, '/') ? strrchr(key, '/') + 1 : key, *dot = strrchr(leaf, '.');
    if (!dot) return 1500;
    for (size_t i = 0; i < sizeof k / sizeof *k; i++)
        if (!strcmp(dot + 1, k[i].ext)) return k[i].f;
    // the types the 1.4.3 table lists at their own size; anything else is unknown
    static const char *const one[] = {"byml", "txtg", "pack", "bntx", "bphsh", "vsts", "ainb", "bphnm", "bars", "chunk",
        "bfevfl", "quad", "baev", "bcul", "crbin", "wbr", "bhtmp", "dpi", "cai", "png", "cutinfo", "blwp", "sarc", "tscb",
        "bfsha", "jpg", "beco", "bfarc", "blarc", "belnk", "genvb", "bnsh", "bslnk", "baatarc", "bagst"};
    for (size_t i = 0; i < sizeof one / sizeof *one; i++)
        if (!strcmp(dot + 1, one[i])) return 1000;
    return 1500;
}
static uint32_t clamp(uint64_t v) { return v > 0xFFFFFFF0u ? 0xFFFFFFF0u : (uint32_t)v; }
static uint32_t estimate(const char *key, uint64_t size) {
    unsigned f = factor(key);
    if (!f) return 0;
    return clamp(((size * f + 999) / 1000 + 8192 + 4095) & ~4095ull);
}
static uint32_t grow(const char *key, uint32_t entry, uint64_t old, uint64_t now, int mc) {
    if (now <= old) return entry;
    if (mc) return clamp(old ? ((uint64_t)entry * now + old - 1) / old : estimate(key, now * 8));
    unsigned f = factor(key);
    if (!f) return entry;
    return clamp((entry + ((now - old) * f + 999) / 1000 + 31) & ~31ull);
}

/* ---- decompression: plain zstd, or zstd with one of the game's dictionaries (Pack/ZsDic.pack.zs, a SARC of them) */
typedef struct { uint8_t *pack; size_t n; } Dicts;
static const uint8_t *dict_by_id(const Dicts *dd, unsigned id, size_t *dn) {
    const uint8_t *p = dd->pack;
    size_t n = dd->n;
    if (!p || n < 0x14 || memcmp(p, "SARC", 4)) return NULL;
    size_t head = rd16(p + 4), data = rd32(p + 12);
    if (head + 12 > n || data > n) return NULL;
    unsigned count = rd16(p + head + 6);
    for (unsigned i = 0; i < count; i++) {
        size_t node = head + 12 + (size_t)i * 16;
        if (node + 16 > n) return NULL;
        uint32_t a = rd32(p + node + 8), b = rd32(p + node + 12);
        if (a < b && data + b <= n && ZSTD_getDictID_fromDict(p + data + a, b - a) == id) { *dn = b - a; return p + data + a; }
    }
    return NULL;
}
// The decompressed size of a zstd file (its frame header), or the size itself for a file that is not zstd.
static uint64_t raw_size(const uint8_t *head, size_t hn, uint64_t file_size) {
    if (hn < 4 || rd32(head) != 0xFD2FB528u) return file_size;
    unsigned long long r = ZSTD_getFrameContentSize(head, hn);
    return r == ZSTD_CONTENTSIZE_UNKNOWN || r == ZSTD_CONTENTSIZE_ERROR ? file_size : (uint64_t)r;
}
static uint8_t *unzstd(const uint8_t *in, size_t n, const Dicts *dd, size_t *out_n) {
    if (n < 4 || rd32(in) != 0xFD2FB528u) return NULL;
    unsigned long long r = ZSTD_getFrameContentSize(in, n);
    if (r == ZSTD_CONTENTSIZE_UNKNOWN || r == ZSTD_CONTENTSIZE_ERROR || r > (512u << 20)) return NULL;
    uint8_t *out = malloc(r ? (size_t)r : 1);
    if (!out) return NULL;
    unsigned id = ZSTD_getDictID_fromFrame(in, n);
    size_t got;
    if (id) {
        size_t dn = 0;
        const uint8_t *d = dict_by_id(dd, id, &dn);
        ZSTD_DCtx *dc = d ? ZSTD_createDCtx() : NULL;
        got = dc ? ZSTD_decompress_usingDict(dc, out, (size_t)r, in, n, d, dn) : (size_t)-1;
        if (dc) ZSTD_freeDCtx(dc);
    } else
        got = ZSTD_decompress(out, (size_t)r, in, n);
    if (ZSTD_isError(got) || got != r) { free(out); return NULL; }
    *out_n = (size_t)r;
    return out;
}

/* ---- packs: SARC archives. "SARC", u16 header size, u16 BOM, u32 size, u32 data offset; SFAT at the header size: u16
   node count at +6, nodes at +12 of {u32 name hash, u32 attributes (bit 24: named, low 24 bits: name offset / 4),
   u32 start, u32 end} relative to the data offset; SFNT after the nodes with the NUL-terminated names. */
typedef struct { const char *name; uint64_t size; } Member;
static int members(const uint8_t *p, size_t n, Member **out, size_t *count) {
    *out = NULL, *count = 0;
    if (n < 0x14 || memcmp(p, "SARC", 4) || rd16(p + 6) != 0xFEFF) return 0;
    size_t head = rd16(p + 4), data = rd32(p + 12);
    if (head + 12 > n || memcmp(p + head, "SFAT", 4) || data > n) return 0;
    size_t cnt = rd16(p + head + 6), nodes = head + rd16(p + head + 4), names = nodes + cnt * 16;
    if (names + 8 > n || memcmp(p + names, "SFNT", 4)) return 0;
    names += rd16(p + names + 4);
    Member *m = malloc((cnt + 1) * sizeof *m);
    if (!m) return 0;
    size_t k = 0;
    for (size_t i = 0; i < cnt; i++) {
        const uint8_t *e = p + nodes + i * 16;
        uint32_t attr = rd32(e + 4), lo = rd32(e + 8), hi = rd32(e + 12);
        if (!(attr >> 24)) continue;
        size_t s = names + (size_t)(attr & 0xFFFFFF) * 4;
        if (s >= n || !memchr(p + s, 0, n - s) || lo > hi || hi > n - data) { free(m); return 0; }
        m[k++] = (Member){(const char *)p + s, hi - lo};
    }
    *out = m, *count = k;
    return 1;
}
static int member_cmp(const void *a, const void *b) { return strcmp(((const Member *)a)->name, ((const Member *)b)->name); }

/* The rebuilt table, or NULL (the game's as it is). game: the game's compressed table; shipped[k]: the compressed
   tables the mods ship (top mod first); files[k]: every mod file served (RomFS path, its first bytes for the zstd
   header, its size, the game's copy's first bytes and size or orig_size 0 when the game lacks it). read(rel, mod) gives
   a whole file (malloc) for the packs: mod 1 the mod's copy, 0 the game's. */
uint8_t *totk_restbl_build(const uint8_t *game, size_t game_n, const uint8_t *const *shipped, const size_t *shipped_n,
                           int nshipped, const RestblFile *files, size_t nfiles,
                           void *(*read)(const char *rel, int mod, size_t *n, void *ctx), void *ctx,
                           const uint8_t *dicts_z, size_t dicts_zn, size_t *out_n) {
    uint64_t t0 = runtime_now_ns();
    Dicts dd = {0};
    size_t rn = 0;
    uint8_t *raw = unzstd(game, game_n, &dd, &rn), *out = NULL, *res = NULL;
    Table base = {0}, t = {0};
    if (!raw || !parse(&base, raw, rn) || !parse(&t, raw, rn)) {
        fprintf(stderr, "[totk] mods: the game's resource size table cannot be read; served as it is\n");
        goto done;
    }
    unsigned merged = 0, raised = 0, added = 0, members_raised = 0;
    for (int k = nshipped - 1; k >= 0; k--) {   // bottom mod first; raising only, so the order does not matter
        size_t sn = 0;
        uint8_t *s = unzstd(shipped[k], shipped_n[k], &dd, &sn);
        Table st;
        if (s && parse(&st, s, sn)) {
            for (size_t i = 0; i < st.nh; i++) {
                merged += raise_hashed(&t, st.h[i].hash, st.h[i].size);
            }
            for (size_t i = 0; i < st.nn; i++) {
                const uint8_t *e = st.named + i * (st.name_len + 4);
                char key[1024];
                snprintf(key, sizeof key, "%.*s", (int)st.name_len, (const char *)e);
                merged += raise_to(&t, key, rd32(e + st.name_len));
            }
            table_free(&st);
        }
        free(s);
    }
    for (size_t i = 0; i < nfiles; i++) {
        const RestblFile *f = &files[i];
        char key[1024];
        key_of(f->rel, key, sizeof key);
        if (!strncmp(key, "System/Resource/ResourceSizeTable", 33)) continue;
        int mc = ends(f->rel, ".mc");
        uint64_t now = mc ? f->size : raw_size(f->head, f->head_n, f->size);
        uint32_t entry = 0, want;
        if (f->orig_size && get(&base, key, &entry)) {
            uint64_t old = mc ? f->orig_size : raw_size(f->orig_head, f->orig_head_n, f->orig_size);
            want = grow(key, entry, old, now, mc);
            if (raise_to(&t, key, want)) raised++;
        } else if (!f->orig_size) {
            want = estimate(key, mc ? now * 8 : now);
            if (want && raise_to(&t, key, want)) added++;
        }
        if (!strncmp(f->rel, "Pack/", 5) && ends(f->rel, ".pack.zs") && read) {
            if (!dd.pack) {   // the dictionaries: the packs are compressed with one
                dd.pack = unzstd(dicts_z, dicts_zn, &(Dicts){0}, &dd.n);
            }
            size_t mn = 0, on = 0, mrn = 0, orn = 0;
            uint8_t *mz = read(f->rel, 1, &mn, ctx), *oz = f->orig_size ? read(f->rel, 0, &on, ctx) : NULL;
            uint8_t *mr = mz ? unzstd(mz, mn, &dd, &mrn) : NULL, *orr = oz ? unzstd(oz, on, &dd, &orn) : NULL;
            Member *mm = NULL, *om = NULL;
            size_t mcnt = 0, ocnt = 0;
            if (mr && members(mr, mrn, &mm, &mcnt)) {
                if (orr && members(orr, orn, &om, &ocnt)) qsort(om, ocnt, sizeof *om, member_cmp);
                for (size_t j = 0; j < mcnt; j++) {
                    char mk[1024];
                    key_of(mm[j].name, mk, sizeof mk);
                    int mmc = ends(mm[j].name, ".mc");
                    Member *was = om ? bsearch(&mm[j], om, ocnt, sizeof *om, member_cmp) : NULL;
                    uint32_t e = 0;
                    if (was) {
                        if (!get(&base, mk, &e)) continue;   // sized without the table
                        want = grow(mk, e, was->size, mm[j].size, mmc);
                    } else
                        want = estimate(mk, mmc ? mm[j].size * 8 : mm[j].size);
                    if (want && raise_to(&t, mk, want)) {
                        if (members_raised++ < 4) fprintf(stderr, "[totk] mods: resource size of %s (in %s) raised to %u\n", mk, f->rel, want);
                    }
                }
            } else if (mz)
                fprintf(stderr, "[totk] mods: %s is not a readable pack; its members keep their sizes\n", f->rel);
            free(mm); free(om); free(mz); free(oz); free(mr); free(orr);
        }
    }
    size_t wn = 0;
    out = write_table(&t, &wn);
    if (!out) goto done;
    size_t cap = ZSTD_compressBound(wn);
    res = malloc(cap);
    size_t zn = res ? ZSTD_compress(res, cap, out, wn, 1) : 0;
    if (!res || ZSTD_isError(zn)) { free(res); res = NULL; goto done; }
    *out_n = zn;
    fprintf(stderr, "[totk] mods: resource size table rebuilt: %u entries from the tables mods ship, %u raised for "
            "bigger files, %u added for new files, %u for pack members (%.0f ms)\n", merged, raised, added, members_raised,
            (runtime_now_ns() - t0) / 1e6);
done:
    free(raw); free(out); free(dd.pack);
    table_free(&base); table_free(&t);
    return res;
}
