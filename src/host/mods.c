/* Player mods: the folder scan, RomFS files, ExeFS patches, cheats and the code built for them (mods.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <strings.h>
#include <dirent.h>
#include <dlfcn.h>
#include <sys/mman.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#endif

#include "host.h"
#include "mods.h"

#define TITLE_ID "0100F2C0115B6000"
enum { MAX_MODS = 256 };

static Mod *g_mods;
static int g_nmods = -1;
static char g_dir[4096];

/* ======================================== files (UTF-8 paths) ======================================== */
typedef struct { const char *name; int dir; uint64_t size; int64_t mtime; } DirEntry;
typedef void (*DirFn)(const DirEntry *e, void *ctx);
#ifdef _WIN32
static wchar_t *wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
static void list_dir(const char *dir, DirFn fn, void *ctx) {
    char pat[4200];
    snprintf(pat, sizeof pat, "%s/*", dir);
    wchar_t *w = wide(pat);
    WIN32_FIND_DATAW d;
    HANDLE h = w ? FindFirstFileExW(w, FindExInfoBasic, &d, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH)
                 : INVALID_HANDLE_VALUE;
    free(w);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(d.cFileName, L".") || !wcscmp(d.cFileName, L"..")) continue;
        char name[1024];
        if (!WideCharToMultiByte(CP_UTF8, 0, d.cFileName, -1, name, sizeof name, NULL, NULL)) continue;
        uint64_t ft = (uint64_t)d.ftLastWriteTime.dwHighDateTime << 32 | d.ftLastWriteTime.dwLowDateTime;
        DirEntry e = {name, !!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY),
                      (uint64_t)d.nFileSizeHigh << 32 | d.nFileSizeLow, (int64_t)(ft / 10000000ull) - 11644473600ll};
        fn(&e, ctx);
    } while (FindNextFileW(h, &d));
    FindClose(h);
}
static int is_dir(const char *p) {
    wchar_t *w = wide(p);
    DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    free(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
int mods_open_file(const char *full) {
    wchar_t *w = wide(full);
    int fd = w ? _wopen(w, O_RDONLY | O_BINARY) : -1;
    free(w);
    return fd;
}
#else
static void list_dir(const char *dir, DirFn fn, void *ctx) {
    DIR *d = opendir(dir);
    if (!d) return;
    for (struct dirent *x; (x = readdir(d));) {
        if (!strcmp(x->d_name, ".") || !strcmp(x->d_name, "..")) continue;
        char full[8192];
        snprintf(full, sizeof full, "%s/%s", dir, x->d_name);
        struct stat st;
        if (stat(full, &st)) continue;
        DirEntry e = {x->d_name, S_ISDIR(st.st_mode), (uint64_t)st.st_size, (int64_t)st.st_mtime};
        fn(&e, ctx);
    }
    closedir(d);
}
static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
int mods_open_file(const char *full) { return open(full, O_RDONLY); }
#endif

static void *read_whole(const char *path, size_t *n, size_t max) {
    int fd = mods_open_file(path);
    if (fd < 0) return NULL;
    off_t size = lseek(fd, 0, SEEK_END);
    uint8_t *d = size >= 0 && (size_t)size <= max ? malloc((size_t)size + 1) : NULL;
    size_t got = 0;
    if (d) {
        lseek(fd, 0, SEEK_SET);
        while (got < (size_t)size) {
            ssize_t r = read(fd, d + got, (size_t)size - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        if (got != (size_t)size) { free(d); d = NULL; } else d[got] = 0;
    }
    close(fd);
    if (d) *n = got;
    return d;
}

/* ======================================== the scan ======================================== */
// The first folder of a mod that holds part (romfs / exefs / cheats): the mod's own or the Atmosphere / emulator layouts.
static int find_part(const char *root, const char *part, char *out, size_t cap) {
    static const char *const forms[] = {"%s/%s", "%s/atmosphere/contents/" TITLE_ID "/%s", "%s/contents/" TITLE_ID "/%s",
                                        "%s/" TITLE_ID "/%s", "%s/atmosphere/contents/0100f2c0115b6000/%s",
                                        "%s/contents/0100f2c0115b6000/%s", "%s/0100f2c0115b6000/%s"};
    for (size_t i = 0; i < sizeof forms / sizeof *forms; i++) {
        snprintf(out, cap, forms[i], root, part);
        if (is_dir(out)) return 1;
    }
    *out = 0;
    return 0;
}
static void add_subdir(const DirEntry *e, void *ctx) {
    char **list = ctx;   // {base, out}
    if (!e->dir) return;
    size_t used = strlen(list[1]);
    snprintf(list[1] + used, 4096 - used, "%s%s/%s", used ? "\n" : "", list[0], e->name);
}
static void collect_mod(const DirEntry *e, void *ctx) {
    (void)ctx;
    if (!e->dir || g_nmods >= MAX_MODS) return;
    Mod *m = &g_mods[g_nmods];
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", e->name);
    char root[4096 + 300];
    snprintf(root, sizeof root, "%s/%s", g_dir, e->name);
    find_part(root, "romfs", m->romfs, sizeof m->romfs);
    find_part(root, "cheats", m->cheats, sizeof m->cheats);
    find_part(root, "exefs", m->exefs, sizeof m->exefs);
    char ep[4400];
    snprintf(ep, sizeof ep, "%s/atmosphere/exefs_patches", root);
    if (is_dir(ep)) { char *list[2] = {ep, m->exefs}; list_dir(ep, add_subdir, list); }
    g_nmods++;
}
static int name_in(const char *list, const char *name) {   // list: names separated by |
    size_t n = strlen(name);
    for (const char *p = list; p && *p;) {
        const char *bar = strchr(p, '|');
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        if (len == n && !memcmp(p, name, n)) return 1;
        p = bar ? bar + 1 : NULL;
    }
    return 0;
}
static int pos_in(const char *list, const char *name) {
    size_t n = strlen(name);
    int k = 0;
    for (const char *p = list; p && *p; k++) {
        const char *bar = strchr(p, '|');
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        if (len == n && !memcmp(p, name, n)) return k;
        p = bar ? bar + 1 : NULL;
    }
    return 1 << 20;
}
static const char *g_order;
static int mod_cmp(const void *a, const void *b) {
    const Mod *x = a, *y = b;
    int px = pos_in(g_order, x->name), py = pos_in(g_order, y->name);
    return px != py ? (px < py ? -1 : 1) : strcmp(x->name, y->name);
}

static void cheats_load(void);
void mods_scan(void) {
    if (g_nmods >= 0) return;
    g_nmods = 0;
    uint64_t t0 = runtime_now_ns();
    const char *d = getenv("TOTK_MODS_DIR");
    snprintf(g_dir, sizeof g_dir, "%s", d && *d ? d : "mods");
    g_mods = calloc(MAX_MODS, sizeof *g_mods);
    if (!g_mods || !is_dir(g_dir)) return;
    list_dir(g_dir, collect_mod, NULL);
    g_order = getenv("TOTK_MODS_ORDER");
    qsort(g_mods, (size_t)g_nmods, sizeof *g_mods, mod_cmp);
    const char *off = getenv("TOTK_MODS_OFF");
    for (int i = 0; i < g_nmods; i++)
        g_mods[i].enabled = !strchr(g_mods[i].name, '|') && !name_in(off, g_mods[i].name);
    fprintf(stderr, "[totk] mods: %d in %s (%.0f ms)\n", g_nmods, g_dir, (runtime_now_ns() - t0) / 1e6);
    for (int i = 0; i < g_nmods; i++)
        fprintf(stderr, "[totk] mods: %d. %s%s:%s%s%s\n", i + 1, g_mods[i].name, g_mods[i].enabled ? "" : " (off)",
                g_mods[i].romfs[0] ? " romfs" : "", g_mods[i].exefs[0] ? " exefs" : "", g_mods[i].cheats[0] ? " cheats" : "");
}
int mods_count(void) { mods_scan(); return g_nmods; }
Mod *mods_at(int i) { mods_scan(); return i >= 0 && i < g_nmods ? &g_mods[i] : NULL; }
const char *mods_dir(void) { mods_scan(); return g_dir; }

/* ======================================== RomFS ======================================== */
typedef struct { ModFile *v; size_t n, cap; int mod; const char *root; } FileList;
static void walk_romfs(FileList *l, const char *dir, const char *rel);
typedef struct { FileList *l; const char *dir, *rel; } WalkCtx;
static void walk_one(const DirEntry *e, void *ctx) {
    WalkCtx *w = ctx;
    char rel[2048], full[8192];
    snprintf(rel, sizeof rel, "%s%s%s", w->rel, *w->rel ? "/" : "", e->name);
    snprintf(full, sizeof full, "%s/%s", w->dir, e->name);
    if (e->dir) { walk_romfs(w->l, full, rel); return; }
    FileList *l = w->l;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 1024;
        l->v = realloc(l->v, l->cap * sizeof *l->v);
        if (!l->v) runtime_fail("mods: file list memory");
    }
    l->v[l->n++] = (ModFile){strdup(rel), strdup(full), e->size, l->mod};
}
static void walk_romfs(FileList *l, const char *dir, const char *rel) {
    WalkCtx w = {l, dir, rel};
    list_dir(dir, walk_one, &w);
}
static int file_cmp(const void *a, const void *b) {
    const ModFile *x = a, *y = b;
    int c = strcmp(x->rel, y->rel);
    return c ? c : x->mod - y->mod;   // the top mod first
}
static int other_version(const char *rel) {   // "<name>.Product.<n>.<ext>"
    const char *p = strstr(rel, ".Product.");
    return p && p[9] >= '0' && p[9] <= '9';
}
size_t mods_romfs_files(const ModFile **files, int (*has)(const char *rel, const char **spelling, void *ctx), void *ctx) {
    static FileList all;
    static int done;
    if (done) { *files = all.v; return all.n; }
    done = 1;
    mods_scan();
    uint64_t t0 = runtime_now_ns();
    for (int i = 0; i < g_nmods; i++) {
        if (!g_mods[i].enabled || !g_mods[i].romfs[0]) continue;
        all.mod = i;
        walk_romfs(&all, g_mods[i].romfs, "");
    }
    for (size_t i = 0; i < all.n; i++) {   // the RomFS's spelling of a path that differs by case
        const char *sp = NULL;
        if (!has(all.v[i].rel, &sp, ctx) && sp) { free((void *)all.v[i].rel); all.v[i].rel = strdup(sp); }
    }
    qsort(all.v, all.n, sizeof *all.v, file_cmp);
    size_t k = 0;
    for (size_t i = 0; i < all.n; i++) {
        if (k && !strcmp(all.v[k - 1].rel, all.v[i].rel)) {   // a higher mod serves it
            g_mods[all.v[i].mod].overridden++;
            continue;
        }
        const char *sp = NULL;
        Mod *m = &g_mods[all.v[i].mod];
        if (has(all.v[i].rel, &sp, ctx)) m->replaced++;
        else { m->added++; if (other_version(all.v[i].rel)) m->other_version++; }
        all.v[k++] = all.v[i];
    }
    all.n = k;
    for (int i = 0; i < g_nmods; i++)
        if (g_mods[i].enabled && g_mods[i].romfs[0])
            fprintf(stderr, "[totk] mods: %s: %u game files replaced, %u added%s%.0u%s, %u served by a higher mod\n",
                    g_mods[i].name, g_mods[i].replaced, g_mods[i].added, g_mods[i].other_version ? " (" : "",
                    g_mods[i].other_version, g_mods[i].other_version ? " named for another game version)" : "",
                    g_mods[i].overridden);
    if (all.n) fprintf(stderr, "[totk] mods: %zu RomFS files from mods (scanned in %.0f ms)\n", all.n, (runtime_now_ns() - t0) / 1e6);
    *files = all.v;
    return all.n;
}

/* ======================================== ExeFS patches ======================================== */
typedef struct { uint64_t off; uint32_t len; const uint8_t *bytes; } PatchWrite;
typedef struct { PatchWrite *v; size_t n, cap; uint8_t *store; size_t store_n, store_cap; } Writes;
static void writes_add(Writes *w, uint64_t off, const uint8_t *b, uint32_t len) {
    if (w->n == w->cap) { w->cap = w->cap ? w->cap * 2 : 64; w->v = realloc(w->v, w->cap * sizeof *w->v); }
    if (w->store_n + len > w->store_cap) {   // offsets into store until the end: kept in v[].bytes as an index
        w->store_cap = (w->store_n + len) * 2 + 256;
        w->store = realloc(w->store, w->store_cap);
    }
    if (!w->v || !w->store) runtime_fail("mods: patch memory");
    memcpy(w->store + w->store_n, b, len);
    w->v[w->n++] = (PatchWrite){off, len, (const uint8_t *)(uintptr_t)w->store_n};
    w->store_n += len;
}
static int hexval(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
// A build id written in hex (a file name or @nsobid) names the module with build id bid: a prefix of at least 16
// digits, the rest of the module's id zeros.
static int bid_match(const char *hex, size_t n, const uint8_t bid[32]) {
    if (n < 16 || n > 64) return 0;
    for (size_t i = 0; i < 64; i++) {
        int want = (bid[i / 2] >> (i % 2 ? 0 : 4)) & 15;
        if (i < n ? hexval(hex[i]) != want : want != 0) return 0;
    }
    return 1;
}
static uint32_t be(const uint8_t *p, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v = v << 8 | p[i]; return v; }
// IPS / IPS32: file offsets in the NSO with its 0x100-byte header.
static int parse_ips(const uint8_t *d, size_t n, Writes *w) {
    int wide32 = n >= 5 && !memcmp(d, "IPS32", 5);
    if (!wide32 && !(n >= 5 && !memcmp(d, "PATCH", 5))) return 0;
    int ob = wide32 ? 4 : 3;
    size_t p = 5;
    for (;;) {
        if (p + ob > n) return 0;
        if (wide32 ? !memcmp(d + p, "EEOF", 4) : !memcmp(d + p, "EOF", 3)) return 1;
        uint32_t off = be(d + p, ob);
        p += ob;
        if (p + 2 > n) return 0;
        uint32_t len = be(d + p, 2);
        p += 2;
        if (len == 0) {   // a run of one byte
            if (p + 3 > n) return 0;
            uint32_t cnt = be(d + p, 2);
            uint8_t *run = malloc(cnt ? cnt : 1);
            if (!run) return 0;
            memset(run, d[p + 2], cnt);
            if (off >= 0x100) writes_add(w, off - 0x100, run, cnt);
            free(run);
            p += 3;
        } else {
            if (p + len > n) return 0;
            if (off >= 0x100) writes_add(w, off - 0x100, d + p, len);
            p += len;
        }
    }
}
// pchtxt (IPSwitch): "@nsobid-<hex>", "@flag offset_shift <n>" (the offsets plus n are file offsets with the header),
// "@flag le" / "@little-endian" (values written least significant byte first), @enabled / @disabled for the lines
// that follow, "@stop" ends; patch lines "<hex offset> <hex bytes>" or "<hex offset> \"text\"". // comments.
static int parse_pchtxt(char *t, Writes *w, char *bid_hex, size_t cap) {
    long shift = 0;
    int on = 1, le = 0, any = 0;
    *bid_hex = 0;
    for (char *line = strtok(t, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        while (*line == ' ' || *line == '\t') line++;
        char *c = strstr(line, "//");
        if (c) *c = 0;
        if (!*line) continue;
        if (!strncasecmp(line, "@nsobid-", 8)) {
            size_t k = 0;
            for (char *p = line + 8; hexval(*p) >= 0 && k + 1 < cap; p++) bid_hex[k++] = *p;
            bid_hex[k] = 0;
        } else if (!strncasecmp(line, "@flag", 5)) {
            char *p = line + 5;
            while (*p == ' ') p++;
            if (!strncasecmp(p, "offset_shift", 12)) shift = strtol(p + 12, NULL, 0);
            else if (!strncasecmp(p, "le", 2)) le = 1;
            else if (!strncasecmp(p, "be", 2)) le = 0;
        } else if (!strncasecmp(line, "@little-endian", 14)) le = 1;
        else if (!strncasecmp(line, "@big-endian", 11)) le = 0;
        else if (!strncasecmp(line, "@enabled", 8)) on = 1;
        else if (!strncasecmp(line, "@disabled", 9)) on = 0;
        else if (!strncasecmp(line, "@stop", 5)) break;
        else if (*line == '@') continue;
        else if (on) {
            char *end;
            unsigned long long off = strtoull(line, &end, 16);
            if (end == line) continue;
            while (*end == ' ' || *end == '\t') end++;
            uint8_t buf[1024];
            size_t len = 0;
            if (*end == '"') {
                for (char *p = end + 1; *p && *p != '"' && len < sizeof buf; p++) buf[len++] = (uint8_t)*p;
            } else {
                for (char *p = end; hexval(p[0]) >= 0 && hexval(p[1]) >= 0 && len < sizeof buf; p += 2)
                    buf[len++] = (uint8_t)(hexval(p[0]) << 4 | hexval(p[1]));
                if (le) for (size_t i = 0; i < len / 2; i++) { uint8_t x = buf[i]; buf[i] = buf[len - 1 - i]; buf[len - 1 - i] = x; }
            }
            long long img = (long long)off + shift - 0x100;
            if (len && img >= 0) writes_add(w, (uint64_t)img, buf, (uint32_t)len), any = 1;
        }
    }
    return any || *bid_hex;
}
static int ends_ci(const char *s, const char *tail) {
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && !strcasecmp(s + a - b, tail);
}
// The writes of every patch file of mod m for build bid; *other: files for another build.
typedef struct { Writes *w; const uint8_t *bid; uint32_t other; const char *dir; } PatchCtx;
static void patch_file(const DirEntry *e, void *ctx) {
    PatchCtx *pc = ctx;
    if (e->dir) return;
    int ips = ends_ci(e->name, ".ips"), pch = ends_ci(e->name, ".pchtxt");
    if (!ips && !pch) return;
    char full[8192];
    snprintf(full, sizeof full, "%s/%s", pc->dir, e->name);
    size_t n = 0;
    uint8_t *d = read_whole(full, &n, 64u << 20);
    if (!d) return;
    Writes one = {0};
    char hex[80] = {0};
    int ok = ips ? parse_ips(d, n, &one) : parse_pchtxt((char *)d, &one, hex, sizeof hex);
    if (ips) snprintf(hex, sizeof hex, "%.*s", (int)(strlen(e->name) - 4), e->name);
    if (ok && bid_match(hex, strlen(hex), pc->bid)) {
        for (size_t i = 0; i < one.n; i++) writes_add(pc->w, one.v[i].off, one.store + (uintptr_t)one.v[i].bytes, one.v[i].len);
    } else if (ok)
        pc->other++;
    else
        fprintf(stderr, "[totk] mods: %s cannot be read as a patch file\n", full);
    free(one.v); free(one.store); free(d);
}
static void mod_patches(const Mod *m, const uint8_t *bid, Writes *w, uint32_t *other) {
    char dirs[4096];
    snprintf(dirs, sizeof dirs, "%s", m->exefs);
    for (char *p = dirs; p && *p;) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        PatchCtx pc = {w, bid, 0, p};
        list_dir(p, patch_file, &pc);
        *other += pc.other;
        p = nl ? nl + 1 : NULL;
    }
}

static uint8_t g_main_bid[32];

void mods_patch_module(const char *name, uint8_t *image, uint64_t size, uint64_t text_off, uint64_t text_size,
                       const uint8_t bid[32]) {
    if (!strcmp(name, "main")) memcpy(g_main_bid, bid, 32);
    mods_scan();
    uint64_t t0 = runtime_now_ns();
    int any = 0;
    for (int i = g_nmods - 1; i >= 0; i--) {   // the bottom mod first: the top one's bytes stay
        Mod *m = &g_mods[i];
        if (!m->enabled || !m->exefs[0]) continue;
        any = 1;
        Writes w = {0};
        uint32_t other = 0;
        mod_patches(m, bid, &w, &other);
        if (!strcmp(name, "main")) m->other_build = other;
        uint32_t data = 0, code = 0, outside = 0;
        for (size_t k = 0; k < w.n; k++) {
            const PatchWrite *p = &w.v[k];
            if (p->off + p->len > size) { outside++; continue; }
            memcpy(image + p->off, w.store + (uintptr_t)p->bytes, p->len);
            uint64_t lo = p->off, hi = p->off + p->len;
            if (lo < text_off + text_size && hi > text_off) {
                for (uint64_t a = (lo > text_off ? lo : text_off) & ~3ull; a < hi && a < text_off + text_size; a += 4) code++;
                if (lo < text_off) data += (uint32_t)(text_off - lo);
                if (hi > text_off + text_size) data += (uint32_t)(hi - text_off - text_size);
            } else
                data += p->len;
        }
        m->data_patches += data, m->code_patches += code;
        if (w.n || (other && !strcmp(name, "main")))
            fprintf(stderr, "[totk] mods: %s: %s: %u bytes of data patched, %u code words%s%s\n", m->name, name, data, code,
                    outside ? " (writes past the module left out)" : "",
                    other && !strcmp(name, "main") ? "; patch files for another game version left out" : "");
        free(w.v); free(w.store);
    }
    if (any) fprintf(stderr, "[totk] mods: %s's patches read and stored in %.1f ms\n", name, (runtime_now_ns() - t0) / 1e6);
}

/* ======================================== cheats ======================================== */
typedef struct {
    char name[128];
    uint32_t id;
    int master, on, code, bad;
    uint32_t *op;
    size_t nop;
    int mod;
} Cheat;
static Cheat *g_cheats;
static size_t g_ncheats;

uint32_t mods_cheat_id(const char *mod, const char *cheat) {
    uint32_t h = 2166136261u;
    for (const char *p = mod; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    h = (h ^ '/') * 16777619u;
    for (const char *p = cheat; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h;
}
static int cheat_on_listed(uint32_t id) {
    const char *l = getenv("TOTK_CHEATS_ON");
    char hex[16];
    snprintf(hex, sizeof hex, "%08x", id);
    for (const char *p = l; p && *p;) {
        while (*p == ',' || *p == ' ') p++;
        if (!strncasecmp(p, hex, 8) && (p[8] == 0 || p[8] == ',')) return 1;
        p = strchr(p, ',');
    }
    return 0;
}
static int op_words(const uint32_t *op, size_t left) {   // the length of the instruction at op, 0 if unknown
    uint32_t w = op[0];
    unsigned t = w >> 28, width = (w >> 24) & 15;
    switch (t) {
    case 0: case 1: return width == 8 ? 4 : 3;
    case 2: return 1;
    case 3: return (w >> 24) & 15 ? 1 : 2;
    case 4: return 3;
    case 5: return 2;
    case 6: return 3;
    case 7: return 2;
    case 8: return 1;
    case 9: return (w >> 8) & 1 ? (width == 8 ? 3 : 2) : 1;
    case 0xA: { unsigned o = (w >> 8) & 15; return o == 2 || o == 4 || o == 5 ? 2 : 1; }
    case 0xC: {
        unsigned sub = (w >> 24) & 15;
        if (sub == 0) {
            unsigned x = (w >> 8) & 15, tw = (w >> 20) & 15;
            return x == 0 || x == 2 ? 2 : x == 4 ? (tw == 8 ? 3 : 2) : 1;
        }
        return sub <= 3 ? 1 : 0;
    }
    default: (void)left; return 0;
    }
}
// A cheat file: [name] starts a cheat, {name} a master cheat; then 8-digit hex words.
static void cheats_file(const char *path, int mod) {
    size_t n = 0;
    char *t = read_whole(path, &n, 4u << 20);
    if (!t) return;
    Cheat *cur = NULL;
    for (char *line = strtok(t, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '[' || *line == '{') {
            char close = *line == '[' ? ']' : '}';
            char *e = strchr(line + 1, close);
            if (!e) continue;
            *e = 0;
            if (strstr(line + 1, "TID=") || !line[1]) { cur = NULL; continue; }
            g_cheats = realloc(g_cheats, (g_ncheats + 1) * sizeof *g_cheats);
            if (!g_cheats) runtime_fail("mods: cheat memory");
            cur = &g_cheats[g_ncheats++];
            memset(cur, 0, sizeof *cur);
            snprintf(cur->name, sizeof cur->name, "%s", line + 1);
            cur->master = close == '}', cur->mod = mod;
            // a name repeated in the file gets #2, #3, ... (the launcher's and the build's id)
            int same = 0;
            for (size_t k = 0; k + 1 < g_ncheats; k++)
                if (g_cheats[k].mod == mod && !strncmp(g_cheats[k].name, cur->name, strlen(cur->name)) &&
                    (g_cheats[k].name[strlen(cur->name)] == 0 || g_cheats[k].name[strlen(cur->name)] == '#'))
                    same++;
            if (same) snprintf(cur->name + strlen(cur->name), sizeof cur->name - strlen(cur->name), "#%d", same + 1);
            cur->id = mods_cheat_id(g_mods[mod].name, cur->name);
            continue;
        }
        if (!cur) continue;
        for (char *p = line; *p;) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            char *e;
            unsigned long v = strtoul(p, &e, 16);
            if (e == p) { cur->bad = 1; break; }
            cur->op = realloc(cur->op, (cur->nop + 1) * sizeof *cur->op);
            if (!cur->op) runtime_fail("mods: cheat memory");
            cur->op[cur->nop++] = (uint32_t)v;
            p = e;
        }
    }
    free(t);
}
static void cheats_load(void) {
    static int done;
    if (done) return;
    done = 1;
    mods_scan();
    char want[17];
    for (int i = 0; i < 8; i++) snprintf(want + i * 2, 3, "%02X", g_main_bid[i]);
    for (int i = 0; i < g_nmods; i++) {
        if (!g_mods[i].enabled || !g_mods[i].cheats[0]) continue;
        char path[8192];
        snprintf(path, sizeof path, "%s/%s.txt", g_mods[i].cheats, want);
        size_t first = g_ncheats;
        cheats_file(path, i);
        if (g_ncheats == first) {   // the name in lower case (a case-sensitive file system)
            for (char *p = path + strlen(g_mods[i].cheats) + 1; *p && *p != '.'; p++)
                if (*p >= 'A' && *p <= 'F') *p += 32;
            cheats_file(path, i);
        }
        for (size_t k = first; k < g_ncheats; k++) {
            Cheat *c = &g_cheats[k];
            for (size_t j = 0; j < c->nop && !c->bad;) {
                int len = op_words(c->op + j, c->nop - j);
                if (!len || j + (size_t)len > c->nop) { c->bad = 1; break; }
                if ((c->op[j] >> 28) == 0 && ((c->op[j] >> 20) & 15) == 0) {   // a store into main
                    uint64_t off = (uint64_t)(c->op[j] & 0xFF) << 32 | c->op[j + 1];
                    uint64_t text = 0;
                    for (int mi = 0; mi < g_nmodules; mi++)
                        if (!strcmp(g_modules[mi].name, "main")) text = g_modules[mi].ro_off;   // code and the padding after it (code caves)
                    if (off < text) c->code = 1;
                }
                j += (size_t)len;
            }
            c->on = (c->master || cheat_on_listed(c->id)) && c->nop && !c->bad;
            g_mods[i].cheats_total++;
            if (c->on) g_mods[i].cheats_on++;
        }
        fprintf(stderr, "[totk] mods: %s: %u cheats, %u on\n", g_mods[i].name, g_mods[i].cheats_total, g_mods[i].cheats_on);
    }
}

/* The cheat VM (Atmosphere's dmnt opcodes 0-A and C0-C3) over guest memory. Code stores (opcode 0 into main's code)
   are not run here: the built code (mods_code_load) has them behind the cheat's switch. */
static uint64_t vm_reg[16], vm_saved[16], vm_static[128];
static uint64_t region_base(unsigned m) {
    uint64_t runtime_region_base(int which);   // svc.c: 1 heap, 2 alias, 3 ASLR
    return m == 0 ? MAIN_BASE : runtime_region_base((int)m);
}
static int mem_ok(uint64_t a, unsigned n) { return a >= GUEST_LO && a + n <= GUEST_HI && a + n >= a; }
static uint64_t rd(uint64_t a, unsigned w) {
    if (!mem_ok(a, w)) return 0;
    switch (w) {
    case 1: return *(volatile uint8_t *)(uintptr_t)a;
    case 2: { uint16_t v; memcpy(&v, (void *)(uintptr_t)a, 2); return v; }
    case 4: { uint32_t v; memcpy(&v, (void *)(uintptr_t)a, 4); return v; }
    default: { uint64_t v; memcpy(&v, (void *)(uintptr_t)a, 8); return v; }
    }
}
static void wr(uint64_t a, unsigned w, uint64_t v) {
    if (!mem_ok(a, w) || (w != 1 && w != 2 && w != 4 && w != 8)) return;
    guest_prepare_write((void *)(uintptr_t)a, w);
    memcpy((void *)(uintptr_t)a, &v, w);   // little-endian host: the low bytes
}
static int cond(unsigned c, uint64_t a, uint64_t b) {
    switch (c) {
    case 1: return a > b; case 2: return a >= b; case 3: return a < b; case 4: return a <= b;
    case 5: return a == b; case 6: return a != b; default: return 0;
    }
}
static uint64_t arith(unsigned op, uint64_t a, uint64_t b, unsigned w) {
    uint64_t r;
    switch (op) {
    case 0: r = a + b; break; case 1: r = a - b; break; case 2: r = a * b; break;
    case 3: r = a << b; break; case 4: r = a >> b; break; case 5: r = a & b; break;
    case 6: r = a | b; break; case 7: r = ~a; break; case 8: r = a ^ b; break;
    default: r = a; break;
    }
    return w == 8 ? r : r & ((1ull << (w * 8)) - 1);
}
// From a false condition at op i: the instruction after its matching else (21) or end (20).
static size_t skip_block(const Cheat *c, size_t i, int to_end) {
    int depth = 0;
    for (size_t j = i; j < c->nop;) {
        uint32_t w = c->op[j];
        unsigned t = w >> 28;
        int len = op_words(c->op + j, c->nop - j);
        if (!len) return c->nop;
        if (t == 1 || t == 8 || (t == 0xC && ((w >> 24) & 15) == 0)) depth++;
        else if (t == 2) {
            if (depth == 0) {
                if (((w >> 24) & 15) == 1 && !to_end) return j + 1;   // else
                if (((w >> 24) & 15) == 0) return j + 1;              // end
            } else if (((w >> 24) & 15) == 0) depth--;
        }
        j += (size_t)len;
    }
    return c->nop;
}
static void vm_run(const Cheat *c, uint64_t keys) {
    size_t loop_start[16] = {0};
    for (size_t i = 0; i < c->nop;) {
        const uint32_t *o = c->op + i;
        uint32_t w = o[0];
        unsigned t = w >> 28, width = (w >> 24) & 15;
        int len = op_words(o, c->nop - i);
        if (!len || i + (size_t)len > c->nop) return;
        size_t next = i + (size_t)len;
        switch (t) {
        case 0: {   // store static: 0TMR00AA AAAAAAAA VVVVVVVV(VVVVVVVV)
            unsigned m = (w >> 20) & 15, r = (w >> 16) & 15;
            uint64_t a = region_base(m) + vm_reg[r] + ((uint64_t)(w & 0xFF) << 32 | o[1]);
            uint64_t v = width == 8 ? (uint64_t)o[2] << 32 | o[3] : o[2];
            if (!(m == 0 && c->code)) wr(a, width, v);
            break;
        }
        case 1: {   // begin conditional: 1TMC00AA AAAAAAAA VVVVVVVV(VVVVVVVV)
            unsigned m = (w >> 20) & 15, cc = (w >> 16) & 15;
            uint64_t a = region_base(m) + ((uint64_t)(w & 0xFF) << 32 | o[1]);
            uint64_t v = width == 8 ? (uint64_t)o[2] << 32 | o[3] : o[2];
            if (!cond(cc, rd(a, width), v)) next = skip_block(c, next, 0);
            break;
        }
        case 2:   // end (20) passes; else (21) reached from the true branch skips to the end
            if (((w >> 24) & 15) == 1) next = skip_block(c, next, 1);
            break;
        case 3: {   // loop: 300R0000 VVVVVVVV ... 310R0000
            unsigned r = (w >> 16) & 15;
            if (!((w >> 24) & 15)) { vm_reg[r] = o[1]; loop_start[r] = next; }
            else if (vm_reg[r] && --vm_reg[r]) next = loop_start[r];
            break;
        }
        case 4: vm_reg[(w >> 16) & 15] = (uint64_t)o[1] << 32 | o[2]; break;
        case 5: {   // load register from memory: 5TMRI0AA AAAAAAAA
            unsigned m = (w >> 20) & 15, r = (w >> 16) & 15, from_reg = (w >> 12) & 15;
            uint64_t off = (uint64_t)(w & 0xFF) << 32 | o[1];
            vm_reg[r] = rd(from_reg ? vm_reg[r] + off : region_base(m) + off, width);
            break;
        }
        case 6: {   // store to register address: 6T0RIor0 VVVVVVVV VVVVVVVV
            unsigned r = (w >> 16) & 15, inc = (w >> 12) & 15, has_off = (w >> 8) & 15, ro = (w >> 4) & 15;
            uint64_t v = (uint64_t)o[1] << 32 | o[2];
            wr(vm_reg[r] + (has_off ? vm_reg[ro] : 0), width, v);
            if (inc) vm_reg[r] += width;
            break;
        }
        case 7: {   // arithmetic with a value: 7T0RC000 VVVVVVVV
            unsigned r = (w >> 16) & 15, op = (w >> 12) & 15;
            vm_reg[r] = arith(op, vm_reg[r], o[1], width);
            break;
        }
        case 8:   // keys held: 8kkkkkkk
            if ((keys & (w & 0x0FFFFFFF)) != (w & 0x0FFFFFFF)) next = skip_block(c, next, 0);
            break;
        case 9: {   // arithmetic with registers: 9TCRSIs0 (VVVVVVVV)
            unsigned op = (w >> 20) & 15, r = (w >> 16) & 15, s = (w >> 12) & 15, imm = (w >> 8) & 15, s2 = (w >> 4) & 15;
            uint64_t b = imm ? (width == 8 ? (uint64_t)o[1] << 32 | o[2] : o[1]) : vm_reg[s2];
            vm_reg[r] = arith(op, vm_reg[s], b, width);
            break;
        }
        case 0xA: {   // store register: ATSRIOxa (aaaaaaaa)
            unsigned s = (w >> 20) & 15, r = (w >> 16) & 15, inc = (w >> 12) & 15, ot = (w >> 8) & 15, x = (w >> 4) & 15;
            uint64_t a = vm_reg[r], imm = len == 2 ? ((uint64_t)(w & 15) << 32 | o[1]) : 0;
            switch (ot) {
            case 1: a += vm_reg[x]; break;
            case 2: a += imm; break;
            case 3: a = region_base(x) + vm_reg[r]; break;
            case 4: a = region_base(x) + imm; break;
            case 5: a = region_base(x) + vm_reg[r] + imm; break;
            }
            wr(a, width, vm_reg[s]);
            if (inc) vm_reg[r] += width;
            break;
        }
        case 0xC: {
            unsigned sub = (w >> 24) & 15;
            if (sub == 0) {   // register conditional: C0TcSX## ...
                unsigned tw = (w >> 20) & 15, cc = (w >> 16) & 15, s = (w >> 12) & 15, x = (w >> 8) & 15;
                unsigned a1 = (w >> 4) & 15, a2 = w & 15;
                uint64_t v = 0, rel = len >= 2 ? ((uint64_t)a2 << 32 | o[1]) : 0;
                switch (x) {
                case 0: v = rd(region_base(a1) + rel, tw); break;
                case 1: v = rd(region_base(a1) + vm_reg[a2], tw); break;
                case 2: v = rd(vm_reg[a1] + rel, tw); break;
                case 3: v = rd(vm_reg[a1] + vm_reg[a2], tw); break;
                case 4: v = tw == 8 ? (uint64_t)o[1] << 32 | o[2] : o[1]; break;
                case 5: v = vm_reg[a1]; break;
                }
                uint64_t mask = tw == 8 ? ~0ull : (1ull << (tw * 8)) - 1;
                if (!cond(cc, vm_reg[s] & mask, v & mask)) next = skip_block(c, next, 0);
            } else if (sub == 1) {   // save / restore a register: C10D0Sx0
                unsigned d = (w >> 16) & 15, s = (w >> 8) & 15, x = (w >> 4) & 15;
                if (x == 0) vm_reg[d] = vm_saved[s];
                else if (x == 1) vm_saved[d] = vm_reg[s];
                else if (x == 2) vm_saved[d] = 0;
                else if (x == 3) vm_reg[d] = 0;
            } else if (sub == 2) {   // save / restore by mask: C2x0XXXX
                unsigned x = (w >> 20) & 15, mask = w & 0xFFFF;
                for (int k = 0; k < 16; k++)
                    if (mask >> k & 1) {
                        if (x == 0) vm_reg[k] = vm_saved[k];
                        else if (x == 1) vm_saved[k] = vm_reg[k];
                        else if (x == 2) vm_saved[k] = 0;
                        else if (x == 3) vm_reg[k] = 0;
                    }
            } else if (sub == 3) {   // static register: C3000XXx (XX < 0x80 read into x, else write from x)
                unsigned idx = (w >> 4) & 0xFF, r = w & 15;
                if (idx < 0x80) vm_reg[r] = vm_static[idx];
                else vm_static[idx & 0x7F] = vm_reg[r];
            }
            break;
        }
        default: return;
        }
        i = next;
    }
}
void mods_cheats_frame(void) {
    static int ready = -1;
    if (ready < 0) {
        cheats_load();
        ready = 0;
        for (size_t i = 0; i < g_ncheats; i++) ready |= g_cheats[i].on && !g_cheats[i].code;
    }
    if (!ready) return;
    uint64_t keys = runtime_input_buttons_last();
    for (size_t i = 0; i < g_ncheats; i++)
        if (g_cheats[i].on && !g_cheats[i].code) vm_run(&g_cheats[i], keys);
}

/* ======================================== the built code ======================================== */
/* data/mods/code.dll (Linux: code.so) and code.txt from tools/mods_code.py. code.txt:
     stamp <16 hex>           the patch and cheat files it was built from (mods_stamp)
     exe <size> <mtime>       the game it was built for
     sw <i> mod <name>        switch i: mod <name> is on
     sw <i> cheat <id> <name> switch i: cheat <id> (8 hex) of mod <name> is on
   The library exports ultrs_fn_count / ultrs_fn_addr / ultrs_fn_ptr (its functions by guest address), ultrs_ext_count
   / ultrs_ext_addr / ultrs_ext_ptr (the game's functions it calls, filled here), ultrs_imp_count / ultrs_imp_name /
   ultrs_imp_ptr (the runtime's functions it calls, filled here), ultrs_sw (the switches), g_simd_nanvec. */
typedef struct { const char *name; void *fn; } Import;
extern int g_simd_nanvec;
void rt_spin_gate(Cpu *c, uint64_t target);
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0);
static uint64_t keys_now(void) { return runtime_input_buttons_last(); }
// The runtime functions lifted code calls, and those tools/lifter/patches.py's replacements call.
#define IMP(f) {#f, (void *)(f)}
int totk_zstd_native(void); void totk_zstd_multiframe(Cpu *c); void totk_zstd_using_ddict(Cpu *c);
float totk_fr_step_ratio30(void); int totk_fr_highfps(void); int totk_fr_realtime(void); int totk_fr_cloth(void);
float totk_fr_step_seconds(void); float totk_fr_step_ticks60(void); int totk_fr_fps60(void); int totk_drs_enabled(void);
int totk_ability_target_ok(uint64_t owner, uint64_t result, uint64_t actor);
int totk_shadow_size(uint32_t *n); int totk_output_size(uint64_t *packed); int totk_scene_size(float *w, float *h);
int totk_aspect(float *ratio);
void totk_mouse_camera_input(Cpu *c); void totk_mouse_orbit_begin(Cpu *c); void totk_mouse_orbit_state(Cpu *c);
void totk_mouse_orbit_angles(Cpu *c); void totk_mouse_hand_input(Cpu *c); void totk_mouse_load(Cpu *c, uint32_t offset);
void totk_mouse_camera_activity(Cpu *c);
static const Import IMPORTS[] = {
    IMP(rt_dispatch), IMP(rt_udf), IMP(rt_brk), IMP(rt_svc), IMP(rt_ticks), IMP(rt_spin_gate), IMP(fnprof_hit),
    {"ultrs_keys", (void *)keys_now},
    IMP(totk_zstd_native), IMP(totk_zstd_multiframe), IMP(totk_zstd_using_ddict), IMP(totk_fr_step_ratio30),
    IMP(totk_fr_highfps), IMP(totk_fr_realtime), IMP(totk_fr_cloth), IMP(totk_fr_step_seconds),
    IMP(totk_fr_step_ticks60), IMP(totk_fr_fps60), IMP(totk_drs_enabled), IMP(totk_ability_target_ok),
    IMP(totk_shadow_size), IMP(totk_output_size), IMP(totk_scene_size), IMP(totk_aspect), IMP(totk_mouse_camera_input),
    IMP(totk_mouse_orbit_begin), IMP(totk_mouse_orbit_state), IMP(totk_mouse_orbit_angles), IMP(totk_mouse_hand_input),
    IMP(totk_mouse_load), IMP(totk_mouse_camera_activity),
};

// FNV-1a 64 of every patch and cheat file of every mod, enabled or not: "<mod>\x1f<path in the mod>\x1f<size>\x1f<mtime>\n"
// sorted. tools/mods_code.py writes the same.
typedef struct { char **v; size_t n, cap; const char *mod; const char *base; } Keys;
static void key_add(Keys *k, const char *rel, uint64_t size, int64_t mtime) {
    if (k->n == k->cap) { k->cap = k->cap ? k->cap * 2 : 32; k->v = realloc(k->v, k->cap * sizeof *k->v); }
    char line[4600];
    snprintf(line, sizeof line, "%s\x1f%s\x1f%llu\x1f%lld\n", k->mod, rel, (unsigned long long)size, (long long)mtime);
    if (!k->v || !(k->v[k->n++] = strdup(line))) runtime_fail("mods: stamp memory");
}
typedef struct { Keys *k; const char *dir; } KeyDir;
static void key_patch(const DirEntry *e, void *ctx) {
    KeyDir *kd = ctx;
    if (e->dir || !(ends_ci(e->name, ".ips") || ends_ci(e->name, ".pchtxt"))) return;
    char full[8192];
    snprintf(full, sizeof full, "%s/%s", kd->dir, e->name);
    key_add(kd->k, full + strlen(kd->k->base) + 1, e->size, e->mtime);
}
static void key_cheat(const DirEntry *e, void *ctx) {
    KeyDir *kd = ctx;
    char want[24];
    for (int i = 0; i < 8; i++) snprintf(want + i * 2, 3, "%02X", g_main_bid[i]);
    strcat(want, ".txt");
    if (e->dir || strcasecmp(e->name, want)) return;
    char full[8192];
    snprintf(full, sizeof full, "%s/%s", kd->dir, e->name);
    key_add(kd->k, full + strlen(kd->k->base) + 1, e->size, e->mtime);
}
static int str_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static uint64_t mods_stamp(void) {
    Keys k = {0};
    for (int i = 0; i < g_nmods; i++) {
        char base[4400];
        snprintf(base, sizeof base, "%s/%s", g_dir, g_mods[i].name);
        k.mod = g_mods[i].name, k.base = base;
        char dirs[4096];
        snprintf(dirs, sizeof dirs, "%s", g_mods[i].exefs);
        for (char *p = dirs; *p;) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = 0;
            KeyDir kd = {&k, p};
            list_dir(p, key_patch, &kd);
            if (!nl) break;
            p = nl + 1;
        }
        if (g_mods[i].cheats[0]) { KeyDir kd = {&k, g_mods[i].cheats}; list_dir(g_mods[i].cheats, key_cheat, &kd); }
    }
    qsort(k.v, k.n, sizeof *k.v, str_cmp);
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < k.n; i++) {
        for (const char *p = k.v[i]; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
        free(k.v[i]);
    }
    free(k.v);
    return k.n ? h : 0;
}

#ifdef _WIN32
#define CODE_LIB "data/mods/code.dll"
static void *lib_open(const char *p) { wchar_t *w = wide(p); void *h = w ? (void *)LoadLibraryW(w) : NULL; free(w); return h; }
static void *lib_sym(void *h, const char *s) { return (void *)GetProcAddress((HMODULE)h, s); }
static uint8_t *near_block(uintptr_t at) {   // 64 KiB of executable memory within 2 GiB of at
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t g = si.dwAllocationGranularity;
    for (uintptr_t d = g; d < (1ull << 31) - (1u << 20); d += g)
        for (int s = 0; s < 2; s++) {
            uintptr_t a = s ? (at + d) & ~(g - 1) : (at - d) & ~(g - 1);
            void *p = VirtualAlloc((void *)a, 1 << 16, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
    return NULL;
}
static int code_write(void *at, const void *bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return 1;
}
#else
#define CODE_LIB "data/mods/code.so"
static void *lib_open(const char *p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static void *lib_sym(void *h, const char *s) { return dlsym(h, s); }
static uint8_t *near_block(uintptr_t at) {
    for (uintptr_t d = 1 << 20; d < (1ull << 31) - (1u << 20); d += 1 << 20)
        for (int s = 0; s < 2; s++) {
            uintptr_t a = (s ? at + d : at - d) & ~0xFFFFull;
            void *p = mmap((void *)a, 1 << 16, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p != MAP_FAILED) return p;
        }
    return NULL;
}
static int code_write(void *at, const void *bytes, size_t n) {
    uintptr_t pg = (uintptr_t)at & ~4095ull, end = ((uintptr_t)at + n + 4095) & ~4095ull;
    if (mprotect((void *)pg, end - pg, PROT_READ | PROT_WRITE | PROT_EXEC)) return 0;
    memcpy(at, bytes, n);
    mprotect((void *)pg, end - pg, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)at, (char *)at + n);
    return 1;
}
#endif

// The game's function at host address from jumps to `to`: a 5-byte jump into a 14-byte absolute jump beside the game.
static int redirect(uint8_t *from, void *to) {
    static uint8_t *block;
    static size_t used;
    if (!block || used + 16 > (1 << 16)) { block = near_block((uintptr_t)from); used = 0; }
    if (!block) return 0;
    uint8_t *t = block + used;
    int64_t rel = (int64_t)(t - (from + 5));
    if (rel != (int32_t)rel) { block = NULL; return redirect(from, to); }
    t[0] = 0xFF, t[1] = 0x25, memset(t + 2, 0, 4);   // jmp [rip+0]
    memcpy(t + 6, &to, 8);
    used += 16;
    uint8_t j[5] = {0xE9};
    int32_t r32 = (int32_t)rel;
    memcpy(j + 1, &r32, 4);
    return code_write(from, j, 5);
}

void mods_code_load(void) {
    uint64_t t0 = runtime_now_ns();
    mods_scan();
    cheats_load();
    int want = 0;
    for (int i = 0; i < g_nmods; i++) want |= g_mods[i].enabled && g_mods[i].code_patches;
    for (size_t i = 0; i < g_ncheats; i++) want |= g_cheats[i].on && g_cheats[i].code;
    size_t n = 0;
    char *txt = read_whole("data/mods/code.txt", &n, 16u << 20);
    if (!txt) {
        if (want) fprintf(stderr, "[totk] mods: code patches and code cheats need data/mods/code.dll (the launcher builds it "
                          "before the game starts): left out\n");
        return;
    }
    for (char *r = txt, *w = txt;; r++) {   // lines end in \n (a \r\n written by hand works too)
        if (*r != '\r') *w++ = *r;
        if (!*r) break;
    }
    uint64_t stamp = mods_stamp(), built = 0;
    unsigned long long exe_size = 0;
    long long exe_mtime = 0;
    char exe[4096];
    struct stat st;
    int exe_ok = plat_exe_path(exe, sizeof exe) && !stat(exe, &st);
    int last = -1, order_ok = 1;   // the mods' code patches were chained in the order of the build
    for (char *l = txt; l && *l;) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = 0;
        if (!strncmp(l, "stamp ", 6)) built = strtoull(l + 6, NULL, 16);
        else if (!strncmp(l, "exe ", 4)) sscanf(l + 4, "%llu %lld", &exe_size, &exe_mtime);
        else if (!strncmp(l, "sw ", 3) && strstr(l, " mod ")) {
            const char *name = strstr(l, " mod ") + 5;
            for (int k = 0; k < g_nmods; k++)
                if (!strcmp(g_mods[k].name, name)) { order_ok &= k > last; last = k; }
        }
        if (nl) *nl = '\n';
        l = nl ? nl + 1 : NULL;
    }
    if (!order_ok) built = 0;
    if (built != stamp || !exe_ok || exe_size != (unsigned long long)st.st_size || exe_mtime != (long long)st.st_mtime) {
        if (want)
            fprintf(stderr, "[totk] mods: data/mods/code.dll was built for other %s: code patches and code cheats left out "
                    "until the launcher builds it again\n", built != stamp ? "patch or cheat files" : "game files");
        free(txt);
        return;
    }
    void *lib = lib_open(CODE_LIB);
    uint64_t *fn_count = lib ? lib_sym(lib, "ultrs_fn_count") : NULL, *fn_addr = lib ? lib_sym(lib, "ultrs_fn_addr") : NULL;
    void **fn_ptr = lib ? lib_sym(lib, "ultrs_fn_ptr") : NULL;
    uint64_t *ext_count = lib ? lib_sym(lib, "ultrs_ext_count") : NULL, *ext_addr = lib ? lib_sym(lib, "ultrs_ext_addr") : NULL;
    void **ext_ptr = lib ? lib_sym(lib, "ultrs_ext_ptr") : NULL;
    uint64_t *imp_count = lib ? lib_sym(lib, "ultrs_imp_count") : NULL;
    const char **imp_name = lib ? lib_sym(lib, "ultrs_imp_name") : NULL;
    void **imp_ptr = lib ? lib_sym(lib, "ultrs_imp_ptr") : NULL;
    uint8_t *sw = lib ? lib_sym(lib, "ultrs_sw") : NULL;
    int *nanvec = lib ? lib_sym(lib, "g_simd_nanvec") : NULL;
    if (!fn_count || !fn_addr || !fn_ptr || !ext_count || !ext_addr || !ext_ptr || !imp_count || !imp_name || !imp_ptr || !sw) {
        fprintf(stderr, "[totk] mods: %s cannot be loaded: code patches and code cheats left out\n", CODE_LIB);
        free(txt);
        return;
    }
    for (uint64_t i = 0; i < *imp_count; i++) {
        imp_ptr[i] = NULL;
        for (size_t k = 0; k < sizeof IMPORTS / sizeof *IMPORTS; k++)
            if (!strcmp(imp_name[i], IMPORTS[k].name)) imp_ptr[i] = IMPORTS[k].fn;
        if (!imp_ptr[i]) {
            fprintf(stderr, "[totk] mods: %s calls %s, which this game does not give it: code patches left out\n", CODE_LIB, imp_name[i]);
            free(txt);
            return;
        }
    }
    for (uint64_t i = 0; i < *ext_count; i++)
        if (!(ext_ptr[i] = (void *)dispatch_lookup(ext_addr[i]))) {
            fprintf(stderr, "[totk] mods: %s calls a game function at %llx that this game lacks: code patches left out\n",
                    CODE_LIB, (unsigned long long)ext_addr[i]);
            free(txt);
            return;
        }
    if (nanvec) *nanvec = g_simd_nanvec;
    // the switches
    unsigned on = 0, total = 0;
    for (char *l = txt; l && *l;) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = 0;
        unsigned i;
        char kind[16];
        int at = 0;
        if (sscanf(l, "sw %u %15s %n", &i, kind, &at) == 2 && at) {
            const char *rest = l + at;
            int v = 0;
            if (!strcmp(kind, "mod")) {
                for (int k = 0; k < g_nmods; k++)
                    if (!strcmp(g_mods[k].name, rest)) { v = g_mods[k].enabled; if (v) g_mods[k].code_active = 1; }
            } else if (!strcmp(kind, "cheat")) {
                uint32_t id = (uint32_t)strtoul(rest, NULL, 16);
                for (size_t k = 0; k < g_ncheats; k++) if (g_cheats[k].id == id) v = g_cheats[k].on;
            }
            sw[i] = (uint8_t)v;
            on += v != 0, total++;
        }
        l = nl ? nl + 1 : NULL;
    }
    free(txt);
    unsigned moved = 0;
    for (uint64_t i = 0; i < *fn_count; i++) {
        GuestFn f = dispatch_lookup(fn_addr[i]);
        if (f && redirect((uint8_t *)(uintptr_t)f, fn_ptr[i])) moved++;
        else fprintf(stderr, "[totk] mods: the game's function %llx cannot be replaced\n", (unsigned long long)fn_addr[i]);
    }
    fprintf(stderr, "[totk] mods: %llu functions from %s in place of the game's (%u replaced), %u of %u switches on (%.0f ms)\n",
            (unsigned long long)*fn_count, CODE_LIB, moved, on, total, (runtime_now_ns() - t0) / 1e6);
}
