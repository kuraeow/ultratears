// Fidelity audit of the Vulkan NVN replayer: counting of substitutions and skips (vkb_audit.h).
// Counting is always on (array increments on the replay thread); the per-shader tables and the report only within
// a TOTK_AUDIT window.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#define _mkdir(path) mkdir(path, 0777)
#endif

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "vkb_audit.h"

#define VLOG(...) LOG("[vk] " __VA_ARGS__)

static const char *const g_name[A_COUNT] = {
    "draw skipped: no program / translation", "draw skipped: primitive type", "draw skipped: targets not attachable",
    "draw skipped: index buffer not imported", "draw skipped: resource outside layout", "render target left out",
    "texture slot unbound", "separate texture handle", "texture: no image", "texture not in sampled layout",
    "texture contents not uploadable", "texture view adjusted to the shader", "storage image missing",
    "uniform buffer not imported (zeros)", "uniform buffer copied (misaligned)", "storage buffer missing",
    "storage buffer source unknown", "vertex buffer not imported (zeros)", "integer attribute read as float",
    "fragment input not written by vertex shader", "multisampling replayed single-sampled",
    "draw while rendering disabled / conditional", "masked clear not replayed", "buffer copy skipped",
    "uniform update skipped", "border colour approximated", "command not replayed",
    "texture copy not replayed (form)", "written buffer left in host memory (slow)",
    "mirror sent back early (straddling access)"};

typedef struct { char key[13]; uint64_t n; } keyc;
typedef struct { uint64_t events, draws, frames; keyc keys[16]; int nkeys, frame_hit; } stat_t;
static stat_t g_stat[A_COUNT];
static uint32_t g_draw_mask[(A_COUNT + 31) / 32];
static uint64_t g_draws, g_presents;
static int g_on;                    // inside a window
static uint64_t g_from, g_to;       // the window [from, to)
static const char *g_spec;          // the rest of TOTK_AUDIT (next windows)
static int g_init;

static void next_window(void) {
    g_from = g_to = 0;
    if (!g_spec || !*g_spec) return;
    char *end;
    g_from = strtoull(g_spec, &end, 10);
    uint64_t n = *end == ':' ? strtoull(end + 1, &end, 10) : 300;
    g_to = g_from + n;
    g_spec = strchr(end, ',');
    if (g_spec) g_spec++;
}

static void flush_draw(void) {
    for (int r = 0; r < A_COUNT; r++)
        if (g_draw_mask[r / 32] >> (r % 32) & 1) {
            g_stat[r].draws++;
            if (!g_stat[r].frame_hit) { g_stat[r].frame_hit = 1; g_stat[r].frames++; }
        }
    memset(g_draw_mask, 0, sizeof g_draw_mask);
}

void vkb_audit_draw_begin(void) {
    if (!g_on) {   // off (and nothing left of the last window): every recording thread passes here, so no writes
        int any = 0;
        for (unsigned i = 0; i < sizeof g_draw_mask / sizeof *g_draw_mask; i++) any |= g_draw_mask[i] != 0;
        if (!any) return;
    }
    flush_draw();
    if (g_on) g_draws++;
}

void vkb_audit(int reason, const char *key) {
    if (!g_on || reason < 0 || reason >= A_COUNT) return;
    stat_t *s = &g_stat[reason];
    s->events++;
    g_draw_mask[reason / 32] |= 1u << (reason % 32);
    if (!key) key = "-";
    for (int i = 0; i < s->nkeys; i++)
        if (!strncmp(s->keys[i].key, key, 12)) { s->keys[i].n++; return; }
    if (s->nkeys < 16) { snprintf(s->keys[s->nkeys].key, 13, "%.12s", key); s->keys[s->nkeys++].n = 1; }
}

static void report(void) {
    char path[256];
    _mkdir("diag");   // development output in the game folder (it describes the game's frames)
    snprintf(path, sizeof path, "diag/audit-%llu.md", (unsigned long long)g_from);
    FILE *f = fopen(path, "w");
    uint64_t frames = g_presents ? g_presents : 1;
    char line[512];
    snprintf(line, sizeof line, "Audit of presents %llu..%llu: %llu draws/dispatches (%.0f per frame)",
             (unsigned long long)g_from, (unsigned long long)g_to - 1, (unsigned long long)g_draws, (double)g_draws / frames);
    VLOG("%s", line);
    if (f) fprintf(f, "# %s\n\n| reason | draws per frame | events per frame | frames | top shaders |\n|---|---|---|---|---|\n", line);
    int order[A_COUNT];
    for (int i = 0; i < A_COUNT; i++) order[i] = i;
    for (int i = 0; i < A_COUNT; i++)   // by affected draws
        for (int j = i + 1; j < A_COUNT; j++)
            if (g_stat[order[j]].draws > g_stat[order[i]].draws) { int t = order[i]; order[i] = order[j]; order[j] = t; }
    for (int k = 0; k < A_COUNT; k++) {
        stat_t *s = &g_stat[order[k]];
        if (!s->events) continue;
        for (int i = 0; i < s->nkeys; i++)   // shaders by count
            for (int j = i + 1; j < s->nkeys; j++)
                if (s->keys[j].n > s->keys[i].n) { keyc t = s->keys[i]; s->keys[i] = s->keys[j]; s->keys[j] = t; }
        char keys[256] = "";
        int n = 0;
        for (int i = 0; i < s->nkeys && i < 6 && n < 200; i++) n += snprintf(keys + n, sizeof keys - n, "%s%s %llu", i ? ", " : "", s->keys[i].key, (unsigned long long)s->keys[i].n);
        VLOG("  %-48s %8.1f draws/frame %8.1f events/frame %5llu frames  %s", g_name[order[k]], (double)s->draws / frames,
             (double)s->events / frames, (unsigned long long)s->frames, keys);
        if (f) fprintf(f, "| %s | %.1f | %.1f | %llu | %s |\n", g_name[order[k]], (double)s->draws / frames, (double)s->events / frames,
                       (unsigned long long)s->frames, keys);
    }
    if (f) fclose(f);
}

void vkb_audit_present(uint64_t present) {
    if (!g_init) {
        g_init = 1;
        g_spec = getenv("TOTK_AUDIT");
        next_window();
    }
    flush_draw();
    for (int r = 0; r < A_COUNT; r++) g_stat[r].frame_hit = 0;
    if (g_on) g_presents++;
    if (g_on && present + 1 >= g_to) {   // the window ends with this present
        report();
        g_on = 0;
        next_window();
    }
    if (!g_on && g_to && present + 1 >= g_from && present + 1 < g_to) {
        g_on = 1;
        memset(g_stat, 0, sizeof g_stat);
        g_draws = g_presents = 0;
    }
}
