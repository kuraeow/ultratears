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

#include "host.h"

#define MAX_HITS 256
#define CHAIN 8

typedef struct { uint64_t rip, addr; uint32_t count, last; uint64_t chain[CHAIN]; int nchain; uint64_t x[6], lr; } hit_t;   // x, lr: guest registers at the last write
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
        for (int i = 0; i < hits[k].nchain; i++) {
            runtime_symbolize(hits[k].chain[i], name, sizeof name);
            fprintf(stderr, "[watch]            <- %s\n", name);
        }
    }
}

void runtime_watch_tick(uint64_t present) {
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
#endif
