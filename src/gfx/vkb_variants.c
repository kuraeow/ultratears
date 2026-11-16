// The vertex shader variants to make ahead: data/shaders/variants.txt (TOTK_SHADER_VARIANTS=<file>; =0: none), written
// by tools/shader_variants.py from the MacNativeGames shared shader list (MIT; the variants players met). Lines:
//   I <vertex shader key> <integer mask, hex> <signed mask, hex>    a draw of it with integer attributes there
//   P <vertex shader key> <fragment shader key>                     a draw of it with that fragment shader
// Read once, kept sorted by vertex shader key. vkb_shader.c compiles these variants ahead with the shaders and makes
// a program's variants when the program is set, so that a draw does not compile them (frames of 50-210 ms where new
// creatures came into view, 2026-11-16).
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "vkb.h"
#include "vkb_variants.h"

#define VLOG(...) LOG("[vk] " __VA_ARGS__)

typedef struct { uint8_t vs[32]; uint32_t im, sm; } int_rec;
typedef struct { uint8_t vs[32], fs[32]; } pair_rec;
static int_rec *g_int;
static pair_rec *g_pair;
static size_t g_nint, g_npair;
static uint32_t g_hash;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static int unhex(const char *s, uint8_t out[32]) {
    for (int i = 0; i < 32; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = s[2 * i + k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) return 0;
            v = v * 16 + d;
        }
        out[i] = (uint8_t)v;
    }
    return 1;
}
static int cmp_int(const void *a, const void *b) { return memcmp(a, b, 32); }
static int cmp_pair(const void *a, const void *b) { return memcmp(a, b, 64); }

static void load(void) {
    const char *path = getenv("TOTK_SHADER_VARIANTS");
    if (path && !strcmp(path, "0")) { VLOG("shader variants ahead: off (TOTK_SHADER_VARIANTS=0)"); return; }
    if (!path || !*path) path = "data/shaders/variants.txt";
    FILE *f = fopen(path, "rb");
    if (!f) { VLOG("shader variants ahead: no %s (tools/shader_variants.py): only the guesses from the shaders", path); return; }
    size_t cap_i = 0, cap_p = 0;
    uint32_t h = 2166136261u;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        for (const char *c = line; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
        char kind, a[80], b[80];
        if (line[0] == '#' || sscanf(line, "%c %79s %79s", &kind, a, b) != 3 || strlen(a) != 64) continue;
        if (kind == 'I') {
            unsigned im, sm = 0;
            if (sscanf(b, "%x", &im) != 1) continue;
            const char *third = strchr(strchr(line + 2, ' ') + 1, ' ');
            if (third) sscanf(third, "%x", &sm);
            if (g_nint == cap_i && !(g_int = realloc(g_int, (cap_i = cap_i ? cap_i * 2 : 4096) * sizeof *g_int))) die("shader variants");
            int_rec *r = &g_int[g_nint];
            if (unhex(a, r->vs)) { r->im = im; r->sm = sm & im; g_nint++; }
        } else if (kind == 'P' && strlen(b) == 64) {
            if (g_npair == cap_p && !(g_pair = realloc(g_pair, (cap_p = cap_p ? cap_p * 2 : 16384) * sizeof *g_pair))) die("shader variants");
            pair_rec *r = &g_pair[g_npair];
            if (unhex(a, r->vs) && unhex(b, r->fs)) g_npair++;
        }
    }
    fclose(f);
    qsort(g_int, g_nint, sizeof *g_int, cmp_int);
    qsort(g_pair, g_npair, sizeof *g_pair, cmp_pair);
    g_hash = h ? h : 1;
    VLOG("shader variants ahead: %zu integer attribute variants, %zu vertex / fragment pairs (%s)", g_nint, g_npair, path);
}

// the first record of key in a sorted array of n records of size sz (key first): its index, n if none
static size_t first_of(const void *base, size_t n, size_t sz, const uint8_t key[32]) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (memcmp((const uint8_t *)base + mid * sz, key, 32) < 0) lo = mid + 1; else hi = mid;
    }
    return lo < n && !memcmp((const uint8_t *)base + lo * sz, key, 32) ? lo : n;
}

int vkb_variants_ints(const char *vs_key, uint32_t (*out)[2], int max) {
    pthread_once(&g_once, load);
    uint8_t k[32];
    if (!g_nint || !unhex(vs_key, k)) return 0;
    int n = 0;
    for (size_t i = first_of(g_int, g_nint, sizeof *g_int, k); i < g_nint && n < max && !memcmp(g_int[i].vs, k, 32); i++)
        out[n][0] = g_int[i].im, out[n][1] = g_int[i].sm, n++;
    return n;
}

int vkb_variants_pairs(const char *vs_key, char (*out)[65], int max) {
    pthread_once(&g_once, load);
    uint8_t k[32];
    if (!g_npair || !unhex(vs_key, k)) return 0;
    int n = 0;
    for (size_t i = first_of(g_pair, g_npair, sizeof *g_pair, k); i < g_npair && n < max && !memcmp(g_pair[i].vs, k, 32); i++, n++)
        for (int b = 0; b < 32; b++) snprintf(out[n] + 2 * b, 3, "%02x", g_pair[i].fs[b]);
    return n;
}

uint32_t vkb_variants_hash(void) {
    pthread_once(&g_once, load);
    return g_hash;
}
