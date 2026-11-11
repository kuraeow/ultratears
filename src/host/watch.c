#ifdef _WIN32
// Memory write watch for development: which code writes a range of game memory.
//   TOTK_WATCH=<address>:<bytes>[@<present>]
// From that present on (default 1) the range's pages are guard pages. Every write into the range is recorded with the
// writing instruction, the value written (as float and hex) and a short call chain; the access then completes by
// single-stepping it, and the guard is set again. TOTK_WATCH_FRAMES (default 20) presents later, or after 256 writes,
// the report goes to stderr: one line per writing instruction (count, function f_<guest address> with the guest
// symbol, last value) and its call chain once. Reads of the pages are passed through the same way without being recorded.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "host.h"

#define MAX_HITS 256
#define CHAIN 8

typedef struct { uint64_t rip, addr; uint32_t count, last, last2; uint64_t chain[CHAIN]; int nchain; uint64_t x[6], lr, xs[6]; float seq[8][2]; int nseq; } hit_t;   // xs: x19..x24; seq: the first values written (hardware breakpoints)   // x, lr: guest registers at the last write
static hit_t hits[MAX_HITS];
static volatile LONG nhits, nwrites, nfaults;
static uint64_t lo, hi, page_lo, page_hi;
static long long arm_at = -1, report_at;
static volatile LONG armed, done;
static __thread uint64_t rearm_addr;   // a guarded access is being single-stepped on this thread
static SRWLOCK lock = SRWLOCK_INIT;
static uint64_t find_at;                     // TOTK_WATCH_FIND: the 16 bytes there, captured when their last word is written
static uint8_t find_pat[16], zero16[16];
static volatile int find_have;

static volatile LONG guarded;   // the pages existed and got the guard (they may be committed later than the arming)
static void guard(void) {
    DWORD old;
    if (VirtualProtect((void *)(uintptr_t)page_lo, (SIZE_T)(page_hi - page_lo), PAGE_READWRITE | PAGE_GUARD, &old)) guarded = 1;
}

static void chain_of(const CONTEXT *ctx, uint64_t *out, int *n) {
    CONTEXT c = *ctx;
    *n = 0;
    for (int i = 0; i < CHAIN; i++) {
        DWORD64 base = 0;
        RUNTIME_FUNCTION *f = RtlLookupFunctionEntry(c.Rip, &base, NULL);
        if (!f) break;
        void *data;
        DWORD64 frame;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &data, &frame, NULL);
        if (!c.Rip) break;
        out[(*n)++] = c.Rip;
    }
}

// TOTK_WATCH_FIND=<offset>: the 16 bytes now at that offset of the range, searched for in the rest of game memory
// (where a copied value comes from).
static void find_copies(void) {
    if (!find_at) return;
    uint64_t at = find_at;
    if (!find_have) { fprintf(stderr, "[watch] +%#llx: no non-zero write captured\n", (unsigned long long)(at - lo)); return; }
    uint8_t pat[16];
    memcpy(pat, find_pat, 16);
    float f[4];
    memcpy(f, pat, 16);
    fprintf(stderr, "[watch] searching for %g %g %g %g (written at +%#llx)\n", f[0], f[1], f[2], f[3], (unsigned long long)(at - lo));
    int found = 0;
    for (uint64_t a = GUEST_LO; a < GUEST_HI && found < 32;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) break;
        uint64_t end = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            for (uint64_t p = (uint64_t)(uintptr_t)mi.BaseAddress; p + 16 <= end && found < 32; p += 4)
                if (!memcmp((const void *)(uintptr_t)p, pat, 16) && (p < lo || p >= hi)) {
                    fprintf(stderr, "[watch] the 16 bytes at +%#llx also at %#llx\n", (unsigned long long)(at - lo), (unsigned long long)p);
                    found++;
                }
        a = end;
    }
    if (!found) fprintf(stderr, "[watch] the 16 bytes at +%#llx are nowhere else in game memory\n", (unsigned long long)(at - lo));
    // TOTK_WATCH_RATIO=1: also three floats in the same proportion as the first three (a colour scaled by an intensity)
    if (!getenv("TOTK_WATCH_RATIO") || !(f[0] > 0)) return;
    float g = f[1] / f[0], b = f[2] / f[0];
    found = 0;
    for (uint64_t a = GUEST_LO; a < GUEST_HI && found < 16;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) break;
        uint64_t end = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            for (uint64_t p = (uint64_t)(uintptr_t)mi.BaseAddress; p + 12 <= end && found < 16; p += 4) {
                float v[3];
                memcpy(v, (const void *)(uintptr_t)p, 12);
                if (!(v[0] > 1e-6f) || v[0] == f[0]) continue;
                float dg = v[1] / v[0] - g, db = v[2] / v[0] - b;
                if (dg > -1e-4f && dg < 1e-4f && db > -1e-4f && db < 1e-4f) {
                    fprintf(stderr, "[watch] proportional at %#llx: %g %g %g (x %g)\n", (unsigned long long)p, v[0], v[1], v[2], f[0] / v[0]);
                    found++;
                }
            }
        a = end;
    }
}

static LONG CALLBACK handler(EXCEPTION_POINTERS *e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code == STATUS_GUARD_PAGE_VIOLATION) {
        uint64_t a = e->ExceptionRecord->ExceptionInformation[1];
        if (a < page_lo || a >= page_hi) return EXCEPTION_CONTINUE_SEARCH;
        InterlockedIncrement(&nfaults);
        int write = e->ExceptionRecord->ExceptionInformation[0] == 1;
        if (write && a >= lo && a < hi) {
            InterlockedIncrement(&nwrites);
            uint64_t rip = e->ContextRecord->Rip;
            AcquireSRWLockExclusive(&lock);
            int k = 0;
            while (k < nhits && hits[k].rip != rip) k++;
            if (k == nhits && nhits < MAX_HITS) {
                hits[k].rip = rip;
                chain_of(e->ContextRecord, hits[k].chain, &hits[k].nchain);
                nhits++;
            }
            if (k < nhits) {
                hits[k].count++;
                hits[k].addr = a;
                Cpu *c = guest_cpu();
                if (c) { memcpy(hits[k].x, c->x, sizeof hits[k].x); hits[k].lr = c->x[30]; }
            }
            ReleaseSRWLockExclusive(&lock);
        }
        if (done) return EXCEPTION_CONTINUE_EXECUTION;   // the guard is gone (one-shot): stop watching
        rearm_addr = a;
        e->ContextRecord->EFlags |= 0x100;   // trap after the access, then guard again
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == STATUS_SINGLE_STEP && rearm_addr) {
        uint64_t a = rearm_addr;
        rearm_addr = 0;
        if (find_at && a == find_at + 12 && memcmp((const void *)(uintptr_t)find_at, zero16, 16)) {   // TOTK_WATCH_FIND: as written
            memcpy(find_pat, (const void *)(uintptr_t)find_at, 16);
            if (!find_have++) find_copies();   // at once: the source may change by the next frame
        }
        if (a >= lo && a < hi) {   // the value now in memory at the last recorded write
            AcquireSRWLockExclusive(&lock);
            for (int k = 0; k < nhits; k++)
                if (hits[k].addr == a) memcpy(&hits[k].last, (const void *)(uintptr_t)(a & ~3ull), 4);
            ReleaseSRWLockExclusive(&lock);
        }
        if (!done) guard();
        e->ContextRecord->EFlags &= ~0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void report(void) {
    fprintf(stderr, "[watch] %#llx+%llu: %ld writes, %ld guard faults, %ld writing instructions\n", (unsigned long long)lo,
            (unsigned long long)(hi - lo), nwrites, nfaults, nhits);
    for (int k = 0; k < nhits; k++) {
        char name[200];
        float f;
        memcpy(&f, &hits[k].last, 4);
        runtime_symbolize(hits[k].rip, name, sizeof name);
        fprintf(stderr, "[watch] %6u x  +%#llx  last %g (%#x)  %s\n", hits[k].count, (unsigned long long)(hits[k].addr - lo), f,
                hits[k].last, name);
        fprintf(stderr, "[watch]            guest x0..x5 %#llx %#llx %#llx %#llx %#llx %#llx  lr %#llx\n",
                (unsigned long long)hits[k].x[0], (unsigned long long)hits[k].x[1], (unsigned long long)hits[k].x[2],
                (unsigned long long)hits[k].x[3], (unsigned long long)hits[k].x[4], (unsigned long long)hits[k].x[5],
                (unsigned long long)hits[k].lr);
        if (hits[k].nseq) {
            fprintf(stderr, "[watch]            at %#llx; x19..x24 %#llx %#llx %#llx %#llx %#llx %#llx; written:", (unsigned long long)hits[k].addr,
                    (unsigned long long)hits[k].xs[0], (unsigned long long)hits[k].xs[1], (unsigned long long)hits[k].xs[2],
                    (unsigned long long)hits[k].xs[3], (unsigned long long)hits[k].xs[4], (unsigned long long)hits[k].xs[5]);
            for (int i = 0; i < hits[k].nseq; i++) fprintf(stderr, " (%g %g)", hits[k].seq[i][0], hits[k].seq[i][1]);
            fputc('\n', stderr);
        }
        for (int i = 0; i < hits[k].nchain; i++) {
            runtime_symbolize(hits[k].chain[i], name, sizeof name);
            fprintf(stderr, "[watch]            <- %s\n", name);
        }
    }
}

// ---- TOTK_BONES (development, L28): where a skinned draw's bone lives in game memory, and which code writes it.
// vkb_draw.c gives one bone (3x4 rows, as the vertex shader gets it); each row's rotation (12 bytes) and the translation
// are searched for in game memory, every place is logged with the floats around it, and a hardware write breakpoint
// (debug registers: no page faults, the game runs on at full speed) goes on the first place of the first row on every
// guest thread for TOTK_WATCH_FRAMES presents; the report: writing code, its call chain, guest x0..x5 and the values.
int runtime_guest_tids(unsigned long *out, int max);   // threads.c
static volatile LONG hw_used, hw_on;
static uint64_t hw_addr, hw_report_at;
// TOTK_BONES_CHAIN=<bone>: stage 1 watches the uniform copy; the skinning function (main+0x821e0) that writes it holds the
// model in x19: its bone matrices are at *(x19+0x20) + 64 * index, the index from the int16 table at *(*(x19)+24).
// Stage 2 watches that bone matrix: who writes the bone (animation / physics).
static volatile uint64_t hw_x19;
static int hw_stage;
// Stage 3 (TOTK_BONES_LOCAL=1, with TOTK_BONES_CHAIN): instead of the model-space matrix, the bone's local pose that
// main+0x28408a0 reads: entries of 0x60 bytes at *(x19+24) (+0 flags, +0x10 scale, +0x20..0x5f a 4x4, translation at
// +0x50); up to four debug registers at once (hw_more: the extra addresses, DR1..DR3).
static uint64_t hw_more[3];
static int hw_nmore;
static void hw_set_all(uint64_t addr, int on) {
    unsigned long tids[512];
    int n = runtime_guest_tids(tids, 512);
    DWORD self = GetCurrentThreadId();
    int ok = 0;
    for (int i = 0; i < n; i++) {
        if (tids[i] == self) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tids[i]);
        if (!h) continue;
        if (SuspendThread(h) != (DWORD)-1) {
            CONTEXT cx;
            memset(&cx, 0, sizeof cx);
            cx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(h, &cx)) {
                cx.Dr0 = on ? (addr & ~7ull) : 0;
                cx.Dr1 = on && hw_nmore > 0 ? (hw_more[0] & ~7ull) : 0;
                cx.Dr2 = on && hw_nmore > 1 ? (hw_more[1] & ~7ull) : 0;
                cx.Dr3 = on && hw_nmore > 2 ? (hw_more[2] & ~7ull) : 0;
                uint64_t d7 = cx.Dr7 & ~0xFFFF00FFull;
                if (on)
                    for (int s = 0; s <= hw_nmore && s < 4; s++) d7 |= (1ull << (2 * s)) | (0x9ull << (16 + 4 * s));   // Ls, write, 8 bytes
                cx.Dr7 = d7;
                cx.Dr6 = 0;
                if (SetThreadContext(h, &cx)) ok++;
            }
            ResumeThread(h);
        }
        CloseHandle(h);
    }
    fprintf(stderr, "[bones] hardware breakpoint %s on %#llx: %d of %d guest threads\n", on ? "set" : "cleared",
            (unsigned long long)(addr & ~7ull), ok, n);
}
static LONG CALLBACK hw_handler(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode != STATUS_SINGLE_STEP || !(e->ContextRecord->Dr6 & 0xF) || !hw_on) return EXCEPTION_CONTINUE_SEARCH;
    int slot = (e->ContextRecord->Dr6 & 1) ? 0 : (e->ContextRecord->Dr6 & 2) ? 1 : (e->ContextRecord->Dr6 & 4) ? 2 : 3;
    uint64_t at = slot ? (hw_more[slot - 1] & ~7ull) : (hw_addr & ~7ull);
    e->ContextRecord->Dr6 = 0;
    InterlockedIncrement(&nwrites);
    uint64_t rip = e->ContextRecord->Rip;
    AcquireSRWLockExclusive(&lock);
    int k = 0;
    while (k < nhits && (hits[k].rip != rip || hits[k].addr != at)) k++;
    if (k == nhits && nhits < MAX_HITS) { hits[k].rip = rip; hits[k].addr = at; chain_of(e->ContextRecord, hits[k].chain, &hits[k].nchain); nhits++; }
    if (k < nhits) {
        hits[k].count++;
        memcpy(&hits[k].last, (const void *)(uintptr_t)at, 4);
        memcpy(&hits[k].last2, (const void *)(uintptr_t)(at + 4), 4);
        if (hits[k].nseq < 8) memcpy(hits[k].seq[hits[k].nseq++], (const void *)(uintptr_t)at, 8);   // the first values written
        Cpu *c = guest_cpu();
        if (c) { memcpy(hits[k].x, c->x, sizeof hits[k].x); hits[k].lr = c->x[30]; memcpy(hits[k].xs, c->x + 19, sizeof hits[k].xs); if (hw_stage == 1 && !hw_x19) hw_x19 = c->x[19]; }
    }
    ReleaseSRWLockExclusive(&lock);
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void dump_near(uint64_t p) {
    const float *f = (const float *)(uintptr_t)(p - 32);
    fprintf(stderr, "[bones]   %#llx-32:", (unsigned long long)p);
    for (int i = 0; i < 24; i++) fprintf(stderr, " %g", f[i]);
    fputc('\n', stderr);
}
// floats equal within 2e-3 (relative above 1): the bone comes from view space through an inverse, not bit for bit
static int near_eq(const float *a, const void *pat, size_t n) {
    const float *b = pat;
    for (size_t i = 0; i < n; i++) {
        float d = a[i] - b[i], m = b[i] < 0 ? -b[i] : b[i];
        if (!(d <= 2e-3f * (m > 1 ? m : 1) && -d <= 2e-3f * (m > 1 ? m : 1))) return 0;
    }
    return 1;
}
static int bones_scan(const void *pat, size_t n, uint64_t skip_lo, uint64_t skip_hi, const char *what, uint64_t *first) {
    int found = 0;
    for (uint64_t a = GUEST_LO; a < GUEST_HI && found < 12;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) break;
        uint64_t end = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            for (uint64_t p = (uint64_t)(uintptr_t)mi.BaseAddress + 32; p + n + 64 <= end && found < 12; p += 4)
                if (near_eq((const float *)(uintptr_t)p, pat, n / 4) && (p < skip_lo || p >= skip_hi)) {
                    fprintf(stderr, "[bones] %s at %#llx\n", what, (unsigned long long)p);
                    dump_near(p);
                    if (!found++ && first && !*first) *first = p;
                }
        a = end;
    }
    return found;
}
void runtime_bones_probe(const float *m12, uint64_t skip_lo, uint64_t skip_hi, uint64_t copy) {
    if (InterlockedExchange(&hw_used, 1)) return;
    static const char *const names[] = {"row 0 rotation", "row 1 rotation", "row 2 rotation"};
    uint64_t at = 0;
    if (getenv("TOTK_BONES_COPY") || getenv("TOTK_BONES_CHAIN")) { at = copy; if (getenv("TOTK_BONES_CHAIN")) hw_stage = 1; goto arm; }   // straight onto the uniform copy: who writes it
    for (int r = 0; r < 3; r++) bones_scan(m12 + 4 * r, 12, skip_lo, skip_hi, names[r], r == 0 ? &at : NULL);
    float col[9] = {m12[0], m12[4], m12[8], m12[1], m12[5], m12[9], m12[2], m12[6], m12[10]};   // the rotation transposed
    uint64_t tat = 0;
    bones_scan(col, 12, skip_lo, skip_hi, "column 0 (transposed)", at ? NULL : &tat);
    float t[3] = {m12[3], m12[7], m12[11]};
    bones_scan(t, 12, skip_lo, skip_hi, "translation", NULL);
    if (!at) at = tat;
    if (!at) { fprintf(stderr, "[bones] not found elsewhere: watching the copy in the uniform ring, %#llx\n", (unsigned long long)copy); at = copy; }
arm:
    hw_addr = at;
    const char *r = getenv("TOTK_WATCH_FRAMES");
    hw_report_at = (r ? (uint64_t)atoll(r) : 20);   // presents, counted from the next tick
    AddVectoredExceptionHandler(1, hw_handler);
    hw_on = 1;
    hw_set_all(at, 1);
}

// ---- TOTK_BONES_BIRTH=<bones>:<index>:<report present>[:<x>,<z>] (development, L28): from the start, every model whose
// skeleton has that many bones is followed from its first matrix update (main+0x28408a0, through the FN_ENTER hook):
// the local pose of that bone index at first sight, and every change of it later (present, the update's caller). With
// <x>,<z> (the local translation of bone 1, the model's signature) the first four such models also get a hardware write
// breakpoint on that local pose (+0x50, translation x/y), reported at <report present>.
extern volatile int g_fnprof;
extern void (*volatile g_fnprof_cb)(uint64_t addr, uint64_t lr, uint64_t x0);
static volatile uint64_t cur_present;
static int bb_nb, bb_idx;
static long long bb_report;
static float bb_sx, bb_sz;
static int bb_sig;
typedef struct { uint64_t model; float local[16], root[16]; uint64_t first; int changes, armed; } bb_t;   // root: the parent matrix (x1) of the last update
static bb_t bbs[256];
static int nbbs;
static volatile LONG bb_pending;   // new breakpoint addresses for the next tick
static SRWLOCK bb_lock = SRWLOCK_INIT;
static void bb_log_local(const char *what, const bb_t *b, uint64_t lr) {
    const float *f = b->local;
    char name[200];
    runtime_symbolize(lr, name, sizeof name);
    fprintf(stderr, "[birth] %s model %#llx present %llu (from %s): %g %g %g | %g %g %g | %g %g %g | t %g %g %g\n", what,
            (unsigned long long)b->model, (unsigned long long)cur_present, name, f[0], f[1], f[2], f[4], f[5], f[6], f[8], f[9],
            f[10], f[12], f[13], f[14]);
}
static void bb_hit(uint64_t addr, uint64_t lr, uint64_t x0) {
    if (addr != 0x8028408a0ull || !x0) return;
    uint64_t res = *(volatile uint64_t *)(uintptr_t)x0;
    if (res < GUEST_LO || res >= GUEST_HI || *(volatile uint16_t *)(uintptr_t)(res + 0x38) != bb_nb) return;
    uint64_t locs = *(volatile uint64_t *)(uintptr_t)(x0 + 24);
    if (locs < GUEST_LO || locs >= GUEST_HI) return;
    const float *l = (const float *)(uintptr_t)(locs + 0x60 * (uint64_t)bb_idx + 0x20);
    AcquireSRWLockExclusive(&bb_lock);
    int k = 0;
    while (k < nbbs && bbs[k].model != x0) k++;
    if (k == nbbs) {
        if (nbbs < 256) {
            bb_t *b = &bbs[nbbs++];
            b->model = x0;
            b->first = cur_present;
            memcpy(b->local, l, 64);
            const float *t1 = (const float *)(uintptr_t)(locs + 0x60 + 0x50);
            fprintf(stderr, "[birth] new model %#llx res %#llx locals %#llx, bone 1 t %g %g %g\n", (unsigned long long)x0,
                    (unsigned long long)res, (unsigned long long)locs, t1[0], t1[1], t1[2]);
            bb_log_local("first", b, lr);
            float dx = t1[0] - bb_sx, dz = t1[2] - bb_sz;
            if (bb_sig && dx > -1e-3f && dx < 1e-3f && dz > -1e-3f && dz < 1e-3f) {
                int armed = 0;
                for (int i = 0; i < nbbs; i++) armed += bbs[i].armed;
                if (armed < 4) { b->armed = 1; InterlockedExchange(&bb_pending, 1); }
            }
        }
    }
    if (k < nbbs) { Cpu *c = guest_cpu(); if (c && c->x[1] >= GUEST_LO && c->x[1] < GUEST_HI) memcpy(bbs[k].root, (const void *)(uintptr_t)c->x[1], 64); }
    if (k == nbbs) {
    } else if (memcmp(bbs[k].local, l, 64)) {
        memcpy(bbs[k].local, l, 64);
        if (bbs[k].changes++ < 16) bb_log_local("changed", &bbs[k], lr);
    }
    ReleaseSRWLockExclusive(&bb_lock);
}
// ---- TOTK_BONES_MOTION=<from>:<to> (development, L28): which skeleton flaps. Over those presents every skeleton update
// (main+0x28408a0, FN_ENTER hook) compares the model-space bone translations (*(model+0x20), 64 bytes a bone) with the
// last update's; per model the largest jump is summed. At <to> the 12 models that moved most are logged.
typedef struct { uint64_t model, res; int nb, calls; float sum, maxj, ext0, extmax; float t[3 * 256]; } bm_t;   // ext: the largest bone distance from bone 0
static bm_t *bms;
static int nbms;
static long long bm_from, bm_to;
static void bm_hit(uint64_t addr, uint64_t lr, uint64_t x0) {
    (void)lr;
    if (addr != 0x8028408a0ull || x0 < GUEST_LO || x0 >= GUEST_HI) return;
    uint64_t res = *(volatile uint64_t *)(uintptr_t)x0, mats = *(volatile uint64_t *)(uintptr_t)(x0 + 0x20);
    if (res < GUEST_LO || res >= GUEST_HI || mats < GUEST_LO || mats >= GUEST_HI) return;
    int nb = *(volatile uint16_t *)(uintptr_t)(res + 0x38);
    if (nb < 2 || nb > 256) return;
    AcquireSRWLockExclusive(&bb_lock);
    int k = 0;
    while (k < nbms && bms[k].model != x0) k++;
    if (k == nbms && nbms < 4096) { memset(&bms[k], 0, sizeof bms[k]); bms[k].model = x0; bms[k].res = res; nbms++; }
    if (k < nbms) {
        bm_t *b = &bms[k];
        const float *m = (const float *)(uintptr_t)mats;
        float mj = 0;
        for (int i = 0; i < nb; i++) {
            float d = 0;
            for (int j = 0; j < 3; j++) { float e = m[16 * i + 12 + j] - b->t[3 * i + j]; d += e * e; b->t[3 * i + j] = m[16 * i + 12 + j]; }
            if (b->calls && b->nb == nb && d > mj) mj = d;
        }
        mj = sqrtf(mj);
        float ext = 0;
        for (int i = 1; i < nb; i++) {
            float d = 0;
            for (int j = 0; j < 3; j++) { float e = b->t[3 * i + j] - b->t[j]; d += e * e; }
            if (d > ext) ext = d;
        }
        ext = sqrtf(ext);
        if (!b->calls) b->ext0 = ext;
        if (ext > b->extmax) b->extmax = ext;
        b->sum += mj;
        if (mj > b->maxj) b->maxj = mj;
        b->nb = nb;
        b->calls++;
    }
    ReleaseSRWLockExclusive(&bb_lock);
}
static void bm_tick(uint64_t present) {
    static int init;
    if (!init) {
        init = 1;
        const char *e = getenv("TOTK_BONES_MOTION");
        if (!e || sscanf(e, "%lld:%lld", &bm_from, &bm_to) != 2) bm_from = -1;
        else bms = calloc(4096, sizeof *bms);
    }
    if (bm_from < 0 || !bms) return;
    if (present >= (uint64_t)bm_from && !g_fnprof_cb) { g_fnprof_cb = bm_hit; g_fnprof = 1; fprintf(stderr, "[motion] following skeletons from %llu\n", (unsigned long long)present); }
    if (present >= (uint64_t)bm_to && g_fnprof_cb == bm_hit) {
        g_fnprof = 0;
        g_fnprof_cb = NULL;
        Sleep(50);   // updates running now finish
        AcquireSRWLockExclusive(&bb_lock);
        fprintf(stderr, "[motion] %d skeletons over presents %lld-%lld; the most moving:\n", nbms, bm_from, bm_to);
        for (int r = 0; r < 40; r++) {
            int best = -1;
            for (int i = 0; i < nbms; i++)
                if (bms[i].calls >= 0 && (best < 0 || bms[i].sum > bms[best].sum)) best = i;
            if (best < 0) break;
            bm_t *b = &bms[best];
            fprintf(stderr, "[motion]  model %#llx res %#llx bones %d updates %d: summed jump %g m, largest %g m; extent %g m first, %g m largest; bone 0 t %g %g %g, last bone t %g %g %g\n",
                    (unsigned long long)b->model, (unsigned long long)b->res, b->nb, b->calls, b->sum, b->maxj, b->ext0, b->extmax, b->t[0], b->t[1], b->t[2],
                    b->t[3 * (b->nb - 1)], b->t[3 * (b->nb - 1) + 1], b->t[3 * (b->nb - 1) + 2]);
            b->calls = -1 - b->calls;
        }
        for (int i = 0; i < nbms; i++) if (bms[i].calls < 0) bms[i].calls = -1 - bms[i].calls;
        fprintf(stderr, "[motion] small skeletons (2-8 bones), the most spread:\n");
        for (int r = 0; r < 30; r++) {
            int best = -1;
            for (int i = 0; i < nbms; i++)
                if (bms[i].calls >= 0 && bms[i].nb <= 8 && (best < 0 || bms[i].extmax > bms[best].extmax)) best = i;
            if (best < 0) break;
            bm_t *b = &bms[best];
            fprintf(stderr, "[motion]  small model %#llx res %#llx bones %d updates %d: extent %g m first, %g m largest; summed jump %g m; bone 0 t %g %g %g, bone 1 t %g %g %g\n",
                    (unsigned long long)b->model, (unsigned long long)b->res, b->nb, b->calls, b->ext0, b->extmax, b->sum, b->t[0], b->t[1], b->t[2],
                    b->t[3], b->t[4], b->t[5]);
            b->calls = -1 - b->calls;
        }
        ReleaseSRWLockExclusive(&bb_lock);
        bm_from = -1;
    }
}

static void bb_tick(uint64_t present) {
    bm_tick(present);
    static int init;
    cur_present = present;
    if (!init) {
        init = 1;
        const char *e = getenv("TOTK_BONES_BIRTH");
        if (e && sscanf(e, "%d:%d:%lld", &bb_nb, &bb_idx, &bb_report) == 3 && bb_nb > bb_idx) {
            const char *s = strchr(e, ':');
            s = s ? strchr(s + 1, ':') : NULL;
            s = s ? strchr(s + 1, ':') : NULL;
            if (s && sscanf(s + 1, "%f,%f", &bb_sx, &bb_sz) == 2) bb_sig = 1;
            g_fnprof_cb = bb_hit;
            g_fnprof = 1;
            fprintf(stderr, "[birth] following %d-bone models, local pose %d%s\n", bb_nb, bb_idx, bb_sig ? ", breakpoints by signature" : "");
        }
    }
    if (!g_fnprof_cb) return;
    static uint64_t last_set;
    if (InterlockedExchange(&bb_pending, 0) || (hw_on == 2 && hw_stage == 4 && present >= last_set + 30)) {   // new addresses, or new threads
        AcquireSRWLockExclusive(&bb_lock);
        uint64_t a[4];
        int n = 0;
        for (int i = 0; i < nbbs && n < 4; i++)
            if (bbs[i].armed) a[n++] = *(uint64_t *)(uintptr_t)(bbs[i].model + 24) + 0x60 * (uint64_t)bb_idx + 0x50;
        ReleaseSRWLockExclusive(&bb_lock);
        if (n && !InterlockedExchange(&hw_used, 1)) AddVectoredExceptionHandler(1, hw_handler);
        if (n) {
            hw_addr = a[0];
            hw_nmore = n - 1;
            for (int i = 1; i < n; i++) hw_more[i - 1] = a[i];
            hw_stage = 4;
            hw_on = 2;
            hw_report_at = (uint64_t)bb_report;
            hw_set_all(hw_addr, 1);
            last_set = present;
        }
    }
    if (bb_report && present >= (uint64_t)bb_report) {
        g_fnprof = 0;
        g_fnprof_cb = NULL;
        bb_report = 0;
        AcquireSRWLockExclusive(&bb_lock);
        for (int i = 0; i < nbbs; i++) {
            bb_log_local(bbs[i].armed ? "final (armed)" : "final", &bbs[i], 0);
            const float *r = bbs[i].root;
            fprintf(stderr, "[birth]   root %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
                    r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
            uint64_t m = bbs[i].model, locs = *(uint64_t *)(uintptr_t)(m + 24), mats = *(uint64_t *)(uintptr_t)(m + 0x20);
            for (int j = 0; j < bb_nb; j++) {   // the whole skeleton: local poses and model-space matrices
                const float *lf = (const float *)(uintptr_t)(locs + 0x60 * (uint64_t)j), *mm = (const float *)(uintptr_t)(mats + 64 * (uint64_t)j);
                fprintf(stderr, "[birth]   %2d flags %08x local %g %g %g | %g %g %g | %g %g %g | t %g %g %g  model %g %g %g | %g %g %g | %g %g %g | t %g %g %g\n",
                        j, *(const uint32_t *)lf, lf[8], lf[9], lf[10], lf[12], lf[13], lf[14], lf[16], lf[17], lf[18], lf[20], lf[21], lf[22],
                        mm[0], mm[1], mm[2], mm[4], mm[5], mm[6], mm[8], mm[9], mm[10], mm[12], mm[13], mm[14]);
            }
        }
        ReleaseSRWLockExclusive(&bb_lock);
    }
}

void runtime_watch_tick(uint64_t present) {
    bb_tick(present);
    static long long hw_env_at = -1;   // TOTK_HWWATCH=<address>@<present>: the hardware write breakpoint by hand
    static uint64_t hw_env_addr;
    if (hw_env_at == -1) {
        const char *e = getenv("TOTK_HWWATCH");
        char *end = NULL;
        hw_env_addr = e ? strtoull(e, &end, 0) : 0;
        hw_env_at = hw_env_addr && end && *end == '@' ? atoll(end + 1) : 0;
    }
    if (hw_env_at > 0 && present >= (uint64_t)hw_env_at && !InterlockedExchange(&hw_used, 1)) {
        hw_env_at = 0;
        hw_addr = hw_env_addr;
        const char *r = getenv("TOTK_WATCH_FRAMES");
        hw_report_at = present + (r ? (uint64_t)atoll(r) : 20);
        AddVectoredExceptionHandler(1, hw_handler);
        hw_on = 2;
        hw_set_all(hw_addr, 1);
    }
    if (hw_on == 1 && hw_report_at < 1000000) hw_report_at += present;   // the deadline, from the first tick after arming
    if (hw_on == 1) hw_on = 2;
    if (hw_on == 2 && hw_stage == 1 && hw_x19) {   // stage 2: the bone matrix the skinning read
        const char *b = getenv("TOTK_BONES_CHAIN");
        int bone = b ? atoi(b) : 0;
        uint64_t obj = hw_x19, res = *(uint64_t *)(uintptr_t)obj, tab = *(uint64_t *)(uintptr_t)(res + 24), mats = *(uint64_t *)(uintptr_t)(obj + 0x20);
        int idx = *(int16_t *)(uintptr_t)(tab + 2 * (uint64_t)bone);
        uint64_t src = mats + 64 * (uint64_t)(int64_t)idx;
        const float *f = (const float *)(uintptr_t)src;
        fprintf(stderr, "[bones] chain: model %#llx, resource %#llx, index table %#llx, matrices %#llx; bone %d = index %d at %#llx: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n",
                (unsigned long long)obj, (unsigned long long)res, (unsigned long long)tab, (unsigned long long)mats, bone, idx,
                (unsigned long long)src, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
        hw_set_all(hw_addr, 0);
        lo = hw_addr; hi = hw_addr + 8;
        report();
        AcquireSRWLockExclusive(&lock); nhits = 0; nwrites = 0; ReleaseSRWLockExclusive(&lock);
        hw_stage = 2;
        hw_addr = src;
        // the whole skeleton: count at *(resource+0x38), local poses at *(model+24) (0x60 each), matrices at *(model+0x20)
        uint64_t locs = *(uint64_t *)(uintptr_t)(obj + 24);
        int nb = *(uint16_t *)(uintptr_t)(res + 0x38);
        fprintf(stderr, "[bones] skeleton: %d bones, local poses %#llx\n", nb, (unsigned long long)locs);
        for (int i = 0; i < nb && i < 64; i++) {
            const uint32_t *l = (const uint32_t *)(uintptr_t)(locs + 0x60 * (uint64_t)i);
            const float *lf = (const float *)l, *m = (const float *)(uintptr_t)(mats + 64 * (uint64_t)i);
            fprintf(stderr, "[bones]  %2d local %08x %08x %08x %08x | s %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g | t %g %g %g %g"
                    "  model %g %g %g | %g %g %g | %g %g %g | t %g %g %g\n", i, l[0], l[1], l[2], l[3], lf[4], lf[5], lf[6], lf[7],
                    lf[8], lf[9], lf[10], lf[11], lf[12], lf[13], lf[14], lf[15], lf[16], lf[17], lf[18], lf[19], lf[20], lf[21], lf[22],
                    lf[23], m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14]);
        }
        if (getenv("TOTK_BONES_LOCAL")) {   // stage 3: the local pose of that bone
            uint64_t l = locs + 0x60 * (uint64_t)(int64_t)idx;
            hw_stage = 3;
            hw_addr = l + 0x50;
            hw_more[0] = l + 0x58; hw_more[1] = l + 0x20; hw_more[2] = l;
            hw_nmore = 3;
            fprintf(stderr, "[bones] stage 3: local pose of index %d at %#llx (+0x50 / +0x58 / +0x20 / +0)\n", idx, (unsigned long long)l);
        }
        const char *r = getenv("TOTK_WATCH_FRAMES");
        hw_report_at = present + (r ? (uint64_t)atoll(r) : 20);
        hw_set_all(hw_addr, 1);
    }
    if (hw_on == 2 && present >= hw_report_at) {
        hw_on = 0;
        hw_set_all(hw_addr, 0);
        lo = hw_addr; hi = hw_addr + 8;
        if (hw_stage == 3) { lo = hw_more[2]; hi = lo + 0x60; }
        report();
    }
    if (arm_at == -1) {
        const char *e = getenv("TOTK_WATCH");
        arm_at = 0;
        if (e) {
            char *end;
            lo = strtoull(e, &end, 0);
            uint64_t n = *end == ':' ? strtoull(end + 1, &end, 0) : 0;
            arm_at = *end == '@' ? strtoll(end + 1, NULL, 10) : 1;
            hi = lo + n;
            page_lo = lo & ~0xFFFull;
            page_hi = (hi + 0xFFF) & ~0xFFFull;
            if (!n) arm_at = 0;
            const char *f = getenv("TOTK_WATCH_FIND");
            if (f) find_at = lo + strtoull(f, NULL, 0);
        }
    }
    if (arm_at <= 0 || done) return;
    if (!armed && present >= (uint64_t)arm_at) {
        armed = 1;
        const char *r = getenv("TOTK_WATCH_FRAMES");
        report_at = (long long)present + (r ? atoll(r) : 20);
        AddVectoredExceptionHandler(1, handler);
        guard();
        fprintf(stderr, "[watch] armed on %#llx+%llu at present %llu\n", (unsigned long long)lo, (unsigned long long)(hi - lo),
                (unsigned long long)present);
    } else if (armed && !guarded) {
        guard();
    } else if (armed && ((long long)present >= report_at || nhits >= MAX_HITS)) {
        done = 1;
        DWORD old;
        VirtualProtect((void *)(uintptr_t)page_lo, (SIZE_T)(page_hi - page_lo), PAGE_READWRITE, &old);
        report();
    }
}
#else
#include "host.h"

void runtime_watch_tick(uint64_t present) {
    (void)present;
}
void runtime_bones_probe(const float *m12, uint64_t skip_lo, uint64_t skip_hi, uint64_t copy) {
    (void)m12; (void)skip_lo; (void)skip_hi; (void)copy;
}
#endif
