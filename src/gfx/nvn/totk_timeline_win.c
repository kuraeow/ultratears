#include <stdint.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/syscall.h>
static inline uint32_t GetCurrentThreadId(void) {
    return (uint32_t)syscall(SYS_gettid);
}
#endif
// [Windows port] One-frame-chain timeline (totk_timeline.h), from MacNativeGames' totk_timeline.c (MIT). Changes: the
// host clock is runtime_now_ns (QueryPerformanceCounter), thread ids from GetCurrentThreadId; GPU spans come from the
// Vulkan back end (a timestamp at the start and end of each command buffer, placed so that it ends when the retire thread
// saw its fence). TOTK_TIMELINE=<file>:<first frame>:<frames>; tools/dev_timeline.py reads the file.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "totk_timeline.h"

uint64_t runtime_now_ns(void);

typedef struct { uint64_t t, a, b; uint32_t kind, tid; uint64_t frame; } tl_ev;
enum { TL_CAP = 1 << 20 };
static tl_ev *g_ev;
static _Atomic uint64_t g_n, g_commit, g_frame;
static _Atomic int g_state = -1;   // -1 unread, 0 off, 1 waiting for the window, 2 recording, 3 written
static uint64_t g_first, g_count;
static char g_path[1024];

static void tl_init(void) {
    const char *e = getenv("TOTK_TIMELINE");
    int st = 0;
    if (e && *e) {   // <file>:<first>:<frames>; the file may hold a drive letter's colon
        const char *c1 = strrchr(e, ':');
        const char *c0 = c1 ? c1 - 1 : NULL;
        while (c0 && c0 > e && *c0 != ':') c0--;
        if (c1 && c0 && c0 > e && *c0 == ':' && (size_t)(c0 - e) < sizeof g_path) {
            memcpy(g_path, e, (size_t)(c0 - e)); g_path[c0 - e] = 0;
            g_first = strtoull(c0 + 1, NULL, 10); g_count = strtoull(c1 + 1, NULL, 10);
            g_ev = calloc(TL_CAP, sizeof *g_ev);
            if (g_ev && g_count) st = 1;
        }
        if (!st) fprintf(stderr, "[timeline] TOTK_TIMELINE=%s: want <file>:<first frame>:<frames>; not recording\n", e);
        else fprintf(stderr, "[timeline] recording %llu frames from frame %llu into %s\n", (unsigned long long)g_count, (unsigned long long)g_first, g_path);
    }
    int want = -1; atomic_compare_exchange_strong(&g_state, &want, st);
}
int totk_tl_on(void) {
    int s = atomic_load_explicit(&g_state, memory_order_relaxed);
    if (s < 0) { tl_init(); s = atomic_load(&g_state); }
    return s == 2;
}
void totk_tl_at(int kind, uint64_t t_ns, uint64_t a, uint64_t b) {
    if (!totk_tl_on()) return;
    uint64_t i = atomic_fetch_add_explicit(&g_n, 1, memory_order_relaxed);
    if (i >= TL_CAP) return;
    g_ev[i] = (tl_ev){t_ns, a, b, (uint32_t)kind, (uint32_t)GetCurrentThreadId(), atomic_load_explicit(&g_frame, memory_order_relaxed)};
}
void totk_tl(int kind, uint64_t a, uint64_t b) {
    if (!totk_tl_on()) return;
    totk_tl_at(kind, runtime_now_ns(), a, b);
}
uint64_t totk_tl_commit_id(void) { return atomic_fetch_add(&g_commit, 1) + 1; }

static void tl_write(void) {
    FILE *f = fopen(g_path, "w");
    if (!f) { fprintf(stderr, "[timeline] cannot write %s\n", g_path); return; }
    uint64_t n = atomic_load(&g_n); if (n > TL_CAP) n = TL_CAP;
    fprintf(f, "# t_ns\tkind\ttid\tframe\ta\tb\n");
    static const char *names[] = {"?", "sub_in", "sub_out", "rep_begin", "rep_end", "commit", "gpu", "wait_in", "wait_out",
                                  "acq_in", "acq_out", "pres_in", "pres_out", "finish_in", "finish_out", "fence", "rs_join"};
    for (uint64_t i = 0; i < n; i++) {
        const tl_ev *e = &g_ev[i];
        fprintf(f, "%llu\t%s\t%u\t%llu\t%llu\t%llu\n", (unsigned long long)e->t, e->kind < sizeof names / sizeof *names ? names[e->kind] : "?",
                e->tid, (unsigned long long)e->frame, (unsigned long long)e->a, (unsigned long long)e->b);
    }
    fclose(f);
    fprintf(stderr, "[timeline] wrote %llu events to %s\n", (unsigned long long)n, g_path);
}
void totk_tl_frame(uint64_t presents) {
    totk_tl_on();
    atomic_store(&g_frame, presents);
    int s = atomic_load(&g_state);
    if (s == 1 && presents >= g_first) atomic_store(&g_state, 2);
    // GPU spans of the last frames arrive after their presents: 30 more frames, then the file
    else if (s == 2 && presents >= g_first + g_count + 30) { atomic_store(&g_state, 3); tl_write(); }
}
