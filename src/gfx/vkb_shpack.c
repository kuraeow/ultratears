// The translated shaders in one file instead of a folder of ~57,000 (issue #22): data/shaders/translated.pack, written by
// tools/pack_shaders.py (the format is described there). Mapped read-only: a lookup is a binary search of the index
// and copies, no file calls; the pages read are the system's file cache, not the game's memory.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <stdint.h>
const void *plat_map_file(const char *path, uint64_t len, uint64_t *size);   // platform/linux.c (mmap: not reachable here)
void plat_unmap_file(const void *p, uint64_t size);
#endif

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "vkb_shpack.h"

#define VLOG(...) LOG("[vk] " __VA_ARGS__)

typedef struct { uint8_t key[32]; uint64_t json_off, spv_off; uint32_t json_len, spv_len, stamp, zero; } rec_t;   // 64 bytes
static const uint8_t *g_map;
static const rec_t *g_idx;
static size_t g_n, g_size;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void open_pack(void) {
    const char *path = getenv("TOTK_SHADER_PACK");
    if (!path || !*path) path = "data/shaders/translated.pack";
#ifdef _WIN32
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    HANDLE m = GetFileSizeEx(f, &sz) && sz.QuadPart >= 32 ? CreateFileMappingA(f, NULL, PAGE_READONLY, 0, 0, NULL) : NULL;
    CloseHandle(f);
    const uint8_t *p = m ? MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0) : NULL;
    if (m) CloseHandle(m);   // the view keeps the mapping
#else
    uint64_t n = 0;
    const uint8_t *p = plat_map_file(path, 0, &n);
    struct { int64_t QuadPart; } sz = {(int64_t)n};
    if (p && n < 32) { plat_unmap_file(p, n); p = NULL; }
#define UnmapViewOfFile(p) plat_unmap_file(p, (uint64_t)sz.QuadPart)
#endif
    if (!p) { VLOG("shader pack %s: cannot map it", path); return; }
    uint32_t version, count;
    uint64_t at;
    memcpy(&version, p + 8, 4);
    memcpy(&count, p + 12, 4);
    memcpy(&at, p + 16, 8);
    if (memcmp(p, "UTSPACK1", 8) || version != 1 || at + (uint64_t)count * sizeof(rec_t) > (uint64_t)sz.QuadPart) {
        VLOG("shader pack %s: not a shader pack or damaged", path);
        UnmapViewOfFile(p);
        return;
    }
    g_map = p;
    g_size = (size_t)sz.QuadPart;
    g_idx = (const rec_t *)(p + at);
    g_n = count;
    VLOG("shader pack %s: %zu shaders, %.0f MiB", path, g_n, g_size / 1048576.0);
}
int vkb_shpack_on(void) { pthread_once(&g_once, open_pack); return g_map != NULL; }

static int unhex(const char *key, uint8_t out[32]) {
    for (int i = 0; i < 32; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = key[2 * i + k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) return 0;
            v = v * 16 + d;
        }
        out[i] = (uint8_t)v;
    }
    return 1;
}

int vkb_shpack_get(const char *key, char **json, uint32_t **spv, size_t *spv_bytes, uint32_t *stamp) {
    uint8_t k[32];
    if (!vkb_shpack_on() || !unhex(key, k)) return 0;
    size_t lo = 0, hi = g_n;
    while (lo < hi) {   // the index is sorted by key
        size_t mid = (lo + hi) / 2;
        int c = memcmp(g_idx[mid].key, k, 32);
        if (!c) {
            const rec_t *r = &g_idx[mid];
            if (r->json_off + r->json_len > g_size || r->spv_off + r->spv_len > g_size) return 0;
            char *j = malloc((size_t)r->json_len + 1);
            uint32_t *s = malloc(r->spv_len ? r->spv_len : 4);
            if (!j || !s) { free(j); free(s); return 0; }
            memcpy(j, g_map + r->json_off, r->json_len);
            j[r->json_len] = 0;
            memcpy(s, g_map + r->spv_off, r->spv_len);
            *json = j; *spv = s; *spv_bytes = r->spv_len; *stamp = r->stamp;
            return 1;
        }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}

size_t vkb_shpack_count(void) { return vkb_shpack_on() ? g_n : 0; }
void vkb_shpack_key(size_t i, char key[65], uint32_t *stamp) {
    static const char hex[] = "0123456789abcdef";
    for (int b = 0; b < 32; b++) { key[2 * b] = hex[g_idx[i].key[b] >> 4]; key[2 * b + 1] = hex[g_idx[i].key[b] & 15]; }
    key[64] = 0;
    *stamp = g_idx[i].stamp;
}
