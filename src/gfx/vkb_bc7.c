// ASTC textures as BC7 (decided 2026-10-24): desktop GPUs and the Steam Deck have no ASTC, so
// ASTC was decoded on the CPU to RGBA8 at every upload (the hitches when new parts of the world stream in, 4x the video
// memory). BC7 is what these GPUs sample natively, at 8 bits per texel - not fewer bits than TOTK's ASTC blocks.
// A cache of BC7 data by the contents of the ASTC blocks: the key of a surface (one level of one layer; every slice of a
// 3D level) is a 128-bit hash of its ASTC blocks in rows (as deswizzled for an upload) with its size and block size
// (tex/bc7key.c) - the same bytes whichever way the game lays the texture out in memory, so the build can fill the cache
// from the RomFS (next step) and the game finds the entries. Until then the cache fills as textures are met: a miss is
// decoded as before and encoded to BC7 in the background for the next time.
// Files (TOTK_BC7_DIR, default data/textures): bc7.pack (the blocks, appended) and bc7.idx (32-byte records: key,
// offset, length, format - 0 BC7, 1 BC1; appended after the blocks are written, so an entry always has its data).
// BC1: the interface's 12x12 ASTC maps when opaque (tools/bc7build: a texture is BC1 only if none of its texels is
// transparent; half the bytes of BC7, and 12x12 ASTC holds less detail than BC1 keeps). Nothing from the game is
// in the repository or a build: the cache is made on the player's machine from the player's copy (README.md, Legal).
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#define _mkdir(path) mkdir(path, 0777)
#define _fseeki64(file, offset, origin) fseeko(file, offset, origin)
#define _ftelli64(file) ftello(file)
#endif

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "vkb.h"
#include "tex/bc7.h"
#include "tex/bc7key.h"

#define VLOG(...) LOG("[vk] " __VA_ARGS__)

int vkb_bc7_on(void) {   // TOTK_BC7=0: decode ASTC to RGBA8 as before (A/B)
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_BC7"); on = !(e && *e == '0'); }
    return on;
}

// ---- the key (tex/bc7key.c: the build computes the same)
vkb_bc7_key vkb_bc7_key_of(const uint8_t *blocks, size_t n, uint32_t w, uint32_t h, uint32_t depth, uint32_t bw, uint32_t bh, int srgb) {
    (void)srgb;   // not part of the key: the BC7 bytes serve both views
    bc7_key k = bc7_key_of(blocks, n, w, h, depth, bw, bh);
    return (vkb_bc7_key){k.a, k.b};
}

// ---- the index in memory (open addressing) and the files
typedef struct { uint64_t a, b, off; uint32_t len, fmt; } rec_t;   // also the record in bc7.idx (fmt: VKB_BC_*)
static rec_t *g_tab;
static size_t g_cap, g_n;
static FILE *g_pack, *g_idx;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_open;   // 1 open, -1 cannot be used
static uint64_t g_hits, g_misses, g_encoded, g_dropped, g_encode_ns;

static rec_t *slot(uint64_t a, uint64_t b) {   // g_mu held, g_cap a power of two
    size_t h = (size_t)(a ^ (b * 0x9E3779B97F4A7C15ull)) & (g_cap - 1);
    while ((g_tab[h].a || g_tab[h].b) && !(g_tab[h].a == a && g_tab[h].b == b)) h = (h + 1) & (g_cap - 1);
    return &g_tab[h];
}
static void put(const rec_t *r) {   // g_mu held
    if ((g_n + 1) * 2 > g_cap) {
        rec_t *old = g_tab;
        size_t n = g_cap;
        g_cap = g_cap ? g_cap * 2 : 1 << 16;
        if (!(g_tab = calloc(g_cap, sizeof *g_tab))) die("BC7 index");
        for (size_t i = 0; i < n; i++) if (old[i].a || old[i].b) *slot(old[i].a, old[i].b) = old[i];
        free(old);
    }
    rec_t *s = slot(r->a, r->b);
    if (!s->a && !s->b) g_n++;
    *s = *r;
}
static const char *dir(void) {
    const char *d = getenv("TOTK_BC7_DIR");
    return d && *d ? d : "data/textures";
}
static int open_files(void) {   // g_mu held
    if (g_open) return g_open > 0;
    g_open = -1;
    char path[512];
    const char *d = dir();
    for (char *p = strcpy(path, d) + 1; *p; p++) if (*p == '/' || *p == '\\') { char c = *p; *p = 0; _mkdir(path); *p = c; }
    _mkdir(path);
    snprintf(path, sizeof path, "%s/bc7.pack", d);
    if (!(g_pack = fopen(path, "r+b")) && !(g_pack = fopen(path, "w+b"))) { VLOG("BC7 cache: cannot open %s", path); return 0; }
    _fseeki64(g_pack, 0, SEEK_END);
    int64_t pack_size = _ftelli64(g_pack);
    snprintf(path, sizeof path, "%s/bc7.idx", d);
    if (!(g_idx = fopen(path, "r+b")) && !(g_idx = fopen(path, "w+b"))) { VLOG("BC7 cache: cannot open %s", path); return 0; }
    rec_t r;
    int64_t good = 0;
    while (fread(&r, sizeof r, 1, g_idx) == 1) {
        if ((int64_t)(r.off + r.len) > pack_size) break;   // the blocks of an interrupted write: dropped
        put(&r);
        good += sizeof r;
    }
    _fseeki64(g_idx, good, SEEK_SET);
    VLOG("BC7 cache %s: %zu surfaces, %.1f MiB", d, g_n, pack_size / 1048576.0);
    g_open = 1;
    return 1;
}

int vkb_bc7_lookup(vkb_bc7_key k, int *fmt, size_t *len) {
    pthread_mutex_lock(&g_mu);
    int ok = open_files() && g_cap;
    if (ok) { rec_t *s = slot(k.a, k.b); ok = (s->a || s->b) && s->len; if (ok) { *fmt = (int)s->fmt; *len = s->len; } }
    pthread_mutex_unlock(&g_mu);
    return ok;
}
int vkb_bc7_has(vkb_bc7_key k, size_t n) {
    int fmt; size_t len;
    return vkb_bc7_lookup(k, &fmt, &len) && fmt == VKB_BC_7 && len == n;
}
int vkb_bc7_read(vkb_bc7_key k, uint8_t *out, size_t n) {
    pthread_mutex_lock(&g_mu);
    int ok = open_files() && g_cap;
    rec_t r = {0};
    if (ok) { r = *slot(k.a, k.b); ok = (r.a || r.b) && r.len == n; }
    if (ok) ok = !_fseeki64(g_pack, (int64_t)r.off, SEEK_SET) && fread(out, 1, n, g_pack) == n;
    if (ok) g_hits++; else g_misses++;
    pthread_mutex_unlock(&g_mu);
    return ok;
}
void vkb_bc7_store(vkb_bc7_key k, const uint8_t *data, size_t n) { vkb_bc_store(k, data, n, VKB_BC_7); }
void vkb_bc_store(vkb_bc7_key k, const uint8_t *data, size_t n, int fmt) {
    pthread_mutex_lock(&g_mu);
    if (open_files() && !(g_cap && (slot(k.a, k.b)->a || slot(k.a, k.b)->b))) {
        _fseeki64(g_pack, 0, SEEK_END);
        rec_t r = {k.a, k.b, (uint64_t)_ftelli64(g_pack), (uint32_t)n, (uint32_t)fmt};
        if (fwrite(data, 1, n, g_pack) == n && !fflush(g_pack)) {
            _fseeki64(g_idx, 0, SEEK_END);
            if (fwrite(&r, sizeof r, 1, g_idx) == 1) { fflush(g_idx); put(&r); }
        }
    }
    pthread_mutex_unlock(&g_mu);
}

// ---- encoding in the background (a miss: next time it is there)
typedef struct job { vkb_bc7_key k; uint8_t *rgba; uint32_t w, h, d; int srgb; struct job *next; } job;
static job *g_jobs, **g_tail = &g_jobs;
static size_t g_queued_bytes;
static pthread_mutex_t g_jmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_jcv = PTHREAD_COND_INITIALIZER;
enum { QUEUE_MAX = 512u << 20, ENCODERS = 2 };
static int quality(void) {   // TOTK_BC7_QUALITY=0..4 (bc7enc's uber level; background encoding)
    static int q = -1;
    // 0 by default: met in play this is a stopgap (one large surface took ~0.75 s at level 1); the build fills the cache
    if (q < 0) { const char *e = getenv("TOTK_BC7_QUALITY"); q = e && *e ? atoi(e) : 0; }
    return q;
}
static void *encoder(void *unused) {
    (void)unused;
    pthread_setname_np("totk-bc7");
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);   // the game's and the recording threads come first
#endif
    for (;;) {
        pthread_mutex_lock(&g_jmu);
        while (!g_jobs) pthread_cond_wait(&g_jcv, &g_jmu);
        job *j = g_jobs;
        g_jobs = j->next;
        if (!g_jobs) g_tail = &g_jobs;
        size_t in = (size_t)j->w * j->h * j->d * 4;
        pthread_mutex_unlock(&g_jmu);
        size_t n = bc7_size(j->w, j->h, j->d);
        if (!vkb_bc7_has(j->k, n)) {
            uint8_t *out = malloc(n);
            if (out) {
                uint64_t t0 = runtime_now_ns();
                bc7_encode(j->rgba, j->w, j->h, j->d, j->srgb, quality(), out);
                vkb_bc7_store(j->k, out, n);
                __atomic_fetch_add(&g_encode_ns, runtime_now_ns() - t0, __ATOMIC_RELAXED);
                __atomic_fetch_add(&g_encoded, 1, __ATOMIC_RELAXED);
                free(out);
            }
        }
        free(j->rgba);
        free(j);
        pthread_mutex_lock(&g_jmu);
        g_queued_bytes -= in;
        pthread_mutex_unlock(&g_jmu);
    }
    return NULL;
}
void vkb_bc7_queue(vkb_bc7_key k, const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t d, int srgb) {
    size_t in = (size_t)w * h * (d ? d : 1) * 4;
    pthread_mutex_lock(&g_jmu);
    if (g_queued_bytes + in > QUEUE_MAX) { g_dropped++; pthread_mutex_unlock(&g_jmu); return; }   // met again later
    g_queued_bytes += in;
    pthread_mutex_unlock(&g_jmu);
    job *j = malloc(sizeof *j);
    uint8_t *copy = malloc(in);
    if (!j || !copy) { free(j); free(copy); pthread_mutex_lock(&g_jmu); g_queued_bytes -= in; pthread_mutex_unlock(&g_jmu); return; }
    memcpy(copy, rgba, in);
    *j = (job){k, copy, w, h, d ? d : 1, srgb, NULL};
    static int started;
    pthread_mutex_lock(&g_jmu);
    if (!started) {
        started = 1;
        for (int i = 0; i < ENCODERS; i++) {
            pthread_t t;
            if (pthread_create(&t, NULL, encoder, NULL) == 0) pthread_detach(t);
        }
    }
    *g_tail = j;
    g_tail = &j->next;
    pthread_cond_signal(&g_jcv);
    pthread_mutex_unlock(&g_jmu);
}
// A miss on an image that is BC7 / BC1 already (its contents changed after it became one): encoded now, quickly.
void vkb_bc7_encode_now(vkb_bc7_key k, const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t d, int srgb, int fmt, uint8_t *out) {
    if (fmt == VKB_BC_1) { bc1_encode(rgba, w, h, d ? d : 1, 4, out); return; }   // (not kept: the cache keys BC1 by the build's choice)
    bc7_encode(rgba, w, h, d ? d : 1, srgb, 0, out);
    vkb_bc7_store(k, out, bc7_size(w, h, d ? d : 1));
}
void vkb_bc7_stats(uint64_t *hits, uint64_t *misses, uint64_t *encoded, uint64_t *dropped, uint64_t *encode_ns, size_t *queued) {
    pthread_mutex_lock(&g_mu);
    *hits = g_hits; *misses = g_misses;
    pthread_mutex_unlock(&g_mu);
    *encoded = __atomic_load_n(&g_encoded, __ATOMIC_RELAXED); *encode_ns = __atomic_load_n(&g_encode_ns, __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_jmu);
    *dropped = g_dropped; *queued = g_queued_bytes;
    pthread_mutex_unlock(&g_jmu);
}
